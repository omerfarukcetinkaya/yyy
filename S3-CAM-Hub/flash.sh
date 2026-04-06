#!/usr/bin/env bash
# projects/esp32/s3_vision_hub/flash.sh
# Convenience wrapper — delegates to the factory dispatcher.
# Optionally set ESPPORT before running to override port detection.
set -Eeuo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FACTORY_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
exec "${FACTORY_ROOT}/bin/factory" esp32 flash "${SCRIPT_DIR}" "$@"
