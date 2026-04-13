# yyy Vision Hub

ESP32-S3 vision, telemetry and alarm system. Serves a JPEG snapshot
stream and a live telemetry dashboard over Wi-Fi.

## Hardware

| Item | Detail |
|---|---|
| MCU | ESP32-S3-WROOM-1 N16R8 (dual-core Xtensa LX7, 16 MB flash, 8 MB Octal PSRAM) |
| Camera | OV3660 (on-chip JPEG, VGA 640×480 @ ~10 fps) |
| Sensors | AHT20 (temp/humidity), BMP280 (pressure), MQ7 (CO) |
| USB-UART | CH343P (VID:1a86 PID:55d3) → `/dev/ttyACM0` |

## Repository layout

This project lives at:

```
~/zzz/yyy/projects/esp32/s3_vision_hub/S3-CAM-Hub/
```

It is a standard ESP-IDF project. ESP-IDF itself is **not** vendored —
it is expected at `/opt/embedded-tools/sdk/esp/esp-idf` (factory layout).

## Setup (one-time)

1. Install ESP-IDF v5.5+ at `/opt/embedded-tools/sdk/esp/esp-idf`
   (already done on the dev machine).

2. Source the IDF environment in the shell you will build from:

   ```bash
   . /opt/embedded-tools/sdk/esp/esp-idf/export.sh
   ```

3. Configure Wi-Fi credentials and admin password. Either edit
   `sdkconfig.defaults` (`CONFIG_VH_WIFI_SSID`, `CONFIG_VH_WIFI_PASSWORD`,
   `CONFIG_VH_WEB_USER`, `CONFIG_VH_WEB_PASS`) or run menuconfig:

   ```bash
   cd ~/zzz/yyy/projects/esp32/s3_vision_hub/S3-CAM-Hub
   idf.py menuconfig          # → "yyy Vision Hub Configuration"
   ```

4. Connect the board over USB. The CH343P USB-UART enumerates as
   `/dev/ttyACM0`. Verify with `ls /dev/ttyACM*`.

## Build

```bash
cd ~/zzz/yyy/projects/esp32/s3_vision_hub/S3-CAM-Hub
. /opt/embedded-tools/sdk/esp/esp-idf/export.sh
idf.py build
```

Output binaries land in `build/`:

- `build/bootloader/bootloader.bin`
- `build/partition_table/partition-table.bin`
- `build/s3_vision_hub.bin`

## Flash to the ESP32-S3

The project ships a `flash.sh` wrapper that calls `esptool` directly
with the right offsets and flash params for the N16R8 module:

```bash
cd ~/zzz/yyy/projects/esp32/s3_vision_hub/S3-CAM-Hub
sudo ./flash.sh                    # default port /dev/ttyACM0
sudo ./flash.sh /dev/ttyACM1       # explicit port
```

`sudo` is needed unless your user is in the `dialout` group **and**
the device node is writable. Quick one-shot grant:

```bash
sudo chmod a+rw /dev/ttyACM0
./flash.sh
```

What gets written:

| Offset | File |
|---|---|
| `0x0`     | `build/bootloader/bootloader.bin` |
| `0x8000`  | `build/partition_table/partition-table.bin` |
| `0x10000` | `build/s3_vision_hub.bin` |

After flashing the chip resets automatically (`--after hard_reset`).

## Serial monitor / log capture

Two options:

```bash
# Live monitor (interactive)
idf.py -p /dev/ttyACM0 monitor

# Background capture into logs/uart.log (used while debugging)
sudo chmod a+rw /dev/ttyACM0
./logmon.sh
```

## Web interface

After boot the device prints its IP on UART. Then in a browser:

| URL | Description |
|---|---|
| `http://<ip>/`                | Admin dashboard (`yyy Vision Hub`) |
| `http://<ip>/snapshot`        | Single JPEG frame (used by dashboard polling) |
| `http://<ip>/api/telemetry`   | JSON health report |
| `http://<ip>/logout`          | Clears session cookie |

All endpoints are protected by Basic Auth. The browser realm is
`yyy Vision Hub`. Default credentials live in
`CONFIG_VH_WEB_USER` / `CONFIG_VH_WEB_PASS` — **change them before
exposing the device on any untrusted network.**

The live view uses **snapshot polling** (each frame is an independent
HTTP request to `/snapshot`), not MJPEG. This avoids iOS Safari TCP
window stalls that broke the original `multipart/x-mixed-replace`
stream. Current baseline: ~4–5 fps VGA, stable, no crashes.

## Architecture

```
Core 0: Wi-Fi, HTTP server, sensor hub, alarm engine, telemetry
Core 1: Camera DMA (esp_camera), motion detection, classifier
```

Camera frame buffer mode is `CAMERA_GRAB_LATEST` with 3 PSRAM frame
buffers (160 KB each). DMA runs continuously and `cam_task` keeps the
latest frame queued, so `/snapshot` requests return immediately.

Pin mapping is in `boards/ov3660_s3_n16r8.h`, based on the
ESP32-S3-EYE reference schematic. **Verify all pins against your
board schematic before first flash.**

## Implementation phases

| Phase | Status | Description |
|---|---|---|
| 1 | Done | Factory infrastructure, project scaffold |
| 2 | Done | Wi-Fi, camera, snapshot stream, admin panel, UART telemetry |
| 3 | TODO | Real sensor drivers (AHT20, BMP280, MQ7) |
| 4 | TODO | Motion detection, ROI tracking, alarm triggers |
| 5 | TODO | Telegram alerts, classifier interface |
