# Menhir: macOS on one Android tablet

Menhir is a fork of [Henge](https://github.com/Leviidev/Henge) (remote `upstream`) that runs
macOS on an Android tablet instead of an iPhone or iPad. It is a personal project for one
device, so specialise freely for that device and do not generalise.

Read `README.md` and `docs/architecture.md` first: they describe Henge, and almost all of it
still applies. The other reference is [Stela](https://github.com/tylertms/Stela), which has an
AIR (Metal shader IR) to NIR compiler that the graphics work can borrow from. Both are
GPL-2.0-or-later, as is Menhir.

## Target device

OPPO Pad 3 Pro (`OPD2402`), read from the tablet over adb on 2026-11-13:

- Snapdragon 8 Gen 3 (`SM8650`): 1 Cortex-X4, 5 Cortex-A720, 2 Cortex-A520. Adreno 750.
- `/proc/cpuinfo` features: `fp asimd aes pmull sha1 sha2 crc32 atomics fphp asimdhp
  asimdrdm jscvt fcma lrcpc dcpop sha3 sm3 sm4 asimddp sha512 asimdfhm dit uscat ilrcpc
  flagm sb paca pacg dcpodp flagm2 frint i8mm bf16 dgh bti ecv afp`. No SVE, SVE2 or SME.
- Android 16 (SDK 36), ColorOS build `OPD2402_16.0.5.1301`, not rooted.
- 4 KB pages. 12 GB RAM (11.8 GB visible). About 170 GB free on `/data`.
- Panel 3000 x 2120 in landscape, 420 dpi, running at 120 Hz.
- No `/dev/kvm`; `/dev/gunyah` exists but protected VMs are not supported.
- It connects by wireless debugging, so its adb serial is an mDNS name
  (`adb devices`); always pass `adb -s`.

Consequences:

- Host and guest are both arm64, so Henge's arm64-to-arm64 JIT is the right engine. Android
  gives apps no hypervisor on Snapdragon, the same constraint Henge has on iOS.
- Android apps may map memory writable and executable, so none of Henge's iOS JIT bootstrap
  (StikJIT, pairing, debugger tricks) is needed. Do not port `ios/Henge/JIT/`.
- The host kernel uses 4 KB pages; the guest uses 16 KB. Upstream Henge assumes both are 16 KB.
- There is no Metal. The guest's paravirtual GPU commands must be replayed with Vulkan.
- bionic uses `TPIDR_EL0` for its own thread pointer and reserves `x18`.

## Layout

Upstream directories keep their meaning (`core/`, `devices/`, `host/`, `ios/`, `tests/`,
`third_party/`, `tools/`). Android work goes here:

| Path | Contents | Owner |
| --- | --- | --- |
| `core/`, `host/` | Engine, made portable to Linux and Android arm64 | engine |
| `android/engine/` | CMake build of the engine, `hengevm` for Android, tests | engine |
| `devices/pvg_vulkan*`, `devices/air/`, `android/gfx/` | Vulkan graphics backend and its build | graphics |
| `android/deps/` | NDK builds of the restore stack, libslirp, TLS, HTTP | services |
| `host/*_android.c` | Audio, HTTP, boot ROM fetch and other host services | services |
| `android/app/`, `android/native/` | The Kotlin app and its JNI bridge | app |
| `docs/android/` | One short doc per area, kept current | each owner |

Stay inside your own paths. If you need a change in someone else's, make the smallest one
that works and name it in your final report so the merge is easy.

## Rules

- Keep the Mac build working: `make && make test` must pass after every engine or device
  change. Guard platform code with `#if defined(__APPLE__)` / `__ANDROID__` / `__linux__`,
  or put it in a separate file chosen by the build.
- Do not delete or rewrite the iOS app. Leave `ios/` as it is unless a file must move to be shared.
- No Apple software in the repository: no macOS images, no boot ROM, no IPSW. They are
  downloaded from Apple at install time, as upstream does.
- Shared downloads go in `~/.menhir/cache/` (boot ROM, IPSW). Check there before
  downloading anything large, and write to a `.part` file then rename. Test machines go in
  `~/.menhir/machines/`. Keep total use under 60 GB.
- Toolchain: Android SDK at `/opt/homebrew/share/android-commandlinetools` (`ANDROID_HOME`),
  NDK `ndk/29.0.14206865`, CMake `cmake/3.31.6`, platform `android-36`, build-tools `36.0.0`,
  an `android-36;google_apis;arm64-v8a` emulator image, JDK 17, Xcode 26.3. ABI is
  `arm64-v8a` only. `minSdk` 34.
- This Mac is an M3 with 16 GB, so the Android emulator runs arm64 code natively. It is the
  test device when the tablet is not connected. Other agents share the machine: name your AVD
  after your area, run one emulator at most, and shut it down when you finish
  (`adb emu kill`, `./gradlew --stop`, no stray `qemu-system-*`, `netsimd` or `java`).
- Shell is zsh with BSD tools; GNU ones are `g`-prefixed. Quote globs.
- **This checkout is shared.** Four agents work in this one working tree, on `main`, at the
  same time. So:
  - Never switch branches, stash, reset, rebase, clean, or check out files you did not write.
    Files you do not recognise belong to another agent; leave them alone.
  - Commit small and often. Stage exact paths (`git add path ...`, never `git add -A` or
    `git commit -a`), look at `git diff --cached` first, and commit only your own files.
    If `git` reports `index.lock`, wait a few seconds and retry; do not delete the lock.
  - Do not push or force-push. The orchestrator handles the remote.
  - Use your own build directory: `make B=build-<area>` on the Mac, and
    `android/<your dir>/build/` for CMake and Gradle. Never build into another agent's.
  - A broken build in someone else's paths is theirs to fix. Work around it and report it.
- Verify what you claim. Say what you ran and what it printed. If something is untested or
  does not work, say so plainly in your final report.
- Write docs and comments in plain, direct English, in the style of the upstream docs.

## App versioning and updates

The tablet gets new builds through the in-app updater (`docs/android/app.md`, Updates), as
the owner's app Slate does. These rules come from what Slate learned.

- `versionName` and `versionCode` live in `android/app/build.gradle.kts`.
- `versionName` is semver `MAJOR.MINOR.PATCH`, staying on `0.x`. MINOR only for a
  substantial new capability (macOS first boots, graphics first appear, a new screen).
  PATCH for everything else, including UI rework, tweaks and fixes. When in doubt, PATCH.
- `versionCode` goes up by exactly 1 with every version bump and is never reused. The
  updater only offers a build with a higher `versionCode` than the one installed, and
  `scripts/publish-update.sh` tags the release `v<versionName>`, so a reused name or code
  breaks publishing.
- Bump once per batch of work that will be installed on the tablet, in the commit that
  completes it; not per small commit. Read the current values just before you commit,
  because another agent may have bumped them.
- Only the orchestrator runs `scripts/publish-update.sh`. Never publish from unpushed or
  uncommitted code. Never put a token in the build, the repository or the APK.
- Use `adb install` only when the updater cannot deliver a build (for example one that
  breaks the updater), and say why. Never uninstall the app or clear its data: the
  installed Mac lives in its storage.
