#!/usr/bin/env bash
set -Eeuo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${SCRIPT_DIR}/build"
PYTHON="/home/omerf/.espressif/python_env/idf5.5_py3.13_env/bin/python"
PORT="${1:-/dev/ttyACM0}"
exec "$PYTHON" -m esptool \
    --chip esp32s3 -p "$PORT" -b 460800 \
    --before default_reset --after hard_reset \
    write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
    0x0     "$BUILD/bootloader/bootloader.bin" \
    0x8000  "$BUILD/partition_table/partition-table.bin" \
    0x10000 "$BUILD/s3_vision_hub.bin"
