# Android engine

The NDK 29 build runs Henge's arm64 JIT on Android without the iOS JIT bootstrap.
It builds `libmenhir-engine.a`, a headless `hengevm`, the nine upstream engine
suites and an Android platform test. macOS keeps its existing backend.

## Build and test

```
android/engine/build.sh
android/engine/run-tests.sh emulator-5556
```

The scripts use SDK `/opt/homebrew/share/android-commandlinetools` unless
`ANDROID_HOME` overrides it, NDK 29.0.14206865, CMake 3.31.6, arm64-v8a and API 34.
Output goes to `android/engine/build/`. The runner accepts an adb serial, or uses
`ANDROID_SERIAL`; automatic selection requires exactly one device. It removes
its temporary device files. It does not create or stop emulators.

For an emulator, create AVD `menhir-engine` from
`system-images;android-36;google_apis;arm64-v8a`. On this Mac, avdmanager stores
AVDs in `~/.config/.android/avd`; set `ANDROID_AVD_HOME` to that directory when
launching the emulator. Use `adb -s SERIAL` for every operation.

The 16 KB test used AVD `menhir-engine-16k` with
`system-images;android-35;google_apis_ps16k;arm64-v8a`. Check with
`adb -s SERIAL shell getconf PAGE_SIZE`, then stop the emulator after testing.
Android documents these images in its [page-size guide](https://developer.android.com/guide/practices/page-sizes).

Mac validation on this machine needs libslirp installed and the SDK's CMake on
PATH:

```
PATH=/opt/homebrew/share/android-commandlinetools/cmake/3.31.6/bin:$PATH make B=build-engine test
```

`make test` also builds all normal Mac executables. The restore build now honors
`B` through `HENGE_RESTORE_BUILD` rather than silently writing to `build/`.

## Host services and graphics

`platform.h` supplies a 24 MHz counter from CLOCK_MONOTONIC, timed condition
waits, and private futex compare-and-wait/wake for CPU acknowledgements.
Darwin keeps mach time and ulock. Android counter reads use the sysreg helper;
Apple's commpage is never accessed on Android. JIT memory uses anonymous RWX
mappings. Cache publication uses the compiler's full instruction-cache routine,
including data-cache maintenance, for blocks and patched branches.

The headless build uses `pvg_null.c`: every valid submitted command retires its
stamp, but frame and snapshot calls report no image. AES uses the vendored
mbedTLS implementation on Android, and CommonCrypto on Apple platforms.

`MENHIR_WITH_NET`, `MENHIR_WITH_RESTORE`, and `MENHIR_WITH_AUDIO` default off.
Enabling them links the services targets from `android/deps`; the small CLI does
not attach those services. The Kotlin app must attach the services it uses.
Filesystem flush and balloon hole punching call the services owner's
`compat_android.c`. The Android library does not compile the Darwin UI/session
wrapper `host/vm.c`, Mac profiling, or `host/main.c`.

The CLI accepts `hengevm BOOT_ROM [--installer]`; it creates a single-CPU machine
with 512 MB RAM and no disks. It installs the Linux signal adapter around the CPU
run. App callers can use `hv_jit_signals_install/remove` once per process around
a session, with CPUs stopped before removal. The adapter reads Linux arm64
`uc_mcontext.pc`, writes a fixed diagnostic for a JIT fault, and calls the prior
SIGILL/SIGBUS/SIGSEGV handler with the original context. Default dispositions are
restored so synchronous faults are redelivered. Prior masks, alternate-stack and
nodefer flags are retained. ART recovery on a live Kotlin session is unverified;
the CLI test checks chaining to a prior user handler.

## Page sizes

Guest MMU granules and the software TLB remain independent of the host kernel.
`core/mmu.c` walks guest descriptors in software. RAM region pointers and TLB
addends use byte offsets; they do not create a host mapping for each guest page.
RAM, ROM and JIT use mmap with zero file offsets, and munmap accepts lengths the
kernel rounds to its page size. No core mprotect operation assumes 16 KB.
The supported host sizes are checked at machine startup: 4 KB and 16 KB.

The balloon intentionally retains 16 KB reclamation groups. These align on both
supported hosts and avoid releasing live quarters of a guest page. Android uses
MADV_DONTNEED or filesystem hole punching; it does not use Darwin's reusable-page
advice. The prefault worker's 16 KB stride is only a performance hint on 4 KB
hosts. It does not affect correctness.

The services owner updated snapshot mincore handling to inspect every host page
within a stored 16 KB guest page. The engine build does not yet include snapshot
or hvsparse; Android snapshot save/restore remains untested.

Tests use host pointers as guest virtual addresses. Their executables disable
Android heap tags before allocating, because those tags are not guest 48-bit
addresses. The library leaves host heap tagging enabled. ELF test fixtures are
generated in the build directory, preserving upstream Mach-O sources; native
reference execution saves/restores host x18 before returning to C. Those native
reference fixtures deliberately use x18 and are not production translated code.

## Registers and CPU features

Guest x6, x7, x15, x18 and x28 live in HvCpu. Operand rewriting replaces every
recognized use of a reserved guest GPR. The entry/helper assembly never loads
guest x18 into host x18. TPIDR_EL0, TPIDRRO_EL0 and TPIDR_EL1 reads/writes are
translated to CPU-context accesses, including inline paths; no guest thread
pointer write is copied into host code. Android C objects reserve x18.
The platform test executes guest x18/TPIDR_EL0 writes followed by a PSCI C helper
and verifies both host values survive. Upstream instruction and memory tests
exercise reserved registers through arithmetic, branches and memory operations.

Guest ID registers keep the upstream feature set. Startup checks AT_HWCAP and
AT_HWCAP2 for the copied instruction families: FP/ASIMD and FP16, AES/PMULL,
SHA1/2/512/3, CRC32, LSE, RDM, JSCVT, FCMA, LRCPC/ILRCPC, DPB, DotProd, FHM,
FlagM, SB and FRINTTS. Missing features reject initialization rather than causing
a later guest-triggered host SIGILL. This follows the kernel's
[arm64 hwcap contract](https://www.kernel.org/doc/html/latest/arch/arm64/elf_hwcaps.html).

PAC operations are stripped or modeled as upstream defines; host PAC keys are
never guest keys. Guest IDs do not expose SVE or SME, and classification rejects
those instruction groups. Apple implementation-defined system registers are
handled in software. This is not a complete instruction emulator: unadvertised
extensions and unknown SIMD encodings are not validated exhaustively. The
Snapdragon hardware feature check and long-running guest workloads still need
the real tablet. Its CPU, kernel page size, display and RAM must be measured
before changing AGENTS.md's device facts.

## Validation (2026-11-13)

Both 4 KB Android 16 and 16 KB Android 15 arm64 emulators passed all nine upstream
suites: classify 43 cases; operands 36; both JIT-region modes returned 42;
JIT 10 programs x 2 seeds with 8 small-region flushes; memory 12 programs x 4
modes plus MMIO; MMU 117 checks; exceptions 52; devices 135; shutdown 0 failures.
The Android platform check covers counter units, prior signal chaining and host
x18/TPIDR_EL0 preservation. Mac `make B=build-engine test` passed all nine suites.
The device test's deliberately invalid bdif/virtio requests print failure messages
as expected; the suite still reports zero failed checks.

The services ROM fetch obtained 214904 bytes with SHA-256
`fd4973e42e609751830df6f3dd2d2b2ad2309fe38f0ca0f5aba02dcbefe45f91`.
The ROM is cached at `~/.menhir/cache/AVPBooter.vmapple2.bin`, outside Git.
On the 16 KB emulator it ran to a clean halt at PC 0x104518 without disks;
installer mode waited until the 20-second limit. Neither attempt produced serial
output. A macOS boot, restore, Vulkan rendering, real ART fault recovery and
physical tablet acceptance are not established by these tests.
