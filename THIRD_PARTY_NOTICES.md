# Third-Party Software Notices & License Inventory

The GPL and LGPL licenses described in `LICENSING.md` apply to original B1NIX material. They do not replace or override third-party software licenses. This document provides a complete inventory of all third-party libraries, toolchains, runtimes, and applications integrated or ported for B1NIX.

---

## 1. Source Included directly in Repository

| Component | Location | Version / Revision | License | Upstream / Reference |
| --- | --- | --- | --- | --- |
| **TinyCC** | `userspace/tcc/` | 0.9.28rc | GNU LGPL v2.1 or later | <https://repo.or.cz/tinycc.git> |
| **Duktape** | `userspace/duktape/` | 2.7.0 | MIT License | <https://duktape.org/> |

---

## 2. Core OS Runtimes & C/C++ Libraries

| Component | Version | License Summary | Description & Purpose |
| --- | ---: | --- | --- |
| **musl libc** | 1.2.5 | MIT License | Primary C standard library (`libc.so`, `ld-musl-x86_64.so.1`, `libc.a`) |
| **LLVM libc++ / libc++abi / libunwind** | 22.1.8 | Apache-2.0 WITH LLVM-exception | Modern C++ standard library, ABI & unwinder for b1nix |
| **openlibm** | 0.8.3 | BSD-2-Clause / Freely redistributable | High-performance standalone C math library (`libopenlibm.a`) |

---

## 3. ISO Bootloader, Shells & System Build Tools (Milestone M98 GNU-Free Environment)

Following Milestone M98 (GNU-free ISO), GNU GRUB, GNU bash, GNU Wget, and GNU Make were replaced in the default ISO distribution by permissively-licensed tools:

| Component | Version | License Summary | Description & Role in B1NIX |
| --- | ---: | --- | --- |
| **Limine Bootloader** | 8.x | BSD-2-Clause | Modern BIOS+UEFI Multiboot2 ISO bootloader (`boot/limine/`) |
| **zsh (Z Shell)** | 5.9 | Zsh License / MIT-style | Primary interactive login shell (`/bin/zsh`) |
| **netbsd-curses** | 9.0 | BSD-3-Clause | Lightweight NetBSD curses/terminfo library for zsh |
| **BusyBox** | 1.38.0 | GNU GPL version 2 | POSIX core utilities & default `/bin/sh` (`ash`) |
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
| **NetSurf FB** | 3.11 | GPL-2.0-only / MIT (libraries) | Framebuffer web browser for b1nix |
| **zlib** | 1.3.1 | zlib License | Data compression library |
| **libpng** | 1.6.43 | libpng License | PNG image decoding/encoding |
| **libjpeg-turbo** | 3.0.3 | IJG / BSD-3-Clause | Accelerated JPEG image decoding/encoding |
| **FreeType** | 2.13.2 | FTL (BSD-style) / GPL-2.0 | Font rendering engine |
| **Fontconfig** | 2.15.0 | MIT License | Font configuration and discovery |
| **Pixman** | 0.43.4 | MIT License | Low-level pixel manipulation library |
| **Cairo** | 1.18.0 | LGPL-2.1 OR MPL-1.1 | 2D vector graphics library |
| **Expat** | 2.6.2 | MIT License | Stream-oriented XML parser |
| **libwayland** | 1.23.0 | MIT License | Display server protocol support |
| **libxkbcommon** | 1.7.0 | MIT License | Keyboard keymap handling library |
| **HarfBuzz** | 8.5.0 | MIT License | Text shaping engine |

---

## 6. Binary Distribution & Compliance Requirements

Placing downloaded source under `build/` or embedding compiled objects into initramfs/rootfs image does not by itself satisfy distribution requirements. Anyone redistributing B1NIX binary images or releases must:

1. Preserve copyright and license notices for all components.
2. Provide the full text of applicable licenses (GPL, LGPL, MIT, BSD, Apache).
3. Provide complete corresponding source code and build scripts for GPL/LGPL covered components.
4. Comply with relinking or source requirements for statically linked LGPL libraries.

*This document is an inventory of third-party licenses and does not constitute legal advice.*
