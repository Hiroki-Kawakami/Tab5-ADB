#!/bin/sh
# Build & run the embedded_adb host tests (the protocol engine, exercised on the
# desktop). Run inside the nix dev shell, from anywhere:
#   nix develop -c components/embedded_adb/test/run.sh           # TEST=test_crypto
#   TEST=test_connect nix develop -c components/embedded_adb/test/run.sh
#   TEST=test_shell   nix develop -c components/embedded_adb/test/run.sh
#
# TEST selects test/<TEST>.cpp. test_crypto needs NO device (pure auth math, only
# mbedTLS). test_connect / test_shell talk to a real phone over libusb — connect
# + authorize it (test_connect's first run prompts "Allow USB debugging?"), and
# the runner does `adb kill-server` first so libusb can claim the interface.
#
# embedded_adb is std::thread-based (no FreeRTOS), so the only idf_compat shims it
# needs are nvs + esp_err. Build artifacts go to test/build/ (gitignored).
set -e

here=$(CDPATH= cd "$(dirname "$0")" && pwd)
comp=$(CDPATH= cd "$here/.." && pwd)        # components/embedded_adb
root=$(CDPATH= cd "$comp/../.." && pwd)      # repo root
cc="$root/simulator/idf_compat"
out="$here/build"
mkdir -p "$out"

if ! command -v g++ >/dev/null 2>&1; then
    echo "g++ not found — run inside the nix dev shell:" >&2
    echo "  nix develop -c $0" >&2
    exit 1
fi

TEST=${TEST:-test_crypto}
test_src="$here/$TEST.cpp"
[ -f "$test_src" ] || { echo "no such test: $test_src" >&2; exit 1; }

bin="$out/$TEST"
echo "[build] $TEST"
if [ "$TEST" = "test_crypto" ]; then
    # Standalone: no device, no NVS/USB — just the crypto unit + mbedTLS.
    g++ -std=c++17 -I"$comp/inc" \
        "$test_src" "$comp/src/adb_crypto.cpp" \
        $(pkg-config --cflags --libs mbedtls mbedcrypto mbedx509) \
        -o "$bin"
    echo "[run] $TEST"
    exec "$bin"
fi

# Device tests: full engine + idf_compat (nvs/esp_err, compiled with gcc — they
# use C void* casts). Cached in build/; delete build/ to force a rebuild.
objs=""
for s in nvs esp_err; do
    o="$out/$s.o"
    [ -f "$o" ] || gcc -c "$cc/src/$s.c" -I"$cc/include" -o "$o"
    objs="$objs $o"
done

g++ -std=c++17 -I"$comp/inc" -I"$cc/include" \
    "$test_src" \
    "$comp/src/adb_protocol.cpp" "$comp/src/adb_crypto.cpp" \
    "$comp/src/adb_keystore.cpp" "$comp/src/adb_connection.cpp" \
    "$comp/src/adb_stream.cpp" "$comp/src/transport_libusb.cpp" \
    $objs \
    $(pkg-config --cflags --libs mbedtls mbedcrypto mbedx509 libusb-1.0 libcjson) \
    -lpthread -o "$bin"

echo "[run] adb kill-server; $TEST"
adb kill-server >/dev/null 2>&1 || true
exec "$bin"
