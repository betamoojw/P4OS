#!/usr/bin/env bash
# Build ABI-checked dynamic apps in isolated silicon-profile directories.
# PROFILE=rev3_x tools/build_apps.sh [app...]
# tools/build_apps.sh --all-profiles --all-boards
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
command -v idf.py >/dev/null || source "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1
exec python "$ROOT/tools/build_apps.py" "$@"
