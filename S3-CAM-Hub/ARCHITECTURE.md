# yyy Vision Hub — System Architecture

**Project**: yy1 / S3-CAM-Hub
**Hardware**: ESP32-S3-WROOM-1 N16R8 (dual-core LX7 240MHz, 16MB flash, 8MB PSRAM)
**Camera**: OV3660 (VGA 640×480 JPEG, onchip ISP, 10MHz PCLK)
**IDF**: ESP-IDF v5.5.3
**Last updated**: 2026-04-13

---

## 1. System Overview

Indoor security camera with real-time video streaming, motion detection,
face detection/recognition, and alarm capabilities. Serves a web-based
admin dashboard over Wi-Fi with WebSocket push-based video delivery.

### Hardware Components

| Component | Detail | Interface |
|-----------|--------|-----------|
| MCU | ESP32-S3-WROOM-1 N16R8 | — |
| Camera | OV3660 3MP (VGA JPEG mode) | SCCB (I2C) + parallel data + XCLK |
| USB-UART | CH343P (VID 1a86:55d3) or native USB JTAG (303a:1001) | /dev/ttyACM0 |
| Sensors (planned) | AHT20 (temp/hum), BMP280 (pressure), MQ7 (CO), MQ137 (NH3) | I2C + ADC |
| Display (planned) | SSD1306 128×32 OLED | I2C |
| External Flash (planned) | W25Q64 | SPI |

### Pin Mapping

Defined in `boards/ov3660_s3_n16r8.h`. Based on ESP32-S3-EYE schematic.

---

## 2. Boot Sequence

```
Time(ms)  Step
───────── ──────────────────────────────────────────────
0         ESP-ROM bootloader → 2nd stage bootloader
~50       Partition table loaded (factory 8MB @ 0x10000)
~200      app_main() entry
~200      1. NVS flash init
~200      2. Default event loop
~200      3. Fault handler (reads previous reset reason)
~200      4. Watchdog init (TWDT 10s, panic on trigger)
~300      5. Telemetry init (mutex + counters)
~300      6. Web auth init (Base64 precompute + session token)
~400      7. Wi-Fi STA init → connects to AP (non-blocking)
~500      8. cam_pipeline_init (stats mutex)
~500      9. frame_pool_init (4 × 160KB PSRAM slots)
~600     10. SCCB bus recovery (9 SCL pulses)
~1800    11. esp_camera_init (OV3660 detected, DMA configured)
~1800    12. cam_pipeline_start → cam_task spawned (Core 1, prio 6)
~1900    13. alarm_engine_init + start
~1900    14. motion_detect_init (DC buffers in DRAM) + start (Core 1, prio 4)
~2000    15. face_detect_init (MSRMNP model load, ~200ms) + start (Core 0, prio 3)
~2100    16. Sensor hub init + start
~2200    17. Ext flash + BIST logger (optional)
~2300    18. OLED display (optional)
~2300    19. Telemetry task spawned (Core 0, prio 2)
~2400       "yyy Vision Hub — boot complete"
~2500    WiFi IP acquired → HTTP server starts on Core 0
         /ws/stream, /snapshot, /, /api/telemetry registered
```

---

## 3. FreeRTOS Task Map

| Task | Core | Prio | Stack | Rate | Purpose |
|------|------|------|-------|------|---------|
| `cam_task` | 1 | 6 | 4 KB | 28 fps | DMA drain → frame_pool publish |
| `motion` | 1 | 4 | 6 KB | 7 fps | DC parse → diff → CC → tracker |
| `face_det` | 0 | 3 | 12 KB | 1 fps | JPEG decode → MSRMNP → recognition |
| `ws_send` (×2) | 0 | 5 | 6 KB | ~10 fps | Per-client WS binary + JSON push |
| `httpd` | 0 | 6 | 8 KB | on-demand | HTTP server workers |
| `telemetry` | 0 | 2 | 6 KB | 0.5 Hz | System metrics → UART + web |
| `alarm` | 0 | 4 | 4 KB | 1 Hz | Threshold evaluation + Telegram |
| `sensor_hub` | 0 | 3 | 4 KB | 1 Hz | I2C/ADC sensor polling |
| `classifier` | 1 | 3 | 4 KB | — | STUB (future TFLite) |
| `display` | 0 | 2 | 3 KB | 1 Hz | OLED refresh |

---

## 4. Data Flow

```
OV3660 (JPEG VGA 28fps)
  │ DMA (3× 160KB PSRAM frame buffers)
  ▼
cam_task (Core 1, prio 6)
  │ esp_camera_fb_get() → memcpy to pool slot → fb_return()
  ▼
frame_pool (4 slots × 160KB PSRAM, atomic refcount)
  │
  ├──► ws_send_task ──► WebSocket binary frame ──► Browser <img>
  │    (Core 0)          + motion/face JSON text     + canvas overlay
  │
  ├──► motion_task ──► jpeg_dc.c (DC luma extraction, ~26ms)
  │    (Core 1)      │  └── 80×60 grayscale grid (no IDCT!)
  │                  ├── abs-diff → dilate → CC labeling → blob merge
  │                  ├── persistent tracker (hit/miss, smooth bbox)
  │                  └── telemetry_set_motion() + alarm_engine_feed_motion()
  │
  ├──► face_det_task ──► esp_jpeg_decode SCALE_1_2 (320×240 RGB888, ~50ms)
  │    (Core 0)        ├── HumanFaceDetect MSRMNP (two-stage, ~100ms)
  │                    ├── HumanFaceRecognizer (enrollment/guard)
  │                    └── face bbox + name → WS JSON
  │
  └──► /snapshot handler (on-demand, frame_pool_get_latest)
```

---

## 5. Frame Pool Design

```
┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐
│  Slot 0     │  │  Slot 1     │  │  Slot 2     │  │  Slot 3     │
│  160 KB     │  │  160 KB     │  │  160 KB     │  │  160 KB     │
│  PSRAM      │  │  PSRAM      │  │  PSRAM      │  │  PSRAM      │
│  refcount=0 │  │  refcount=1 │  │  refcount=2 │  │  refcount=0 │
│  (free)     │  │  (latest)   │  │  (motion+ws)│  │  (free)     │
└─────────────┘  └─────────────┘  └─────────────┘  └─────────────┘
```

- **Producer** (cam_task): `acquire_writable()` → memcpy → `publish()`
- **Consumers**: `get_latest()` or `wait_newer(last_seq)` → use → `release()`
- **Notification**: `xTaskNotifyGive()` to all subscribers on publish
- **Lifecycle**: refcount 0 = free; "latest" holds ref=1; each consumer adds +1
- **Subscriber list**: max 6 tasks. `subscribe()` on task start, `unsubscribe()` before `vTaskDelete()`

---

## 6. Motion Detection Pipeline

### JPEG DC-Coefficient Parser (`jpeg_dc.c`)

Extracts Y-channel DC coefficients directly from JPEG bitstream:
- Parses SOI → SOF0 → DHT → DQT → SOS markers
- Walks entropy-coded data MCU by MCU
- Decodes only DC (skips 63 AC coefficients per block)
- Output: 80×60 luminance grid (~3-8ms, vs ~200ms for full tjpgd)
- Handles corrupt JPEG (truncated/NO-EOI): bs_get returns -1 → graceful exit

### Diff + Morphology

1. `build_diff_mask()`: abs(curr - prev) > threshold → foreground mask
2. `dilate_cross_max()`: 3×3 cross dilation closes 1-pixel gaps (merges fragmented head/body parts)
3. `blob_finder_run()`: two-pass union-find CC labeling → up to 8 blobs with bbox + area + score
4. `merge_close_blobs()`: iterative bbox proximity merge (gap ≤ 4 mask pixels)

### Persistent Tracker

```
State Machine:
  NEW (invisible, hits < 2)
    │ hits >= CONFIRM_HITS
    ▼
  TRACKING (green box)
    │ sustained score > threshold for N frames
    ▼
  ALERT (red box, alarm trigger)
    │ no match this frame
    ▼
  POSSIBLE (yellow box, "threat not cleared")
    │ miss_count > HOLD (20 frames = ~5.7s)
    ▼
  FREE (slot released)
```

- **Matching**: greedy centroid distance (< 12 mask pixels)
- **Smoothing**: exponential α=0.45 on bbox center/size/score
- **Stable ID**: monotonic counter, survives across frames

---

## 7. Face Detection (ESP-DL)

### Model

- **HumanFaceDetect MSRMNP_S8_V1**: two-stage detector
  - Stage 1 (MSR): coarse face detection, outputs candidate regions
  - Stage 2 (MNP): refine candidates, extract 5 landmarks (eyes, nose, mouth)
- **Model location**: flash RODATA (embedded in binary, ~3MB)
- **Input**: RGB888 image (any resolution, preprocessor resizes internally)
- **Output**: `list<result_t>` with `box[4]`, `score`, `keypoint[10]`

### Optimization

- Decode at SCALE_1_2 (320×240) instead of full VGA → 4× less memory, ~50ms decode
- Runs on Core 0 (not Core 1) → no contention with cam_task/motion
- STRIDE=28 → ~1 fps (sufficient for face detection)
- Face bbox coordinates scaled 2× back to source frame space

### Recognition (planned)

- **HumanFaceRecognizer**: face embedding + cosine similarity matching
- **Enrollment**: first 5 faces → "Ömer" profile
- **Guard mode**: known face → green label + name; unknown → red + alarm
- **Blocked by**: SPIFFS mount needed for persistent face database

---

## 8. WebSocket Stream

### Protocol

- Endpoint: `/ws/stream?token=<session_token>`
- Auth: query-parameter token (bypasses cookie policy issues on iOS)
- Binary frames: raw JPEG from frame_pool (camera output, no re-encoding)
- Text frames: JSON messages with `"t"` field discriminator:
  - `{"t":"motion", "tracks":[...], "det":bool, "score":F}`
  - `{"t":"face", "faces":[...], "mode":"enrollment"|"guard"}`

### Client Lifecycle

1. Browser: `new WebSocket(ws_url)` with binaryType='blob'
2. Server: auth check → acquire client slot (max 2) → spawn `ws_send` task
3. `ws_send`: subscribes to frame_pool → loop: `wait_newer()` → `httpd_ws_send_frame_async()` (binary) + motion/face JSON (text)
4. On disconnect: `frame_pool_unsubscribe()` → `httpd_sess_trigger_close()` → slot release → `vTaskDelete()`

---

## 9. Memory Budget

### PSRAM (8 MB total)

| Consumer | Size | Notes |
|----------|------|-------|
| Camera DMA buffers | 480 KB | 3 × 160 KB (esp_camera) |
| Frame pool | 640 KB | 4 × 160 KB |
| Face detect RGB buf | 230 KB | 320×240×3 |
| Face detect model | ~1.3 MB | MSRMNP weights (loaded from flash RODATA) |
| ESP-DL inference | ~500 KB | Tensor workspace |
| WiFi/LWIP | ~200 KB | TX/RX buffers |
| **Total used** | **~3.4 MB** | |
| **Free** | **~4.6 MB** | Available for future models/buffers |

### Internal DRAM (~160 KB total)

| Consumer | Size | Notes |
|----------|------|-------|
| Motion DC gray bufs | 10 KB | dc_gray + prev_gray (DRAM for speed) |
| FreeRTOS stacks | ~60 KB | All task stacks combined |
| LWIP/WiFi internals | ~30 KB | TCP buffers, mboxes |
| ESP-DL internals | ~10 KB | Quantization workspace |
| **Total used** | **~110 KB** | |
| **Free** | **~50 KB** | |

### Flash (16 MB total)

| Partition | Offset | Size | Content |
|-----------|--------|------|---------|
| nvs | 0x9000 | 24 KB | NVS key-value store |
| phy_init | 0xF000 | 4 KB | WiFi calibration |
| factory | 0x10000 | 8 MB | Application binary (~2.8 MB) |
| spiffs | 0x810000 | 1 MB | Face DB (planned) |
| ota_0 | 0x910000 | 3 MB | OTA slot A |
| ota_1 | 0xC10000 | 3 MB | OTA slot B |
| nvs_key | 0xF10000 | 4 KB | NVS encryption key |

---

## 10. CPU Budget

### Core 0 (~30% utilized)

| Task | CPU % | Notes |
|------|-------|-------|
| WiFi stack | 5% | RX/TX, beacon, PM |
| httpd workers | 3% | HTML serve, telemetry JSON |
| WS broadcast | 5% | Binary frame push (10 fps) |
| Face detection | 10% | 1 fps × ~100ms inference |
| Telemetry | 1% | 0.5 Hz UART + metrics refresh |
| Alarm + sensors | 1% | 1 Hz evaluation + I2C |
| **IDLE** | **~75%** | |

### Core 1 (~30% utilized)

| Task | CPU % | Notes |
|------|-------|-------|
| cam_task | 3% | fb_get (35ms block) + memcpy (~0.1ms) |
| motion | 25% | 7 fps × ~28ms (DC parse + diff + CC + tracker) |
| **IDLE** | **~72%** | Available for future classifier/vision tasks |

---

## 11. Configuration Reference (sdkconfig.defaults)

### Critical entries

| Config | Value | Purpose |
|--------|-------|---------|
| CONFIG_IDF_TARGET | esp32s3 | Target chip |
| CONFIG_SPIRAM | y | Enable 8MB octal PSRAM |
| CONFIG_SPIRAM_SPEED_80M | y | 80 MHz PSRAM clock |
| CONFIG_LWIP_MAX_SOCKETS | 16 | Enough for httpd + WS clients |
| CONFIG_LWIP_TCP_SND_BUF_DEFAULT | 65535 | One JPEG frame in kernel buffer |
| CONFIG_HTTPD_WS_SUPPORT | y | WebSocket in httpd |
| CONFIG_HTTPD_MAX_REQ_HDR_LEN | 1024 | Cookie + auth headers |
| CONFIG_VH_CAM_FRAME_SIZE | 10 | VGA (640×480) |
| CONFIG_VH_CAM_JPEG_QUALITY | 35 | Compression level |
| CONFIG_VH_CAM_FB_COUNT | 3 | Camera DMA frame buffers |
| CONFIG_CAMERA_CORE1 | y | Pin camera DMA to Core 1 |
| CONFIG_FLASH_HUMAN_FACE_DETECT_MSRMNP_S8_V1 | y | Face detection model |
| CONFIG_ESP_TASK_WDT_TIMEOUT_S | 10 | Watchdog timeout |

---

## 12. Known Issues & TODOs

### Issues
- Face recognition DB requires SPIFFS mount (not yet configured) → enrollment disabled
- OV3660 occasionally produces truncated JPEG (NO-EOI) → DC parser handles gracefully
- Sensor I2C pins set to -1 (GPIO assignment TBD per board schematic)

### TODOs (priority order)
1. Mount SPIFFS partition → enable face recognition enrollment/guard
2. ROI-based face detection: decode only motion track bbox, not full frame
3. Telegram alert transport integration
4. Real sensor drivers (AHT20, BMP280, MQ7 — pins need wiring)
5. TFLite Micro classifier (person/animal/vehicle)
6. OTA firmware update support
7. ESP-NOW mesh for multi-device communication (→ yy2 Scout bridge)
