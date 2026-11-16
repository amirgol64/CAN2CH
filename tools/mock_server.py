"""Serve web/ with a simulated gateway behind it, for working on the UI.

    pip install aiohttp
    python tools/mock_server.py [--port 8080]

Then open http://localhost:8080. Nothing here talks to real hardware.
"""

import argparse
import asyncio
import json
import os
import random
import time

from aiohttp import WSMsgType, web

WEB_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "web")
START = time.time()


class Sim:
    def __init__(self):
        self.filters = {(0x182, False), (0x427, False)}
        self.filter_gen = 1
        self.clear_gen = 0
        self.forwarding = True
        self.cfg = {"br1": 250, "br2": 250, "lo1": False, "xtal": 0}
        self.dbc = {1: None, 2: None}
        self.password = None
        self.sessions = set()
        self.counters = dict(rx1=0, tx1=0, fwd=0, blk=0, off=0, drop=0, tx2=0, rx2=0)
        self.load = [0, 0]
        # (bus, id, ext, period_s)
        self.sources = [(1, 0x100 + i * 0x11, False, random.choice([0.01, 0.02, 0.05, 0.1, 0.5])) for i in range(24)]
        self.sources += [(1, 0x182, False, 0.02), (1, 0x427, False, 0.1),
                         (1, 0x18FF50E5, True, 0.1), (1, 0x0CF00400, True, 0.02),
                         (2, 0x7E8, False, 0.2), (2, 0x18DAF110, True, 1.0)]
        self.rows = {}   # (bus, id, ext) -> [count, dlc, data, last_time]
        self.dirty = set()
        self.next_due = {src: time.time() + random.random() for src in self.sources}

    def step(self):
        now = time.time()
        bits = [0, 0]
        for src in self.sources:
            bus, can_id, ext, period = src
            while self.next_due[src] <= now:
                self.next_due[src] += period
                self.frame(bus, can_id, ext, bytes(random.getrandbits(8) for _ in range(8)), now)
                bits[bus - 1] += 131 if ext else 111
        for b in range(2):
            kbps = self.cfg["br1"] if b == 0 else self.cfg["br2"]
            self.load[b] = min(1000, int(bits[b] / 0.25 / kbps))

    def frame(self, bus, can_id, ext, data, now):
        key = (bus, can_id, ext)
        row = self.rows.setdefault(key, [0, 8, b"", now])
        row[0] += 1
        row[1], row[2], row[3] = len(data), data, now
        self.dirty.add(key)
        c = self.counters
        if bus == 1:
            c["rx1"] += 1
            if not self.forwarding:
                c["off"] += 1
            elif (can_id, ext) in self.filters:
                c["blk"] += 1
            else:
                c["fwd"] += 1
                c["tx2"] += 1
        else:
            c["rx2"] += 1

    def live(self):
        c = self.counters
        return {
            "up": int((time.time() - START) * 1000),
            "b1": dict(st="running", br=self.cfg["br1"], lo=self.cfg["lo1"], rx=c["rx1"], tx=c["tx1"],
                       tec=0, rec=0, err=0, miss=0, boff=0, load=self.load[0], untracked=0, qpk=7, qsz=1024),
            "b2": dict(st="active", br=self.cfg["br2"], xtal=self.cfg["xtal"] or 16, rx=c["rx2"], tx=c["tx2"],
                       fail=0, tec=0, rec=0, ovf=0, boff=0, load=self.load[1], untracked=0),
            "gw": dict(on=self.forwarding, fwd=c["fwd"], blk=c["blk"], off=c["off"], drop=c["drop"],
                       q=random.randint(0, 3), qpk=12, qsz=2048, fg=self.filter_gen, cg=self.clear_gen),
        }

    def entries(self, keys):
        now = time.time()
        out = []
        for key in keys:
            row = self.rows.get(key)
            if row:
                out.append([key[0], key[1], int(key[2]), row[0], row[1], 0, row[2].hex().upper(),
                            int((now - row[3]) * 1000)])
        return out


sim = Sim()
sockets = set()
routes = web.RouteTableDef()


def authed(request):
    return sim.password is None or request.cookies.get("sid") in sim.sessions


def guard(request, need_auth=True):
    if "X-CANGW" not in request.headers:
        raise web.HTTPForbidden(text=json.dumps({"error": "missing X-CANGW header"}),
                                content_type="application/json")
    if need_auth and not authed(request):
        raise web.HTTPUnauthorized(text=json.dumps({"error": "login required"}),
                                   content_type="application/json")


def ok():
    return web.json_response({"ok": True})


def bad(msg, status=400):
    return web.json_response({"error": msg}, status=status)


def parse_id(value):
    return int(value, 16) if isinstance(value, str) else int(value)


@routes.get("/api/status")
async def status(request):
    data = {"fw": "mock", "serial": "CF-A1B2C3"}
    data.update(sim.live())
    data.update({
        "wifi": {"mode": "sta", "ssid": "MockNet", "sta": "MockNet", "ip": "127.0.0.1", "rssi": -52, "connected": True},
        "auth": {"required": sim.password is not None, "authed": authed(request)},
        "dbc": [len(sim.dbc[1] or b"") or -1, len(sim.dbc[2] or b"") or -1],
        "heap": 200000, "minheap": 150000, "psram": 8000000, "partition": "ota_0", "filters": len(sim.filters),
        "reset": "power on",
    })
    return web.json_response(data)


@routes.get("/api/config")
async def config_get(request):
    return web.json_response({
        "version": 1,
        "bus1": {"bitrate": sim.cfg["br1"], "listen_only": sim.cfg["lo1"]},
        "bus2": {"bitrate": sim.cfg["br2"], "xtal_mhz": sim.cfg["xtal"]},
        "forwarding": sim.forwarding,
        "wifi": {"ssid": "MockNet"},
        "filters": [{"id": "0x%X" % i, "ext": e} for i, e in sorted(sim.filters, key=lambda f: (f[1], f[0]))],
    })


@routes.put("/api/config")
async def config_put(request):
    guard(request)
    data = await request.json()
    if "filters" in data:
        sim.filters = {(parse_id(f["id"]), bool(f.get("ext"))) for f in data["filters"]}
        sim.filter_gen += 1
    sim.forwarding = data.get("forwarding", sim.forwarding)
    return ok()


@routes.get("/api/filters")
async def filters_get(request):
    return web.json_response([{"id": i, "ext": e} for i, e in sorted(sim.filters, key=lambda f: (f[1], f[0]))])


@routes.post("/api/filters")
async def filters_post(request):
    guard(request)
    data = await request.json()
    sim.filters.add((parse_id(data["id"]), bool(data.get("ext"))))
    sim.filter_gen += 1
    return ok()


@routes.delete("/api/filters")
async def filters_delete(request):
    guard(request)
    data = await request.json()
    if data.get("all"):
        sim.filters.clear()
    else:
        sim.filters.discard((parse_id(data["id"]), bool(data.get("ext"))))
    sim.filter_gen += 1
    return ok()


@routes.put("/api/bus/{bus}")
async def bus_put(request):
    guard(request)
    data = await request.json()
    if request.match_info["bus"] == "1":
        sim.cfg["br1"] = data.get("bitrate", sim.cfg["br1"])
        sim.cfg["lo1"] = data.get("listen_only", sim.cfg["lo1"])
    else:
        sim.cfg["br2"] = data.get("bitrate", sim.cfg["br2"])
        sim.cfg["xtal"] = data.get("xtal_mhz", sim.cfg["xtal"])
    return ok()


@routes.put("/api/forwarding")
async def forwarding_put(request):
    guard(request)
    sim.forwarding = bool((await request.json())["enabled"])
    return ok()


@routes.post("/api/tx")
async def tx_post(request):
    guard(request)
    data = await request.json()
    try:
        payload = bytes.fromhex(str(data.get("data", "")).replace(" ", ""))
    except ValueError:
        return bad("data must be up to 8 hex bytes")
    if len(payload) > 8:
        return bad("data must be up to 8 hex bytes")
    sim.counters["tx%d" % data["bus"]] += 1
    return ok()


@routes.get("/api/dbc/{bus}")
async def dbc_get(request):
    blob = sim.dbc.get(int(request.match_info["bus"]))
    if blob is None:
        return bad("no DBC stored for this bus", 404)
    return web.Response(body=blob, content_type="text/plain", charset="utf-8")


@routes.put("/api/dbc/{bus}")
async def dbc_put(request):
    guard(request)
    sim.dbc[int(request.match_info["bus"])] = await request.read()
    return ok()


@routes.delete("/api/dbc/{bus}")
async def dbc_delete(request):
    guard(request)
    sim.dbc[int(request.match_info["bus"])] = None
    return ok()


@routes.post("/api/login")
async def login(request):
    guard(request, need_auth=False)
    data = await request.json()
    if sim.password is not None and data.get("password") != sim.password:
        return bad("wrong password", 401)
    resp = ok()
    if sim.password is not None:
        sid = "%032x" % random.getrandbits(128)
        sim.sessions.add(sid)
        resp.set_cookie("sid", sid, httponly=True, samesite="Strict")
    return resp


@routes.post("/api/logout")
async def logout(request):
    guard(request, need_auth=False)
    sim.sessions.discard(request.cookies.get("sid"))
    resp = ok()
    resp.del_cookie("sid")
    return resp


@routes.put("/api/password")
async def password_put(request):
    guard(request)
    pw = (await request.json()).get("password", "")
    sim.password = pw or None
    sim.sessions.clear()
    resp = ok()
    if sim.password:
        sid = "%032x" % random.getrandbits(128)
        sim.sessions.add(sid)
        resp.set_cookie("sid", sid, httponly=True, samesite="Strict")
    return resp


@routes.post("/api/stats/clear")
async def stats_clear(request):
    guard(request)
    sim.rows.clear()
    sim.dirty.clear()
    sim.clear_gen += 1
    for ws in list(sockets):
        await ws.send_str(json.dumps({"t": "clr"}))
    return ok()


@routes.put("/api/wifi")
@routes.put("/api/ap-password")
@routes.post("/api/reboot")
@routes.post("/api/factory-reset")
@routes.post("/api/ota")
async def accepted(request):
    guard(request)
    await request.read()
    return ok()


@routes.get("/ws")
async def ws_handler(request):
    ws = web.WebSocketResponse()
    await ws.prepare(request)
    await ws.send_str(json.dumps({"t": "i", "e": sim.entries(list(sim.rows))}))
    sockets.add(ws)
    try:
        async for msg in ws:
            if msg.type == WSMsgType.ERROR:
                break
    finally:
        sockets.discard(ws)
    return ws


@routes.get("/")
async def index(request):
    return web.FileResponse(os.path.join(WEB_DIR, "index.html"))


async def ticker(app):
    async def loop():
        while True:
            await asyncio.sleep(0.25)
            sim.step()
            stats = dict(t="s", **sim.live())
            keys, sim.dirty = sim.dirty, set()
            rows = {"t": "i", "e": sim.entries(keys)}
            for ws in list(sockets):
                try:
                    await ws.send_str(json.dumps(stats))
                    if rows["e"]:
                        await ws.send_str(json.dumps(rows))
                except ConnectionError:
                    sockets.discard(ws)

    task = asyncio.create_task(loop())
    yield
    task.cancel()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--host", default="127.0.0.1")
    args = parser.parse_args()

    app = web.Application(client_max_size=16 * 1024 * 1024)
    app.add_routes(routes)
    app.router.add_static("/", WEB_DIR)
    app.cleanup_ctx.append(ticker)
    web.run_app(app, host=args.host, port=args.port)


if __name__ == "__main__":
    main()
