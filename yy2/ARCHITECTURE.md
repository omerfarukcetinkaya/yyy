# Scout (yy2) — ESP32-C5 Architecture & Operations

**Project**: yy2 / Scout Network Bridge
**Hardware**: ESP32-C5 DevKit (RISC-V single-core, WiFi 6 dual-band 2.4G/5G)
**IDF**: ESP-IDF v5.5.3
**Last updated**: 2026-04-15

---

## 1. Mission

Scout is the **network policy engine** for the yyy sensor fleet. Its roles:

1. **Multi-sensor aggregator** on the 2.4G IoT mesh (`localhost-ofc-dev0`)
   — currently S3 Vision Hub as sensor 0, expandable.
2. **Internet bridge** via the 5G network (`Pr.Kothsv.`) — briefly joins
   to relay alerts to Telegram and sync time (SNTP).
3. **Telegram command & control** in the "yyy" group's `SecBridge` topic.
4. **Local admin panel** at `http://<2.4G IP>` with tenten/1234 Basic Auth.
5. **RGB LED alarm indicator** (WS2812 on GPIO 27).
6. **Self-diagnostics**: uptime, CPU%, heap, reset reason, reboot counter
   (NVS-persisted), Telegram I/O counters.

---

## 2. Boot Sequence

```
Time(ms)  Step
─────────  ────────────────────────────────────────────────
0          ESP-ROM → 2nd stage bootloader
~50        Partition table, factory app @ 0x10000 (2 MB)
~200       app_main() entry
  1.       NVS flash init (scout namespace holds reboot_count, boot_ts)
  2.       Default event loop
  2b.      scout_health_init → loads reboot count from NVS, reset reason
           scout_health_start → 1Hz self-telemetry task
  3.       wifi_dual_init → starts on 2.4G, band_switch_task begins
  4.       SNTP (runs after WiFi 5G window)
  5.       telegram_client_init → HTTPS cert bundle loaded, poll task
  6a.      sensor_registry_init → registers S3 as sensor 0
  6b.      sensor_registry_start → poll task (Core 0, prio 3)
  7.       espnow_bridge_init → BIST heartbeat task (Core 0, prio 3)
  8.       status_reporter_init/start → UART + HTTP task (Core 0, prio 2)
  9.       rgb_led_init → WS2812 GPIO 27 driver
  10.      watchdog_init → TWDT 15s, no idle check
  11.      boot_notify_task — waits for 2.4G IP + NTP, 10-min rate limit
~2.6s      "Scout — ESP32-C5 Network Bridge" banner
~2.7s      2.4G connected, HTTP server bound to 2.4G netif
```

---

## 3. FreeRTOS Task Map

| Task | Prio | Stack | Rate | Purpose |
|------|------|-------|------|---------|
| `status_rpt` | 2 | 4 KB | 0.5 Hz | UART health log + RGB LED + **WDT subscriber** |
| `scout_hlth` | 2 | 4 KB | 1 Hz | Self CPU/heap/RAM sampler |
| `sensor_reg` | 3 | 6 KB | 0.1 Hz | Iterate sensors, HTTP GET telemetry |
| `bridge` | 3 | 6 KB | 0.1 Hz | Aggregate BIST counters, send heartbeat |
| `tg_poll` | 3 | 8 KB | 0.33 Hz | Telegram long-poll when on 5G |
| `band_sw` | 2 | 4 KB | — | WiFi band switcher (2.4G 5min, 5G 20s) |
| `boot_notify` | 2 | 8 KB | once | One-shot boot Telegram message |
| `wifi` | 23 | system | — | Espressif WiFi driver |
| `httpd` | 5 | 8 KB | on-demand | Admin panel worker (event-driven) |

---

## 4. Data Flow

```
          ┌──────────────────── 2.4G (localhost-ofc-dev0) ──────────┐
          │                                                          │
          │   Sensors:  S3 Vision Hub ────► /api/telemetry (JSON)   │
          │                                                          │
          │   Phone ──► http://<scout_ip>/ (admin panel, Basic Auth)│
          │                                                          │
          │   Scout HTTP server (port 80)                            │
          └──────────────────────────┬───────────────────────────────┘
                                     │
                                     ▼
                    ┌── Scout (ESP32-C5, single core) ──┐
                    │                                    │
                    │  sensor_registry poll loop         │
                    │  → parse alarm_active + motion     │
                    │  → edge-detect → telegram_send_alert()
                    │                                    │
                    │  band_switch_task (periodic):      │
                    │     2.4G 300s → 5G 20s → 2.4G...   │
                    │                                    │
                    │  scout_health_task 1Hz             │
                    │  → CPU %, heap, reset reason       │
                    └────────────────┬───────────────────┘
                                     │
                                     ▼ (only during 5G window)
          ┌──────────────────── 5G (Pr.Kothsv.) ───────────────────┐
          │   api.telegram.org (HTTPS, mbedTLS cert bundle)          │
          │   → sendMessage (alerts, heartbeats, command replies)    │
          │   → getUpdates (long-poll for user commands)             │
          │                                                          │
          │   pool.ntp.org (SNTP)                                    │
          └──────────────────────────────────────────────────────────┘
```

---

## 5. Key Design Decisions

### 5.1 Dual-band (270+30s) cycle

**Problem**: 2.4G has no internet, 5G has no S3. Need both.
**Solution**: timed band alternation. 94% on 2.4G (admin panel always fresh,
S3 polling), 6% on 5G (Telegram I/O, SNTP).

### 5.2 HTTP server refresh on every 2.4G `got_ip`

**Problem**: After band switch, netif may become stale.
**Solution**: `status_reporter_ensure_http_server()` (idempotent) called
from wifi_dual's `IP_EVENT_STA_GOT_IP` handler whenever band is 2.4G.
Pattern borrowed from yy1 (`wifi_manager.c:84`).

### 5.3 C5 as alarm policy engine

**Problem**: S3's `alarm_engine` only fires on gas/temp thresholds, not
motion. User expects motion detection to produce an alert.
**Solution**: Scout treats `motion_detected=true` as an alert condition
locally. S3's `alarm.active` is still honored separately. LED, admin
panel, and Telegram all reflect the aggregated view.

### 5.4 Boot notification rate limit

**Problem**: If Scout crash-loops, every boot sends a Telegram message —
SecBridge floods.
**Solution**: NVS-persisted last boot-notification timestamp. 10-minute
minimum gap between messages. Waits for NTP before deciding (avoids
sending wrong time).

### 5.5 Watchdog strategy

**Problem**: Original `watchdog_init()` was a stub. Default sdkconfig
subscribed IDLE task to TWDT. Single-core C5 idle starved during band
switches → 15s timeout → reboot loop.
**Solution**: Disable idle task WDT check. Only `status_reporter` task
(which resets every 2s) is subscribed to the 15s TWDT. 7.5× safety margin.

---

## 6. Networking & Security

### 6.1 Wi-Fi credentials (Kconfig → sdkconfig.defaults)

| Param | Value |
|-------|-------|
| `SCOUT_WIFI_24G_SSID` | `localhost-ofc-dev0` |
| `SCOUT_WIFI_24G_PASSWORD` | `1994Far.?!1994Far.?!` |
| `SCOUT_WIFI_5G_SSID` | `Pr.Kothsv.` |
| `SCOUT_WIFI_5G_PASSWORD` | `ProfessorK!` |

### 6.2 Telegram

- Bot: `tentenxxxBot`
- Group: `yyy` (id `-1003943316480`)
- Topic: `SecBridge` (thread_id `2`)
- Transport: HTTPS via `esp_http_client` with `crt_bundle_attach`
- Commands handled: `/status`, `/mute`, `/unmute`, `/alarm`, `/camera`,
  `/telemetry`, `/ip`, `/version`, `/reboot`, `/help`

### 6.3 Admin panel

- URL: `http://<2.4G_IP>/` (port 80)
- Auth: Basic (tenten / 1234)
- Refresh: 1 Hz (meta-refresh)
- Content: Scout self-card + Alarm/TG card + one card per sensor

### 6.4 S3 polling

- URL: `http://192.168.39.157/api/telemetry` (Basic Auth)
- Interval: 10 s (on 2.4G only)
- Parses: uptime, cam/motion fps+score, alarm.active, last_reason,
  CPU core0/1 %, heap, psram, WiFi RSSI

---

## 7. Persistence (NVS namespace: `scout`)

| Key | Type | Purpose |
|-----|------|---------|
| `reboots` | u32 | Monotonic reboot counter (incremented on init) |
| `boot_ts` | blob | Unix epoch of last successful boot notification |

---

## 8. Partition Layout (4 MB flash)

```
Offset      Size     Name       Use
0x0000      0x2000   (boot)
0x2000      0x6000   bootloader
0x8000      0x1000   partition-table
0x9000      0x6000   nvs          NVS (scout namespace + system)
0xF000      0x1000   phy_init
0x10000     0x1F0000 factory      scout.bin (~1.2 MB)
```

---

## 9. Sensor Registry

- Static table in `sensor_registry.c` init function
- Each entry: `name`, `ip`, `telemetry_path`, `username`, `password`
- Add sensor = 5 new lines in `sensor_registry_init()`
- Runtime state per sensor: online, poll ok/fail, uptime, cam/motion,
  alarm, heap, PSRAM, RSSI, CPU 0/1
- Edge detection: alarm off→on and motion off→on both trigger alerts

---

## 10. Heartbeat (BIST) — every 15 min

Single concise Telegram message:

```
💓 Heartbeat #N · HH:MM:SS · up Xs
━━━━━━━━━━━━━━━━━━━━━━━
🛡 Scout 2.4G · RSSI -15 · heap 155KB
🌐 Sensör Ağı: 1/1 online
━━━━━━━━━━━━━━━━━━━━━━━
[0] 🟢 S3 Vision Hub · up 3600s · cam 28fps · motion 7fps (0.15)
    polls 85/3 · heap 5800KB · CPU 25/27%
━━━━━━━━━━━━━━━━━━━━━━━
📊 15-min: alarms 2 · motion 34 events · max score 0.45
🎥 cam fps range 26-28
🔊 Mute: off · Schedule 09-19
```

Heartbeat sequence is monotonically incremented so messages are clearly
distinguished from boot notifications.

---

## 11. Known limitations & roadmap

- **Remote access**: Scout is 2.4G-only. For out-of-home access:
  recommended **Tailscale subnet router** on home laptop (see
  `VPN_REMOTE_ACCESS.md`).
- **ESP-NOW mesh**: Module scaffolded but not yet active — future
  addition will let sensors push events to Scout without HTTP polling.
- **OTA**: Not yet implemented. Factory partition only. Plan: add two
  `ota_0`/`ota_1` partitions and OTA update via 5G.
- **HTTPS admin panel**: Currently HTTP. Could add self-signed cert.
- **Schedule configurable via Telegram**: `/schedule 8-20` command TODO.

---

## 12. Troubleshooting

| Symptom | Check | Fix |
|---------|-------|-----|
| Crash loop | `yy2 --monitor` → look for `task_wdt` or `Guru` | Check WDT subscriptions, heap starvation |
| Admin panel unreachable | `ping 192.168.39.197` from phone on 2.4G | HTTP server auto-refresh should rebind |
| Telegram no reply | Scout on 5G? Check UART `poll_updates:` logs | 5G window too short vs message timing |
| LED doesn't go red on alarm | `sensor_registry_any_alarm()` returns false? | Verify S3 `motion_detected=true` in `/api/telemetry` |
| Repeated boot messages | `scout_health.c` NVS timestamp logic | Rate limit (10 min) should suppress |

---

## 13. Recent commits worth knowing

- `94cd417` — critical: watchdog, alarm-from-motion, boot spam protection
- `f72d558` — admin panel self-BIST + per-sensor full telemetry (1Hz)
- `50ae70e` — sensor registry multi-device aggregation
- `3d0fb4e` — heartbeat redesign, admin panel fresh-on-2.4G
- `ef40738` — initial Scout C5 implementation
