# yy2 — Scout (ESP32-C5 Network Bridge)

Dual-band WiFi 6 gateway that bridges local ESP32 IoT mesh to the internet.

## Hardware

| Item | Detail |
|---|---|
| MCU | ESP32-C5 (RISC-V, WiFi 6, 2.4G + 5G dual-band) |
| Role | Network bridge, alarm relay, VPN gateway |

## Features

- **Dual-band WiFi**: 2.4 GHz for local IoT mesh, 5 GHz for internet access
- **Telegram alerts**: receives alarms from yy1 (S3 Vision Hub) via ESP-NOW, sends to phone
- **VPN relay**: forwards yy1 camera stream to remote VPN clients
- **Multi-device aggregation**: collects status from multiple ESP32 devices

## Build

```bash
yy2 --start    # build + flash + monitor
yy2 --build    # build only
yy2 --flash    # flash only
yy2 --monitor  # serial monitor
```
