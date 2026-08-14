# Third-Party Software Notices & License Inventory

The MIT license described in `LICENSING.md` applies to original B1NIX material. It does not replace or override third-party software licenses. This document provides a complete inventory of all third-party libraries, toolchains, runtimes, and applications integrated or ported for B1NIX.

---

### Fetched at build time, never vendored

| Component | Staged at | Version / Revision | License | Upstream / Reference |
| --- | --- | --- | --- | --- |
| **Linux DRM core** (`drivers/gpu/drm`, `include/drm`, `include/uapi/drm`, `drivers/video/{hdmi,nomodeset}.c`) | `build/src/drm-core-6.6/` | Linux 6.6, SHA-256 `d926a06c…8e56d0` | MIT (`drivers/gpu/drm`, `include/drm`); GPL-2.0 WITH Linux-syscall-note (`include/uapi/drm`) | <https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.6.tar.xz> |

| **Intel i915** (`drivers/gpu/drm/i915`) | `build/src/i915-6.6/` | Linux 6.6, SHA-256 `d926a06c…8e56d0` | MIT, and the historical X11-style permission grant on the untagged files | <https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.6.tar.xz> |

i915 is staged **only on request** (`make i915-fetch`), and built only when asked
for (`B1NIX_I915=1`): it is 13 MiB and 262 objects, and a kernel built without a
GPU should not pay for it. A tree that has not been staged changes nothing about
the build.

Five files are **not staged**, because they are plain `GPL-2.0` with no
permissive alternative — unlike the DRM core, whose GPL-touched files are all
`GPL-2.0 or MIT`. Four are pure ftrace plumbing (`i915_trace.h`,
`i915_trace_points.c`, `display/intel_display_trace.{c,h}`) and are replaced by
MIT tracepoint headers of our own; the fifth (`display/intel_acpi.c`) is
`CONFIG_ACPI`-only and is not in `i915-y`. `tools/drm/fetch-i915.sh` refuses to
finish if any other `GPL-2.0`-only file appears outside the selftests, so this
stays a decision someone made rather than something discovered later.

The DRM core is **imported and never edited** — see [`docs/drm-import.md`](docs/drm-import.md).
`tools/drm/fetch-drm-core.sh` pins the release and verifies the checksum before
extracting, the same way the port scripts under `tools/ports/` pin theirs.
Nothing under Linux's `include/linux` is staged: those headers are GPL-2.0
without exception, and are exactly what b1nix reimplements from scratch under
MIT in `kernel/include/linux` and `kernel/lkpi`.

---

## 2. Core OS Runtimes & C/C++ Libraries

| Component | Version | License Summary | Description & Purpose |
| --- | ---: | --- | --- |
| **musl libc** | 1.2.5 | MIT License | Primary C standard library (`libc.so`, `ld-musl-x86_64.so.1`, `libc.a`) |
| **LLVM libc++ / libc++abi / libunwind** | 22.1.8 | Apache-2.0 WITH LLVM-exception | Modern C++ standard library, ABI & unwinder for b1nix |
| **openlibm** | 0.8.3 | BSD-2-Clause / Freely redistributable | High-performance standalone C math library (`libopenlibm.a`) |
| **Brotli** | 1.1.0 | MIT License | Generic lossless data compression library (`libbrotli`) |
| **libffi** | 3.5.2 | MIT License | Portable Foreign Function Interface library (`libffi.a`) |
| **libutf8proc** | 2.9.0 | MIT License | Clean C library for processing UTF-8 Unicode data |
| **libharu** | 2.4.4 | Zlib / libpng License | Cross-platform C library for generating PDF files |

---

## 3. ISO Bootloader, Shells, Service Managers & System Build Tools (Milestone M98 GNU-Free Environment)

Following Milestone M98 (GNU-free ISO), GNU GRUB, GNU bash, GNU Wget, and GNU Make were replaced in the default ISO distribution by permissively-licensed tools:

| Component | Version | License Summary | Description & Role in B1NIX |
| --- | ---: | --- | --- |
| **Limine Bootloader** | 8.x | BSD-2-Clause | Modern BIOS+UEFI Multiboot2 ISO bootloader (`boot/limine/`) |
| **zsh (Z Shell)** | 5.9 | Zsh License / MIT-style | Primary interactive login shell (`/bin/zsh`) |
| **netbsd-curses** | 9.0 | BSD-3-Clause | Lightweight NetBSD curses/terminfo library for zsh |
| **BusyBox** | 1.38.0 | GNU GPL version 2 | POSIX core utilities & default `/bin/sh` (`ash`) |
| **OpenRC** | 0.54 | BSD-2-Clause | Dependency-based init & service management system |
| **runit** | 2.1.2 | BSD-3-Clause / Public Domain | UNIX init scheme with service supervision |
| **bmake** | current | BSD-3-Clause | NetBSD Make build automation tool (`/bin/make`) |
| **samurai** | 1.2 | 0BSD | C reimplementation of the Ninja build system (`/bin/samu`, `/bin/ninja`) |

---

## 4. Networking, Security & Communication Stack

| Component | Version | License Summary | Role in B1NIX |
| --- | ---: | --- | --- |
| **curl** | 8.20.0 | curl License (MIT-style) | Command line HTTP/HTTPS/FTP transfer tool |
| **Dropbear** | 2022.83 | Dropbear License (MIT-style) | Lightweight SSH server & client (`dropbear`, `dbclient`) |
| **Mbed TLS** | 3.6.0 | Apache-2.0 OR GPL-2.0-or-later | Cryptography and TLS/SSL protocol support |
| **OpenSSL** | 1.1.1w | OpenSSL & SSLeay Licenses | Cryptographic routines and SSL/TLS engine |
| **PCRE2** | 10.44 | BSD-3-Clause | Perl-compatible regular expressions library |
| **GNU libidn2** | 2.3.7 | GNU LGPL-3.0-or-later OR GPL-2.0+ | Internationalized Domain Names library |
| **GNU libunistring** | 1.2 | GNU LGPL-3.0-or-later OR GPL-2.0+ | Unicode string handling library |
| **libpsl** | 0.21.5 | MIT License | Public Suffix List cookie/domain checking library |
| **Mozilla CA Certificate Bundle** | build-time | Mozilla MPL 2.0 / CA Extract | Root TLS certificates for secure network connections |

---

## 5. Graphics, Display, Fonts & Multimedia Porting Stack

| Component | Version | License Summary | Description |
| --- | ---: | --- | --- |
| **Mesa 3D** | 24.1.0 | MIT License | OpenGL / Gallium software & hardware graphics renderer |
| **Skia** | m124 | BSD-3-Clause | Complete 2D graphic library for drawing text, geometries, and images |
| **TinyGL** | 0.4 | Zlib / MIT License | Small software implementation of a subset of OpenGL |
| **NetSurf FB** | 3.11 | GPL-2.0-only / MIT (libraries) | Framebuffer web browser for b1nix |
| **NetSurf Libraries** | 0.9 / 0.4 | MIT License | Support libraries (`libcss`, `libdom`, `libhubbub`, `libparserutils`, `libwapcaplet`, `libnsbmp`, `libnsgif`, `libnslog`, `libnsutils`, `librosprite`, `libsvgtiny`) |
| **litehtml** | 0.9 | MIT License | Fast and lightweight HTML rendering engine |
| **zlib** | 1.3.1 | zlib License | Data compression library |
| **libpng** | 1.6.43 | libpng License | PNG image decoding/encoding |
| **libjpeg-turbo** | 3.0.3 | IJG / BSD-3-Clause | Accelerated JPEG image decoding/encoding |
| **libwebp** | 1.4.0 | BSD-3-Clause | WebP image format decoding and encoding library |
| **libvpx** | 1.14.1 | BSD-3-Clause | VP8/VP9 video codec library |
| **libjxl** | 0.11.1 | BSD-3-Clause | JPEG XL reference implementation |
| **FreeType** | 2.13.2 | FTL (BSD-style) / GPL-2.0 | Font rendering engine |
| **Fontconfig** | 2.15.0 | MIT License | Font configuration and discovery |
| **Pixman** | 0.43.4 | MIT License | Low-level pixel manipulation library |
| **Cairo** | 1.18.0 | LGPL-2.1 OR MPL-1.1 | 2D vector graphics library |
| **Expat** | 2.6.2 | MIT License | Stream-oriented XML parser |
| **libwayland** | 1.23.0 | MIT License | Display server protocol support |
| **libxkbcommon** | 1.7.0 | MIT License | Keyboard keymap handling library |
| **HarfBuzz** | 8.5.0 | MIT License | Text shaping engine |

## 6. Binary Distribution & Compliance Requirements

Placing downloaded source under `build/` or embedding compiled objects into initramfs/rootfs image does not by itself satisfy distribution requirements. Anyone redistributing B1NIX binary images or releases must:

1. Preserve copyright and license notices for all components.
2. Provide the full text of applicable licenses (GPL, LGPL, MIT, BSD, Apache).
3. Provide complete corresponding source code and build scripts for GPL/LGPL covered components.
4. Comply with relinking or source requirements for statically linked LGPL libraries.

*This document is an inventory of third-party licenses and does not constitute legal advice.*
