#!/bin/sh
# Builds the agent into app/agent/tab5adb-agent.jar, which the firmware embeds.
# Run in the dev shell: nix develop -c android-agent/build.sh
set -e
cd "$(dirname "$0")"

: "${ANDROID_JAR:?ANDROID_JAR not set; run inside 'nix develop'}"

OUT=build
JAR=../app/agent/tab5adb-agent.jar
rm -rf "$OUT"
mkdir -p "$OUT/classes"

find src -name '*.java' >"$OUT/sources.txt"
javac -source 8 -target 8 -Xlint:none -classpath "$ANDROID_JAR" -d "$OUT/classes" @"$OUT/sources.txt"
d8 --release --lib "$ANDROID_JAR" --output "$OUT/tab5adb-agent.jar" $(find "$OUT/classes" -name '*.class')
cp "$OUT/tab5adb-agent.jar" "$JAR"
echo "$JAR: $(wc -c <"$JAR") bytes"
