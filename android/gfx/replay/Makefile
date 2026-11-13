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
