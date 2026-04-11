#!/usr/bin/env bash
# logmon.sh — Build + flash + monitor with rolling UART log capture.
#
# Usage:
#   ./logmon.sh          — monitor only (capture UART)
#   ./logmon.sh flash    — build + flash + monitor (capture build + UART logs)
#
# Log files (project-local, readable by Claude):
#   logs/uart.log        — last 200 lines of UART output (rolling)
#   logs/build.log       — most recent build+flash output (last 200 lines)
#
# idf.py monitor reads keyboard (Ctrl-]) directly from the tty — piping
# stdout for log capture does not break interactivity.
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FACTORY_ROOT="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
LOG_DIR="${SCRIPT_DIR}/logs"
UART_LOG="${LOG_DIR}/uart.log"
BUILD_LOG="${LOG_DIR}/build.log"
MAX_LINES=200

mkdir -p "$LOG_DIR"

MODE="${1:-monitor}"

# ── Build + flash ──────────────────────────────────────────────────────────────
if [[ "$MODE" == "flash" ]]; then
    echo "=== $(date '+%Y-%m-%d %H:%M:%S') BUILD+FLASH ===" | tee -a "$BUILD_LOG"
    "${FACTORY_ROOT}/bin/factory" esp32 build "${SCRIPT_DIR}" 2>&1 | tee -a "$BUILD_LOG"
    "${FACTORY_ROOT}/bin/factory" esp32 flash "${SCRIPT_DIR}" 2>&1 | tee -a "$BUILD_LOG"
    echo "=== $(date '+%Y-%m-%d %H:%M:%S') FLASH DONE ===" | tee -a "$BUILD_LOG"
    # Trim build log
    tail -n $MAX_LINES "$BUILD_LOG" > "${BUILD_LOG}.tmp" \
        && mv "${BUILD_LOG}.tmp" "$BUILD_LOG"
    echo "[logmon] Build log → ${BUILD_LOG}"
fi

# ── Monitor with rolling UART log ─────────────────────────────────────────────
# Rotate previous session (keep last MAX_LINES from it)
if [[ -s "$UART_LOG" ]]; then
    tail -n $MAX_LINES "$UART_LOG" > "${UART_LOG}.tmp" \
        && mv "${UART_LOG}.tmp" "$UART_LOG" || true
fi

echo "[logmon] UART → ${UART_LOG} (last ${MAX_LINES} lines, rolling)"
echo "[logmon] Press Ctrl+] to exit monitor."

# Pipe idf.py monitor stdout through Python rolling logger.
# python3 passes every line to the terminal (sys.stdout) and appends it
# to uart.log; every 200 new lines it trims the file to MAX_LINES.
# idf.py monitor gets keyboard input from the tty (not stdin), so Ctrl-]
# works normally even inside this pipe.
"${FACTORY_ROOT}/bin/factory" esp32 monitor "${SCRIPT_DIR}" 2>&1 | \
    python3 -u -c "
import sys
log = sys.argv[1]
max_lines = int(sys.argv[2])
buf = []
try:
    with open(log, 'a') as f:
        for line in sys.stdin:
            sys.stdout.write(line)
            sys.stdout.flush()
            f.write(line)
            f.flush()
            buf.append(line)
            if len(buf) > max_lines * 2:
                buf = buf[-max_lines:]
                f.seek(0)
                f.truncate()
                f.writelines(buf)
                f.flush()
except BrokenPipeError:
    pass
finally:
    buf = buf[-max_lines:]
    with open(log, 'w') as f:
        f.writelines(buf)
" "$UART_LOG" "$MAX_LINES"

echo ""
echo "[logmon] Session ended. Log: ${UART_LOG}"
