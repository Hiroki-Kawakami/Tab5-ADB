#!/bin/sh
# Build & run the term_emu host unit test — pure parser/grid checks, no phone,
# no GUI. Run inside the nix dev shell, from anywhere:
#   nix develop -c components/term_emu/test/run.sh
#
# Build artifacts go to test/build/ (gitignored).
# Env: TEST=test_term (default) selects which test/<TEST>.cpp to build.
set -e

here=$(CDPATH= cd "$(dirname "$0")" && pwd)
comp=$(CDPATH= cd "$here/.." && pwd)        # components/term_emu
comps=$(CDPATH= cd "$comp/.." && pwd)       # components
root=$(CDPATH= cd "$comps/.." && pwd)       # repo root
cc="$root/simulator/idf_compat"
out="$here/build"
mkdir -p "$out"

if ! command -v g++ >/dev/null 2>&1; then
    echo "g++ not found — run inside the nix dev shell:" >&2
    echo "  nix develop -c $0 $*" >&2
    exit 1
fi

TEST=${TEST:-test_term}
test_src="$here/$TEST.cpp"
[ -f "$test_src" ] || { echo "no such test: $test_src" >&2; exit 1; }

# idf_compat host shim (C, compiled with gcc). Cached in build/.
o="$out/esp_heap_caps.o"
[ -f "$o" ] || gcc -c "$cc/src/esp_heap_caps.c" -I"$cc/include" -o "$o"

bin="$out/$TEST"
echo "[build] $TEST"
g++ -std=c++17 \
    -I"$comp/inc" -I"$cc/include" \
    "$test_src" \
    "$comp/src/vt_parser.cpp" "$comp/src/term_emu.cpp" \
    "$o" \
    -o "$bin"

echo "[run] $TEST"
exec "$bin"
