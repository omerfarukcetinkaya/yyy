#!/usr/bin/env bash
# scripts/env/load_common.sh
# Sources shared factory paths and logging. Must be sourced by all
# target-specific loaders before any tool-specific setup.

_LC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${_LC_DIR}/../common/paths.sh"
source "${_LC_DIR}/../common/log.sh"
