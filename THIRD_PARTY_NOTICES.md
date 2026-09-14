# Third-Party Software Notices & License Inventory

The GNU General Public License, version 2 only (`GPL-2.0-only`, see [LICENSE](LICENSE)) applies to original b1nix material. It does not replace or override third-party software licenses, and every component below is conveyed under its own terms. This document is the complete inventory of the third-party libraries, toolchains, runtimes and applications integrated or ported for b1nix.

---

## 1. Imported into the kernel — fetched at build time, never vendored

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
without exception, and are exactly what b1nix reimplements from scratch in
`kernel/include/linux` and `kernel/lkpi` — our own code, under b1nix's licence,
not Linux's.

---

## 2. What b1nix builds from source

Nothing third-party. The kernel, its tests and `b1cc` are b1nix's own; every
library and program on the image, musl included, is an Alpine binary package
(section 3).

---

## 3. Alpine packages shipped in the image

The image ships the binary packages pinned in
[`tools/packages/alpine.lock`](tools/packages/alpine.lock) — currently 255 of
them, verified by SHA-256 before installation. Each carries its own upstream
licence, recorded in Alpine's package index.

That list is **not duplicated here by hand**, because a hand-kept copy of a
machine-chosen set is exactly what drifted before: this document listed Mesa,
TinyGL and NetSurf long after they left the tree. Instead:

```sh
sh tools/packages/licenses.sh           # package, version, licence, description
sh tools/packages/licenses.sh --check   # fails if any package has no licence
```

reads the licence straight out of the package index that the fetch already
downloads, so it cannot fall behind the lock file.

The bulk of what a user sees — sway, wlroots, foot, seatd, the Wayland
libraries, GTK, Mesa's GL/EGL/GBM libraries, OpenSSL, curl, zsh, Dropbear,
FreeType, Fontconfig, HarfBuzz, Pixman, the image and video codecs, and Chromium
(which bundles Skia, BSD-3-Clause) — is in that set.

---

## 4. Binary Distribution & Compliance Requirements

b1nix's own code is GPL-2.0-only (see [LICENSE](LICENSE)), which already
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
are taken under their MIT option, and `tools/drm/fetch-*.sh` refuses to stage a
file that is GPL-2.0-only with no permissive alternative. That keeps the import
narrow; it is not a licence constraint now that b1nix is GPL-2.0-only itself.

*This document is an inventory of third-party licenses and does not constitute legal advice.*
