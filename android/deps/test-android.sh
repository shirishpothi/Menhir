#!/bin/zsh
# Build first. Use an already running emulator; never choose a physical device.
set -eu
cd "${0:A:h}"
serial=${1:?usage: test-android.sh emulator-SERIAL}
[[ $serial == emulator-* ]] || { echo 'An emulator serial is required' >&2; exit 2; }
SDK=${ANDROID_HOME:-/opt/homebrew/share/android-commandlinetools}
adb=("$SDK/platform-tools/adb" -s "$serial")
remote="/data/local/tmp/menhir-services-$$"
"${adb[@]}" shell "mkdir '$remote'"
trap '"${adb[@]}" shell "rm -rf '\''$remote'\''" >/dev/null' EXIT
for test in services_restore_link services_audio services_slirp services_http services_fetch services_compat; do
 "${adb[@]}" push "build/tests/$test" "$remote/$test" >/dev/null
done
for test in services_restore_link services_audio services_slirp services_http services_compat; do
 "${adb[@]}" shell "$remote/$test"
done
"${adb[@]}" shell "$remote/services_fetch rom $remote/rom.bin"
"${adb[@]}" shell "sha256sum $remote/rom.bin"
# Repeat to check the verified-ROM cache path.
"${adb[@]}" shell "$remote/services_fetch rom $remote/rom.bin"
url=https://curl.se/download/curl-8.12.1.tar.xz
if "${adb[@]}" shell "$remote/services_fetch image $url $remote/resume.xz '' 1048576"; then
 echo 'Expected cancellation before completion' >&2; exit 1
else
 [[ $? == 3 ]] || exit 1
fi
"${adb[@]}" shell "$remote/services_fetch image $url $remote/resume.xz"
hash=$("${adb[@]}" shell "sha256sum $remote/resume.xz")
[[ $hash == 0341f1ed97a26c811abaebd37d62b833956792b7607ea3f15d001613c76de202* ]]
echo 'Native cancellation/resume SHA-256: PASS'
