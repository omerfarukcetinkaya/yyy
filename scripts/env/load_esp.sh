#!/usr/bin/env bash
# scripts/env/load_esp.sh
# Loads the ESP-IDF environment into the current shell session.
# Source this file; do not execute it directly.
#
# Workaround: system Python may be 3.13+ (miniconda) but the ESP-IDF venv
# was built with 3.10. IDF_PYTHON_ENV_PATH must be exported before sourcing
# export.sh, otherwise idf.py constructs the wrong venv path and fails.

_LESP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${_LESP_DIR}/load_common.sh"

# Validate IDF root exists
if [[ ! -f "${IDF_ROOT}/export.sh" ]]; then
    die "ESP-IDF not found at '${IDF_ROOT}'. Check TOOLS_ROOT and IDF_ROOT."
fi

# Validate Python venv exists
if [[ ! -x "${IDF_PYTHON_ENV_PATH}/bin/python" ]]; then
    die "ESP-IDF Python venv not found at '${IDF_PYTHON_ENV_PATH}'. Run: ${IDF_ROOT}/install.sh"
fi

log_step "Loading ESP-IDF from ${IDF_ROOT} ..."
export IDF_PYTHON_ENV_PATH
# shellcheck source=/opt/embedded-tools/sdk/esp/esp-idf/export.sh
source "${IDF_ROOT}/export.sh" >/dev/null 2>&1

# Verify idf.py is now on PATH
if ! command -v idf.py >/dev/null 2>&1; then
    die "idf.py not found on PATH after sourcing ESP-IDF. Check export.sh output."
fi

_idf_ver=$(idf.py --version 2>&1 | head -1)
log_ok "ESP-IDF ready: ${_idf_ver}"
