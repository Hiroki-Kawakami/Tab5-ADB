#!/bin/sh
# Host-JVM unit tests for the agent's device-independent logic (no phone, no
# android.jar at runtime). Today: ProjectionTest — the GPU-projection arithmetic
# in Projection.java (rotate/scale-fit/center) that used to be CPU geometry in
# FramePipeline and was covered by the headless C++ test.
#
# Run through the nix dev shell:  nix develop -c android-agent/test/run.sh
set -e
cd "$(dirname "$0")/.."   # android-agent/

OUT=test/build
rm -rf "$OUT"
mkdir -p "$OUT"

# Only the android-free sources are compilable on the plain JDK: Projection (the
# unit under test) + the test itself. The rest of the agent pulls in android.* and
# is built/verified by build.sh + the headless C++ harness instead.
echo "[test] javac"
javac -d "$OUT" src/com/tab5adb/agent/Projection.java test/ProjectionTest.java

echo "[test] run"
java -cp "$OUT" com.tab5adb.agent.ProjectionTest
