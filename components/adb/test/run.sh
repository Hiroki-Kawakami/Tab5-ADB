#!/bin/sh
# Host tests for the adb crate. Run inside the nix dev shell, from anywhere.
# Builds the BoringSSL oracle the pairing tests compare against, then runs the
# crate's #[test]s in the simulator build.
set -e

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../../.." && pwd)
SIM=$ROOT/simulator
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

nix-shell -p boringssl --run "cc -O2 -o '$OUT/spake2_oracle' '$DIR/spake2_oracle.c' -lcrypto"

cmake -G Ninja -S "$SIM" -B "$SIM/build"
ADB_SPAKE2_ORACLE=$OUT/spake2_oracle cmake --build "$SIM/build" --target rust_test_adb
