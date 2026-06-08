#!/bin/sh
# Dev loop: push the agent to the phone and run it via app_process (standard adb).
# Run through the nix dev shell:  nix develop -c android-agent/run.sh
#
# While it runs, in another shell connect to the socket from the PC:
#   nix develop -c adb forward tcp:8080 localabstract:tab5adb-agent
#   nc localhost 8080
set -e
cd "$(dirname "$0")"

JAR=build/tab5adb-agent.jar
[ -f "$JAR" ] || { echo "build first: nix develop -c android-agent/build.sh"; exit 1; }

REMOTE=/data/local/tmp/tab5adb-agent.jar
MAIN=com.tab5adb.agent.Server

echo "[run] push -> $REMOTE"
adb push "$JAR" "$REMOTE" >/dev/null

echo "[run] app_process ($MAIN); Ctrl-C to stop"
exec adb shell CLASSPATH="$REMOTE" app_process / "$MAIN"
