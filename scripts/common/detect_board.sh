#!/usr/bin/env bash
# scripts/common/detect_board.sh
# Board detection: identifies connected boards from USB descriptors
# and persists results under state/known_devices/.
# Source this file; do not execute it directly.

_DETECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${_DETECT_DIR}/log.sh"
source "${_DETECT_DIR}/paths.sh"
source "${_DETECT_DIR}/usb.sh"

# Print a table of all connected serial devices with USB details.
detect_board_report() {
    local found=0

    log_step "Scanning USB serial devices..."

    for port in $(usb_list_serial_ports); do
        local vid="" pid="" manuf="" prod="" serial_id=""

        for dev_path in /sys/bus/usb/devices/*/; do
            local hit
            hit=$(find "${dev_path}" -name "$(basename "${port}")" \
                  -maxdepth 5 2>/dev/null | head -1)
            if [[ -n "${hit}" ]]; then
                vid=$(cat "${dev_path}idVendor" 2>/dev/null)
                pid=$(cat "${dev_path}idProduct" 2>/dev/null)
                manuf=$(cat "${dev_path}manufacturer" 2>/dev/null)
                prod=$(cat "${dev_path}product" 2>/dev/null)
                serial_id=$(cat "${dev_path}serial" 2>/dev/null)
                break
            fi
        done

        echo "  Port:    ${port}"
        [[ -n "${vid}" ]]       && echo "  VID:PID: ${vid}:${pid}"
        [[ -n "${manuf}" ]]     && echo "  Manuf:   ${manuf}"
        [[ -n "${prod}" ]]      && echo "  Product: ${prod}"
        [[ -n "${serial_id}" ]] && echo "  Serial:  ${serial_id}"
        echo ""
        found=$((found + 1))
    done

    if [[ "${found}" -eq 0 ]]; then
        log_warn "No serial devices found."
        return 1
    fi
    log_ok "Found ${found} serial device(s)."
}

# detect_board_save <port> <family> <board_name>
detect_board_save() {
    local port="${1:?}" family="${2:?}" board_name="${3:?}"
    local ts; ts=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
    local out_dir="${FACTORY_STATE}/known_devices"
    mkdir -p "${out_dir}"
    local fname="${out_dir}/$(basename "${port}").json"
    cat > "${fname}" <<EOF
{
  "port": "${port}",
  "family": "${family}",
  "board": "${board_name}",
  "detected_at": "${ts}"
}
EOF
    log_ok "Device record saved: ${fname}"
}
