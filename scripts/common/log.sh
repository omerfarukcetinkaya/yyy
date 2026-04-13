#!/usr/bin/env bash
# scripts/common/log.sh
# Coloured logging helpers for factory scripts.
# Source this file; do not execute it directly.

# Respect NO_COLOR (https://no-color.org)
if [[ -t 2 && -z "${NO_COLOR:-}" ]]; then
    _CLR_RESET='\033[0m'
    _CLR_INFO='\033[0;36m'
    _CLR_OK='\033[0;32m'
    _CLR_WARN='\033[0;33m'
    _CLR_ERROR='\033[0;31m'
    _CLR_STEP='\033[0;35m'
else
    _CLR_RESET='' _CLR_INFO='' _CLR_OK='' _CLR_WARN='' _CLR_ERROR='' _CLR_STEP=''
fi

log_info()  { echo -e "${_CLR_INFO}[INFO]${_CLR_RESET}  $*" >&2; }
log_ok()    { echo -e "${_CLR_OK}[OK]${_CLR_RESET}    $*" >&2; }
log_warn()  { echo -e "${_CLR_WARN}[WARN]${_CLR_RESET}  $*" >&2; }
log_error() { echo -e "${_CLR_ERROR}[ERROR]${_CLR_RESET} $*" >&2; }
log_step()  { echo -e "${_CLR_STEP}[STEP]${_CLR_RESET}  $*" >&2; }

# die <message> [exit_code]
die() {
    log_error "$1"
    exit "${2:-1}"
}
