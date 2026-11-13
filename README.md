# Menhir

An unfinished attempt to run macOS on an Android tablet, built as a port of
[Henge](https://github.com/Leviidev/Henge), which runs macOS on iPhone and iPad.

**This project is abandoned.** It never booted macOS on Android, and nothing here is usable as
an app. The code is published as-is in case it helps someone continue the work. There are no
releases meant for use, no support, and no plans to continue.

The only device it ran on was an OPPO Pad with an Adreno 750 GPU.

## Where it got to

As of 2026-11-14. "Verified" means it was run and seen working; everything else was only built.

### Verified

- **The engine runs on Android.** Henge's own arm64 JIT, MMU and device models (no QEMU, no
  hypervisor) build with the NDK and run on the tablet as a command-line program, `hengevm`.
  The engine's tests pass on the tablet.
- **A full macOS install completed on the tablet.** On 2026-11-14 a command-line run restored
  macOS Sequoia 15.6.1 onto a blank 48 GB disk image in about 18 minutes, ending with
  `Restore Finished`. It used Apple's restore image, downloaded on the tablet, and Apple's
  signing server, over the emulated Mac's virtual USB port. Nothing was decrypted or patched.
- **The install stall was fixed.** Earlier installs always stopped at 71% of the 5.2 GB system
  image. The cause was a race in the virtual libusb shim: two event loops shared one completion
  queue, so USB mux packets reached the guest out of order and it stopped acknowledging data.
  Commits `72c7ce0` and `22585b3` fix it, and the completed install ran with that fix.
- **Graphics replay on the tablet's GPU.** A Vulkan backend replays a recording of macOS's
  virtual GPU commands (Setup Assistant's first screen). The first frame matches Apple's Metal
  output within tolerance (PSNR 43.9 dB) on both the Mac and the tablet, and an earlier
  version drew it in a real window on the tablet.
- **Host services**, on an Android emulator: networking (libslirp), TLS to Apple's signing
  host, resumable downloads from Apple's CDN, audio output (AAudio) and boot-ROM extraction.
- **The app shell.** A Kotlin app (`android/app`) builds, passes lint and its unit tests,
  installs on the tablet, and shows its home screen. It has an in-app updater that installs new
  builds from GitHub Releases.

### Not done or never tested

- **macOS has never booted on Android.** The completed install was never started. Whether the
  installed disk boots, reaches the login screen, or runs anything is unknown.
- **The app has never run the real engine.** The app's real-engine build compiles and links,
  but every run of the app with a working Mac session used the fake engine (a simulated Mac
  that draws a test pattern). The successful install was done from the command line, not from
  the app, and the app cannot see that disk.
- **No live picture.** The Vulkan backend has only replayed a recording. It has never drawn a
  running macOS, and the app's session screen has never shown macOS output.
- **Input is untested** against a real guest. Touch-as-trackpad, keyboard, mouse and the
  volume-key buttons were only partly checked against the fake engine.
- **Untested on Android:** snapshots and instant resume on 4 KB pages, pausing with a GPU, the
  clipboard helper, the shared folder, guest networking end to end, and audio from a guest.
- **Performance is poor and largely unmeasured.** The install ran on one emulated CPU and made
  the tablet lag badly. It used up to about 3.2 GB of RAM, and starting a second engine from
  the app at the same time pushed the tablet into memory thrashing.

## What would be needed to continue

1. Boot the installed disk from the command line (`hengevm` without `--install`, with the same
   ECID, disk and aux file) and capture the serial log. This is the next real unknown.
2. If it boots, connect the Vulkan backend's window output to a live run, then to the app's
   session screen.
3. Let the app import a disk installed from the command line, or run an install end to end
   from the app.
4. Test input, multiple CPUs and memory use on the tablet, then snapshots and sharing.
5. Expect a lot of performance work: Henge's engine was tuned for Apple silicon and iOS.

## Layout

The Android work sits beside Henge's original code:

| Path | What it is |
| --- | --- |
| `android/engine/` | NDK build of the engine and the `hengevm` CLI, with device tests |
| `android/deps/` | NDK builds of the restore stack, libslirp, TLS, HTTP and audio |
| `android/gfx/` | Vulkan backend for macOS's virtual GPU, shader compiler, replay tools |
| `android/app/`, `android/native/` | Kotlin app and its JNI layer, with a fake engine for UI work |
| `docs/android/` | Detailed notes: [engine](docs/android/engine.md), [graphics](docs/android/graphics.md), [services](docs/android/services.md), [app](docs/android/app.md) |

The rest of the tree (`core/`, `devices/`, `host/`, `ios/`, `third_party/`) comes from Henge,
with small changes for the port. See [docs/architecture.md](docs/architecture.md) for how the
emulator works.

## Building

On a Mac with the Android SDK at `/opt/homebrew/share/android-commandlinetools`, NDK 29 and
CMake 3.31:

```sh
android/deps/fetch-sources.sh
android/engine/build.sh                            # hengevm for the tablet
cd android/app
./gradlew assembleDebug                            # app with the fake engine
./gradlew -PmenhirFakeEngine=false assembleDebug   # app with the real engine (never run)
```

The real-engine and graphics builds also need the Mesa and LLVM builds described in
[docs/android/graphics.md](docs/android/graphics.md). The command-line install procedure is in
[docs/android/engine.md](docs/android/engine.md).

## Credit and licence

Almost all of the emulator is [Henge](https://github.com/Leviidev/Henge) by Leviidev; Menhir
adds the Android port. Most of the Android code was written by AI coding agents (Claude and
Codex) under one person's direction.

GPL-2.0-or-later, the same as Henge. See [LICENSE](LICENSE) and
[docs/licensing.md](docs/licensing.md). macOS and the Mac boot ROM belong to Apple and are not
part of this repository; the install downloads them from Apple.
