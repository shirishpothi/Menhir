# Android app

`android/app/` is the Kotlin app (`app.menhir`), and `android/native/` its native side,
`libmenhir.so`. The app does what `ios/Henge/` does: make a Mac, install macOS on it, run it
full screen, and drive it by touch, keyboard and mouse. It is landscape only, arm64-v8a only,
`minSdk` 34, target and compile SDK 36. There are no settings beyond what this page describes.

## Build and install

```
cd android/app
export ANDROID_HOME=/opt/homebrew/share/android-commandlinetools
./gradlew assembleDebug lint testDebugUnitTest
adb -s SERIAL install -r build/outputs/apk/debug/menhir-debug.apk
```

The default build uses `android/native/fake_vm.c` and `fake_fetch.c` in place of the engine
and the downloader (CMake `MENHIR_FAKE_ENGINE=ON`). It simulates the machine: start, stop,
pause, a restore through the real step numbers in about 20 seconds, a saved Mac as a small
text file, and a moving test pattern with the fingers, pointer, buttons and last key drawn on
it. Every input call is logged under the logcat tag `menhir-fake`. The home screen says
"Test build" when this engine is in use. Nothing is downloaded and no macOS runs.

`./gradlew -PmenhirFakeEngine=false assembleDebug` builds the real variant. It adds
`android/engine` (which adds `android/deps`) as CMake subdirectories and links
`menhir-engine`, `menhir::net`, `menhir::restore`, `menhir::audio`, `menhir::compat` and
`menhir::http` into `libmenhir.so`. Run `android/deps/fetch-sources.sh` once first. The engine
draws through `devices/pvg_null.c` until the Vulkan backend exists, so this variant has no
picture yet. `MENHIR_FAKE_FETCH` follows the engine option. Turn it off on its own to test real
downloads with the fake engine.

The real variant needs `host/vm.c`, which is Darwin-only (CoreFoundation, libdispatch,
`os_unfair_lock`, Mach signal contexts). `android/native/vm_android.c` is that file with those
parts replaced: pthread locks, an `ANativeWindow` display target, the engine's
`host/jit_signal.c` for JIT faults, and a worker thread for the pointer queue and timers. All
other code is unchanged. Keep the two files in step until `host/vm.c` is made portable.

The clipboard helper that runs inside macOS is bundled when it has been built. Run
`android/deps/build-guest-agent.sh`, then copy `build-services/guest/henge-agent.gz` to
`build-app/henge-agent.gz` at the repository root. Gradle copies it into the APK's assets.
Without it, Paste types text and the set-up command installs nothing.

## Storage

Everything is in the app's external files directory, `/sdcard/Android/data/app.menhir/files`.
It survives app updates and can be reached with `adb shell`. Android bind-mounts an app's own
`Android/data` onto the underlying file system, so disk images do not go through FUSE.

| Path | Contents |
| --- | --- |
| `Machines/machines.json` | The list of Macs |
| `Machines/<uuid>/` | `AVPBooter.vmapple2.bin`, `disk.img` (sparse), `auxiliary.bin`, `machine.json` (Henge's export format), `installed`, `resume.hsnap` |
| `Restore images/<build>.ipsw` | Downloads; `.ipsw.part` while in progress |
| `BootROM/AVPBooter.vmapple2.bin` | The boot ROM, fetched from Apple's Sonoma image and hash-checked |
| `Shared/` | The shared folder, also in Android's Files app as "Menhir shared folder" |

## Macs and install

The home screen is built around one Mac: the first in `machines.json`. Any others are ignored.
It follows the system's light or dark theme (`Theme.kt`) and shows one state at a time:

- **Set up** (no Mac, or macOS not installed): memory (4, 6 or 8 GB), CPUs, disk size (48 to
  128 GB) and a macOS version from `Installer.swift`'s list, with defaults chosen, and
  Download and install.
- **Installing**: the step, its progress, bytes and percent, an estimate of the time left
  while downloading, and Stop download.
- **Failed**: the reason in plain words and Try again. A failed install's Mac folder (disk,
  auxiliary storage, restore files) is deleted at once; the downloaded restore image is kept.
  At startup Menhir also deletes any Mac whose install never finished and any folder under
  `Machines/` that no Mac owns. Delete Mac removes the Mac's whole folder.
- **Ready**: the Mac (name, macOS, memory, CPUs, disk used) with Start, or Restart Menhir
  when this process has already run a Mac. More holds Memory and CPUs, Reinstall macOS and
  Delete Mac; the last two ask first.
- **Running or paused**: Resume and Stop, which asks first.

Settings (top right) holds the update token and version, and the downloaded installers. The
update pill sits beside it.

Motion lives in `Motion.kt`: springs, a staggered entrance, a platform splash screen
(`themes.xml`) that MainActivity animates away, content transitions between the states, a
sliding segmented control, and a press dip with a haptic tick on primary buttons. Start grows
the Mac's screen to full black before the session opens, and the session shrinks back into it
when it closes. With Remove animations on, the delays and the launch overlay are skipped.

The install The steps are: fetch the
boot ROM, download the IPSW (it resumes a `.part` file and retries 20 times), make the
machine's files, then run `henge_vm_install`. The restore gets 2 to 4 GB whatever the Mac's
own memory is, as on iOS. Progress comes from `henge_install_step` and
`henge_install_progress`. Stop download keeps the `.part` file, so the next install continues
from there.

The install runs in the app process with a partial wake lock and a foreground service, so it
goes on with the screen off. A Mac whose install failed returns to Set up with its settings
kept. The engine runs one machine per process, and an
install leaves its restore stack attached. Afterwards the app offers Restart Menhir, which
starts a new process.

## Session

The session is a `SurfaceView` covering the whole panel, in immersive mode, with the screen
kept on. The guest display mode is read at run time from the panel's physical pixels
(`Display.Mode`) and density (`DisplayMetrics.xdpi`, `ydpi`). Its long side goes first,
rounded down to a multiple of 8. On the OPPO Pad 3 Pro that is 3000 x 2120 pixels and
250 x 177 mm, about 304 ppi, so macOS draws at 2x. The surface buffer is set to the same size,
so the image fills the panel with no bars. A density outside 90 to 700 ppi gives 0 x 0 mm
(the engine's 1x default).

`surfaceCreated` passes the surface to `henge_vm_set_display_surface`, and `surfaceDestroyed`
passes NULL before it returns. The surface is destroyed whenever the app leaves the screen.

Four fingers tapped together, or Back, show the controls, a bar at the top: Keyboard, Paste,
Type setup command, Home, Stop and Done. Stop asks first. With the keyboard up, a compact key
row floats above it: esc, tab, ⌃ ⌥ ⌘ (lit with a short spring while held for the next key), the arrows and hide.
Every key is at least 48 dp. The session is always dark. When macOS stops, the reason is shown. A
restart that macOS asked for starts the Mac again, as on iOS.

## Input

**Touch.** The screen is a Magic Trackpad 2 (`input/Trackpad.kt`, from `Trackpad.swift`).
Fingers map to sensor units 8134 x 5206 at one scale for both axes, with the panel centred on
the sensor. Each finger gets a stable id from 1 to 5; a sixth is ignored. Each sample is
sent with its own `MotionEvent` time. Every historical sample of a batched move is sent as a
frame of its own (`henge_vm_touch` for each finger, then `henge_vm_touch_frame`). Downs and
ups are reports of their own and get no frame call, as in the engine.

The touchscreen also clicks, as in iOS's Touchscreen mode. A tap under 0.3 s moving less than
8 dp is a left click, and a two-finger tap is a right click. A finger held still for 0.4 s
presses the left button, so moving it drags until it lifts. A second finger turns a drag into
a gesture.

**Volume keys.** Volume up is the left button and volume down the right. Android reports a
real press and release, so the button is down exactly as long as the key, like iOS's
intercept mode. A press is a click, and holding one while a finger moves drags. Auto-repeat is
dropped and the system volume does not change. Touch, volume keys and mouse share the buttons:
a button goes up only when the last of them lets go.

**Hardware keyboard.** Android key codes map to USB HID usages (`input/Keyboard.kt`) and go to
`henge_vm_key_hid`, down and up. Meta (the Windows key) is Command, Alt is Option, and Ctrl is
Control, each left and right. Android's auto-repeat is dropped, because macOS repeats held keys
itself. Fn stays on the Android side and changes the keys pressed with it, as on a Mac: the
arrows become Page Up, Page Down, Home and End, and Delete becomes Forward Delete. Keys are
released when the window loses focus.

Android gives ordinary apps no way to capture system shortcuts. The permission for it,
`OVERRIDE_SYSTEM_KEY_BEHAVIOR_IN_FOCUSED_WINDOW`, is system-only. So Meta+Tab, Meta on its
own, and other ColorOS shortcuts stay Android's.

**On-screen keyboard.** Keyboard shows Android's keyboard and a row above it: esc, tab, ⌃, ⌥,
⌘ and the arrows. ⌃, ⌥ and ⌘ stay down from a tap until the next key, click or gesture, as on
iOS. Text goes in as visible-password input (no suggestions), and is typed as U.S. keyboard
Linux key codes (`henge_vm_key`). Composing text is corrected with Backspace. Enter and Delete
arrive as key events and go the hardware keyboard's way.

**Mouse and touchpad.** When a mouse or touchpad is connected, the session takes pointer
capture. A mouse sends relative motion, its three buttons and the wheel
(`henge_vm_mouse_move`, `_button`, `_scroll`); fractions carry over to the next event. A
touchpad that Android reports as `SOURCE_TOUCHPAD` (the keyboard case's) has its fingers sent
to the Mac's trackpad, so macOS does the gestures. Its physical click is `henge_vm_touch_button`.

## Staying alive

- A foreground service (type `specialUse`) with a notification runs while a Mac is running or
  paused, or macOS is installing. Notifications need the permission the app asks for at the
  first install or Start. The service still runs without it.
- The home screen offers Allow background use while Android still optimises the app's battery
  use.
- Before a start, the app compares the Mac's memory plus 1 GB with what Android has free
  (`ActivityManager.MemoryInfo.availMem`) and warns if it does not fit.
- `onTrimMemory` logs the level and the Mac's state. At `TRIM_MEMORY_BACKGROUND` it saves a
  paused Mac that is not on storage yet.
- **ColorOS** has its own background limits, which apps cannot change. Set them by hand in
  Settings, under Battery, then the app's entry: allow background activity and auto-launch.
  Also lock Menhir in the recent apps view, so the ColorOS cleaner does not close it.

## Lifecycle: pause and instant resume

This follows iOS's `InstantResume.swift`, with one difference: on iOS it is a setting, and here
it is always on. Otherwise a Mac nobody can see would keep running and drain the battery.

- When the session leaves the screen (`onStop`: Home, recents, another app, screen off), the
  Mac is paused (`henge_vm_pause`) and saved (`henge_vm_save`) to `resume.hsnap`, on a worker
  thread.
- When it returns (`onStart`) while the process is still alive, the file is deleted and the
  Mac runs on (`henge_vm_continue`).
- If Android closed Menhir, the next Start uses `henge_vm_resume` when `henge_vm_saved_ok`
  accepts the file for this ident: the app's install time, the Mac, its memory, CPUs and
  display mode. Otherwise the Mac boots from cold. The file is used once.

Each save writes the guest's whole memory to flash. With an 8 GB Mac, that is up to 8 GB for
each trip away from the session.

## Clipboard and shared folder

While a Mac runs, what macOS copies is put on Android's clipboard every half second
(`henge_share_take_clipboard`). Paste puts Android's clipboard on the Mac's clipboard
(`henge_share_set_clipboard`) and presses Command-V when the helper is running. Without the
helper it types the text. Type the set up command types
`curl -s http://10.0.2.4/.henge/install | sh` and never presses Return.

`Shared/` is served to macOS by `host/share.c` over a Unix socket in the app's cache folder. It
is also a `DocumentsProvider` (`SharedFolderProvider`), so it shows in the Files app and file
pickers as "Menhir shared folder".

## Native interface

`android/native/henge-vm.h` keeps the iOS header's functions and names. The changes:

- `henge_vm_set_display_surface(ANativeWindow *)` replaces `henge_vm_set_display_layer`. NULL
  means the surface is gone. The JNI side holds a reference until the next call. In the real
  variant it goes to `hv_vm_set_display_window`, then `hv_pvg_lite_set_layer`, then
  `hv_pvg_metal_set_layer(m, window)`. The Vulkan backend implements that last call for an
  `ANativeWindow`. It must be done with the old window when the call returns, and acquire the
  new one if it keeps it.
- `henge_available_memory` reads `MemAvailable` from `/proc/meminfo`.
- No JIT region is handed in.

Downloads use the services' `host/menhir_fetch.h` as it is. The app keeps its own release list
in `Releases.kt`, copied from `Installer.swift`.

## Tests

`./gradlew testDebugUnitTest` runs 30 JVM tests: key code to HID mapping and the Fn layer,
chords and repeat, soft-keyboard typing and composing, the sensor mapping and timestamps,
historical samples as frames, the tap, two-finger tap, hold-to-drag and four-finger state
machine, volume buttons and shared button holders, relative mouse remainders, and the display
mode calculation.

## Verified and not verified

What was run and what it showed, as of 2026-11-13:

- `./gradlew assembleDebug lint testDebugUnitTest` passes: 30 tests, no lint errors.
- The real variant builds and links. Its `libmenhir.so` (4 MB) has no unresolved engine or
  service symbols, and contains `hv_vm_create`, `hv_install_start`, `hv_net_attach`,
  `hv_audio_attach`, `menhir_fetch_ipsw`, `hv_pvg_metal_submit`, `hv_jit_signals_install` and
  `hv_snapshot_save`. It has not been run.
- On the tablet, the fake build installed and launched: logcat showed "Menhir started; fake
  engine: true", with storage in `/storage/emulated/0/Android/data/app.menhir/files`. A
  uiautomator dump showed the Macs screen in landscape, with its content at
  `[0,0][3000,2120]`, the whole panel. `dumpsys display` gives the panel as 2120 x 3000 in
  portrait at xdpi 304.2 and ydpi 304.8, so the session's mode works out to 3000 x 2120 and
  250 x 177 mm. The tablet was in use, and then locked, so New Mac and the session were not
  run on it.
- The emulator system image was missing from the SDK during this work, so nothing ran on an
  emulator either.

Still to check on the tablet, with the fake build:

- that the session fills the 3000 x 2120 panel with no bars;
- touch, keys and volume keys (`adb logcat -s menhir-fake`);
- pause and continue across Home, and a saved resume after the process is killed;
- the keyboard case's touchpad under pointer capture;
- the soft keyboard with ColorOS's input method.

These need the real engine:

- that `vm_android.c` runs at all (the fault chaining with ART, input timing);
- a full restore, and saving and restoring snapshots on 4 KB pages;
- pausing with a GPU, which on iOS has to happen before the app leaves the foreground;
- the clipboard helper and the shared folder with macOS.

Needs the graphics backend: any picture at all.

## Updates

In Settings on the home screen, paste a fine-grained GitHub token into Update token and save it once.
Limit it to `shirishpothi/Menhir` with read-only Contents. It stays in no-backup storage,
never in the APK or repository. Replace it there when it expires. The screen shows the
installed version and last check result. Checks run on arrival (at most every 30 seconds),
every five minutes, and on a ping from Menhir's own ntfy topic. Repeat checks use ETags;
failed checks retry after 15 seconds. Only higher version codes are offered.

Tap the header pill to download. It fills with progress and waits while a Mac is active
or macOS is installing. Android verifies the signing key and keeps app data. Outcomes,
confirmation, reopening, superseded downloads and the two-minute retry fallback follow Slate;
there is no document resume point, editor dot or compile profile.

ColorOS needs all three settings to reopen: Display over other apps; App info → Battery
usage → Allow background activity (Smart mode blocks reopening and replacement broadcasts);
and Apps → Special app access → Display pop-ups while in background. The last setting is
not on App info and cannot be read by apps; search Settings for “pop-ups” and enable it
by hand. Otherwise open Menhir yourself. The installer activity reopens the app; a recent
in-app update marker lets the replacement receiver act as backup, without reopening after adb installs.

To publish, increment `versionCode` and choose an unused `versionName` in
`android/app/build.gradle.kts`, then run from the repository root:

```sh
(cd android/app && ./gradlew assembleDebug lint testDebugUnitTest && ./gradlew --stop)
git add android/app/build.gradle.kts
git commit -m "Bump Android update version"
git push origin main
./scripts/publish-update.sh
```

The script refuses a dirty or unpushed checkout, publishes `menhir-<code>.apk` to the private
repository, then pings ntfy. It uses the debug APK to match the build on the tablet today.
Release builds also use `signingConfigs.getByName("debug")`; preserve the existing debug
keystore and back it up privately. Never replace it or commit it. Publishing is a separate
step; building does not contact GitHub or ntfy.
