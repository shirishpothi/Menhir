# The shader compiler

Metal functions (AIR, from the metallibs the guest sends) to SPIR-V for the Vulkan backend. The route and why it
was chosen are in `docs/android/graphics.md`, section 3.

| Path | What | From |
| --- | --- | --- |
| `air_library.c` | a metallib's function list and each function's bitcode | Menhir |
| `air_spirv.c` | AIR → NIR (Stela) → lowering for the emitter → SPIR-V | Menhir |
| `stela/` | Stela's AIR to NIR compiler, GPL-2.0-or-later | [Stela](https://github.com/tylertms/Stela) `crates/stela-driver-gpu/src/compiler` at `25f661c`, with the changes marked `Menhir:`; `LICENSE` and `NOTICE` are Stela's |
| `ntv/` | Mesa's NIR to SPIR-V emitter, MIT | Mesa 26.2.5 (`ddaf85e3`) `src/gallium/drivers/zink/nir_to_spirv`, with the changes marked `Menhir:` |

Changes to `stela/`, each marked in the source:

- `air_metadata.c` accepts the module flag `air.workaround.sampler_state_variable_length_encoding`, which macOS
  15.6's shaders carry (Stela was written against macOS 27.2's). What it changes in the constexpr sampler encoding
  is not known; a shader that uses one should be checked against Metal by replay.

Changes to `ntv/`, each marked in the source:

- In `is_native_vulkan` mode every stage uses the `PhysicalStorageBuffer64` addressing model: Stela's buffers are
  device addresses in all stages, not only compute.
- Runtime (unsized) arrays of images and textures, as samplers already had: Stela's descriptor heaps.
- Array derefs of push constant variables, and separate textures typed as `emit_image` types them.
- Physical buffer loads retain NIR's proven alignment, including packed halfword fields. Forcing word alignment
  made the Adreno replay read vertex tint alpha incorrectly.

The compiler needs Mesa's NIR at the same commit with four patches (`android/gfx/mesa/patches`: three generic NIR
changes from Stela's series, and a build change so NIR builds without a driver), built by
`android/gfx/mesa/build.sh`, and LLVM 23's C API (Homebrew's `llvm` on the Mac, `android/gfx/llvm/build.sh` for
Android): Stela checks the major version at run time.

`android/gfx/tools/aircc.c` compiles a metallib's functions with all of this, for checking on the Mac and on the
device.
