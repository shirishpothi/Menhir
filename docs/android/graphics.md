# Graphics on Android

macOS draws through Apple's paravirtual GPU driver (`AppleParavirtGPU`). On iOS, Henge answers that driver with
`devices/pvg_lite.c` and replays its work with Metal in `devices/pvg_metal.m`. Android has no Metal, so Menhir
replays the same work off-screen with Vulkan, in `devices/pvg_vulkan.c`. Window presentation is not implemented.

This doc is the protocol as far as the backend needs it, the formats the guest sends, the shader route, what is
covered, and how it is tested without the tablet. Section 7 says what is done and what is not; keep it true.

## 1. The device and its protocol

`pvg_lite.c` is the device; the backend never touches registers or rings. Everything below the line "backend" in
this picture is what `pvg_vulkan.c` replaces.

```
guest driver ── MMIO doorbells ──> pvg_lite.c: rings, stamps, display page, iosurface mapper
                                       │ hv_pvg_metal_submit(channel, command)   (one command, in order)
                                       ▼
                               backend (pvg_metal.m / pvg_vulkan.c)
                                       │ reads task memory and surfaces through hv_pvg_lite_* accessors
                                       │ hv_pvg_lite_complete(channel, stamp) when the GPU is done
```

**Registers and rings** are described in the header comment of `devices/pvg_lite.c`. In short: a root ring and up
to 15 channel rings, each made of guest pages listed in a page table; every command starts `{op, length, stamp}`;
bits 31..16 of `op` count `{channel, stamp}` barriers that come first. A command is complete when its stamp is
stored in the FIFO buffer's control area (`+4 * channel`) and the interrupt is raised.

**Tasks.** Each guest process that uses the GPU is a task. Command 0x38 (on the root ring or a channel) names the
task's root page; its memory is a 1 to 3 level table of 16 KB pages (`task_ptr`). Command 0x44 (macOS 15 and later)
names the task's object list and placement list; older drivers keep the object list at task offset 0x4000.

**Objects** are numbered per task (a *ref*). The object list has a 12-byte entry per ref: `{type | size << 8,
task offset (u64)}`. The backend makes an object the first time a command names it, and makes it again if the
entry changes. Types (`pvg_metal.m object:task:`):

| Type | What | Entry data |
| --- | --- | --- |
| 1, 19 | buffer over task memory | `{u64 length, u32 first page, u32 flags}` |
| 2, 3 | texture with a copy in task memory | `{u64 length, u32 page}`, a layout table (mips, slices, 36 bytes a mip), then a texture description record |
| 6 | function | `{u64 offset, u32 length}` of a metallib in task memory |
| 7 | serialized object, by its tag: 3 sampler, 4 depth-stencil, 0xb compute pipeline, 0xd fence, 0xe render pipeline, 0x33 tile pipeline | see section 2 |
| 8, 9 | texture without memory, in a heap, a view, or over a buffer (by record tag: 1/0x14/0x34 private; 0x15/0x38 in a heap; 7/8/0x1b view; 9/0x37 over a buffer) | |
| 0xb | texture over a surface (IOSurface): `{u64 mapping, tag, size, ref, description}`, plane after the description | |
| 13, 14 | heap `{page, length, flags}`, buffer in a heap `{heap, offset, length}` | |

**Channel commands** the backend handles (`hv_pvg_metal_submit`):

| Op | Meaning |
| --- | --- |
| 0x43, 0x45 | exec: `{task, resources, buffers}`, `{ref, flags}` per resource (bit 0 guest wrote it, bit 1 this exec writes it, bit 2 copy back after), `{offset, length}` per command buffer |
| 0x37 | exec of macOS 13's driver (24-byte resource entries) |
| 0x25, 0x28 | release an object |
| 0x20 | a task ended |
| 0x34 | invalidate: per ref, which side (host or guest) holds valid data |
| 0x35, 0x3e | synchronize: copy refs back to guest memory |
| 0x3f | purgeable (ignored) |
| 1, 2 | display: name the shared display page, acknowledge a mode |
| 4 | new pointer image `{port, 0, u64 offset, u64 length, u64 stride, u16 w, h, hot x, hot y}`, premultiplied BGRA in task 0 |
| 5 | pointer shown or hidden |
| 8 | swap: `{port, 0, u64 mapping, u64 gamma offset, u64 gamma length, count, sum}`: show surface `mapping` |

**Command buffers** are segments `{u32 length, u32 info}` (info: encoder type in bits 7..0: 0 render, 1 compute,
2 blit, 4 queries; bit 8..15 continues the previous encoder; 16..23 more segments follow; type 5 wraps a
protection header). A segment is a list of `{u32 tag, u32 size}` records, one per Metal call. Render tags are below
200, compute 200..299, blit 300..399, queries 450..469. Tag 26 begins a render pass (the pass description record:
three attachment blocks of 0x3c bytes, clear values, render target size); tags 30..36 describe the pass that
follows. The full list, with every record's layout, is the decoder in `pvg_metal.m` (`render:`, `compute:`, `blit:`,
`info:`); section 6 lists which ones the Vulkan backend does.

**Queries (450..469)** write host facts into guest buffers: GPU addresses of buffers (461) and heaps (463), resource
IDs of textures, samplers and pipelines (462, 464, 466..468), pipeline limits (450, 457), heap sizes (451, 469),
unique IDs (460). The guest's Metal puts these values in its argument buffers, so they must be what the backend's
shaders read: in Menhir a buffer's "GPU address" is its Vulkan device address and a texture's resource ID is its
index in the backend's descriptor heap (section 3).

**Surfaces.** The iosurface mapper ring maps and unmaps surfaces by *mapping id*. A surface's 1 KB descriptor
(0x200 bytes for macOS 14's driver) is found through the mapper's table (`hv_pvg_lite_surface`): page list
(physical >> 12, bit 0 present, one per 16 KB) at +0, fourcc +4, allocation +0x18, width +0x20, height +0x28, bytes
per row +0x30, plane count +0x3e, first byte +0x60, planes from +0x80 at 0x80 each. A surface's pixels are guest
memory, shared between processes: WindowServer reads what apps drew.

**The display.** The guest names a 16 KB shared display page (op 1). `pvg_lite.c` fills the mode list (one mode,
the size set with `hv_pvg_lite_set_mode`), and counts swaps there. The screen is a 1920x1080 (or the mode's size)
RGBA16Float surface, fourcc `RGhA`, holding display-encoded values (`docs/agents/colorprobe.md`): shown by clamping,
not by encoding again. Some macOS versions swap a BGRA8 surface instead.

**The pointer** is not in the screen surface: it is overlaid, as display hardware would. Op 4 gives its image, op 5
shows or hides it, and its position is the u32 at display page +0xe00 (x low, y high, `0xffffffff` none); a write to
gfx +0x1220 says it moved (`hv_pvg_metal_cursor_moved`).

## 2. Serialized descriptors

Render, tile and compute pipelines arrive as Metal's own serialization (`-[_MTLDevice
serializeRenderPipelineDescriptor:]`, read back on Apple platforms with `newRenderPipelineDescriptorWithSerializedData:
deserializationContext:`). The object entry is `{u32 tag, u32 size, u32 ref, u32 length}` then `length` bytes.

The format was worked out by serializing descriptors that differ in one property on a Mac (macOS 27's Metal) and
checked against the guest's own data (section 5). It is a tree of *objects*:

```
object  := u8 count, count x { u8 key, u8 size, value (size bytes, little-endian) }, then the payloads
```

A scalar field's value is the value. A string, an object or an array is stored after the field table and its value
is its offset from the start of the object that holds the field. Strings end with a NUL. An array is `u32 count,
count x u32 offset` (each from the array's start), then its elements, which are objects. Payloads after a field
table start at the next multiple of 4 when they hold arrays. Fields at their default value are left out.

Render pipeline (`MTLRenderPipelineDescriptor`) keys:

| Key | Field | Key | Field |
| --- | --- | --- | --- |
| 0x00 | label (string) | 0x0c | tessellation partition mode |
| 0x01 | vertex function (function ref) | 0x0d | max tessellation factor |
| 0x02 | fragment function (function ref) | 0x0e | tessellation factor scale enabled |
| 0x03 | vertex descriptor (object) | 0x10 | control point index type |
| 0x04 | raster sample count | 0x11 | tessellation factor step function |
| 0x05 | alpha to coverage | 0x12 | output winding order |
| 0x06 | alpha to one | 0x15 | vertex buffers (array: 0 index, 1 mutability) |
| 0x07 | rasterization enabled (default 1) | 0x16 | fragment buffers (array, as 0x15) |
| 0x08 | color attachments (array) | 0x19 | supports indirect command buffers |
| 0x09 | depth attachment pixel format | 0x1a | max vertex amplification count |
| 0x0a | stencil attachment pixel format | 0x0b | input primitive topology |

Color attachment: 0 index, 1 pixel format, 2 blending enabled, 3 source RGB factor, 4 destination RGB factor, 5 RGB
operation, 6 source alpha factor, 7 destination alpha factor, 8 alpha operation, 9 write mask (default 0xf). Values
are Metal's enums.

Vertex descriptor: 0 attributes (array: 0 index, 1 format, 2 offset, 3 buffer index), 1 layouts (array: 0 buffer
index, 1 step function, 2 step rate, 3 stride). Only layouts an attribute uses are written.

Compute pipeline (`MTLComputePipelineDescriptor`): 0 compute function, 1 thread group size is a multiple of the
execution width, 2 label, 3 stage input descriptor (as the vertex descriptor, plus 2 index type, 3 index buffer
index), 6 buffers (array, as 0x15), 7 supports indirect command buffers, 8 max total threads per threadgroup.

Textures (descriptions v1 and v2), samplers (tag 3) and depth-stencil states (tag 4) use the fixed layouts in
`pvg_metal.m` (`descriptor_v1`, `descriptor_v2`, `makeSerialized:`); they need no parser of their own.

## 3. Shaders

The guest sends shaders as metallib files: a header, a function list, and per function an AIR module, which is
LLVM bitcode with Metal's intrinsics (`air.*`) and metadata (`!air.vertex`, `!air.fragment`, `!air.kernel`, argument
descriptions). Vulkan wants SPIR-V.

**Route: AIR → NIR with Stela's compiler, NIR → SPIR-V with Mesa's emitter, SPIR-V to the system Vulkan driver.**

- [Stela](https://github.com/tylertms/Stela) (GPL-2.0-or-later) has a complete AIR to NIR compiler in C
  (`crates/stela-driver-gpu/src/compiler`, about 32,000 lines): it reads the bitcode with LLVM's C API, handles
  Metal's texture, atomic, SIMD-group, matrix, tessellation and indirect-command semantics, and lowers resources to
  a binding ABI (`metal_abi.h`) that suits Vulkan: push constants hold the address of a per-stage argument table;
  buffers are 64-bit device addresses in that table (`load_global`); textures and samplers are indices into runtime
  arrays of descriptors in one set (`nir_var_image`/`nir_var_uniform` derefs). Metal's argument buffers hold the
  same addresses and indices, which is why query 461 answers with the Vulkan device address and 462 with the
  descriptor index.
- It needs Mesa's NIR at the commit Stela pins (Mesa 26.2.5, `ddaf85e3`) plus three small generic NIR changes from
  Stela's patch series (0004 `nir_lower_goto_ifs` irreducible routing, 0005 `nir_lower_bit_size` widening, 0017
  `nir_opt_access`). The other 23 patches are NVIDIA driver changes and are not needed.
- Mesa 26.2's zink emitter (`src/gallium/drivers/zink/nir_to_spirv`, about 7,700 lines) takes an `ntv_info` and has
  an `is_native_vulkan` mode that leaves out zink's own conventions; it supports `load_global`/`store_global` through
  `PhysicalStorageBuffer64`, push constants, and descriptor arrays.

Why not the alternatives:

- *Bundle Mesa's Turnip driver and give it NIR directly*, as Stela gives NIR to NVK. That removes the SPIR-V step,
  but needs Turnip on the tablet's KGSL kernel driver from an unrooted app, its own presentation path (no system
  swapchain for a driver loaded outside the loader), and cannot be tested at all before the tablet arrives. Kept as
  the fallback if Qualcomm's SPIR-V compiler proves the problem.
- *Parse the bitcode and emit SPIR-V directly*, or use LLVM's SPIR-V backend: both mean redoing what Stela's 32,000
  lines already do, against a less mature target. SPIR-V also needs structured control flow, which NIR's
  `nir_lower_goto_ifs` already provides.

Cost for the app: LLVM's bitcode reader, IR, passes and support libraries for `arm64-v8a`, built with the NDK (no
LLVM backends), plus Mesa's `src/util` and `src/compiler/nir`, plus Stela's compiler and the emitter. Estimate:
25 to 35 MB of native code before stripping unused passes; to be measured (section 7).

Compiling is slow (LLVM then NIR then the vendor's SPIR-V compiler). Pipelines are kept in memory for each object;
a `VkPipelineCache` is used during the session. There is no persistent shader or pipeline cache yet.

## 4. The Vulkan backend

`devices/pvg_vulkan.c` keeps `pvg_metal.m`'s structure: a worker thread takes commands in order, objects are made
lazily per task and ref, and a command's stamp is completed after its GPU work finishes. The worker waits on a Vulkan fence after each
exec; there is no overlap or separate completion thread yet. The interface is `pvg_lite.h`'s `hv_pvg_metal_*`; the engine links one backend, so the
names stay until every backend has moved to the neutral ones (section 8).

Memory: on Apple platforms Metal wraps guest pages without copying (`vm_remap` then `newBufferWithBytesNoCopy`). On
Android the first version copies: a buffer or surface is uploaded when the guest says it wrote it (resource flag
bit 0, op 0x34) or its pages' hash changed, and written back after an exec that wrote it (flag bit 1) or when the
guest asks (op 0x35/0x3e). `VK_EXT_external_memory_host` over a memfd view of guest RAM would remove the copies, if
the tablet's driver supports it (section 9).

Presentation is outside this milestone. Swap commands select the display surface; the replay reads the surface
back into guest memory and writes its composed image to a PNG. `hv_pvg_metal_set_layer` rejects a non-null layer.
No app or engine build is wired to this backend, and it does not create an Android window or swapchain.

## 5. Testing without the tablet

**Capture.** `HV_PVG_CAPTURE=PREFIX` (`devices/pvg_vulkan_capture.h`) makes `pvg_lite.c` note every guest page
that the device or its backend reads. While `PREFIX.on` exists it records `PREFIX-N.pvgcap`: the device state, the
display state and every noted page, then for each command the pages read for the first time and those that changed,
the command, and the pages that changed while it ran (flagged as the GPU's), each command run to completion before
the next. After a swap it records the shown surface's pixels. `HV_PVG_CAPTURE_FRAMES` (default 30) ends a
recording after that many swaps. Starting a recording first runs `hv_pvg_lite_quiesce`, so what only the GPU held is
in guest memory, as for instant resume. The guest runs much slower while recording.

**Replay.** `android/gfx/replay/pvg_replay.c` runs `pvg_lite.c` on a stand-in machine whose memory comes from the
recording and gives each command to a backend: `pvg_replay_metal` (Metal; checks the rig) or `pvg_replay_vulkan`
(MoltenVK on the Mac). By default the GPU-written pages are applied after each command, and the count of those that
differ from what the replay's backend wrote is reported; `--free` keeps the backend's own results instead, so each
frame is the backend's work from the recording's start. At each swap the shown surface is compared with the recorded
one (8-bit, tolerance 2, 99.9% of pixels); `--out DIR` writes recorded, replayed and difference PNGs.

```sh
make -f android/gfx/replay/Makefile              # android/gfx/build/mac/pvg_replay_metal
android/gfx/build/mac/pvg_replay_metal --free --out /tmp/r ~/.menhir/captures/setup-0.pvgcap
```

Recording on the Mac: build `hengevm`, make a machine under `~/.menhir/machines/` (`hengevm --install`, about 10
minutes for macOS 15.6.1 on an M3), then boot it with `--phone` (Henge's own device and Metal backend, as on iOS):

```sh
HV_PVG_CAPTURE=$HOME/.menhir/captures/setup HV_PVG_CAPTURE_FRAMES=12 \
  build/hengevm --phone --rom AVPBooter.vmapple2.bin --disk disk.img --aux auxiliary.bin --ecid N --ram 4096 --cpus 4
touch ~/.menhir/captures/setup.on     # records 12 swaps, then stops
```

MoltenVK is a fair check of the backend's Vulkan and of the SPIR-V (it goes through SPIRV-Cross to Metal), not of
Adreno's compiler or driver. The Android emulator's Vulkan is MoltenVK underneath on this Mac, so it adds nothing for
graphics; the tablet is the only test of the driver.

## 6. Coverage

Status: **done** works and is checked by a replay; **partial** works for the cases named; **no** not yet.

| Area | Metal backend | Vulkan backend |
| --- | --- | --- |
| Exec, object model, releases, task end | done | partial: ordered worker, synchronous execs; no general validation of malformed streams |
| Buffers, heaps, heap buffers | done | partial: copied host-visible buffers; no texture heap aliasing |
| Textures: private, in task memory, views, over buffers, in heaps | done | partial: uncompressed uploads, views and buffer textures; heap textures use separate images |
| Surfaces: single plane | done | partial: macOS 15 BGRA and RGhA |
| Surfaces: multi-plane video (420v, x420, b3a8) | done | no |
| Render pass, attachments, load/store, resolve | done | partial: dynamic rendering; tested single-sample color/stencil paths, resolves unverified |
| Draws: plain, indexed, instanced, indirect | done | partial: decoder implemented, only the recording's draws checked |
| Tessellation draws | done | no |
| Tile shaders, imageblocks | done | no |
| Compute dispatch, threadgroup memory | done | partial: direct dispatch; not exercised by the first-frame check |
| Blits: copies, fill, mipmaps | done | partial: basic copies, aligned fill and mip generation; not all formats or subresources |
| Fences, barriers, useResource/useHeap | done | partial: ordered passes and execs; residency and fence records are ignored |
| Queries 450..469 | done | partial: basic limits, sizes, addresses and IDs; pipeline IDs do not support indirect execution |
| Samplers, depth-stencil states | done | partial: state decoder, runtime pixel-coordinate samplers; per-binding LOD clamps ignored |
| Render, tile, compute pipelines | done (Metal SPI) | partial: render and compute; no tile pipelines |
| Shaders | done (Metal) | partial: recording's AIR through Stela and Mesa; runtime sampler flags enabled |
| Swap, display, pointer | done | partial: off-screen display surface readback; no window or pointer overlay |
| Instant resume (`quiesce`, display state) | done | no: display metadata only; private GPU resources are not saved |

Unsupported or incomplete command paths include tessellation render tags 12–15 and 18–19; tile tags 155, 157,
161, 162 and 164; indirect compute tag 201; depth-clip and fill state 109/124; and per-binding sampler LOD clamps
113/128. Tag 154's vertex amplification mapping is ignored. Synchronize/invalidate ops 0x34/0x35/0x3e rely on
exec-end copies, which do not implement general guest-backed texture writeback. Compressed texture uploads,
framebuffer fetch, advanced queries and macOS 14's compact surface descriptors remain incomplete. Anything not
exercised by the recording is unverified even where the decoder has a case for it.

## 7. Status

The inherited capture rig, descriptor parser and AIR compiler are committed. The off-screen Vulkan backend and
Android static library now build independently of the app and engine. This milestone uses the existing
`~/.menhir/captures/setup-0.pvgcap` (796 MiB, Setup Assistant's Language screen); no new recording was made.
The recording is version 1 and does not identify the CPU count. Replay uses the existing page-writer heuristic:
a page the backend did not change may be treated as a guest write. Thus free-running replay is useful image
regression evidence, but not proof that every GPU-written page was attributed correctly.

First-frame invocation (17 commands, 1920×1080 RGhA; default tolerance 2/255, at least 99.9% of pixels):

```sh
make -f android/gfx/replay/Makefile vulkan
android/gfx/build/mac/pvg_replay_vulkan --free --frames 1 --out /tmp/menhir-gfx-final-mac \
  ~/.menhir/captures/setup-0.pvgcap
```

`--frames N` stops after N swaps; without it the whole recording is replayed. Output is line buffered, and cold
shader compilation may take up to 120 seconds before a command is considered timed out.

The Metal baseline (`pvg_replay_metal --free`) matches all 12 recorded frames exactly. The Mac Vulkan first frame
passes: 0.0096% of pixels differ by more than 2, worst channel difference 236, PSNR 43.9 dB, zero timeouts. This is
a tolerance match, not byte equality; small corner-edge differences remain. PNGs include the recorded frame,
replayed frame and a thresholded red mask.
Only the first frame is accepted for this milestone; broader replay coverage remains future work.

For Android, first build the pinned dependencies with `android/gfx/mesa/build.sh android` and
`android/gfx/llvm/build.sh`, then:

```sh
SDK=/opt/homebrew/share/android-commandlinetools
$SDK/cmake/3.31.6/bin/cmake -S android/gfx -B android/gfx/build/android -G Ninja \
  -DCMAKE_MAKE_PROGRAM=$SDK/cmake/3.31.6/bin/ninja \
  -DCMAKE_TOOLCHAIN_FILE=$SDK/ndk/29.0.14206865/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-34 -DCMAKE_BUILD_TYPE=Release
$SDK/cmake/3.31.6/bin/cmake --build android/gfx/build/android -j4
```

This builds `libmenhir_gfx.a` and `pvg_replay_vulkan`. The CMake target carries the static Mesa and LLVM dependency
archives through its link interface. Strip the replay binary with the NDK's `llvm-strip` before copying it.
Run `adb devices` to select the tablet's current serial, then pass `adb -s "$SERIAL"` for every device operation.
Use only `/data/local/tmp/menhir-gfx/` for the binary, recording and PNGs. The separate installation under
`/data/local/tmp/menhir/` is unrelated and must be left alone.

The tablet first frame also passes: 0.0096% of pixels differ by more than 2, worst difference 236, PSNR 43.9 dB,
zero timeouts. Both hosts use the Vulkan memory model. The Adreno 750 reports Vulkan 1.3.128.
`make B=build-gfx` and `make B=build-gfx test` pass on the Mac. No app, engine build wiring or files outside
the graphics ownership paths were changed. The tablet replay files are removed after PNGs are pulled to the Mac.

The compiler uses word-sized physical buffer loads on Adreno; narrow loads caused pipeline rejection (-13).
The lowering and emitter preserve the original access alignment: forcing word alignment produced a separate
last-halfword access that read the globe tint alpha as zero on Adreno. Preserving alignment restores alpha to one.
Runtime sampler flags are needed to normalize Metal's pixel coordinates before Vulkan sampling. Without those
flags the Language screen loses text and its rounded window edges. SPIR-V can be dumped with
`HV_PVG_SPIRV_DIR=DIR` (create DIR first) for validation and driver diagnosis.

## 8. Interface changes

None yet. The engine links `hv_pvg_metal_*` from whichever backend it is built with (`pvg_metal.m` on Apple,
`pvg_null.c` headless, `pvg_vulkan.c` on Android).

## 9. To check on the tablet

- `adb shell getprop ro.hardware.vulkan`, `dumpsys SurfaceFlinger`, and `vulkaninfo` (or the app's own log of
  `VkPhysicalDeviceProperties`): driver version, Vulkan 1.3, and these features the backend relies on:
  `bufferDeviceAddress`, `descriptorIndexing` (runtime arrays, update-after-bind, partially bound),
  `timelineSemaphore`, `shaderInt64`, `shaderFloat16`, `storageBuffer8BitAccess`, `VK_KHR_swapchain`,
  `VK_EXT_external_memory_host` (optional).
- The swapchain's formats and present modes on ColorOS, and whether `RGBA16F` scanout is offered.
- Adreno's SPIR-V compiler: compile every shader of a capture (`pvg_replay_vulkan --compile-only` on the tablet).
