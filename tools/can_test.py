"""Bench tests for the CAN gateway, using python-can (PEAK PCAN-USB by default).

Wiring: the --channel adapter goes on the gateway's CAN1 side (it transmits),
the optional --rx-channel adapter goes on the CAN2 side (it listens). Each bus
needs 120 ohm termination at both ends.

    pip install python-can pyserial

    # two adapters (or a dual-channel one): everything is checked on the wire
    python tools/can_test.py --interface pcan --channel PCAN_USBBUS1 \\
        --rx-channel PCAN_USBBUS2 --host cangw.local all

    # one adapter: results are read back from the gateway's own counters
    python tools/can_test.py --channel PCAN_USBBUS1 --port COM12 sweep burst

Tests: sweep, toggle, burst, latency, all.
The gateway is reached over HTTP (--host) or its serial console (--port); one
of the two is needed for the toggle test and for any single-adapter run.
"""

import argparse
import json
import re
import struct
import sys
import time
import urllib.request

import can

DEFAULT_BLOCKED = {(0x182, False), (0x427, False)}


# ---------------------------------------------------------------- gateway --

class HttpDevice:
    def __init__(self, host, password=None):
        self.base = "http://" + host
        self.cookie = None
        if password:
            resp = self._request("POST", "/api/login", {"password": password}, raw=True)
            self.cookie = (resp.headers.get("Set-Cookie") or "").split(";")[0]

    def _request(self, method, path, body=None, raw=False):
        req = urllib.request.Request(self.base + path, method=method)
        if method != "GET":
            req.add_header("X-CANGW", "1")
        if self.cookie:
            req.add_header("Cookie", self.cookie)
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            req.add_header("Content-Type", "application/json")
        resp = urllib.request.urlopen(req, data=data, timeout=5)
        return resp if raw else json.loads(resp.read().decode())

    def block(self, can_id, ext):
        self._request("POST", "/api/filters", {"id": can_id, "ext": ext})

    def unblock(self, can_id, ext):
        self._request("DELETE", "/api/filters", {"id": can_id, "ext": ext})

    def blocked(self):
        return {(f["id"], f["ext"]) for f in self._request("GET", "/api/filters")}

    def counters(self):
        s = self._request("GET", "/api/status")
        return {"rx1": s["b1"]["rx"], "forwarded": s["gw"]["fwd"], "blocked": s["gw"]["blk"],
                "dropped": s["gw"]["drop"], "tx2": s["b2"]["tx"], "missed": s["b1"]["miss"]}


class SerialDevice:
    def __init__(self, port):
        import serial  # pyserial
        # Set the control lines before opening: with them asserted, opening
        # the ESP32-S3's USB serial port would reset the gateway.
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.3
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        time.sleep(0.2)
        self._command("")

    def _command(self, line):
        self.ser.reset_input_buffer()
        self.ser.write((line + "\r\n").encode())
        deadline = time.time() + 1.5
        out = b""
        while time.time() < deadline:
            chunk = self.ser.read(4096)
            out += chunk
            if not chunk and out.rstrip().endswith(b"cangw>"):
                break
        return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", out.decode(errors="replace"))

    @staticmethod
    def _arg(can_id, ext):
        return "%X%s" % (can_id, "x" if ext else "")

    def block(self, can_id, ext):
        self._command("block " + self._arg(can_id, ext))

    def unblock(self, can_id, ext):
        self._command("unblock " + self._arg(can_id, ext))

    def blocked(self):
        found = re.findall(r"0x([0-9A-Fa-f]+) (std|ext)", self._command("list"))
        return {(int(h, 16), kind == "ext") for h, kind in found}

    def counters(self):
        text = self._command("status")

        def grab(pattern):
            m = re.search(pattern, text)
            if not m:
                raise RuntimeError("could not read the gateway status over serial:\n" + text)
            return int(m.group(1))

        return {"rx1": grab(r"CAN1 .*?rx (\d+)"), "forwarded": grab(r"forwarded (\d+)"),
                "blocked": grab(r"blocked (\d+)"), "dropped": grab(r"dropped (\d+)"),
                "tx2": grab(r"CAN2 .*?tx (\d+)"), "missed": grab(r"missed (\d+)")}


# ---------------------------------------------------------------- helpers --

class Bench:
    def __init__(self, args):
        self.args = args
        self.tx = can.Bus(interface=args.interface, channel=args.channel, bitrate=args.bitrate * 1000)
        self.rx = None
        if args.rx_channel:
            self.rx = can.Bus(interface=args.interface, channel=args.rx_channel,
                              bitrate=(args.rx_bitrate or args.bitrate) * 1000)
        self.dev = None
        if args.host:
            self.dev = HttpDevice(args.host, args.password)
        elif args.port:
            self.dev = SerialDevice(args.port)

    def close(self):
        self.tx.shutdown()
        if self.rx:
            self.rx.shutdown()

    def send(self, can_id, ext, data):
        msg = can.Message(arbitration_id=can_id, is_extended_id=ext, data=data)
        deadline = time.time() + 2
        while True:
            try:
                self.tx.send(msg, timeout=0.2)
                return
            except can.CanError:
                # Adapter transmit queue full: the bus is the bottleneck.
                if time.time() > deadline:
                    raise
                time.sleep(0.0005)

    def drain(self, seconds=0.3):
        """Collect everything that arrives on the CAN2 side until it goes quiet."""
        out = []
        if not self.rx:
            time.sleep(seconds)
            return out
        while True:
            msg = self.rx.recv(timeout=seconds)
            if msg is None:
                return out
            if not msg.is_error_frame:
                out.append(msg)

    def blocked(self):
        return self.dev.blocked() if self.dev else set(DEFAULT_BLOCKED)

    def need(self, what, ok):
        if not ok:
            raise SkipTest(what)


class SkipTest(Exception):
    pass


def report(name, ok, detail):
    print("%-8s %s  %s" % (name, "PASS" if ok else "FAIL", detail))
    return ok


# ------------------------------------------------------------------ tests --

def test_sweep(b):
    """Every ID except the blocked ones must come out on CAN2."""
    b.need("needs --rx-channel, or --host/--port to read counters", b.rx or b.dev)
    blocked = b.blocked()
    ids = [(i, False) for i in range(0x800)]
    ids += [(i, True) for i in (0x0, 0x182, 0x7FF, 0x800, 0x18FF50E5, 0x0CF00400, 0x1FFFFFFF)]
    ids += [(i, True) for i in range(0x18DA0000, 0x18DA0040)]
    expected = [key for key in ids if key not in blocked]

    b.drain(0.2)
    before = b.dev.counters() if b.dev else None
    for can_id, ext in ids:
        b.send(can_id, ext, struct.pack(">I", can_id))
    got = b.drain(0.5)

    ok, notes = True, []
    if b.rx:
        seen = [(m.arbitration_id, m.is_extended_id) for m in got]
        missing = sorted(set(expected) - set(seen))
        leaked = sorted(set(seen) & blocked)
        ok = not missing and not leaked and len(seen) == len(expected)
        notes.append("%d sent, %d received on CAN2 (expected %d)" % (len(ids), len(seen), len(expected)))
        if missing:
            notes.append("missing: " + ", ".join("0x%X" % i for i, _ in missing[:8]))
        if leaked:
            notes.append("blocked IDs that got through: " + ", ".join("0x%X" % i for i, _ in leaked))
        if ok and seen != expected:
            ok = False
            notes.append("order changed")
    if before:
        after = b.dev.counters()
        fwd = after["forwarded"] - before["forwarded"]
        blk = after["blocked"] - before["blocked"]
        drop = after["dropped"] - before["dropped"]
        notes.append("gateway counted %d forwarded, %d blocked, %d dropped" % (fwd, blk, drop))
        ok = ok and fwd == len(expected) and blk == len(ids) - len(expected) and drop == 0
    return report("sweep", ok, "; ".join(notes))


def test_toggle(b):
    """Blocking and unblocking an ID while it is flowing takes effect at once."""
    b.need("needs --host or --port to change the blocklist", b.dev)
    b.need("needs --rx-channel", b.rx)
    can_id, ext = b.args.test_id, False
    b.need("0x%X is already blocked, pick another --test-id" % can_id, (can_id, ext) not in b.blocked())

    def flow(seconds):
        end = time.time() + seconds
        n = 0
        while time.time() < end:
            b.send(can_id, ext, struct.pack(">I", n))
            n += 1
            time.sleep(0.001)
        return n, len(b.drain(0.2))

    b.drain(0.2)
    sent1, got1 = flow(0.5)
    b.dev.block(can_id, ext)
    time.sleep(0.05)
    b.drain(0.1)
    sent2, got2 = flow(0.5)
    b.dev.unblock(can_id, ext)
    time.sleep(0.05)
    sent3, got3 = flow(0.5)
    ok = got1 == sent1 and got2 == 0 and got3 == sent3
    return report("toggle", ok, "open %d/%d, blocked %d/%d, open again %d/%d"
                  % (got1, sent1, got2, sent2, got3, sent3))


def test_burst(b):
    """Back-to-back frames at full bus load: nothing dropped, order kept."""
    b.need("needs --rx-channel, or --host/--port to read counters", b.rx or b.dev)
    can_id = b.args.test_id
    b.need("0x%X is blocked, pick another --test-id" % can_id, (can_id, False) not in b.blocked())

    b.drain(0.2)
    before = b.dev.counters() if b.dev else None
    received = []
    seq = 0
    start = time.time()
    while time.time() - start < b.args.duration:
        # Queue in batches, and keep the receive side from overflowing.
        for _ in range(50):
            b.send(can_id, False, struct.pack(">IHH", seq, seq & 0xFFFF, 0xA55A))
            seq += 1
        if b.rx:
            while True:
                msg = b.rx.recv(timeout=0)
                if msg is None:
                    break
                received.append(msg)
    elapsed = time.time() - start
    received += b.drain(0.5)

    ok, notes = True, ["%d frames in %.1f s (%.0f frames/s)" % (seq, elapsed, seq / elapsed)]
    if b.rx:
        seqs = [struct.unpack(">I", m.data[:4])[0] for m in received
                if m.arbitration_id == can_id and len(m.data) == 8]
        in_order = all(a + 1 == c for a, c in zip(seqs, seqs[1:]))
        ok = len(seqs) == seq and in_order
        notes.append("%d received on CAN2, order %s" % (len(seqs), "kept" if in_order else "BROKEN"))
    if before:
        after = b.dev.counters()
        drop = after["dropped"] - before["dropped"]
        missed = after["missed"] - before["missed"]
        fwd = after["forwarded"] - before["forwarded"]
        notes.append("gateway counted %d forwarded, %d dropped, %d missed on CAN1" % (fwd, drop, missed))
        ok = ok and drop == 0 and missed == 0 and fwd == seq
    return report("burst", ok, "; ".join(notes))


def test_latency(b):
    """Time from the end of a frame on CAN1 to the end of its copy on CAN2."""
    b.need("needs --rx-channel", b.rx)
    can_id = b.args.test_id
    b.need("0x%X is blocked, pick another --test-id" % can_id, (can_id, False) not in b.blocked())

    # A second handle on the CAN1 adapter sees our own frames with a hardware
    # timestamp from the same clock as the CAN2 adapter.
    try:
        echo = can.Bus(interface=b.args.interface, channel=b.args.channel,
                       bitrate=b.args.bitrate * 1000, receive_own_messages=True)
    except Exception as exc:  # depends on the adapter driver
        raise SkipTest("this adapter cannot echo its own frames (%s)" % exc)
    try:
        b.drain(0.2)
        deltas = []
        for n in range(200):
            echo.send(can.Message(arbitration_id=can_id, is_extended_id=False, data=struct.pack(">Q", n)))
            sent = echo.recv(timeout=0.5)
            got = b.rx.recv(timeout=0.5)
            if sent is None or got is None:
                continue
            deltas.append(got.timestamp - sent.timestamp)
            time.sleep(0.005)
    finally:
        echo.shutdown()
    b.need("no echoed frames came back, so there are no timestamps to compare", deltas)

    # The CAN2 timestamp is taken at the end of the frame, so subtract the
    # time the copy spends on the wire to get the gateway's own delay.
    rx_kbps = b.args.rx_bitrate or b.args.bitrate
    frame_s = 111 * 1.1 / (rx_kbps * 1000.0)
    inside = sorted(max(0.0, d - frame_s) for d in deltas)
    med = inside[len(inside) // 2]
    worst = inside[-1]
    ok = len(deltas) >= 190 and worst < 0.001
    return report("latency", ok, "%d samples, gateway delay median %.0f us, worst %.0f us (wire time %.0f us removed)"
                  % (len(deltas), med * 1e6, worst * 1e6, frame_s * 1e6))


TESTS = {"sweep": test_sweep, "toggle": test_toggle, "burst": test_burst, "latency": test_latency}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--interface", default="pcan", help="python-can interface name (default: pcan)")
    p.add_argument("--channel", default="PCAN_USBBUS1", help="adapter on the CAN1 side (default: PCAN_USBBUS1)")
    p.add_argument("--rx-channel", help="adapter on the CAN2 side, e.g. PCAN_USBBUS2")
    p.add_argument("--bitrate", type=int, default=250, help="CAN1 bitrate in kbit/s (default: 250)")
    p.add_argument("--rx-bitrate", type=int, help="CAN2 bitrate in kbit/s (default: same as --bitrate)")
    p.add_argument("--host", help="gateway address for the HTTP API, e.g. cangw.local or 192.168.4.1")
    p.add_argument("--password", help="admin password, if one is set")
    p.add_argument("--port", help="gateway serial port for the CLI, e.g. COM12 (used if --host is not given)")
    p.add_argument("--test-id", type=lambda s: int(s, 16), default=0x321, help="hex ID used by toggle/burst/latency (default: 321)")
    p.add_argument("--duration", type=float, default=5.0, help="burst length in seconds (default: 5)")
    p.add_argument("tests", nargs="+", choices=list(TESTS) + ["all"])
    args = p.parse_args()

    names = list(TESTS) if "all" in args.tests else args.tests
    bench = Bench(args)
    failed = 0
    try:
        for name in names:
            try:
                if not TESTS[name](bench):
                    failed += 1
            except SkipTest as why:
                print("%-8s SKIP  %s" % (name, why))
    finally:
        bench.close()
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
