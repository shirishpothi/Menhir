#!/bin/zsh
# LLVM 23 for the shader compiler on Android: what Stela's AIR reader needs (bitcode reader, IR, passes), no code
# generation targets, static, into android/gfx/build/llvm-android/install. The same release as Homebrew's llvm on
# the Mac (23.1.5), which is what the Mac build of the compiler links. About 30 minutes on an M3.
set -eu
HERE=${0:A:h}
BUILD=${HERE:h}/build
SRC=$BUILD/llvm-src
TAG=llvmorg-23.1.5
SDK=${ANDROID_HOME:-/opt/homebrew/share/android-commandlinetools}
NDK=$SDK/ndk/29.0.14206865
CMAKE=$SDK/cmake/3.31.6/bin/cmake

if [[ ! -d $SRC/llvm ]]; then
    git init -q $SRC
    git -C $SRC remote add origin https://github.com/llvm/llvm-project.git
    git -C $SRC config core.sparseCheckout true
    printf '/llvm/\n/cmake/\n/third-party/\n/libc/shared/\n/libc/src/__support/\n/libc/hdr/\n/libc/include/llvm-libc-macros/\n/libc/include/llvm-libc-types/\n/libc/LICENSE.TXT\n' > $SRC/.git/info/sparse-checkout
    git -C $SRC fetch -q --depth 1 origin tag $TAG
    git -C $SRC checkout -q $TAG
fi
OUT=$BUILD/llvm-android
$CMAKE -S $SRC/llvm -B $OUT -G Ninja -DCMAKE_MAKE_PROGRAM=$SDK/cmake/3.31.6/bin/ninja \
    -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-34 \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$OUT/install \
    -DLLVM_TARGETS_TO_BUILD= -DLLVM_ENABLE_PROJECTS= -DLLVM_BUILD_TOOLS=OFF -DLLVM_INCLUDE_TOOLS=OFF \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_UTILS=OFF \
    -DLLVM_INCLUDE_DOCS=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_THREADS=ON -DLLVM_ENABLE_RTTI=OFF -DLLVM_ENABLE_PIC=ON \
    -DLLVM_HOST_TRIPLE=aarch64-linux-android -DLLVM_DEFAULT_TARGET_TRIPLE=aarch64-linux-android \
    -DLLVM_NATIVE_TOOL_DIR= -DCMAKE_CXX_FLAGS="-ffunction-sections -fdata-sections" -DCMAKE_C_FLAGS="-ffunction-sections -fdata-sections" | tail -3
nice $CMAKE --build $OUT --parallel ${JOBS:-6} --target install-llvm-headers LLVMCore LLVMBitReader LLVMBitstreamReader \
    LLVMSupport LLVMDemangle LLVMAnalysis LLVMTransformUtils LLVMPasses LLVMTargetParser LLVMBinaryFormat LLVMRemarks \
    LLVMIRReader LLVMAsmParser LLVMLinker
echo "llvm (android): $OUT"
