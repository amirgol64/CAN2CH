# esp32s3-can-gateway — Architecture

A one-way CAN gateway for the Waveshare ESP32-S3-CAN-2CH. It forwards every frame
received on **CAN1** to **CAN2**, except the IDs on a blocklist you can edit from
the web UI.

## 1. Hardware

| Item | Detail |
|---|---|
| MCU | ESP32-S3R8 (8 MB PSRAM), 16 MB flash |
| CAN1 | ESP32-S3 native TWAI controller, TX = GPIO15, RX = GPIO16, isolated transceiver |
| CAN2 | XL2515 (MCP2515-compatible) on SPI: CS = GPIO17, INT = GPIO18, SCLK = GPIO21, MOSI = GPIO41, MISO = GPIO42, isolated transceiver |
| Other | PCF85063 RTC on I2C (SCL = 38, SDA = 39, INT = 40), BOOT button (GPIO0), CAN_1/CAN_2 RX/TX LEDs (driven by hardware) |
| Termination | 120 Ω per channel, enabled by jumper (fitted to neither channel from the factory) |
| Limits | CAN 2.0A/B, up to 1 Mbit/s, no CAN FD |

## 2. Requirements (decided)

| # | Topic | Decision |
|---|---|---|
| 1 | Framework | ESP-IDF 5.5 |
| 2 | Build | PlatformIO, upload port selectable (`--upload-port COMx`) |
| 3 | Direction | CAN1 → CAN2 only. CAN2 RX is shown in the monitor but never forwarded |
| 4 | Filter | Blocklist: forward everything except the selected IDs |
| 5 | Defaults | Blocked: `0x182`, `0x427` (standard 11-bit). Bitrate 250 kbit/s on both buses |
| 6 | Bitrate | Set independently per bus: 25/50/100/125/250/500/800/1000 kbit/s |
| 7 | Frames | Standard and extended frames both forwarded and filterable (ID + Std/Ext flag). RTR frames forwarded unchanged |
| 8 | ID entry | Live list of IDs seen on CAN1, with checkboxes, plus manual hex entry |
| 9 | Wi-Fi | STA using saved credentials, falling back to an AP named `CANGW-XXXX`. mDNS name `cangw.local` |
| 10 | Auth | Optional admin password (off by default). Without login the monitor is read-only |
| 11 | Monitor | Per-ID table (count, rate, last data, DLC, forwarded/blocked) and bus stats, pushed over WebSocket at about 4 Hz |
| 12 | Frontend | Plain HTML/CSS/JS, gzipped and embedded in the firmware |
| 13 | Storage | Settings in NVS, with JSON export/import |
| 14 | Apply | Filter changes take effect immediately and are auto-saved after a 1 s debounce |
| 15 | Overflow | Never block CAN1 RX. Count drops, recover from bus-off automatically |
| 16 | CAN1 mode | Normal mode by default, listen-only selectable in the UI |
| 17 | Extras | Master forwarding on/off switch, test-frame sender, web OTA with rollback, serial CLI over USB |
| 18 | Button | Holding BOOT for 5 s at runtime restores factory defaults |
| 19 | Test | PC script using python-can with PEAK PCAN-USB. Interface, channel and COM port are all arguments |
| 20 | Performance | Handle 100% bus load at 250k and 500k with no drops, forwarding latency under 1 ms |
| 21 | DBC | Optional, one per channel. Stored on the device (LittleFS), parsed and decoded in the browser |
| 23 | License | GPL-3.0 |
| 24 | Repo | `esp32s3-can-gateway`, public on GitHub and Codeberg |

## 3. Firmware architecture

```
          core 1 (real-time)                         core 0 (network)
 ┌──────────────────────────────┐        ┌───────────────────────────────┐
 │ TWAI ISR → twai RX queue     │        │ Wi-Fi / lwIP / mDNS           │
 │        │                     │        │ esp_http_server + WebSocket   │
 │ can1_rx_task (prio 20)       │        │   REST API, OTA, DBC upload   │
 │   filter_lookup(id) O(1)     │        │ stats_task: snapshot → WS 4Hz │
 │   stats_update(id)           │        │ config_task: debounce → NVS   │
 │   if pass && fwd_enabled:    │        │ console_task: CLI over USB    │
 │     ring_push(can2_txq) ─────┼──┐     │ button_task: BOOT 5 s reset   │
 │                              │  │     └───────────────────────────────┘
 │ can2_task (prio 19)          │◄─┘
 │   INT(GPIO18) / TX-done      │
 │   SPI 10 MHz → XL2515 TXB0   │
 │   XL2515 RX → stats (monitor)│
 │   EFLG / bus-off recovery    │
 └──────────────────────────────┘
```

### Key components (`src/`)

| Module | Purpose |
|---|---|
| `can1_twai.c` | TWAI driver setup and bitrate/listen-only changes (requires stopping and restarting the driver). Alert-based error and bus-off handling |
| `xl2515.c` | MCP2515-compatible SPI driver: register access, CNF1–3 calculated for the crystal in use (detected at start-up by timing a loopback frame, or set by hand), load TX buffer, RTS, interrupt service |
| `gateway.c` | Forwarding loop, lock-free SPSC ring buffer (1024 frames, internal RAM), drop counters |
| `filter.c` | Standard IDs: a 2048-bit bitmap. Extended IDs: a sorted array with binary search (max 256). Two copies: edits go to the inactive copy, then an atomic pointer swap, so the RX path never takes a lock |
| `stats.c` | Per-ID counters in an open-addressed hash table (1024 entries, PSRAM), bus load estimate, error counters |
| `config.c` | Config struct ↔ NVS blob (versioned), JSON serialize/parse (cJSON), factory defaults |
| `web.c` | HTTP routes, WebSocket push, session cookie auth |
| `wifi.c` | STA with timeout, AP fallback, mDNS |
| `ota.c` | `esp_ota` streaming upload. The new image is marked valid only after CAN and web start successfully, otherwise the bootloader rolls back |
| `cli.c` | `esp_console` commands: `status`, `block <id>`, `unblock <id>`, `list`, `bitrate <1|2> <kbps>`, `fwd on|off`, `wifi <ssid> <pass>`, `send <bus> <id> <data>`, `factory-reset`, `reboot` |

### Frame ordering
The MCP2515 picks the next frame to send from its TX buffers by priority, not by
arrival order. To keep frames in CAN1 arrival order, the gateway writes to
**TXB0 only** and loads the next frame on the TX-done interrupt. At 250 kbit/s a
frame lasts about 0.5 ms, and the SPI load takes about 20 µs, so one buffer keeps
up with 100% bus load. Option for later: a "max throughput" mode that rotates
through all three buffers and gives up ordering.

### REST API
| Method | Path | Body / result |
|---|---|---|
| GET | `/api/status` | firmware version, uptime, Wi-Fi, bus states |
| GET/PUT | `/api/config` | full config JSON (export/import) |
| GET | `/api/filters` | `[{id, ext}]` |
| POST/DELETE | `/api/filters` | add or remove one `{id, ext}` |
| PUT | `/api/bus/{1,2}` | `{bitrate, listen_only}` |
| PUT | `/api/forwarding` | `{enabled}` |
| POST | `/api/tx` | `{bus, id, ext, rtr, data}` test frame |
| GET/PUT/DELETE | `/api/dbc/{1,2}` | raw DBC file |
| POST | `/api/ota` | firmware .bin |
| POST | `/api/login` | `{password}` → session cookie |
| WS | `/ws` | server → client: stats snapshots, per-ID deltas |

### Partitions (16 MB)
`nvs` 24K · `otadata` 8K · `phy` 4K · `ota_0` 3M · `ota_1` 3M · `littlefs` (DBC files) about 9.9M

## 4. Web UI (tabs)
1. **Monitor**: bus cards (state, RX/TX/s, load %, errors, drops) and a per-ID table with block checkboxes. Shows the DBC message name and decoded signals when a DBC is loaded.
2. **Filters**: blocklist with add (hex, Std/Ext), remove, and search by DBC message name.
3. **DBC**: upload, replace or delete the DBC for CAN1 and CAN2.
4. **Send**: test-frame sender for either bus.
5. **Settings**: bitrates, CAN1 listen-only, forwarding switch, Wi-Fi, admin password, config export/import, OTA, factory reset.

## 5. Repository layout
```
esp32s3-can-gateway/
├── platformio.ini            # env: esp32s3; upload_port via CLI or env var
├── partitions.csv
├── sdkconfig.defaults
├── src/                      # firmware modules listed above
├── web/                      # index.html, app.js, style.css, dbc.js
├── tools/
│   ├── embed_web.py          # gzip web/ → generated header (pre-build hook)
│   ├── can_test.py           # python-can PCAN: throughput, filter, latency tests
│   └── mock_server.py        # serves web/ with simulated traffic (for UI dev)
├── test/                     # host unit tests: filter, ring buffer, DBC parser
├── docs/ARCHITECTURE.md, docs/images/*.png
├── README.md, LICENSE (GPL-3.0)
```

## 6. Test plan
- **Host unit tests**: filter bitmap and extended-ID array, filter swap during concurrent lookup, ring buffer, config JSON round trip.
- **Bench with PCAN-USB** (`tools/can_test.py --interface pcan --channel PCAN_USBBUS1 [--rx-channel PCAN_USBBUS2]`):
  1. Send a sweep of standard and extended IDs on CAN1 and check that every ID except `0x182`/`0x427` arrives on CAN2.
  2. Block or unblock an ID from the UI or CLI while traffic is running, and check it takes effect within one frame.
  3. 100% bus load burst at 250k and 500k: zero drops, frame order preserved.
  4. Pull CAN2 off the bus: the gateway goes bus-off, drops are counted, and it recovers on reconnect.
- With only one PCAN channel, put it on CAN2 and generate CAN1 traffic another way (or swap cables between runs).

## 7. Milestones
1. Project skeleton, PlatformIO build, flash to the selected COM port, CLI `status`.
2. CAN1 TWAI RX and XL2515 TX, forwarding with the hard-coded default blocklist.
3. Filter module, NVS config, CLI filter commands.
4. Web server, REST API, WebSocket monitor, embedded UI.
5. Wi-Fi STA/AP fallback, mDNS, auth, OTA, BOOT factory reset.
6. DBC upload and in-browser decoding, test sender.
7. PCAN bench tests, performance tuning.
8. README with screenshots of the real device, push to GitHub and Codeberg.
