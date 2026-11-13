#!/bin/sh
set -eu
cd "$(dirname "$0")"
SDK=${ANDROID_HOME:-/opt/homebrew/share/android-commandlinetools}
ADB="$SDK/platform-tools/adb"
serial=${1:-${ANDROID_SERIAL:-}}
if [ -z "$serial" ]; then
  devices=$("$ADB" devices | awk 'NR>1 && $2=="device" {print $1}')
  count=$(printf '%s\n' "$devices" | awk 'NF {n++} END {print n+0}')
  [ "$count" = 1 ] || { echo 'Pass an adb serial when multiple devices are attached.' >&2; exit 2; }
  serial=$devices
fi
remote=/data/local/tmp/menhir-engine
"$ADB" -s "$serial" shell "mkdir -p $remote"
trap '"$ADB" -s "$serial" shell "rm -rf /data/local/tmp/menhir-engine" >/dev/null 2>&1 || true' EXIT
for t in classify jit_region operands jit mem mmu exc devices shutdown platform; do
  "$ADB" -s "$serial" push "build/test_$t" "$remote/" >/dev/null
  "$ADB" -s "$serial" shell "chmod 700 $remote/test_$t; cd $remote; ulimit -s 65520; export HV_TEST_WORK=$remote; ./test_$t"
done
