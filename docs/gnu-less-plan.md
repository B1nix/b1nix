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

### 1. Bootloader: GNU GRUB → Limine

* **Replacement**: [Limine Bootloader](https://limine-bootloader.org/) (BSD 2-Clause License).
* **Rationale**: Limine is modern, ultra-lightweight, actively maintained, and natively supports Multiboot2, x86_64, BIOS, and UEFI out of the box.
* **Implementation Plan**:
  * Replace `grub-mkrescue` invocation in `Makefile` with `xorriso` + `limine-deploy`.
  * Create `limine.conf` to replace `boot/grub/grub.cfg`.
  * Update QEMU and bare-metal test runners.

### 2. Shell: GNU bash → zsh & BusyBox ash

* **Replacement Options**:
  * **zsh (Z Shell)** (Custom MIT/BSD-style Permissive License) for interactive use (`/bin/zsh`).
  * **BusyBox `ash`** (GPLv2/BSD) for fast POSIX boot scripts (`/bin/sh`).
* **Rationale**:
  * `zsh` provides an extremely powerful interactive environment (advanced tab-completion, globbing, theme support, line editing) without GPL restrictions.
  * b1nix already supports all POSIX/termios APIs required by `zsh`.
  * Pairing `zsh` as default interactive shell with `ash` as `/bin/sh` yields the optimal balance of interactive power and boot speed.
* **Implementation Plan**:
  * Cross-compile `zsh` against musl libc and package as `/bin/zsh`.
  * Remove `bash` from the build configuration.
  * Set `/bin/zsh` as default user/login shell in `/etc/passwd`.

### 3. Downloader: GNU Wget → curl / BusyBox wget

* **Replacement**: **curl** (MIT / curl License) & **BusyBox `wget`**.
* **Rationale**: `curl` with mbedTLS support is already fully ported and integrated (Milestones M32a, M54).
* **Implementation Plan**:
  * Remove `INITRAMFS_WGET_INC` and `wget` build recipe from `Makefile`.
  * Direct all system network fetches through `curl` or `busybox wget`.

### 4. Build Automation: GNU Make → Ninja / samurai / bmake

* **Replacement Options**:
  * **Ninja** (Apache 2.0 License) or **`samurai`** (0BSD/ISC C-reimplementation of Ninja).
  * **`bmake`** (NetBSD Make, BSD License).
* **Rationale**:
  * **Ninja** is lightning fast and already used in b1nix to build V8, Skia, Chromium, and Mesa.
  * **`samurai`** is a C-based Ninja replacement under ~30 KB with zero C++ dependencies.
  * `bmake` provides traditional POSIX Make syntax when a Makefile runner is explicitly needed.
* **Implementation Plan**:
  * Cross-compile `ninja` (C++ / libc++) or `samurai` (C / musl) and ship as `/bin/ninja`.
  * Optionally ship `bmake` as `/bin/make` for legacy Makefile compatibility.

### 5. Gnulib → Automatic Removal

* **Replacement**: None required.
* **Rationale**: Gnulib is present only as bundled source code within `bash` and `wget`. Removing `bash` and `wget` automatically eliminates Gnulib from the codebase entirely.

---

## Action Items Checklist

- [ ] **Phase 1: Shell & Downloader**
  - [ ] Port `zsh` to b1nix as the primary interactive shell (`/bin/zsh`).
  - [ ] Remove `bash` from build pipeline; default `/bin/sh` to BusyBox `ash`.
  - [ ] Remove `wget` from build pipeline; default to `curl` / `busybox wget`.
  - [ ] Verify smoke test suite passes with `zsh` and `ash`.

- [ ] **Phase 2: Build Tools**
  - [ ] Port `ninja` / `samurai` to b1nix for fast build graph execution.
  - [ ] (Optional) Port `bmake` (NetBSD Make) for legacy Makefile compatibility.
  - [ ] Replace GNU Make in `rootfs.img`.

- [ ] **Phase 3: Bootloader**
  - [ ] Add `limine` configuration (`limine.conf`).
  - [ ] Update `Makefile` ISO targets (`iso`, `iso-live`, `disk-iso`) to use `limine` + `xorriso`.
  - [ ] Verify QEMU boot and test suite execution with Limine.
