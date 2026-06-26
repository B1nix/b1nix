# Chromium port — technical debt / deferred work

What was made to **compile/link** but is a stub, no-op, approximation, or
compile-only-not-functional. These don't block the build; they're what needs
real work for Chromium to actually **run and work** on b1nix. Companion to the
"what's done" log in [`chromium-port-log.md`](chromium-port-log.md).

Status legend: 🔴 must fix before it runs · 🟡 works degraded, fix for quality ·
🟢 acceptable for b1nix (likely never needs real work).

## Post-build plan (agreed sequence)

After content_shell compiles + links + runs, work the backlog in this order:
1. **Link phase** — run `tools/v8/chromium-link.sh` (relink the 4 graphics `.so`s
   + content_shell as b1nix ELFs); boot headless in QEMU.
2. **Close the debts** — apply the designed kernel patches (real `SYS_FALLOCATE`/
   `CLOCK_NANOSLEEP`/`MLOCK`/`MUNLOCK`, real CPU-count, real `if_nameindex`),
   kernel build + `tests/smoke.sh x86_64`, bump v0.69.10. (Needs `rm -rf
   build/curl-b1nix` first — the ISO build blocker.)
3. **Sweep every disabled feature/flag** and sort each into one of three buckets
   — enable-for-free, needs-a-port-then-enable, or keep-off. Then port/enable
   accordingly. Honest triage (revisit each once the browser runs):

   **A. ✅ Enable for free (no port — just flip / wire existing b1nix capability)**
   - **GPU/GL acceleration** — once the SwiftShader/ANGLE `.so`s link + virtio-gpu
     is wired (b1nix already has virtio-gpu from M52/VirGL). Flip to HW-accel.
   - **real CPU-count thread pools** — lands with the CPU-count debt (already real).
   - **`-Werror`** — restore selectively after the clang-skew warnings are
     addressed per-warning (quality, not correctness).

   **B. 🔧 Needs a small port/shim, THEN enable (b1nix HAS the capability)**
   - **libpci** (`use_libpci`) — b1nix's kernel enumerates PCI; a thin `libpci`
     shim over that could feed ANGLE real GPU info. LOW value (the non-libpci
     fallback works), but doable.
   - **libudev** (`use_udev`) — b1nix has its own device model (`/dev`+sysfs); a
     minimal `libudev` shim over it would enable udev-driven device monitoring.
     MEDIUM (the non-udev fallback works).
   - **crashpad** — would need real `ptrace`/coredump/proc-task in the kernel
     (a genuine kernel feature, large). Only if crash reporting is wanted.

   **C. 🚫 Keep OFF (no backend AND porting isn't worth it / impossible now)**
   - **NSS** — built-in cert verifier is the modern default and correct; porting
     NSS (huge) is pointless.
   - **kerberos/GSSAPI** — niche (Negotiate auth); porting MIT-krb5 is huge.
   - **ALSA/PulseAudio** — b1nix has **no audio hardware/driver/server** at all;
     enabling needs a whole audio subsystem (HDA driver + mixer + an ALSA/dummy
     shim) — a real OS milestone, not a flag. Headless browser needs no audio.
   - **sandbox/seccomp** — real seccomp-BPF + namespaces in the kernel is a
     security milestone, **M63**. **Build strategy = compile-as-dead-code, NOT a
     stub.** First considered `use_seccomp_bpf=false`, but rejected it: the
     `sandbox/policy/linux` layer (`bpf_*_policy_linux.cc`, `sandbox_seccomp_bpf_
     linux.cc`) is added unconditionally under `if (is_linux)`, not gated on the
     flag, and `SandboxLinux`'s API even returns `bpf_dsl::ResultExpr` — so a no-op
     stub is infeasible. b1nix runs `--no-sandbox`, so the whole sandbox is dead at
     runtime; it only needs to **compile + link**. Make it compile:
     - **C38** — `linux_syscalls.h` skips `#include <sys/syscall.h>` on b1nix. Root
       cause of every `__NR_*` collision/undeclared: b1nix `<syscall.h>` aliases
       `__NR_foo→SYS_FOO` (its own numbers); that leaked in and defeated the seccomp
       headers' `#if !defined(__NR_foo)` canonical-Linux guards (SYS_FSYNC==18
       dup-case, etc.). Skipping it → pure Linux `__NR_*` (fine; dead code).
     - **C39** — drop the `linux_signal.h` `LINUX_SIGHUP==SIGHUP` static_asserts on
       b1nix (compile-time-only; b1nix signal numbers differ by design; seccomp
       never runs here).
     - **Residual dead-code source patches (post-build, iterative):** `trap.cc`
       uses `ucontext::uc_sigmask` + `siginfo_t::_sifields`, which b1nix's structs
       lack. Do NOT change b1nix `ucontext`/`siginfo_t` layout (kernel+libc must
       agree) — patch the dead `trap.cc` handler for b1nix instead. Plus constants
       `MAP_POPULATE` (sys/mman.h) and `TCGETS` reachability for the policy ioctl
       filters. Verify the full seccomp gap set (sandbox_bpf.cc, sigsys_handlers.cc)
       after C38/C39 land + rebuild.
     This keeps the real sandbox code present (dead) so content/zygote link cleanly;
     **M63 = wire it to real b1nix seccomp/namespaces.**
   - **V4L2 camera** — no camera hardware.

## Post-build header batch (one clean-rebuild blast, do NOT do mid-grind)

These are real b1nix libc/header gaps surfaced by the Chromium grind. Each edits a
*widely-included* sysroot header (`fcntl.h`/`unistd.h`/`sys/socket.h`/`sched.h`/
`time.h`), so adding them mid-grind would dirty ~all compiled `.o`. Batch them
into ONE post-`-k 0` edit + rebuild. All are genuine and correct to add:
- **`fcntl.h`** — `O_PATH` (0x200000; retire the C37 source-local stopgap),
  `O_DIRECT`, `O_SYNC`/`O_DSYNC`, `O_NOATIME`, `O_LARGEFILE` (0 — off_t already
  64-bit), `O_ASYNC`. (broker_file_permission.cc consumes these.)
- **`sys/socket.h`** — `MSG_CMSG_CLOEXEC` (broker_client.cc).
- **`sched.h`** — `CLONE_VFORK` (credentials.cc).
- **`limits.h`** — `HOST_NAME_MAX` (64; components/policy cloud_policy_util.cc
  gethostname buffer). Standard POSIX limit b1nix simply lacks.
- **`wchar.h`** — `wcscasecmp` (googletest gtest.cc:2217). Small real libc gap.
- **`dlfcn.h`** — `RTLD_NODELETE` (0x1000; services/audio audio_sandbox_hook +
  content/common gpu_pre_sandbox_hook). b1nix has every other RTLD_* flag. Define
  it real; ld-b1nix.so can honor "don't unload on dlclose" later (for now the
  sandbox-preload hooks are inert under --no-sandbox).

**Test/benchmark infra (verify-not-linked, do NOT port speculatively):**
- **google_benchmark** — `benchmark.cc`/`sysinfo.cc` want `<sys/personality.h>`,
  `perf_counters.cc` wants `<linux/perf_event.h>`. These are a whole Linux perf
  subsystem b1nix lacks and will never run a microbenchmark against. It leaked
  into the content_shell subgraph (25 edges). **Do not port perf_event/personality
  (YAGNI).** At link phase, confirm content_shell doesn't actually link benchmark;
  if it does, gate `google_benchmark` out of b1nix's content_shell deps (GN) or
  drop minimal stub headers. Same for googletest beyond wcscasecmp — testonly.
- **`unistd.h` + libc** — real `readlinkat()` (proc_util.cc; b1nix has `readlink`,
  kernel likely has the *at variant or AT_FDCWD path-build suffices).
- **`linux/net.h`** — provide a minimal b1nix `<linux/net.h>` (the `SYS_*`
  socketcall enum) OR patch the 2 broker consumers; missing header.
- **`linux/usb/ch9.h`** — provide the standard USB 2.0 chapter-9 descriptor UAPI
  header (new file in the b1nix sysroot; `usb_device_handle_usbfs.cc` needs ~17
  `usb_*_descriptor`/`USB_*` symbols). usb_device_linux.cc references
  `UsbDeviceHandleUsbfs`, so GN-excluding usbfs cascades — provide the header
  instead (link-safe). WebUSB is inert on b1nix (no USB stack); compile-only.
  New-file add → non-blasting (only usbfs.cc recompiles).
- **`time.h` `B1nixTimePtr`** — the wrapper-struct param on `gmtime`/`localtime`
  makes `&gmtime` un-assignable to a `struct tm*(*)(const time_t*)` fn-pointer
  (breaks sandbox libc_interceptor.cc:281). Give `gmtime`/`localtime` a real
  `const time_t*` overload taking the address cleanly, or revert the wrapper to a
  plain `const time_t*` param (check which ports needed the unsigned-int*/nullptr
  conversions first). Until then, a source shim in libc_interceptor (dead code on
  b1nix — sandbox off) is the build stopgap.

4. **Third-party ports sweep** — confirm which libs Chromium actually consumes
   from the *system* vs its own bundled copies. Chromium bundles most
   (freetype/harfbuzz/zlib/png/icu/protobuf/…), so b1nix's existing ports aren't
   used by Chromium except where it expects a system lib (so far only
   **xkbcommon**, staged). Walk the GN `use_system_*` flags; for any that point
   at an absent system lib, prefer Chromium's bundled copy (a GN flip, like the
   expat C23 patch) over porting. Only port a system lib if Chromium can't bundle
   it AND b1nix lacks it.

## Compile-only subsystems (don't function at runtime)

- **crashpad** 🟢 — built via the lss→libc shim (`tools/patches/chromium/files/
  lss_b1nix.h`), out-of-process handler dropped. b1nix has no ptrace / coredump /
  proc-task ABI, so crash capture can't work. *Defer:* keep disabled, or a real
  crash-reporter port later. Acceptable: a browser runs fine without crash dumps.
- **evdev input** (`<linux/input.h>` etc.) 🟢 — headers are compile-only; ozone
  is headless so the evdev backend isn't built. Real input comes through b1nix's
  own stack, not evdev. *Defer:* only matters if a non-headless ozone platform
  is added.
- **dma-buf / sync_file** (`<linux/dma-buf.h>`, `<linux/sync_file.h>`) 🟡 — header
  layouts only; b1nix has no DRM buffers or sync fences. GPU buffer sharing /
  fencing won't work. *Defer:* needed if/when real GPU compositing is wired.
- **ptrace headers** (`<sys/user.h>`, `<asm/ldt.h>`, `PTRACE_*`) 🟢 — layouts/
  constants only; b1nix `ptrace()` returns ENOSYS. Only crashpad used them.

## Syscall stubs / approximations

- **`posix_fallocate` / lss `sys_fallocate`** 🟡 — no real allocation; just
  `ftruncate` to the size (or no-op in the crashpad shim). Won't pre-reserve
  blocks, so a later write can still ENOSPC. *Fix:* a real `SYS_FALLOCATE`.
- **`sys_sigtimedwait`** (crashpad shim) 🟢 — returns ENOSYS; only the crashpad
  exception handler uses it (never runs on b1nix).
- **`SYS_clock_nanosleep` / `SYS_pkey_*`** 🟡 — out-of-range sentinels → ENOSYS;
  callers take fallback paths. *Fix:* real `clock_nanosleep` for precise sleeps;
  pkeys are unlikely to ever matter.
- **`sendmmsg`/`recvmmsg`** 🟡 — libc loop over `sendmsg`/`recvmsg`, not an atomic
  batch syscall. Correct results, lower throughput. *Fix:* batch syscall if perf.
- **Linux-only signal/syscall name maps** (`rt_sigaction`→`SIGNAL`, `tgkill`→
  `KILL`, `exit_group`→`EXIT`) 🟡 — `tgkill` drops the tgid (ok: b1nix tids are
  unique); `rt_sigaction` takes a Linux-layout struct that differs from b1nix
  `struct sigaction`, so the between-fork/exec handler reset is best-effort.
- **`O_PATH`** 🟢 — b1nix `<fcntl.h>` has no `O_PATH`. Patch C37 defines it
  locally (real Linux value `0x200000`) only in the one consumer,
  `dbus/xdg/file_transfer_portal.cc` (a headless-dead xdg portal), to avoid
  dirtying the widely-included `fcntl.h` mid-grind. *Fix (closeout):* add real
  `O_PATH` to `userspace/include/fcntl.h` on the next clean rebuild; the kernel
  currently ignores the unknown open bit (access mode 0 == `O_RDONLY`), which is
  fine until a real path-only-fd semantics is wanted (large; not needed headless).

## Debt fixes DESIGNED (patches ready, pending kernel-build + smoke)

A review pass produced exact patches for the b1nix-side items below. They are
**NOT yet applied/committed** — each is a real kernel change (4 new syscalls,
SYS 165-168) that MUST pass `tests/smoke.sh x86_64` first, and verification is
currently blocked (Chromium compile occupies the machine; kernel/ISO build needs
`rm -rf build/curl-b1nix`). Apply as one verified unit; bump to v0.69.10.
- **`posix_fallocate`/`fallocate`** → real `SYS_FALLOCATE`→`vfs_fallocate` (writes
  zeros through the range so ext4/ext2 allocate real blocks; FALLOC_FL_KEEP_SIZE
  honored; deallocating modes -EOPNOTSUPP). No more silent ENOSPC-later.
- **`sysconf(_SC_NPROCESSORS_ONLN)`** → real online-CPU count (count bits from the
  existing `SYS_SCHED_GETAFFINITY`/`CPU_COUNT` — NO new syscall needed). 🔴→done.
- **`mlock`/`munlock`** → real `SYS_MLOCK`/`MUNLOCK` pinning pages against the
  swap-eviction clock. **CORRECTION: b1nix DOES swap** (eviction.c + swap.c, M14
  swap-smoke) — the no-op below was WRONG; faulting-resident + a `locked` flag in
  the eviction ring is the correct fix.
- **`clock_nanosleep`** → real interruptible `SYS_CLOCK_NANOSLEEP` (TIMER_ABSTIME,
  per-clock; ENOSYS sentinel retired). 100 Hz tick granularity remains.
- **`if_indextoname`/`if_nametoindex`** fixed (was: any string→1) + `if_nameindex`
  added; kept eth0=index 1 to stay consistent with the kernel's SIOCGIFINDEX.

## Thread / scheduling approximations

- **`pthread_{cond,mutex}attr_setpshared` = ignored** 🟡 — PTHREAD_PROCESS_SHARED
  accepted but not honored (no cross-process shared sync). *Fix:* shared-memory
  futex if any port needs cross-process condvars/mutexes.
- **`pthread_setaffinity_np` = no-op** 🟡 — threads aren't pinned;
  `getaffinity` reports the online-CPU set. *Fix:* real affinity if scheduling
  perf needs it.
- **`sysconf(_SC_NPROCESSORS_ONLN)` = 1** 🔴 — reports a single CPU, so Chromium
  sizes its thread pools / raster threads for 1 core. *Fix:* wire to the real
  online-CPU count (b1nix is SMP) — this directly limits browser parallelism.

## Networking approximations

- **`if_indextoname` = single interface** 🟡 — index 1 → "eth0", else ENXIO
  (mirrors the existing `if_nametoindex` stub). *Fix:* real interface enumeration
  if multi-NIC / scoped-IPv6 display matters.
- **`_res` resolver state** 🟢 — `res_ninit` parses only `nameserver` lines from
  /etc/resolv.conf (no `search`/`options`); display/parse-only. b1nix resolves
  via `SYS_NET_DNS`, so this is enough.

## Link phase (the current frontier)

- **b1nix gn toolchain can't link** 🔴 — `link`/`solink` (command bodies live in
  the *shared* `build/toolchain/gcc_toolchain.gni`, driven by `ld = cxx` in
  `build/toolchain/b1nix/BUILD.gn`) run `clang++` with no `--target`/sysroot →
  host glibc → `errno` TLS-vs-non-TLS mismatch against `/usr/lib/libc.so.6`.
  - *Unblock (crutch, in use):* `tools/v8/chromium-link.sh` relinks off gn's
    `.rsp`s with the b1nix recipe (crt0 + linker-cxx.ld + lib group + whole-
    archived libb1nix; `-shared`+libc.so.1 for `.so`s). A standalone post-build
    step — proves the recipe end-to-end with zero gn-regen risk.
  - *Proper close (post-Chromium debt sweep):* Option A — set `ld =` a small
    b1nix linker-driver shim in `b1nix/BUILD.gn` (one line; touches only
    link/solink, NOT `cxx`, so it does **not** recompile the 16k objects —
    confirmed). Then `ninja content_shell` links natively and
    **`chromium-link.sh` is deleted**. Do this only after the script proves the
    recipe — integrating an unproven recipe into gn is harder to debug.
- **b1nix-target `.so`s** 🔴 — SwiftShader Vulkan ICD + ANGLE EGL/GLESv2 are
  loadable `.so`s; need real b1nix shared-object linking (PIE + ld-b1nix.so), or
  static-ize/disable them. b1nix supports `.so` (M30 PIE, ld-b1nix.so) but no
  `.so` has been built *by gn* before.

## Build-side debt

- **Kernel/ISO build blocked** 🔴 — `make ARCH=x86_64` dies in the curl port
  (stale generated Makefile → deleted worktree path). *Fix:* `rm -rf
  build/curl-b1nix` before the run/ISO step (forces a fresh curl reconfigure).
- **No version bump during the grind** — port commits don't bump
  `B1NIX_VERSION_STR`; bump once at milestone close.

## Verified-real (NOT debt — listed so they're not re-flagged)

`clone()`, `SYS_MINCORE` (real page-table walk), `random()`/`random_r` (BSD
TYPE_3), `dirent.d_type` (from real kernel type), `in6_addr` union, NSS-off +
built-in cert verifier, all the pure constant/struct/macro additions.
