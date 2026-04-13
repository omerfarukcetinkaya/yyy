#!/usr/bin/env bash
# scripts/common/usb.sh
# USB and serial port detection utilities.
# Source this file; do not execute it directly.

_USB_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${_USB_SCRIPT_DIR}/log.sh"

# Print all detected serial device nodes, one per line.
usb_list_serial_ports() {
    ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || true
}

# usb_find_by_vid_pid <VID> <PID>
# Outputs the tty node if found, empty string otherwise.
# VID and PID are lowercase hex, no "0x" prefix, e.g. "1a86" "55d3"
usb_find_by_vid_pid() {
    local vid="${1:?VID required}"
    local pid="${2:?PID required}"
    local result=""

    for dev_path in /sys/bus/usb/devices/*/; do
        local dv dp
        dv=$(cat "${dev_path}idVendor" 2>/dev/null) || continue
        dp=$(cat "${dev_path}idProduct" 2>/dev/null) || continue
        if [[ "${dv}" == "${vid}" && "${dp}" == "${pid}" ]]; then
            local tty
            tty=$(find "${dev_path}" -name "tty*" -maxdepth 4 2>/dev/null \
                  | grep -Eo 'ttyUSB[0-9]+|ttyACM[0-9]+' | head -1)
            [[ -n "${tty}" ]] && result="/dev/${tty}"
        fi
    done
    echo "${result}"
}

# usb_find_esp32
# Heuristic: find a likely ESP32-family serial device.
# Checks by-id symlinks first (most stable), then known VID:PIDs.
#
# Known VID:PIDs:
#   1a86:55d3  QinHeng CH343P/CH9102 (CDC ACM)           ← external USB-UART
#   1a86:7523  QinHeng CH340
#   10c4:ea60  Silicon Labs CP2102/CP2104
#   0403:6001  FTDI FT232R
#   303a:1001  Espressif USB JTAG/serial debug unit      ← ESP32-S3/C3 native USB
#   303a:4001  Espressif USB Serial/JTAG (alternate PID)
usb_find_esp32() {
    local port=""

    local by_id
    by_id=$(ls /dev/serial/by-id/ 2>/dev/null \
            | grep -iE 'ch343|ch9102|cp210|ftdi|ch340|usb_serial|espressif|jtag' | head -1)
    if [[ -n "${by_id}" ]]; then
        port=$(readlink -f "/dev/serial/by-id/${by_id}" 2>/dev/null)
    fi

    if [[ -z "${port}" ]]; then
        for vidpid in "1a86:55d3" "1a86:7523" "10c4:ea60" "0403:6001" \
                      "303a:1001" "303a:4001"; do
            local v="${vidpid%%:*}" p="${vidpid##*:}"
            port=$(usb_find_by_vid_pid "${v}" "${p}")
            [[ -n "${port}" ]] && break
        done
    fi

    echo "${port}"
}

# _save_flash_record <project_dir> <port>
# Appends one line to state/flash_history/esp32.log.
_save_flash_record() {
    local project_dir="${1:?project_dir required}"
    local port="${2:?port required}"
    local state_dir
    state_dir="$(cd "${_USB_SCRIPT_DIR}/../.." && pwd)/state/flash_history"
    mkdir -p "${state_dir}"
    printf '%s  %-12s  %s\n' \
        "$(date '+%Y-%m-%d %H:%M:%S')" "${port}" "${project_dir}" \
        >> "${state_dir}/esp32.log"
}

# usb_assert_port_exists <port>
usb_assert_port_exists() {
    local port="${1:?port argument required}"
    if [[ ! -c "${port}" ]]; then
        die "Serial port '${port}' not found. Run 'bin/factory detect' to scan."
    fi
    if [[ ! -r "${port}" || ! -w "${port}" ]]; then
        die "No access to '${port}'. Add user to dialout: sudo usermod -aG dialout \$USER"
    fi
    log_ok "Port ${port} is accessible."
}
