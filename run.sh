#!/bin/sh
set -e

TARGET=${1:-simulator}

case "$TARGET" in
  simulator)
    [ -d build ] || cmake --fresh -S simulator -B build -G Ninja
    cmake --build build
    ./build/simulator
    ;;
  simverify)
    # Headless, scripted UI verification (no host window): build, then run the
    # given harness script (see esp-devkit/tools/harness/harness.py). Captured
    # frames go wherever the script's `capture` lines point.
    SCRIPT=$2
    if [ -z "$SCRIPT" ]; then
      echo "Usage: $0 simverify <script>   (e.g. simulator/verify/home.txt)"
      exit 1
    fi
    [ -d build ] || cmake --fresh -S simulator -B build -G Ninja
    cmake --build build
    python3 esp-devkit/tools/harness/harness.py --log --sim ./build/simulator "$SCRIPT"
    ;;
  esp32p4)
    idf.py -C esp32p4 flash monitor
    ;;
  *)
    echo "Usage: $0 [simulator|simverify <script>|esp32p4]"
    exit 1
    ;;
esac
