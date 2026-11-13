#!/bin/zsh
# Mesa's NIR and util libraries, which the shader compiler (devices/air/) is built on.
#   android/gfx/mesa/build.sh mac        -> android/gfx/build/mesa-mac
#   android/gfx/mesa/build.sh android    -> android/gfx/build/mesa-android (arm64-v8a, NDK)
# Mesa is fetched once into android/gfx/build/mesa-src at the commit Stela pins (26.2.5) and patched with
# android/gfx/mesa/patches: three generic NIR changes from Stela's series, and one build change so NIR is built
# without any driver. Needs python3 (a venv with mako, pyyaml and packaging is made in android/gfx/build/venv),
# meson and ninja (Homebrew), and for android the NDK from AGENTS.md.
set -eu
TARGET=${1:-mac}
HERE=${0:A:h}
BUILD=${HERE:h}/build
SRC=$BUILD/mesa-src
COMMIT=ddaf85e353442d9d7f192fdcf1823e652e3f2ee3
SDK=${ANDROID_HOME:-/opt/homebrew/share/android-commandlinetools}
NDK=$SDK/ndk/29.0.14206865

mkdir -p $BUILD
if [[ ! -d $SRC/.git ]]; then
    git init -q $SRC
    git -C $SRC remote add origin https://gitlab.freedesktop.org/mesa/mesa.git
    git -C $SRC fetch -q --depth 1 origin $COMMIT
    git -C $SRC checkout -q FETCH_HEAD
    for p in $HERE/patches/*.patch; do git -C $SRC apply --include='src/compiler/*' $p; done
fi
if [[ ! -x $BUILD/venv/bin/python3 ]]; then
    python3 -m venv $BUILD/venv
    $BUILD/venv/bin/pip -q install mako pyyaml packaging
fi
export PATH=$BUILD/venv/bin:/opt/homebrew/bin:$PATH

OPTS=(-Dbuildtype=release -Dplatforms= -Dgallium-drivers= -Dvulkan-drivers= -Dopengl=false -Dgles1=disabled
      -Dgles2=disabled -Degl=disabled -Dgbm=disabled -Dglx=disabled -Dllvm=disabled -Dbuild-tests=false
      -Dvideo-codecs= -Dshared-glapi=disabled -Dzstd=disabled -Dexpat=disabled -Dvalgrind=disabled
      -Dlibunwind=disabled -Dxmlconfig=disabled)
OUT=$BUILD/mesa-$TARGET
if [[ $TARGET == android ]]; then
    TC=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin
    CROSS=$BUILD/android-arm64.cross
    cat > $CROSS <<EOF
[binaries]
c = '$TC/aarch64-linux-android34-clang'
cpp = '$TC/aarch64-linux-android34-clang++'
ar = '$TC/llvm-ar'
strip = '$TC/llvm-strip'
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'
EOF
    OPTS+=(--cross-file $CROSS -Dplatform-sdk-version=34)
fi
[[ -f $OUT/build.ninja ]] || (cd $SRC && meson setup $OUT $OPTS | tail -3)
ninja -C $OUT src/compiler/nir/libnir.a src/compiler/libcompiler.a src/util/libmesa_util.a \
    src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a
echo "mesa ($TARGET): $OUT"
