# s3_vision_hub

ESP32-S3 vision, telemetry, and alarm system.

## Hardware

| Item | Detail |
|---|---|
| MCU | ESP32-S3-WROOM-1 N16R8 (dual-core Xtensa LX7, 16 MB flash, 8 MB Octal PSRAM) |
| Camera | OV3660 |
| Sensors | AHT20 (temp/humidity), BMP280 (pressure), MQ7 (CO) |
| USB-UART | CH343P (VID:1a86 PID:55d3) → `/dev/ttyACM0` |

## Quick start

```bash
# Configure Wi-Fi and admin credentials
factory esp32 menuconfig projects/esp32/s3_vision_hub

# Build
factory esp32 build projects/esp32/s3_vision_hub

# Flash
factory esp32 flash projects/esp32/s3_vision_hub

# Monitor
factory esp32 monitor projects/esp32/s3_vision_hub
```

Or use the project-level wrappers:

```bash
cd projects/esp32/s3_vision_hub
./build.sh
./flash.sh
./monitor.sh
```

## Web Interface

After boot and Wi-Fi connection:

| URL | Description |
|---|---|
| `http://<ip>/` | Admin dashboard (auth required) |
| `http://<ip>/stream` | MJPEG live stream (auth required) |
| `http://<ip>/api/telemetry` | JSON health report (auth required) |

Default credentials: `tenten` / `1234` — **change before any production use**.

## Architecture

```
Core 0: Wi-Fi, HTTP server, sensor hub, alarm engine, telemetry
Core 1: Camera DMA (esp_camera), motion detection, classifier
```

Full architecture documentation: `workspace/s3_vision_hub_architecture.md`

## Pin mapping

See `boards/ov3660_s3_n16r8.h`. Based on ESP32-S3-EYE reference schematic.
**Verify all pins against your board schematic before first flash.**

## Implementation phases

| Phase | Status | Description |
|---|---|---|
| 1 | Done | Factory infrastructure, project scaffold |
| 2 | Done | Wi-Fi, camera, MJPEG stream, admin panel, UART telemetry |
| 3 | TODO | Real sensor drivers (AHT20, BMP280, MQ7) |
| 4 | TODO | Motion detection, ROI tracking, alarm triggers |
| 5 | TODO | Telegram alerts, classifier interface |

## Sensor I2C pins

`BOARD_SENSOR_SDA_GPIO` and `BOARD_SENSOR_SCL_GPIO` in `boards/ov3660_s3_n16r8.h`
are currently set to -1 (TBD). Set them to the correct GPIOs from your board schematic
before enabling Phase 3 sensor drivers.
