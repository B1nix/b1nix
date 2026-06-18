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
| Mojo IPC: unix-domain sockets + shared-memory FD passing (`SCM_RIGHTS`) | ✅ unix sockets + `SCM_RIGHTS` FD passing verified (Wayland passes buffers/keymap fds through it daily); M56 adds eventfd/epoll/timerfd for the message loop | **resolved** (Mojo core still to build, but the OS primitives exist) |
| GPU process: EGL/GBM/DRM, or ANGLE→Vulkan | OSMesa softpipe only — no DRI/EGL/GBM, no LLVMpipe JIT, no Vulkan | blocking for GPU path; pure software-raster Skia is poorly supported on new OSes |
| V8 JIT: W^X executable mmap, signals for GC/deopt, threads | mmap + full libstdc++ threads done (M55); jitless V8 still a milestone-sized port | very hard (jitless V8 possible but a large change) — **the main remaining engine blocker** |
| Ozone backend (X11 / Wayland / headless) | ✅ displayd is now a near-complete Wayland compositor (M49: xdg-shell, wl_data_device DnD, wl_touch, subcompositor, viewporter, presentation, xdg-decoration) | **largely resolved** — an Ozone/Wayland backend is now viable |
| Full libc++/libstdc++ with exceptions + RTTI + threads | ✅ done (M55): libstdc++ with exceptions, RTTI, thread-safe statics, std::thread, iostream, std::filesystem — proven by litehtml + cxx/iostream smokes | **resolved** |

## Decision

- **Vulkan: do not port** until a real application requires it (as the M53 item
  states). Nothing in the tree needs it today.
- **JS-heavy sites:** NetSurf already has Duktape (M54). That is the realistic
  vector, not Chromium.
- **If a modern engine is ever wanted:** lighter engines (litehtml, Servo-style
  components) are more realistic, but still require a full C++ runtime first.

## Conditions to revisit (any one removed → re-open this assessment)

1. ~~A complete C++ runtime (libstdc++/libc++ with exceptions + RTTI + threads)~~
   — ✅ **MET** (M55).
2. A real GPU path (EGL/GBM/DRM, or LLVMpipe JIT) beyond OSMesa-to-memory.
   — still open (M59 plans EGL over the M52/M53 virgl host-GPU path).
3. ~~Verified shared-memory FD passing (`SCM_RIGHTS`) over unix sockets.~~
   — ✅ **MET** (Wayland; M56 event-loop primitives in progress).

**Update (post-M55, M49 compositor, M56 in progress):** 2 of the 3 original
revisit-conditions are now met, plus the Ozone/Wayland-backend blocker is largely
resolved. The assessment is **no longer "blocked on whole subsystems b1nix
lacks" across the board** — the remaining hard blockers narrowed to: **(a) V8**
(even jitless is a milestone-sized port — the realistic next gate), **(b) a real
GPU path** (EGL/GBM, M59), **(c) the sandbox** (seccomp/namespaces, M63), and
**(d) the sheer 35M-LOC GN/Ninja build + adding b1nix as a GN target**. Verdict
stays **not pursued as full Chromium** — but the path is now "port V8 jitless and
build a content_shell-class target," not "b1nix lacks the basics." Lighter
engines (litehtml already runs, M55) remain the pragmatic vector.
