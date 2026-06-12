#!/bin/sh
# Build & run the m5stack-bsp host unit tests. Run inside the nix dev shell,
# from anywhere:
#   nix develop -c components/m5stack-bsp/test/run.sh                  # TEST=test_audio_dsp
#   TEST=test_bsp_audio nix develop -c components/m5stack-bsp/test/run.sh
#   TEST=test_sdl_audio nix develop -c components/m5stack-bsp/test/run.sh
#
# test_audio_dsp: pure DSP math (no audio device, no GUI). test_bsp_audio:
# the dispatch policy (DSP voicing modes, amp arming, HP-route re-voicing)
# against a stub provider. test_sdl_audio:
# plays a sine through the bsp_audio dispatch + SDL provider — audible when a
# sound device exists, and falls back to the pacing null sink otherwise (set
# SIMULATOR_HEADLESS=1 to force the null sink), so it also verifies the
# write() backpressure timing headless.
#
# Build artifacts go to test/build/ (gitignored).
set -e

here=$(CDPATH= cd "$(dirname "$0")" && pwd)
comp=$(CDPATH= cd "$here/.." && pwd)        # components/m5stack-bsp
root=$(CDPATH= cd "$comp/../.." && pwd)     # repo root
cc="$root/simulator/idf_compat"
out="$here/build"
mkdir -p "$out"

if ! command -v gcc >/dev/null 2>&1; then
    echo "gcc not found — run inside the nix dev shell:" >&2
    echo "  nix develop -c $0 $*" >&2
    exit 1
fi

TEST=${TEST:-test_audio_dsp}
test_src="$here/$TEST.c"
[ -f "$test_src" ] || { echo "no such test: $test_src" >&2; exit 1; }

# idf_compat host shims (the pthread-backed FreeRTOS API). Cached in build/.
objs=""
for s in freertos_port freertos_queue freertos_task; do
    o="$out/$s.o"
    [ -f "$o" ] || gcc -c "$cc/src/$s.c" -I"$cc/include" -o "$o"
    objs="$objs $o"
done

bin="$out/$TEST"
echo "[build] $TEST"
case "$TEST" in
test_sdl_audio)
    gcc -std=c11 \
        -I"$comp/inc" -I"$comp/inc_private" -I"$comp/simulator" -I"$cc/include" \
        "$test_src" \
        "$comp/src/audio_dsp.c" "$comp/src/bsp_audio.c" "$comp/simulator/sdl_audio.c" \
        $objs \
        $(pkg-config --cflags --libs sdl2) -lm -lpthread \
        -o "$bin"
    ;;
test_bsp_audio)
    gcc -std=c11 \
        -I"$comp/inc" -I"$comp/inc_private" -I"$cc/include" \
        "$test_src" \
        "$comp/src/audio_dsp.c" "$comp/src/bsp_audio.c" \
        $objs \
        -lm -lpthread \
        -o "$bin"
    ;;
*)
    gcc -std=c11 \
        -I"$comp/inc" -I"$cc/include" \
        "$test_src" \
        "$comp/src/audio_dsp.c" \
        $objs \
        -lm -lpthread \
        -o "$bin"
    ;;
esac

echo "[run] $TEST"
exec "$bin"
