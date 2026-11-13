# The replay rig on the Mac (docs/android/graphics.md). From the repository root:
#   make -f android/gfx/replay/Makefile            # android/gfx/build/mac/pvg_replay_metal
#   make -f android/gfx/replay/Makefile vulkan     # android/gfx/build/mac/pvg_replay_vulkan (MoltenVK)
# PVG_LITE=path picks another pvg_lite.c (a snapshot, while someone else is editing the tree's).
CC = clang
O = android/gfx/build/mac
PVG_LITE ?= devices/pvg_lite.c
CFLAGS = -std=c11 -Wall -Wextra -O2 -g -Icore -Ihost -Idevices
METAL_LIBS = -framework Foundation -framework QuartzCore -framework Metal -framework IOSurface -framework CoreGraphics \
             -framework ImageIO -framework UniformTypeIdentifiers -framework CoreFoundation
COMMON = android/gfx/replay/pvg_replay.c $(PVG_LITE) devices/pvg_vulkan_capture.c

all: $(O)/pvg_replay_metal

$(O)/pvg_replay_metal: $(COMMON) devices/pvg_metal.m devices/metallib.c devices/*.h | $(O)
	$(CC) $(CFLAGS) -Wno-unused-parameter -c devices/pvg_metal.m -fobjc-arc -Wno-deprecated-declarations -o $(O)/pvg_metal.o
	$(CC) $(CFLAGS) $(COMMON) devices/metallib.c $(O)/pvg_metal.o $(METAL_LIBS) -lz -o $@

$(O):
	mkdir -p $(O)

.PHONY: all

# The descriptor parser against Metal's own serializer.
test-desc: $(O)/test_desc
	$(O)/test_desc
$(O)/test_desc: android/gfx/tests/test_desc.m devices/pvg_vulkan_desc.c devices/pvg_vulkan_desc.h | $(O)
	$(CC) -fobjc-arc -O2 -g -Wall -Idevices android/gfx/tests/test_desc.m devices/pvg_vulkan_desc.c -framework Metal -framework Foundation -o $@
.PHONY: test-desc

# The shader compiler on the Mac (devices/air/README.md): android/gfx/mesa/build.sh mac first.
MESA_SRC = android/gfx/build/mesa-src
MESA_OUT = android/gfx/build/mesa-mac
LLVM = /opt/homebrew/opt/llvm
AIR_DEFS = -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS -DHAVE_PTHREAD -DHAVE___BUILTIN_CLZ \
  -DHAVE___BUILTIN_CLZLL -DHAVE___BUILTIN_CTZ -DHAVE___BUILTIN_EXPECT -DHAVE___BUILTIN_FFS -DHAVE___BUILTIN_FFSLL \
  -DHAVE___BUILTIN_POPCOUNT -DHAVE___BUILTIN_POPCOUNTLL -DHAVE___BUILTIN_UNREACHABLE -DHAVE___BUILTIN_TYPES_COMPATIBLE_P \
  -DHAVE_FUNC_ATTRIBUTE_PACKED -DHAVE_FUNC_ATTRIBUTE_CONST -DHAVE_FUNC_ATTRIBUTE_PURE -DHAVE_FUNC_ATTRIBUTE_UNUSED \
  -DHAVE_FUNC_ATTRIBUTE_NORETURN -DHAVE_FUNC_ATTRIBUTE_FORMAT -DHAVE_FUNC_ATTRIBUTE_WARN_UNUSED_RESULT \
  -DHAVE_FUNC_ATTRIBUTE_MALLOC -DHAVE_FUNC_ATTRIBUTE_RETURNS_NONNULL -DHAVE_FUNC_ATTRIBUTE_VISIBILITY \
  -DUSE_GCC_ATOMIC_BUILTINS -DNDEBUG -DMESA_DEBUG=0 -DHAVE_STRUCT_TIMESPEC
AIR_INC = -I$(MESA_SRC)/include -I$(MESA_SRC)/src -I$(MESA_SRC)/src/compiler -I$(MESA_SRC)/src/compiler/nir \
  -I$(MESA_SRC)/src/gallium/include -I$(MESA_SRC)/src/gallium/auxiliary -I$(MESA_OUT)/src -I$(MESA_OUT)/src/compiler \
  -I$(MESA_OUT)/src/compiler/nir -I$(MESA_OUT)/src/util -Idevices/air/stela/llvm/include -I$(LLVM)/include \
  -Idevices/air/stela -Idevices/air/ntv -Idevices/air
AIR_CFLAGS = -std=c11 -O2 -g -fno-math-errno -Wno-unused-function -Wno-unused-variable -Wno-missing-field-initializers
AIR_SRC = $(wildcard devices/air/stela/*.c) devices/air/ntv/nir_to_spirv.c devices/air/ntv/spirv_builder.c \
  devices/air/air_spirv.c devices/air/air_library.c
AIR_OBJ = $(patsubst devices/air/%.c,$(O)/air/%.o,$(AIR_SRC))
MESA_LIBS = $(MESA_OUT)/src/compiler/nir/libnir.a $(MESA_OUT)/src/compiler/libcompiler.a $(MESA_OUT)/src/util/libmesa_util.a \
  $(MESA_OUT)/src/util/blake3/libblake3.a $(MESA_OUT)/src/c11/impl/libmesa_util_c11.a

$(O)/air/%.o: devices/air/%.c
	@mkdir -p $(dir $@)
	$(CC) $(AIR_CFLAGS) $(AIR_DEFS) $(AIR_INC) -c $< -o $@
$(O)/aircc: android/gfx/tools/aircc.c $(AIR_OBJ)
	$(CC) -std=c11 -O2 -g -Idevices/air $^ $(MESA_LIBS) $(LLVM)/lib/libLLVM.dylib -Wl,-rpath,$(LLVM)/lib -lz -o $@
aircc: $(O)/aircc
.PHONY: aircc
