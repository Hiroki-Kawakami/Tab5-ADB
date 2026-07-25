#!/bin/sh
# Build & run the adb host tests (the app-facing layer, exercised on the desktop
# against a real Android phone over libusb). Run inside the nix dev shell, from
# anywhere:
#   nix develop -c components/adb/test/run.sh                 # TEST=test_client
#   TEST=test_shell nix develop -c components/adb/test/run.sh
#   TEST=test_sync  nix develop -c components/adb/test/run.sh
#
# TEST selects test/<TEST>.cpp. All adb tests need a phone connected + authorized
# (run test_client once and tap "Allow USB debugging?"); the runner does
# `adb kill-server` first so libusb can claim the interface.
#
# adb runs its reader/worker tasks on the FreeRTOS API, so this links the full
# idf_compat freertos shims (+ nvs/esp_err/esp_timer). Build artifacts go to
# test/build/ (gitignored).
set -e

here=$(CDPATH= cd "$(dirname "$0")" && pwd)
comp=$(CDPATH= cd "$here/.." && pwd)        # components/adb
comps=$(CDPATH= cd "$comp/.." && pwd)       # components
root=$(CDPATH= cd "$comps/.." && pwd)       # repo root
cc="$root/esp-devkit/idf_compat"
out="$here/build"
mkdir -p "$out"

if ! command -v g++ >/dev/null 2>&1; then
    echo "g++ not found — run inside the nix dev shell:" >&2
    echo "  nix develop -c $0" >&2
    exit 1
fi

TEST=${TEST:-test_client}
test_src="$here/$TEST.cpp"
[ -f "$test_src" ] || { echo "no such test: $test_src" >&2; exit 1; }

# idf_compat host shims (C, compiled with gcc — they use C void* casts). Cached
# in build/; delete build/ to force a rebuild.
objs=""
for s in nvs esp_err freertos_task freertos_queue freertos_port \
         freertos_timers freertos_event_groups esp_timer; do
    o="$out/$s.o"
    [ -f "$o" ] || gcc -c "$cc/src/$s.c" -I"$cc/include" -o "$o"
    objs="$objs $o"
done

bin="$out/$TEST"
echo "[build] $TEST"
g++ -std=c++17 -I"$comp/inc" -I"$comps/embedded_adb/inc" -I"$cc/include" \
    "$test_src" \
    "$comp/src/adb_client.cpp" "$comp/src/adb_error.cpp" \
    "$comp/src/adb_raw_stream.cpp" "$comp/src/adb_shell.cpp" "$comp/src/adb_sync.cpp" \
    "$comps/embedded_adb/src/adb_protocol.cpp" "$comps/embedded_adb/src/adb_crypto.cpp" \
    "$comps/embedded_adb/src/adb_keystore.cpp" "$comps/embedded_adb/src/adb_connection.cpp" \
    "$comps/embedded_adb/src/adb_stream.cpp" "$comps/embedded_adb/src/transport_libusb.cpp" \
    "$comps/embedded_adb/src/adb_tcp_socket.cpp" \
    "$comps/embedded_adb/src/adb_tls_stream.cpp" \
    "$comps/embedded_adb/src/transport_tcp.cpp" \
    $objs \
    $(pkg-config --cflags --libs mbedtls mbedcrypto mbedx509 libusb-1.0 libcjson) \
    -lpthread -o "$bin"

echo "[run] adb kill-server; $TEST"
adb kill-server >/dev/null 2>&1 || true
exec "$bin"
