# The DRM core import (M101)

Where the DRM core comes from, why there are two of them, and the one rule that
makes the arrangement affordable.

## The decision

**Upstream `drivers/gpu/drm` is imported verbatim and never edited.** Everything
it stands on — the `linux/*` headers it includes, the primitives behind them — is
b1nix's own, written from scratch in `kernel/include/linux` and `kernel/lkpi`,
under b1nix's own license rather than Linux's.

**M100's own DRM core stays, for virtio-gpu.** That is two cores in one kernel,
which is normally a smell. It is allowed here for exactly one reason: one of them
is never edited, so it costs no maintenance — a new upstream release is a new
tarball, not a re-port. The moment someone patches the staged tree, that
justification is gone.

The corollary is the rule the whole milestone rests on:

> **A patch to anything under the staged tree is a bug in the shim, not a fix
> here.** There is deliberately no patch directory and no place to put one.

## What is staged, and what is not

`tools/drm/fetch-drm-core.sh` stages roughly 4.6 MB into
`build/src/drm-core-<version>/`:

- `drivers/gpu/drm/*.[ch]` — the core itself
- `include/drm`, `include/uapi/drm`
- `include/linux/hdmi.h`, `include/video/nomodeset.h`,
  `drivers/video/{hdmi,nomodeset}.c` — MIT, so imported rather than rewritten

**Nothing else from `include/linux` is staged.** Those headers are GPL-2.0
without the syscall-note exception, and reimplementing them is the point of the
shim. `include/uapi/drm` carries `GPL-2.0 WITH Linux-syscall-note`, whose
exception explicitly permits non-GPL use of the interface definitions.

The vendor drivers (i915, amdgpu, nouveau) are M102's business and are staged by
their own milestones; pulling all 477 MB of `drivers/gpu` here would import code
nothing builds.

## The pin

The release is pinned the way `tools/ports/*` pin theirs: a version variable and
a SHA-256, **verified before extraction** — a truncated or substituted tarball
must never reach the tree, and "it built fine" is not a checksum. Bumping
`LINUX_VERSION` is a deliberate act that also requires a new hash.

Currently pinned: **Linux 6.6**, `d926a06c63dd8ac7df3f86ee1ffc2ce2a3b81a2d168484e76b5b389aba8e56d0`.

The staged tree carries a `B1NIX-IMPORT` file recording all of that next to the
source, so a stray copy can still be identified, and a `B1NIX-OBJECTS` file
listing what is built.

## What gets built

Not all 92 files in `drivers/gpu/drm` are meant to compile — upstream's Kconfig
selects them, and `drm_of.c` is device-tree-only. The honest denominator is
upstream's own `drm-y` list, read out of its Makefile rather than chosen here:
**41 objects**, plus `drm_kms_helper-y` (the atomic modeset, probe and rectangle
helpers every vendor driver builds on) and the MIT hdmi infoframe library — 58
objects in total.

Splitting `drm.ko` from `drm_kms_helper.ko` is a module boundary; b1nix links the
whole thing into the kernel, so the boundary buys nothing and leaving the helpers
out would only mean discovering they were needed later.

## The boundary rule

**A translation unit compiling imported source must not see b1nix's own
headers.** b1nix and Linux share names — `spinlock_t`, `kmalloc`, `spin_lock`,
`ERR_PTR`, `mutex` — with different meanings. Rescuing each one with a macro
works until include order shifts, and it is a *class* of bug rather than a list:
`#define mutex lkpi_mutex` rewrote struct *member* names (`drm_plane::mutex`) and
surfaced as a pointer-type error in another file.

So `kernel/include/lkpi/env.h` declares the kernel services the `linux/*` shims
need, and `kernel/lkpi/env.c` is the only file where both naming worlds meet.
`<b1nix/types.h>` is the sole exception, and only because it is nothing but
typedefs.

## Traps worth remembering

- `tar --wildcards "drivers/gpu/drm/*.c"` matches `i915/*.c` too — GNU tar lets
  `*` cross `/`. Needs `--no-wildcards-match-slash` (478 MB → 4.6 MB).
- The kernel's normal CFLAGS have no `-nostdinc`, so the host's
  `/usr/include/linux/*` silently satisfied `linux/types.h`. Imported source is
  built with `-nostdinc` plus only clang's own resource include.
- `EXPORT_SYMBOL(x);` expanding to nothing leaves a stray `;` that C11 rejects,
  and the error points at the symbol rather than the macro. It expands to a
  harmless declaration instead.
- Imported objects depend on the shim, and the staged `.c` files never change —
  so without `-MMD -MP` on the import rule, `make` never rebuilds them after a
  shim fix, and the kernel runs a mixture of old and new headers that no state of
  the tree corresponds to. The import rule generates and includes depfiles for
  this reason; it is not optional.
- A cast is not a layout guarantee. An earlier `INTERVAL_TREE_DEFINE` forwarded
  to lkpi's own interval tree by casting the caller's object to that tree's node
  type. `struct drm_mm_node` begins with `color`, `start`, `size` and `mm` — its
  `rb_node` is six fields in — so the erase path wrote tree pointers over the
  allocator's bookkeeping, and the damage surfaced later as a cycle between two
  unrelated nodes in a *different* tree. The macro now generates a real typed
  implementation over the members it is given.
