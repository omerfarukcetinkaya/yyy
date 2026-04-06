#!/usr/bin/env bash
# projects/esp32/s3_vision_hub/monitor.sh
# Convenience wrapper — delegates to the factory dispatcher.
set -Eeuo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FACTORY_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
exec "${FACTORY_ROOT}/bin/factory" esp32 monitor "${SCRIPT_DIR}" "$@"
