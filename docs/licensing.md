# Licensing

Henge is GPL-2.0-or-later ([LICENSE](../LICENSE)). The shipped binary is a combined
GPL work because usbmuxd and code from Husk are compiled into it, so it cannot go on
the App Store.

| Code | Licence | Where |
| --- | --- | --- |
| Henge | GPL-2.0-or-later | `core/`, `devices/`, `host/`, `ios/`, `tools/` |
| Code from Husk | GPL-2.0-or-later | `ios/Henge/JIT/`, `HengeLog.swift`, `Native/henge-ios-jit.*`, `henge-brk.S`, `HengeJITHelper/` |
| QEMU's vmapple AES device, as a reference | GPL-2.0-or-later | `devices/aes.c` |
| QEMU's `tcg-apple-jit.h` | LGPL-2.1-or-later | `ios/Henge/Native/` |
| usbmuxd | GPL-2.0 or GPL-3.0 | `third_party/usbmuxd` |
| idevicerestore | LGPL (see below) | `third_party/idevicerestore` |
| libimobiledevice, libplist | LGPL-2.1, with GPL-2.0 for their tools | `third_party/` |
| libimobiledevice-glue, libirecovery, libtatsu, libusbmuxd | LGPL-2.1 | `third_party/` |
| libzip | BSD | `third_party/libzip` |
| Mbed TLS | Apache-2.0 or GPL-2.0-or-later | `third_party/mbedtls` |
| StikJIT (the Bitrise build applies `ci/stikjit-high-address.patch`) | MPL-2.0 | `ios/Henge/Resources/legal/` |
| idevice | MIT | `ios/Henge/Resources/legal/` |
| Pairing library's Rust crates | Listed per crate | `ios/Henge/Resources/legal/LICENSES-rppairing-crates.txt` |
| libslirp, pcre2 | BSD | Prebuilt from Husk |
| glib, libintl | LGPL-2.1-or-later | Prebuilt from Husk |

idevicerestore's `COPYING` file is LGPL-3.0, while its source headers say LGPL-2.1
or later. If the `COPYING` file governs, the combined binary is GPL-3.0, which
GPL-2.0-or-later allows.

## Not covered

These are Apple's, and none of the licences above apply to them:

- **macOS**, which Henge downloads from Apple when you install it.
- **The boot ROM**, `AVPBooter.vmapple2.bin`. Henge fetches it on the device, from Apple's
  macOS 14.6.1 restore image, when you install. It isn't in this repository or in the app.

An exported Mac is a copy of that Mac, so its folder contains macOS and the ROM, as does
one made by `tools/export_ios.sh`. Those folders are for your own devices, not for sharing.

## Android services

The Android build uses the vendored restore libraries above and fetches these
source releases through `android/deps/fetch-sources.sh`, checking SHA-256 before
extraction. Their source and licence files remain in the extracted source tree;
packaging must include those notices alongside the existing restore-stack notices.

| Code | Licence | Source |
| --- | --- | --- |
| curl 8.12.1 | curl licence (MIT-style) | [curl](https://curl.se/download/curl-8.12.1.tar.xz), `COPYING` |
| libslirp 4.9.0 | BSD-3-Clause and source-file BSD notices | [libslirp](https://gitlab.freedesktop.org/slirp/libslirp/-/tree/v4.9.0), `COPYRIGHT` and source headers |
| LZFSE 1.0 | BSD-3-Clause | [LZFSE](https://github.com/lzfse/lzfse/tree/lzfse-1.0), `LICENSE` |
| LZ4 1.10.0 library | BSD-2-Clause | [LZ4](https://github.com/lz4/lz4/tree/v1.10.0/lib), `lib/LICENSE` |
| Menhir's GLib subset and native adapters | GPL-2.0-or-later | `android/deps/glib/`, new `host/*_android.c` |

Android does not use Husk's prebuilt GLib, libintl, pcre2 or libslirp. It uses a
small local GLib subset for libslirp instead. The Mbed TLS already in `third_party`
provides real TLS for curl and the restore stack.

`android/deps/certs/apple_root.h` contains Apple's public X.509 Root CA certificate
as trust data for signing-server verification, obtained from Apple's certificate
site. It contains no Apple executable or implementation source. Its origin, hash
and restricted use are documented in [Android services](android/services.md).
Apple's ROM and IPSW remain downloads outside the repository.
