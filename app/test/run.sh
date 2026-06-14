#!/bin/sh
# Build & run the app-layer host unit tests — pure string-parser checks
# (device_info), no phone, no GUI, no LVGL. Run inside the nix dev shell,
# from anywhere:
#   nix develop -c app/test/run.sh
#
# Build artifacts go to test/build/ (gitignored).
# Env: TEST=test_device_info (default) selects which test/<TEST>.cpp to build.
set -e

here=$(CDPATH= cd "$(dirname "$0")" && pwd)
app=$(CDPATH= cd "$here/.." && pwd)         # app/
out="$here/build"
mkdir -p "$out"

if ! command -v g++ >/dev/null 2>&1; then
    echo "g++ not found — run inside the nix dev shell:" >&2
    echo "  nix develop -c $0 $*" >&2
    exit 1
fi

TEST=${TEST:-test_device_info}
test_src="$here/$TEST.cpp"
[ -f "$test_src" ] || { echo "no such test: $test_src" >&2; exit 1; }

bin="$out/$TEST"
echo "[build] $TEST"
g++ -std=c++17 \
    -I"$app" \
    "$test_src" \
    "$app/device_info.cpp" \
    "$app/apk_info.cpp" \
    "$app/sysclock.cpp" \
    -lz \
    -o "$bin"

echo "[run] $TEST"
# argv[1] = repo root, for tests with file fixtures (apk_info).
exec "$bin" "$app/.."
