#!/bin/bash
# build.sh [rev3_x|rev1_3] [lvA|lvB|lvC] [idf.py args...]
# One build directory per chip profile and LVGL variant, so the generated
# sdkconfig of one never leaks into another (docs/hw/waveshare/CI.md).
set -e
cd "$(dirname "$0")"
profile=${1:-rev3_x}; variant=${2:-lvB}; shift 2 || true
command -v idf.py >/dev/null || source ~/esp/esp-idf/export.sh >/dev/null 2>&1
board=${AOS_BOARD:-lcd5}
case "$board" in lcd5|lcd7|lcd8|lcd10_1) ;; *) echo "Unknown AOS_BOARD: $board"; exit 1 ;; esac
dir="build/$profile-$variant"
[ "$board" = lcd5 ] || dir="build/$board/$profile-$variant"
idf.py -D "AOS_BOARD=$board" -B "$dir" -D "SDKCONFIG=$PWD/$dir/sdkconfig" \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.$profile;sdkconfig.defaults.$variant" \
  "${@:-build}"
