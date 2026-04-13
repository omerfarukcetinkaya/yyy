#!/usr/bin/env bash
# scripts/common/paths.sh
# Centralized path constants for the embedded factory.
# Source this file; do not execute it directly.

# ── Factory root ──────────────────────────────────────────────────────────────
FACTORY_ROOT="${FACTORY_ROOT:-${HOME}/zzz/yyy}"

# ── External tools root ───────────────────────────────────────────────────────
TOOLS_ROOT="${TOOLS_ROOT:-/opt/embedded-tools}"

# ── ESP-IDF ───────────────────────────────────────────────────────────────────
IDF_ROOT="${IDF_ROOT:-${TOOLS_ROOT}/sdk/esp/esp-idf}"
# The ESP-IDF venv was built with Python 3.10. Must be set explicitly so
# idf.py does not try to construct the venv path from system Python (3.13+).
# Auto-detect: prefer py3.13 (used by current project), fall back to py3.10.
if [[ -x "${HOME}/.espressif/python_env/idf5.5_py3.13_env/bin/python" ]]; then
    IDF_PYTHON_ENV_PATH="${IDF_PYTHON_ENV_PATH:-${HOME}/.espressif/python_env/idf5.5_py3.13_env}"
else
    IDF_PYTHON_ENV_PATH="${IDF_PYTHON_ENV_PATH:-${HOME}/.espressif/python_env/idf5.5_py3.10_env}"
fi

# ── Pico SDK ──────────────────────────────────────────────────────────────────
PICO_SDK_PATH="${PICO_SDK_PATH:-${TOOLS_ROOT}/sdk/pico/pico-sdk}"

# ── Toolchains ────────────────────────────────────────────────────────────────
XTENSA_TOOLCHAIN_BIN="${HOME}/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20251107/xtensa-esp-elf/bin"

# ── Factory sub-dirs (derived, not overrideable) ──────────────────────────────
FACTORY_SCRIPTS="${FACTORY_ROOT}/scripts"
FACTORY_CONFIGS="${FACTORY_ROOT}/configs"
FACTORY_PROJECTS="${FACTORY_ROOT}/projects"
FACTORY_ARTIFACTS="${FACTORY_ROOT}/artifacts"
FACTORY_STATE="${FACTORY_ROOT}/state"
FACTORY_TEMPLATES="${FACTORY_ROOT}/templates"
FACTORY_WORKSPACE="${FACTORY_ROOT}/workspace"
FACTORY_TMP="${FACTORY_ROOT}/tmp"
