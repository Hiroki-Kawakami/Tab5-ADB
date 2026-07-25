#!/bin/sh
# Build & run the agent_link host test (the Tab5 role, played by the real
# adb + agent_link stack over libusb vs a real phone — no GUI).
#
# Run inside the nix dev shell, from anywhere:
#   nix develop -c components/agent_link/test/run.sh [jar-path]
#
# Prereqs: a phone connected + authorized (run components/adb/test/test_client
# once and tap Allow), and the agent jar built:
#   nix develop -c android-agent/build.sh
# This runs `adb kill-server` first so libusb can claim the interface.
#
# Build artifacts (incl. cached idf_compat .o) go to test/build/ (gitignored).
# Env: TEST=test_hello (default) selects which test/<TEST>.cpp to build.
set -e

here=$(CDPATH= cd "$(dirname "$0")" && pwd)
comp=$(CDPATH= cd "$here/.." && pwd)        # components/agent_link
comps=$(CDPATH= cd "$comp/.." && pwd)       # components
root=$(CDPATH= cd "$comps/.." && pwd)       # repo root
cc="$root/esp-devkit/idf_compat"
out="$here/build"
mkdir -p "$out"

if ! command -v g++ >/dev/null 2>&1; then
    echo "g++ not found — run inside the nix dev shell:" >&2
    echo "  nix develop -c $0 $*" >&2
    exit 1
fi

TEST=${TEST:-test_hello}
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
g++ -std=c++17 \
    -I"$comp/inc" -I"$comps/adb/inc" \
    -I"$comps/adb/src/core/include" -I"$comps/adb/src/core" \
    -I"$cc/include" \
    "$test_src" \
    "$comp/src/agent_link.cpp" \
    "$comps/adb/src/adb_client.cpp" "$comps/adb/src/adb_error.cpp" \
    "$comps/adb/src/adb_pairing.cpp" \
    "$comps/adb/src/adb_raw_stream.cpp" "$comps/adb/src/adb_shell.cpp" \
    "$comps/adb/src/adb_sync.cpp" \
    "$comps/adb/src/core/adb_protocol.cpp" "$comps/adb/src/core/adb_crypto.cpp" \
    "$comps/adb/src/core/adb_keystore.cpp" "$comps/adb/src/core/adb_spake2.cpp" \
    "$comps/adb/src/core/adb_pairing_crypto.cpp" \
    "$comps/adb/src/core/adb_pairing_protocol.cpp" \
    "$comps/adb/src/core/adb_pairing.cpp" \
    "$comps/adb/src/core/adb_connection.cpp" "$comps/adb/src/core/adb_stream.cpp" \
    "$comps/adb/src/core/transport_libusb.cpp" \
    "$comps/adb/src/core/adb_tcp_socket.cpp" \
    "$comps/adb/src/core/adb_tls_stream.cpp" \
    "$comps/adb/src/core/transport_tcp.cpp" \
    $objs \
    $(pkg-config --cflags --libs mbedtls mbedcrypto mbedx509 libusb-1.0 libcjson libjpeg) \
    -lpthread -o "$bin"

# Default the jar path to build.sh's output unless the caller overrides argv[1].
jar=${1:-"$root/android-agent/build/tab5adb-agent.jar"}

if [ "${BUILD_ONLY:-0}" = 1 ]; then
    echo "[build-only] $TEST"
    exit 0
fi

echo "[run] adb kill-server; $TEST"
adb kill-server >/dev/null 2>&1 || true
exec "$bin" "$jar"
