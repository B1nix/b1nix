# GNU-Less Migration Plan for b1nix

## Overview

b1nix has already achieved significant independence from GNU components by migrating its toolchain to **LLVM/Clang/LLD** (M64, M90), its C library to **musl libc** (M92), its C++ standard library to **LLVM libc++** (M89), and its core utilities to **BusyBox** (M42, M44).

This document outlines the remaining steps required to eliminate all residual GNU/GPL software from the ISO image and achieve a **100% GNU-less (GNU-free)** operating system environment.

---

## Current Inventory of GNU Components in ISO

| # | Component | Current Usage in b1nix | License | Proposed Replacement |
|---|---|---|---|---|
| 1 | **GNU GRUB 2.x** | ISO bootloader (`/boot/grub/grub.cfg`, `grub-mkrescue`) | GPLv3+ | **Limine** (BSD 2-Clause) |
| 2 | **GNU bash** (5.2.37) | Interactive shell in ISO (`/bin/bash`) | GPLv3+ | **zsh** (MIT/BSD-like) & **BusyBox `ash`** |
| 3 | **GNU Wget** | Standalone HTTP/HTTPS downloader (`/bin/wget`) | GPLv3+ | **curl** (MIT) / **BusyBox wget** |
| 4 | **GNU Make** | In-guest build automation tool (`/bin/make`) | GPLv3+ | **Ninja** (Apache 2.0) / **samurai** (0BSD) / **bmake** |
| 5 | **Gnulib** | Embedded static routines in `bash` and `wget` | GPLv3+ / LGPL | **Automatic Removal** |

---

## Replacement Strategy & Architecture

### 1. Bootloader: GNU GRUB → Limine — **DONE**

* **Replacement**: [Limine Bootloader](https://limine-bootloader.org/) (BSD 2-Clause License).
* **Rationale**: Limine is modern, ultra-lightweight, actively maintained, and natively supports Multiboot2, x86_64, BIOS, and UEFI out of the box.
* **Status**: GRUB is gone. `tools/mkiso.sh` stages the Limine boot files, expands
  `boot/limine/limine.conf.in` and builds a hybrid BIOS+UEFI ISO with `xorriso`
  + `limine bios-install`; all nine `iso*` targets, `disk-iso`, the alpine
  harness and both in-guest proof scripts call it. `boot/grub/` is deleted.
  `mk-disk-image.sh` now installs Limine straight into the image file, which
  removed its losetup/mount/`sudo` steps (and `tools/images/sudoers.d-b1nix-diskimage`).

### 2. Shell: GNU bash → zsh & BusyBox ash — **DONE**

* **Replacement Options**:
  * **zsh (Z Shell)** (Custom MIT/BSD-style Permissive License) for interactive use (`/bin/zsh`).
  * **BusyBox `ash`** (GPLv2/BSD) for fast POSIX boot scripts (`/bin/sh`).
* **Rationale**:
  * `zsh` provides an extremely powerful interactive environment (advanced tab-completion, globbing, theme support, line editing) without GPL restrictions.
  * b1nix already supports all POSIX/termios APIs required by `zsh`.
  * Pairing `zsh` as default interactive shell with `ash` as `/bin/sh` yields the optimal balance of interactive power and boot speed.
* **Status**: `/bin/zsh` is the login shell for root and user, `/bin/sh` is the
  BusyBox `ash` symlink, and bash is gone — its port script is deleted, and
  because the published package index still carries a bash package,
  `install-ports.sh` purges it from the rootfs after extraction.
  `tools/ports/build-zsh.sh` cross-builds zsh 5.9 as a dynamic musl PIE.
  It needed one prerequisite: zsh refuses to configure without a terminal
  library and b1nix had none (bash used its own bundled termcap), so
  `tools/ports/build-netbsd-curses.sh` ports the BSD-licensed netbsd-curses —
  chosen over GNU ncurses, which is permissively licensed but still a GNU
  package. `--disable-dynamic` makes configure mark every optional module
  `link=no`, which silently drops shell features, so the port relinks
  `zsh/regex` (without it `[[ =~ ]]` is a parse error) plus mathfunc/stat/
  system/files/zselect into the binary. `ZSH-SMOKE` re-covers every feature
  `BASH-SMOKE` asserted.

### 3. Downloader: GNU Wget → curl / BusyBox wget — **DONE**

* **Replacement**: **curl** (MIT / curl License) & **BusyBox `wget`**.
* **Status**: GNU Wget is gone — its port script, `Makefile` recipe and rootfs
  binary are removed. `/bin/wget` is now the BusyBox applet, and the M32 network
  smoke drives `curl` for the loopback and IPv6 downloads
  (`M32-NET: ok curl-loopback` / `ok curl-ipv6`). The wget-only checks
  (`--regex-type pcre`, IRI/punycode, NTLM) were assertions about wget's own
  build options and went with it; the HTTPS pair was already covered by the
  curl TLS tests.
* **Rationale**: `curl` with mbedTLS support is already fully ported and integrated (Milestones M32a, M54).
* **Implementation Plan**:
  * Remove `INITRAMFS_WGET_INC` and `wget` build recipe from `Makefile`.
  * Direct all system network fetches through `curl` or `busybox wget`.

### 4. Build Automation: GNU Make → samurai + bmake — **DONE**

* **Replacement Options**:
  * **Ninja** (Apache 2.0 License) or **`samurai`** (0BSD/ISC C-reimplementation of Ninja).
  * **`bmake`** (NetBSD Make, BSD License).
* **Rationale**:
  * **Ninja** is lightning fast and already used in b1nix to build V8, Skia, Chromium, and Mesa.
  * **`samurai`** is a C-based Ninja replacement under ~30 KB with zero C++ dependencies.
  * `bmake` provides traditional POSIX Make syntax when a Makefile runner is explicitly needed.
* **Status**: both shipped as dynamic musl PIEs. `tools/ports/build-samurai.sh`
  produces `/bin/samu` (aliased to `/bin/ninja`) and `tools/ports/build-bmake.sh`
  produces `/bin/make` (also `/bin/bmake`) plus the `/usr/share/mk` system
  makefiles. `tools/toolchain/build-make.sh` is deleted and `/bin/make` no longer
  needs a static-allowlist exemption. `tools/inguest/Makefile` and its test
  fixture were rewritten in portable BSD make syntax; `M98-SMOKE` proves both
  tools parse a build description, run its recipe and report an up-to-date
  target on the second run.

### 5. Gnulib → Automatic Removal — **DONE**

* **Replacement**: None required.
* **Rationale**: Gnulib is present only as bundled source code within `bash` and `wget`. Removing `bash` and `wget` automatically eliminates Gnulib from the codebase entirely.

---

## Action Items Checklist

- [x] **Phase 1: Shell & Downloader**
  - [x] Port `zsh` to b1nix as the primary interactive shell (`/bin/zsh`).
  - [x] Remove `bash` from build pipeline; default `/bin/sh` to BusyBox `ash`.
  - [x] Remove `wget` from build pipeline; default to `curl` / `busybox wget`.
  - [x] Verify smoke test suite passes with `zsh` and `ash`.

- [x] **Phase 2: Build Tools**
  - [x] Port `samurai` to b1nix for fast build graph execution (`/bin/samu`, `/bin/ninja`).
  - [x] Port `bmake` (NetBSD Make) for Makefile compatibility (`/bin/make`).
  - [x] Replace GNU Make in `rootfs.img`.

- [x] **Phase 3: Bootloader**
  - [x] Add `limine` configuration (`boot/limine/limine.conf.in`, `limine-disk.conf.in`).
  - [x] Update `Makefile` ISO targets (`iso`, `iso-live`, `disk-iso`, ...) to use `limine` + `xorriso`.
  - [x] Verify QEMU boot and test suite execution with Limine.
