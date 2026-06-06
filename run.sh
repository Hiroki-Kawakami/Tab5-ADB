#!/bin/sh
set -e

TARGET=${1:-simulator}

case "$TARGET" in
  simulator)
    [ -d build ] || cmake --fresh -S simulator -B build -G Ninja
    cmake --build build
    ./build/simulator
    ;;
  esp32p4)
    idf.py -C esp32p4 flash monitor
    ;;
  *)
    echo "Usage: $0 [simulator|esp32p4]"
    exit 1
    ;;
esac
