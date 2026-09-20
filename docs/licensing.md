# Licensing

b1nix's own code is under the GNU General Public License, version 2 only
(`GPL-2.0-only`); the full text is in [LICENSE](../LICENSE). The rest of this
document is the inventory of third-party material: every component is conveyed
under its own terms, which this licence does not replace or override.

Version **2 only** — not "or later". b1nix's own code carries
`SPDX-License-Identifier: GPL-2.0-only` in every file, and a file that carries a
different tag is third-party and says where it came from. The `LICENSE` text is
the unmodified GPLv2; the "any later version" sentence inside it is part of that
text and is not b1nix's offer. The `LICENSING.md` this document absorbed claimed
original b1nix code was MIT; that claim is withdrawn — it contradicted `LICENSE`
and could not cover the GPL-2.0 Linux source the kernel imports.


This is the complete inventory of the third-party libraries, toolchains,
runtimes and applications integrated or ported for b1nix.

---

## 1. Imported into the kernel — fetched at build time, never vendored

| Component | Staged at | Version / Revision | License | Upstream / Reference |
| --- | --- | --- | --- | --- |
| **Linux DRM core** (`drivers/gpu/drm`, `include/drm`, `include/uapi/drm`, `drivers/video/{hdmi,nomodeset}.c`, `drivers/gpu/buddy.c`, `include/linux/gpu_buddy.h`) | `build/src/drm-core-6.18.51/` | Linux 6.18.51, SHA-256 `ba2f60f8…58df613` | MIT (`drivers/gpu/drm`, `include/drm`, the buddy allocator); GPL-2.0 WITH Linux-syscall-note (`include/uapi/drm`) | <https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.51.tar.xz> |
| **Linux filesystems** (`fs/{btrfs,ext4,jbd2,iomap,quota}`, `fs/mbcache.c`, `lib/{maple_tree,xarray,radix-tree,idr,xxhash}.c`, `lib/{zlib_*,lzo,zstd}`) | `build/src/fs-6.18.51/` | Linux 6.18.51, SHA-256 `ba2f60f8…58df613` | GPL-2.0-only (zstd: BSD-3-Clause OR GPL-2.0) | <https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.51.tar.xz> |

| **Linux io_uring uAPI** (`include/uapi/linux/io_uring.h`) | `kernel/include/b1nix/io_uring_abi.h`, vendored verbatim | Linux 6.18.51 | `GPL-2.0 WITH Linux-syscall-note` OR MIT upstream; MIT taken here | <https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.51.tar.xz> |
| **Intel i915** (`drivers/gpu/drm/i915`) | `build/src/i915-6.18.51/` | Linux 6.18.51, SHA-256 `ba2f60f8…58df613` | MIT, and the historical X11-style permission grant on the untagged files | <https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.51.tar.xz> |

i915 is staged **only on request** (`make i915-fetch`), and built only when asked
for (`B1NIX_I915=1`): it is 13 MiB and 262 objects, and a kernel built without a
GPU should not pay for it. A tree that has not been staged changes nothing about
the build.

Seven files are **not staged**, because they are plain `GPL-2.0` with no
permissive alternative — unlike the DRM core, whose GPL-touched files are all
`GPL-2.0 or MIT`. Six are pure ftrace plumbing (`i915_trace.h`,
`i915_trace_points.c`, `display/intel_display_trace.{c,h}`,
`intel_uncore_trace.{c,h}`) and are replaced by tracepoint headers of our own;
the seventh (`display/intel_acpi.c`) is
`CONFIG_ACPI`-only and is not in `i915-y`. `tools/import/drm/fetch-i915.sh` refuses to
finish if any other `GPL-2.0`-only file appears outside the selftests, so this
stays a decision someone made rather than something discovered later.

The DRM core is **imported and never edited** — see [`docs/drivers-and-graphics.md`](kernel/drivers-and-graphics.md).
`tools/import/drm/fetch-drm-core.sh` pins the release and verifies the checksum before
extracting, the same way the port scripts under `tools/ports/` pin theirs.
Linux's `include/linux` is not staged wholesale: the interfaces the imports
stand on are reimplemented in `kernel/include/linux` and `kernel/lkpi`. A few
headers that are data structures or pure macros rather than interfaces are
carried there copied from Linux 6.18.51 and marked as such — `xarray.h`,
`radix-tree.h`, `idr.h`, `maple_tree.h` (matching the staged
`lib/{xarray,radix-tree,idr,maple_tree}.c`),
`cleanup.h`, `args.h` and `unaligned.h` — under GPL-2.0, which b1nix's own
GPL-2.0-only licence is compatible with.

---

## 1a. Third-party files carried in the tree

Everything above is fetched at build time. A short list of files is carried in
the repository itself, and each keeps the licence it came with:

| File(s) | Origin | License |
| --- | --- | --- |
| `kernel/include/b1nix/io_uring_abi.h` | Linux 6.18.51 `include/uapi/linux/io_uring.h`, verbatim | MIT (upstream is `GPL-2.0 WITH Linux-syscall-note` OR MIT) |
| `kernel/include/linux/{xarray,maple_tree}.h` | Linux 6.18.51, verbatim | GPL-2.0+ |
| `kernel/include/linux/radix-tree.h` | Linux 6.18.51, verbatim | GPL-2.0-or-later |
| `kernel/include/linux/idr.h` | Linux 6.18.51, verbatim | GPL-2.0-only |
| `kernel/include/linux/{cleanup,args,unaligned}.h`, `kernel/include/linux/unaligned/`, `kernel/include/vdso/unaligned.h` | Linux 6.18.51, verbatim | GPL-2.0 |
| `tools/board/rp2350/{uart-bridge.c,usb-descriptors.c,tusb_config.h}` | pico-sdk / TinyUSB examples | MIT |

`kernel/include/uapi/linux/` and `kernel/include/linux/` otherwise hold b1nix's
own shims — the guards spell `LKPI_`, the prose is ours — and they are
`GPL-2.0-only` like the rest. They are not installed into the image, so the
Linux syscall-note exception, which exists for headers userspace compiles
against, does not apply to them.

`tools/toolchain/check-license.sh` (also `make check-license`) is the enforcing
copy of this list: it fails on a source file with no SPDX tag, and on one whose
tag is neither `GPL-2.0-only` nor the entry recorded for it there.

---

## 2. What b1nix builds from source

Nothing third-party. The kernel, its tests and `b1cc` are b1nix's own; every
library and program on the image, musl included, is an Alpine binary package
(section 3).

---

## 3. Alpine packages shipped in the image

The image ships the binary packages pinned in
[`tools/image/alpine/alpine.lock`](../tools/image/alpine/alpine.lock) — currently 255 of
them, verified by SHA-256 before installation. Each carries its own upstream
licence, recorded in Alpine's package index.

That list is **not duplicated here by hand**, because a hand-kept copy of a
machine-chosen set is exactly what drifted before: this document listed Mesa,
TinyGL and NetSurf long after they left the tree. Instead:

```sh
sh tools/image/alpine/licenses.sh           # package, version, licence, description
sh tools/image/alpine/licenses.sh --check   # fails if any package has no licence
```

reads the licence straight out of the package index that the fetch already
downloads, so it cannot fall behind the lock file.

The bulk of what a user sees — sway, wlroots, foot, seatd, the Wayland
libraries, GTK, Mesa's GL/EGL/GBM libraries, OpenSSL, curl, zsh, Dropbear,
FreeType, Fontconfig, HarfBuzz, Pixman, the image and video codecs, and Chromium
(which bundles Skia, BSD-3-Clause) — is in that set.

---

## 4. Binary Distribution & Compliance Requirements

b1nix's own code is GPL-2.0-only (see [LICENSE](../LICENSE)), which already
requires the source to travel with any binary you distribute. The components
above add their own requirements on top. Anyone redistributing a b1nix image
must:

1. Preserve every component's copyright and licence notices.
2. Ship the full text of each applicable licence.
3. Ship the complete corresponding source, and the scripts used to build it,
   for b1nix itself and for every GPL/LGPL component.
4. Meet the relinking or source obligations for any statically linked LGPL
   library.

Note for whoever assembles a release: the imported Linux DRM and i915 sources
are taken under their MIT option, and `tools/import/drm/fetch-*.sh` refuses to stage a
file that is GPL-2.0-only with no permissive alternative. That keeps the import
narrow; it is not a licence constraint now that b1nix is GPL-2.0-only itself.

*This document is an inventory of third-party licenses and does not constitute legal advice.*
