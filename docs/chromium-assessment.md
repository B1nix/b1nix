# Chromium on b1nix — feasibility assessment (M53 closeout)

**Status: `stubbed`.** This document is where the M53 "assess Chromium" item is
tracked and will be closed. Verdict so far: **not pursued** — Chromium is
blocked on whole *subsystems* b1nix lacks, not on a handful of syscalls. Each
blocker is comparable in size to everything done across M50–M53. Kept as a
living record so the decision can be revisited if a blocker is independently
removed.

## Scale

- ~35M lines of C++ (C++17/20), GN + Ninja, ~100 GB build tree, hours to build
  even on large hosts.
- Cross-compiling to a brand-new OS target means adding b1nix to GN's
  `build/config`, `base/`, `sandbox/`, `//net`, Ozone — upstream does not
  support this; it is itself a multi-month effort.

For comparison: NetSurf (the engine b1nix *does* run, M53) is three orders of
magnitude smaller and simpler.

## Blockers vs. what b1nix has

| Chromium needs | b1nix today | Severity |
|---|---|---|
| Multiprocess: zygote + sandbox (seccomp-bpf, user/PID namespaces, setuid sandbox) | no seccomp, no namespaces | blocking (`--no-sandbox` drops the sandbox, not the process model) |
| Mojo IPC: unix-domain sockets + shared-memory FD passing (`SCM_RIGHTS`) | unix sockets yes; `SCM_RIGHTS` FD passing unverified | likely blocking |
| GPU process: EGL/GBM/DRM, or ANGLE→Vulkan | OSMesa softpipe only — no DRI/EGL/GBM, no LLVMpipe JIT, no Vulkan | blocking for GPU path; pure software-raster Skia is poorly supported on new OSes |
| V8 JIT: W^X executable mmap, signals for GC/deopt, threads | mmap yes; libstdc++ threads landed with effort | very hard (jitless V8 possible but a large change) |
| Ozone backend (X11 / Wayland / headless) | displayd is "Wayland-shaped", not full protocol | needs a new Ozone platform layer |
| Full libc++/libstdc++ with exceptions + RTTI + threads | HarfBuzz built *without* libstdc++ and with `-fno-exceptions` | Chromium cannot build this way — needs a complete C++ runtime |

## Decision

- **Vulkan: do not port** until a real application requires it (as the M53 item
  states). Nothing in the tree needs it today.
- **JS-heavy sites:** NetSurf already has Duktape (M54). That is the realistic
  vector, not Chromium.
- **If a modern engine is ever wanted:** lighter engines (litehtml, Servo-style
  components) are more realistic, but still require a full C++ runtime first.

## Conditions to revisit (any one removed → re-open this assessment)

1. A complete C++ runtime (libstdc++/libc++ with exceptions + RTTI + threads)
   usable across ports.
2. A real GPU path (EGL/GBM/DRM, or LLVMpipe JIT) beyond OSMesa-to-memory.
3. Verified shared-memory FD passing (`SCM_RIGHTS`) over unix sockets.

Until then this stays `stubbed` and the M53 item is considered assessed.
