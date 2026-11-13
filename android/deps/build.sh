#!/bin/zsh
set -eu
cd "${0:A:h}"
./fetch-sources.sh
SDK=${ANDROID_HOME:-/opt/homebrew/share/android-commandlinetools}
CMAKE="$SDK/cmake/3.31.6/bin/cmake"
if [[ ${1:-} == mac ]]; then
 "$CMAKE" -S . -B ../../build-services -G Ninja -DCMAKE_MAKE_PROGRAM="$SDK/cmake/3.31.6/bin/ninja" -DCMAKE_BUILD_TYPE=Release
 "$CMAKE" --build ../../build-services -j4
else
 "$CMAKE" -S . -B build -G Ninja -DCMAKE_MAKE_PROGRAM="$SDK/cmake/3.31.6/bin/ninja" -DCMAKE_TOOLCHAIN_FILE="$SDK/ndk/29.0.14206865/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-34 -DCMAKE_BUILD_TYPE=Release
 "$CMAKE" --build build -j4
fi
