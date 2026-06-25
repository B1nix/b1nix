# Chromium port — technical build log

Running log of the work to compile **Chromium `content_shell`** for b1nix
(clang + Chromium's bundled libc++, Rust + Temporal on). Updated as the
compile-error grind proceeds. The V8-with-clang+libc++ path is already proven
(d8 runs on b1nix); this is the same toolchain applied to the full browser.

## How the grind runs

```
cd build/toolchain_build/chromium/src
ninja -C out/b1nix content_shell   # logs to smoke_run/chromium-clang-build.log
```

Loop: read the first `FAILED:` → find root cause → fix in **b1nix source**
(`userspace/include/*`, `userspace/libc/*`) with a glibc-compatible
implementation (honest no-op only where b1nix genuinely can't, with a comment) →
restage the header into the cross sysroot
(`build/toolchain_build/x86_64-b1nix/cross/x86_64-b1nix/include/`) → rebuild libc
if a `.c` changed → verify the failing TU compiles directly, then relaunch.

Fast single-TU verify (no full rebuild):
```
cd out/b1nix
eval "$(grep -A1 'FAILED.*<tu>.o' <log> | tail -1) -ferror-limit=0"
```

Build target count drops as more objects cache through (67k → ~60k so far).

## Architectural decisions

- **NSS disabled** (`use_nss_certs=false`, apply.sh Patch C26): b1nix has no NSS
  library; Chromium uses its built-in cert verifier. (The `PRInt64 long` vs
  b1nix `int64_t long long` clash was the symptom.)
- **crashpad: lss→libc shim, not a port** (Patch C27 + `tools/patches/chromium/
  files/lss_b1nix.h`; Patch C28 drops the out-of-process handler exe). crashpad
  can't *run* on b1nix (no ptrace/coredump/proc-task ABI); the shim forwards the
  ~12 `sys_*` it uses to b1nix libc so it only compiles+links. The remaining
  crashpad gaps were all standard Linux headers b1nix lacked (sys/user.h,
  linux/elf.h, linux/auxvec.h, asm/ldt.h, PTRACE_*), not an lss explosion.
- **ozone headless only** (`ozone_platform_headless=true`,
  `ozone_auto_platforms=false`): the evdev input backend is **not** built, so
  only the always-built DOM keycode converter needed `<linux/input.h>`.
- **ANGLE libpci off** (`use_libpci=false`, Patch C29): b1nix has no libpci and
  is headless/SwiftShader; ANGLE uses its non-libpci GPU-detection fallback.
- **No ALSA/PulseAudio** (Patch C31) and **no udev** (`use_udev=false`, C32):
  b1nix has no audio server / libudev; dummy audio backend + non-udev device
  enumeration.
- **-Werror dropped** (clang-filter `-Wno-error`): stock clang 22 raises more
  warnings than Chromium's bundled clang; the skew (memcall, uninitialized-
  const-pointer, …) is on vetted upstream code. Real errors still fail.
- **variations platform** (Patch C30): `b1nix`→`linux` for fieldtrial codegen.
- **lss excluded elsewhere** (Patch C-LSS, pre-existing): third_party/lss issues
  raw Linux syscall numbers, wrong on b1nix; excluded from rand_util/partition_alloc.
- **clang-filter** (`tools/v8-clang-filter.sh`): drops bundled-clang-only flags;
  injects `-Wno-unknown-warning-option` and `-Wno-nullability-completeness`
  (WebRTC's absl nullability turns the completeness check on under `-Werror`).

## Ported system-library headers (staged into the cross sysroot)

Chromium bundles most third-party libs but expects a few from the *system*.
b1nix already ports these, so their headers are staged into the cross sysroot
include dir (which the build `-isystem`'s); the libs link at the final relink:

- **xkbcommon** (M51 port) — `cp build/xkbcommon-b1nix/x86_64-b1nix/install/
  include/xkbcommon/*.h build/toolchain_build/x86_64-b1nix/cross/x86_64-b1nix/
  include/xkbcommon/`. Needed by `ui/events/.../xkb*` (no bundled copy in the tree).

## libc / header gaps filled (glibc-compatible unless noted)

Grouped by area; each shipped in a commit on `main` (no version bump during the
grind — bumps at milestone close).

### syscalls / process
- Complete lowercase `SYS_<name>` alias set + Linux-only maps (`rt_sigaction`→
  `SIGNAL`, `tgkill`→`KILL`, `exit_group`→`EXIT`; `clock_nanosleep`/`pkey_*`→
  ENOSYS sentinels). `__b1nix__`-correct `<asm/unistd_64.h>` forwarder.
- glibc `clone()` (trampoline → `fn(arg)` → `SYS_EXIT_THREAD`; `CLONE_PARENT_SETTID`
  emulated). `SYS_MINCORE` (164) — **real** kernel page-table residency walk.
- `mincore`, `getdtablesize` + `_SC_OPEN_MAX`, `mkstemps`, `posix_fallocate`
  (via ftruncate), `pread64`/`pwrite64`/`lseek64` (LFS aliases — off_t is 64-bit).

### signals / ucontext
- `<sys/ucontext.h>` `greg_t`/`gregset_t`; `<signal.h>` includes it (SA_SIGINFO
  handlers see `ucontext_t`/`REG_*`). `SIGIO`/`SIGPOLL` + `SI_USER/QUEUE/TIMER/
  MESGQ/ASYNCIO/SIGIO/TKILL`. `__WALL`/`__WCLONE`/`__WNOTHREAD`.

### networking
- `in6_addr` → glibc anonymous union (`s6_addr`/`s6_addr16`/`s6_addr32`, 3-brace
  init). `IN6_IS_ADDR_*` family. `EAI_NODATA`. `if_indextoname`.
- Full `RES_*` resolver flags + `MAXDNSRCH`; reentrant `res_ninit`/`res_nclose`
  + `res_state`. TCP keepalive (`SOL_TCP`, `TCP_KEEP*`, `TCP_USER_TIMEOUT`).
  IPv6/multicast sockopts + `ip_mreq`/`ip_mreqn`/`ipv6_mreq`/`group[_source]_req`.
  `IP[V6]_PMTUDISC_*`. `MSG_CONFIRM`, `SO_TIMESTAMP`/`SCM_TIMESTAMP`. `mmsghdr`
  + `sendmmsg`/`recvmmsg` (loop emulation). `<asm-generic/socket.h>` forwarder.

### files / misc
- `<elf.h>` GNU + core note types (`NT_GNU_BUILD_ID`, `NT_PRSTATUS`, …);
  `<linux/elf.h>` forwarder. `O_ACCMODE`/`O_TMPFILE`, `POLLRDHUP`, `MADV_REMOVE`,
  `PR_*_DUMPABLE`/`PR_SET_PTRACER`, `ETIME`. `off64_t`/`loff_t`, `ino64_t`.
- `struct dirent` `d_type` + `DT_*` (filled from the kernel entry type). 8-bit
  `SCN*` scanf macros. BSD `timeval` macros (`timercmp`/`timeradd`/…).
  `random()`/`random_r()` family (BSD TYPE_3 additive generator).
- New ptrace/graphics UAPI headers (compile-only — b1nix has none of these):
  `<sys/user.h>`, `<asm/ldt.h>`, `<linux/auxvec.h>`, `<linux/input.h>` +
  `<linux/input-event-codes.h>`, `<linux/sync_file.h>`, `<linux/dma-buf.h>`;
  `PTRACE_GET/SET_THREAD_AREA`.

### broad `-k 0` clusters (compiling Blink/content)
Running `ninja -k 0` past the link blocker revealed clusters with few root causes:
- clang-filter `-Wno-nontrivial-memcall` (ANGLE/Blink memset/memcpy on non-POD).
- `<inttypes.h>` full `PRI*`/`SCN*` `LEAST`/`FAST` families (vulkan-validation's
  `PRIuLEAST64` was an undefined-macro parse error).
- `mlock`/`munlock` (no-op success), `sincosf`/`sincos` decls.

### threads / math
- `coshf`/`sinhf`/`tanhf` declarations (openlibm provides the symbols).
- `PTHREAD_PROCESS_SHARED/PRIVATE` + `{cond,mutex}attr_{set,get}pshared` (b1nix
  ignores pshared). `pthread_{get,set}affinity_np` (no-op set / online-CPU get).

## Progress

base/ → net/ → crypto/ → third_party (webrtc, fontconfig, **crashpad**) → ui/ →
third_party (libsync, angle, **swiftshader** + marl). 35+ commits, all on `main`,
each verified by a direct TU compile before relaunch.

**STATUS (2026-06-24, corrected):** NOT fully compiled. The foundational
libraries compiled (base/net/crypto/ui + parts of third_party — roughly the
first ~15-20% of the graph). ninja then hit a **link** failure
(`libvk_swiftshader.so`) early in its order and stopped (`-k` default = halt on
first error), so it never reached the bulk: a `ninja -n` dry-run shows **~53,600
edges still pending**, including **all of Blink** (the renderer), `content/`, and
the final link. Two parallel fronts remain:
1. **Compile** the rest (Blink/content/…). The `.so` link blocker hides these —
   build with **`ninja -k 0`** so links can fail independently while the
   remaining objects compile and reveal their header/libc gaps.
2. **Link** (below) — needs the b1nix toolchain link work regardless.

## LINK phase

gn's link step runs the b1nix toolchain's `ld = clang++` with no `--target`/
sysroot in *ldflags*, so it links against the **host** glibc (`/usr/lib/libc.so.6`)
→ fails (e.g. `libvk_swiftshader.so`: errno TLS mismatch). For d8 this was fine —
gn produced static `.a`s, the final exe link failed as expected, and
`tools/v8/v8-link-d8.sh` hand-relinked d8 as a static b1nix ELF (crt0 +
linker-cxx.ld + libc++/libgcc/libm + whole-archived libb1nix, one --start-group,
after ranlib'ing the thin archives).

content_shell differs: `is_component_build=false` (static), but there are ~5
**b1nix-target `.so`s** (SwiftShader Vulkan ICD `libvk_swiftshader.so`, ANGLE
EGL/GLESv2) that gn links *during* the build — before the final exe — so the
hand-relink-at-the-end approach can't be reached. The host rust proc-macro `.so`s
(`clang_x64_for_rust_host_build_tools/*.so`) are host-toolchain and link fine.

**Plan:** teach the b1nix gn toolchain (`build/toolchain/b1nix/BUILD.gn`, source
in `tools/patches/v8/toolchain/b1nix/`) to drive real b1nix links for both
`solink` and `link` — encode the v8-link-d8.sh recipe (`--target`, b1nix sysroot,
crt0, linker script, lib group, whole-archived libb1nix; `-shared`+b1nix `libc.so`
for `.so`s). This replaces the per-binary hand-relink and lets gn link the whole
graph. (Alternative if that proves too costly: static-ize or drop the graphics
`.so`s and hand-relink content_shell like d8.)

## Kernel-rebuild blocker (for the run step, not the compile grind)

A full `make ARCH=x86_64` (kernel/ISO) currently dies in the curl port — a stale
generated Makefile references a deleted worktree path. Fix with
`rm -rf build/curl-b1nix` (forces a fresh curl reconfigure) when the kernel/ISO
is needed. The compile grind doesn't need the kernel.
