#!/bin/sh
set -eu
cd "$(dirname "$0")"
SDK=${ANDROID_HOME:-/opt/homebrew/share/android-commandlinetools}
"$SDK/cmake/3.31.6/bin/cmake" -S . -B build -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$SDK/cmake/3.31.6/bin/ninja" \
  -DCMAKE_TOOLCHAIN_FILE="$SDK/ndk/29.0.14206865/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-34 -DCMAKE_BUILD_TYPE=RelWithDebInfo "$@"
"$SDK/cmake/3.31.6/bin/cmake" --build build --parallel 4
