# Menhir: macOS on one Android tablet

Menhir is a fork of [Henge](https://github.com/Leviidev/Henge) (remote `upstream`) that runs
macOS on an Android tablet instead of an iPhone or iPad. It is a personal project for one
device, so specialise freely for that device and do not generalise.

Read `README.md` and `docs/architecture.md` first: they describe Henge, and almost all of it
still applies. The other reference is [Stela](https://github.com/tylertms/Stela), which has an
AIR (Metal shader IR) to NIR compiler that the graphics work can borrow from. Both are
GPL-2.0-or-later, as is Menhir.

## Target device

OPPO Pad 3 Pro, as far as we know: Snapdragon 8 Gen 3, Adreno 750, 12.1 inch 2800 x 2000
panel, 12 or 16 GB RAM, ColorOS on Android 15 or later, not rooted. **This is unverified.**
The tablet has not been connected to this Mac yet. When it is, read the real values with
`adb shell getprop`, `/proc/cpuinfo`, `getconf PAGE_SIZE` and `dumpsys display`, then correct
this section. Until then, do not hard-code anything that depends on these numbers.

Consequences that hold either way:

- Host and guest are both arm64, so Henge's arm64-to-arm64 JIT is the right engine. Android
  gives apps no hypervisor on Snapdragon, the same constraint Henge has on iOS.
- Android apps may map memory writable and executable, so none of Henge's iOS JIT bootstrap
  (StikJIT, pairing, debugger tricks) is needed. Do not port `ios/Henge/JIT/`.
- The host kernel probably uses 4 KB pages; the guest uses 16 KB. Henge assumes both are 16 KB.
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
  test device until the tablet is connected. Other agents share the machine: name your AVD
  after your area, run one emulator at most, and shut it down when you finish
  (`adb emu kill`, `./gradlew --stop`, no stray `qemu-system-*`, `netsimd` or `java`).
- Shell is zsh with BSD tools; GNU ones are `g`-prefixed. Quote globs.
- Commit small and often on your own branch with clear messages. Do not push, force-push,
  or touch `main`; the orchestrator merges.
- Verify what you claim. Say what you ran and what it printed. If something is untested or
  does not work, say so plainly in your final report.
- Write docs and comments in plain, direct English, in the style of the upstream docs.
