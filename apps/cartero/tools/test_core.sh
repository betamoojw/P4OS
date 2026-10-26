#!/bin/sh
# Cartero's core on the Mac, with the address and undefined-behaviour
# sanitizers: apps/cartero/tools/test_core.sh  (V=1 shows the consoles)
set -e
here=$(cd "$(dirname "$0")" && pwd)
m="$here/../main"
out="${TMPDIR:-/tmp}/cartero_test_core"
clang -g -O1 -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter -DAOS_SIM -DCT_HOST_TEST \
    -I"$m" -I"$here/../../../components/aos_hal/include" \
    "$here/test_core.c" "$here/test_stubs.c" "$m/ct_val.c" "$m/ct_json.c" "$m/ct_js.c" "$m/ct_pm.c" "$m/ct_http.c" \
    -o "$out"
"$out"
