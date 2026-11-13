# Android services

`android/deps/build.sh` builds native services for arm64-v8a, NDK 29 and API 34.
It uses CMake 3.31.6 from the Android SDK and four build workers. Outputs stay in
`android/deps/build/`. `build.sh mac` builds the desktop tests in `build-services/`.
No Husk checkout or Java service is used.

## Libraries and integration

Run `android/deps/fetch-sources.sh` before adding `android/deps` with CMake's
`add_subdirectory`. Download archives are SHA-256 checked. Archives and extracted
sources stay in the ignored `android/deps/sources/` directory. The build uses:

| Target | Sources |
| --- | --- |
| `menhir::restore` | Vendored libplist, libimobiledevice-glue, libusbmuxd, libimobiledevice, libtatsu, libirecovery, usbmuxd, idevicerestore and libzip; `host/install.c` and `host/libusb_shim.c` |
| `menhir::http` | curl 8.12.1, vendored Mbed TLS, LZFSE 1.0, `host/fetch_android.c`, `host/http_android.c` |
| `menhir::net` | libslirp 4.9.0, the existing network and sharing adapters, Android DNS and filesystem helpers |
| `menhir::audio` | `host/audio_android.c`, linked with Android's AAudio |
| `menhir::compat` | POSIX filesystem helpers and LZ4 1.10.0 with Apple's documented snapshot framing |

These are static targets with transitive dependencies. Link the CMake targets,
not just `libmenhir_restore.a`: restore requires all the other restore archives.
Set `MENHIR_SERVICES_TESTS=OFF` when embedding without the command-line tests.
The engine's three feature options can link restore, net and audio independently.
The app bridge can link HTTP directly and include `host/menhir_fetch.h`.

Libslirp uses a small local implementation of its GLib surface, under GPL-2.0-or-later,
so GLib, libintl and pcre2 are not needed. Allocation, strings, logs and random port
selection use libc. Process forwarding is disabled: command parsing fails with an
explicit error. NAT and Unix-socket forwarding remain available. Android DNS uses
`android_res_nsend`/`android_res_nresult` on the app's current network, with a bounded
poll and cancellation when the network session stops. This keeps VPN resolver selection
in Android rather than looking for `/etc/resolv.conf`.

AAudio opens shared output streams in the guest's S16 or float format. Stop closes
the stream before the virtio sound ring can be reset. A disconnected output is
reported by a failed start; automatic route-change recovery is not implemented.

## TLS trust

Curl uses real Mbed TLS, with peer and hostname verification enabled. Android reads
system roots from `/apex/com.android.conscrypt/cacerts`, falling back to
`/system/etc/security/cacerts`. User-installed roots and Java network-security
configuration are not imported. No CA bundle is downloaded at runtime.

`gs.apple.com` presents a chain ending at **Apple Root CA**, which the tested Android
system store does not trust. `android/deps/certs/apple_root.h` contains that public
certificate, obtained from [Apple](https://www.apple.com/appleca/AppleIncRootCertificate.cer).
Its DER SHA-256 is:

```
b0b1730ecbc7ff4505142c49f1295e6eda6bcaed7e2c68c5be91b5a11001f024
```

An SSL-context callback selects this root only for `gs.apple.com`,
`gs.apple.com.akadns.net` and `gs.apple.com.v.aaplimg.com`. Mbed TLS still verifies
certificate signatures, validity and the requested hostname. Other hosts use the
system roots. The root is trust data, not Apple executable or implementation code.

The Android build copies vendored restore C sources into its build directory before
changing `CURLOPT_SSL_VERIFYPEER, 0` to `1L` and the signing-server HTTP fallbacks to
HTTPS. Restore curl handles also get the trust callback and HTTPS-only protocol and
redirect policy. The checked-in third-party and iOS sources are unchanged. A restore
URL supplied as plain HTTP is rejected; a full restore must confirm whether any such
URLs need a deliberate HTTPS upgrade. No verification failure falls back to HTTP.

## Downloads

`host/menhir_fetch.h` describes the app interface: start a ROM or IPSW job, poll a
copied progress structure, cancel, then free to join the worker. Arguments are copied.
Do not free a job while another thread is polling it. Only one job may own a destination.
Parents must exist, and paths must be in app-private storage.

The release catalogue is copied from `ios/Henge/Installer.swift`, including the
approximate sizes; it is not a live claim about availability or signing status.
Tie each IPSW path to an immutable build URL. Completed files are reused; interrupted
ones remain in `.part`. Resume uses 64-bit curl offsets and validates Content-Range
before appending. A server that ignores Range causes a fresh download. HTTP 416 is
reported as a failure rather than assuming an unverified partial is complete.
IPSWs have no upstream SHA-256 in the catalogue and are not hashed by this API.
The caller must manage free space and keep the shared cache below 60 GB.

ROM jobs reuse only a file with the expected length and SHA-256. Otherwise they fetch
859,848 bytes of the Sonoma image, check the exact range, decode LZFSE and its LZVN
resource fork, then verify the 214,904-byte ROM against upstream's SHA-256:

```
fd4973e42e609751830df6f3dd2d2b2ad2309fe38f0ca0f5aba02dcbefe45f91
```

Files are flushed and fsynced before `.part` is renamed. Nothing is decrypted or
patched. Desktop downloads belong in `~/.menhir/cache/`; tests on Android use temporary
app-independent files under `/data/local/tmp` and remove their test directory.

## Sharing and guest helper

`host/share.c` keeps its WebDAV and clipboard protocol. The small portability change
selects POSIX copy/delete helpers, Linux timestamps and `MSG_NOSIGNAL`. Android reports
ctime for the creation-time property, because portable stat has no birth time. Recursive
copy rejects symbolic links and special files; it does not preserve Darwin metadata.
The share stays on a private Unix socket. The app still supplies and reads clipboard
text through the existing C API; Android clipboard access itself belongs to the app.

Run `android/deps/build-guest-agent.sh` on the Mac to produce
`build-services/guest/henge-agent.gz`. Bundle that gzip as an Android asset, copy it
to private storage, and pass the gzip path as the agent argument to `hv_share_start`. The helper
remains a signed arm64 Mach-O for the guest; Android never executes it.

## Disk integration for the engine owner

The services work does not edit `host/disk.c`, `host/hvsparse.c` or `host/snapshot.c`.
`host/compat_android.h` exposes `menhir_disk_sync` (fsync), `menhir_disk_punch`
(fallocate PUNCH_HOLE with KEEP_SIZE) and tree copy/delete. Hole punching can fail
on a filesystem without support; the caller must retain its zero-write fallback.
Use the copy helper for a portable copy fallback where clonefile was used. This is
an actual copy, without clonefile's copy-on-write or metadata behavior.

In `disk.c`'s sync switch, use `menhir_disk_sync` instead of Darwin barrier/full-sync
fcntl operations on Android. Keep F_NOCACHE guarded: this helper does not emulate it.
`hvsparse.c` already uses POSIX I/O and SEEK_DATA/SEEK_HOLE; the engine owner must handle
filesystems without those seek operations rather than treating an error as end-of-data.

Link `menhir::compat` for `snapshot.c`. Its Android-only `compression.h` provides the
four existing buffer/scratch calls for COMPRESSION_LZ4. The implementation reads and
writes Apple's documented bv4 blocks, including dependent blocks. It does not change
the snapshot format. The CMake include directory must be visible when compiling
snapshot.c, not just at final link. These call-site changes remain with the engine owner.

## Verification and limits

Ran the standalone Android build and `test-android.sh emulator-5580` on the engine
owner's Android 16 arm64 emulator with 16 KB pages. Earlier tests also ran on its
4 KB-page emulator. Results:

- Restore executable linked the complete stack and virtual libusb: null USB rejected
  with `-1`. Only engine transport functions are test substitutes.
- AAudio opened and started a stereo 48 kHz S16 stream: `AAudio stream start: 0`.
- Libslirp answered an injected ICMP echo after ARP: `got it!`.
- Signing-host TLS verified: `curl=0 HTTP=401`. An empty or missing trust directory
  was rejected (`curl=60` or `77`). HTTP 401 here is the expected unauthenticated page,
  not a restore personalization result.
- A 4 MiB Apple CDN range was interrupted near 1 MiB, resumed with HTTP 206, and
  compared byte for byte with a fresh range: `PASS`.
- ROM extraction matched the hash above; the next job reused the verified file.
- Native download cancellation left about 1 MiB; resuming the 2,768,160-byte curl
  source archive produced its pinned SHA-256: `PASS`.
- A 4 MiB LZ4 round trip passed and truncated input was rejected.

The Mac CMake build also passed. Desktop restore-link, libslirp and TLS/range tests
passed. The codec test confirmed both directions against system libcompression:
`Apple libcompression interoperability: PASS in both directions`.
The guest helper build also passed and produced a signed arm64 Mach-O plus gzip;
its runtime inside macOS is not tested here.

A full macOS restore, guest networking through the host adapter, DNS under an actual
Android app UID/VPN, WebDAV and clipboard end to end, audio route changes, playback
quality, a multi-gigabyte IPSW and physical-tablet behavior remain untested. The full
Mac VM Makefile build was not run; the independent services build compiled the changed
sharing source on both platforms. No iOS files were changed.

Dependency licence details are in [licensing.md](../licensing.md). Source URLs and hashes
are in `android/deps/fetch-sources.sh`; curl uses its curl licence, libslirp and LZFSE
use BSD terms, and the LZ4 library uses BSD-2-Clause. None requires a prebuilt binary.
