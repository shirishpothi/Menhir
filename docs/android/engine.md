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

`MENHIR_WITH_NET`, `MENHIR_WITH_RESTORE`, and `MENHIR_WITH_AUDIO` default on.
The headless CLI links the real services targets, including the USB host transport,
Android resolver library, and `menhir::compat`. It attaches networking and audio,
polls networking on CPU 0, and starts restore with `--install IPSW`. Set
`HV_NO_NET=1` or `HV_NO_SOUND=1` to disable those services for a reference run.

The CLI accepts `--rom`, `--disk`, `--aux`, `--aux-offset`, `--ecid`, `--seed`,
`--ram` (MiB), `--cpus`, `--log`, `--trace`, `--stats`, `--no-chain`, and
`--install IPSW`. `--installer` enters DFU without starting restore. The old
positional ROM argument still works. Disks must already exist. CPU workers have
8 MiB stacks, alternate signal stacks and blocked asynchronous signals; a 1 ms
kicker lets chained code return for device work. `HV_VUSB_PROBE=1` performs a
bounded USB reset and device-descriptor request, then stops the machine.

`menhir-fetch rom DEST` verifies and reuses a cached ROM.
`menhir-fetch image URL DEST` downloads or resumes the pinned image into
`DEST.part`, then renames it. It does not print intermediate progress; inspect
the partial file size while it runs. Both tools are in `android/engine/build/`.

Disk sync and balloon punching already call the Android compatibility helpers.
Sparse packing now scans data if extent queries are unsupported, and reports
other seek errors instead of silently treating them as end-of-data. Snapshot
code compiles on Android with the services LZ4 codec and sequential chunk I/O;
Mac keeps its dispatch workers. Android reads all anonymous pages because Linux
mincore cannot distinguish untouched pages from swapped-out data. Snapshot
save/restore is not wired into this CLI and has not been exercised on Android.

The Linux signal adapter uses arm64 `uc_mcontext.pc` and chains the prior
SIGILL/SIGBUS/SIGSEGV handler. ART recovery in a live Kotlin session remains
unverified. The platform test checks chaining to a prior user handler.

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
within a stored 16 KB guest page. The engine archive includes snapshot
and hvsparse; Android snapshot save/restore remains untested.

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

## Tablet integration, 2026-11-13

The real device serial was `adb-e8074ab2-kiNpL3._adb-tls-connect._tcp`.
After enabling all services, all nine upstream suites and the platform suite
passed on its 4 KiB host pages. Mac `make B=build-engine` and
`make B=build-engine test` also passed. Existing Mac warnings concern an unused
profiling variable and Homebrew libraries built for a newer macOS target.

A bare ROM is silent on both hosts. With blank storage, ECID 123456789,
6 GiB RAM and one CPU, Mac requested restart after 2,946 blocks and Android
after 2,955. Both stopped at PC `0x104518`, ESR `0x56000000`, ELR `0x1044f0`.
The small block-count difference has not been traced instruction by instruction.
DFU is also silent until a USB host sends requests; it is not a serial boot log.
Both hosts returned this descriptor after a successful reset:

```
12 01 00 02 00 00 00 40 ac 05 27 12 00 00 02 03 04 01
```

Mac additionally read `Apple Mobile Device (DFU Mode)` and
`SRTG:[iBoot-10151.140.19]`. Android's bounded probe stopped after 3,068 blocks.
An idle Android DFU run used 88,100 KiB RSS (43 threads, 6 GiB guest RAM).
These observations do not establish an installed kernel boot.

Device test files now stay under `/data/local/tmp/menhir/engine-tests/` and are
removed by the runner. No app installation or emulator is needed.

Reproduction (the shell commands below are for the tablet, after copying
`hengevm`, `menhir-fetch` and the verified ROM into its work directory):

```
cd /data/local/tmp/menhir
chmod 700 hengevm menhir-fetch
# New storage only: do not truncate an existing installed machine.
truncate -s 48G disk.img
printf '\245\325\263\376\227\255\035\047\001\000\000\000\000\000\000\000\001\000\000\000\000\000\000\000' > aux.bin
truncate -s 33570816 aux.bin
HV_NO_SOUND=1 HV_VUSB_PROBE=1 ./hengevm --rom AVPBooter.vmapple2.bin \
  --disk disk.img --aux aux.bin --ecid 123456789 --ram 6144 --installer
nohup ./menhir-fetch image \
  https://updates.cdn-apple.com/2025SummerFCS/fullrestores/093-10809/CFD6DD38-DAF0-40DA-854F-31AAD1294C6F/UniversalMac_15.6.1_24G90_Restore.ipsw \
  Sequoia_15.6.1_24G90.ipsw > fetch.log 2>&1 < /dev/null &
# Wait for state=1 and the completed file before starting restore.
nohup env HV_NO_SOUND=1 HV_INSTALL_LOG=1 ./hengevm \
  --rom AVPBooter.vmapple2.bin --disk disk.img --aux aux.bin \
  --ecid 123456789 --ram 6144 --cpus 1 --stats 5 \
  --install Sequoia_15.6.1_24G90.ipsw > install.log 2>&1 < /dev/null &
```

The same ECID, disk, aux and default 16,384-byte auxiliary offset must be used
on subsequent boots. Remove `--install ...` to boot the installed disk, and
write that run to `boot.log`. These install/boot commands are the intended
procedure; completion is not yet verified. Monitor `du -sk .` as well as
`/proc/PID/status`, and stop if allocated storage approaches 60 GiB. A sparse
disk's logical length is not its allocated storage.
