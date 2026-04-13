#!/usr/bin/env bash
# scripts/targets/esp32.sh
# ESP32-family build / flash / monitor dispatcher.
# Called by bin/factory with: source scripts/targets/esp32.sh <action> <project_dir> [args...]
#
# Actions: build | flash | monitor | clean | size
set -Eeuo pipefail

_ESP32_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${_ESP32_DIR}/../env/load_esp.sh"
source "${_ESP32_DIR}/../common/usb.sh"

ACTION="${1:?Usage: esp32.sh <build|flash|monitor|clean|size> <project_dir> [extra args]}"
PROJECT_DIR="${2:?Project directory required}"
shift 2

if [[ ! -f "${PROJECT_DIR}/CMakeLists.txt" ]]; then
    die "Not a valid ESP-IDF project: '${PROJECT_DIR}' (missing CMakeLists.txt)"
fi

# ── Load board profile if present ─────────────────────────────────────────────
BOARD_PROFILE="${PROJECT_DIR}/boards/active_board.env"
if [[ -f "${BOARD_PROFILE}" ]]; then
    # shellcheck disable=SC1090
    source "${BOARD_PROFILE}"
    log_info "Loaded board profile: ${BOARD_PROFILE}"
fi

# ── Port detection ─────────────────────────────────────────────────────────────
# Prefers ESPPORT if set in environment; otherwise auto-detects.
if [[ -z "${ESPPORT:-}" ]]; then
    ESPPORT=$(usb_find_esp32)
    if [[ -z "${ESPPORT}" ]]; then
        log_warn "No ESP32 serial port found. Connect the board and retry."
        ESPPORT=""
    else
        log_info "Auto-detected port: ${ESPPORT}"
    fi
fi

# ── Action dispatch ────────────────────────────────────────────────────────────
case "${ACTION}" in
    build)
        log_step "Building ${PROJECT_DIR} ..."
        idf.py -C "${PROJECT_DIR}" build "$@"
        log_ok "Build complete."
        ;;
    flash)
        [[ -z "${ESPPORT}" ]] && die "Cannot flash: no serial port detected."
        usb_assert_port_exists "${ESPPORT}"
        log_step "Flashing ${PROJECT_DIR} → ${ESPPORT} ..."
        idf.py -C "${PROJECT_DIR}" -p "${ESPPORT}" \
               -b "${FLASH_BAUD:-460800}" flash "$@"
        log_ok "Flash complete."
        _save_flash_record "${PROJECT_DIR}" "${ESPPORT}"
        ;;
    monitor)
        [[ -z "${ESPPORT}" ]] && die "Cannot monitor: no serial port detected."
        usb_assert_port_exists "${ESPPORT}"
        log_step "Monitoring ${ESPPORT} (Ctrl-] to exit) ..."
        idf.py -C "${PROJECT_DIR}" -p "${ESPPORT}" \
               --baud "${MONITOR_BAUD:-115200}" monitor "$@"
        ;;
    flash_monitor)
        [[ -z "${ESPPORT}" ]] && die "Cannot flash+monitor: no serial port detected."
        usb_assert_port_exists "${ESPPORT}"
        log_step "Flash + monitor ${PROJECT_DIR} → ${ESPPORT} ..."
        idf.py -C "${PROJECT_DIR}" -p "${ESPPORT}" \
               -b "${FLASH_BAUD:-460800}" flash monitor "$@"
        ;;
    clean)
        log_step "Cleaning ${PROJECT_DIR} ..."
        idf.py -C "${PROJECT_DIR}" fullclean "$@"
        log_ok "Clean complete."
        ;;
    size)
        log_step "Size analysis for ${PROJECT_DIR} ..."
        idf.py -C "${PROJECT_DIR}" size "$@"
        ;;
    menuconfig)
        idf.py -C "${PROJECT_DIR}" menuconfig "$@"
        ;;
    *)
        die "Unknown action '${ACTION}'. Valid: build | flash | monitor | flash_monitor | clean | size | menuconfig"
        ;;
esac

# ── Helpers ────────────────────────────────────────────────────────────────────
_save_flash_record() {
    local proj_dir="${1}" port="${2}"
    local ts; ts=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
    local proj_name; proj_name=$(basename "${proj_dir}")
    local out_dir="${FACTORY_STATE}/flash_history"
    mkdir -p "${out_dir}"
    local fname="${out_dir}/${proj_name}_$(date -u +%Y%m%d_%H%M%S).json"
    cat > "${fname}" <<EOF
{
  "project": "${proj_name}",
  "project_dir": "${proj_dir}",
  "port": "${port}",
  "flashed_at": "${ts}"
}
EOF
    log_ok "Flash record saved: ${fname}"
}
