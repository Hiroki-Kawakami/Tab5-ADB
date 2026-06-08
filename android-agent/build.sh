#!/bin/sh
# Build the Java agent into a dex jar runnable via app_process.
# Run through the nix dev shell:  nix develop -c android-agent/build.sh
set -e
cd "$(dirname "$0")"

: "${ANDROID_JAR:?ANDROID_JAR not set — run inside 'nix develop'}"

OUT=build
rm -rf "$OUT"
mkdir -p "$OUT/classes"

echo "[build] javac"
find src -name '*.java' >"$OUT/sources.txt"
javac -source 8 -target 8 -Xlint:none -classpath "$ANDROID_JAR" \
      -d "$OUT/classes" @"$OUT/sources.txt"

echo "[build] d8"
# d8 writes a zip (classes.dex inside) when --output ends in .jar — that jar is
# what app_process loads via CLASSPATH.
CLASSES=$(find "$OUT/classes" -name '*.class')
d8 --release --lib "$ANDROID_JAR" --output "$OUT/tab5adb-agent.jar" $CLASSES

echo "[build] -> $OUT/tab5adb-agent.jar ($(wc -c <"$OUT/tab5adb-agent.jar") bytes)"
