#!/bin/zsh
# Build a Mach-O for the guest, never an Android executable.
set -eu
cd "${0:A:h}/../.."
mkdir -p build-services/guest
 env -u SDKROOT -u IPHONEOS_DEPLOYMENT_TARGET xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=12.0 -fobjc-arc -Wall -Wextra -O2 -framework AppKit tools/henge-agent.m -o build-services/guest/henge-agent
codesign -f -s - build-services/guest/henge-agent
gzip -9 -n -c build-services/guest/henge-agent > build-services/guest/henge-agent.gz
