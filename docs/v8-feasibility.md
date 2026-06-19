# M58 — V8 (jitless / Ignition-only) feasibility probe

**Verdict: NO-GO for now. Multi-month effort. Do NOT start a V8 port.**
**Pragmatic alternative already in the tree: Duktape** (ships and runs inside
NetSurf, M54). If "run some JS on b1nix" is the real goal, that is the cheap win.

This is an honest probe, not an attempt at the port. It establishes *concretely*
what porting `--jitless` V8 to b1nix would require, what the first hard blockers
are, and roughly how big each is. Evidence is from the host toolchain and the
b1nix sysroot/libc as they exist today (v0.55.x).

---

## TL;DR scorecard

| Dimension | State | Severity for jitless V8 |
|---|---|---|
| GN build system on host | **absent** (no real `gn`, no `depot_tools`) — only a Qt6-vendored `gn 6.11.1.qtwebengine` at `/usr/lib/qt6/gn`, not the Chromium one | blocking (infra) |
| b1nix as a GN/V8 `target_os` | **does not exist upstream** — V8 only knows linux/mac/win/fuchsia/etc. Must be added from scratch to `v8/build/config`, `toolchain/`, and V8's `base/platform/` | **the dominant blocker (months)** |
| Host snapshot tooling (`mksnapshot`, `torque`, `bytecode_builtins_list`) | buildable on host (Linux x86_64, clang 22, ninja 1.13, python 3.14) but must be wired as a *host* toolchain while target is b1nix | hard (cross/host split) |
| C++ toolchain fit | GCC 13.2 `x86_64-b1nix-g++`, C++17 **and** C++20 OK, libstdc++ with exceptions/RTTI/threads (M55) | **OK** — V8 builds fine with GCC; clang/libc++ not required |
| libc / pthread / futex / TLS | pthread keys + cond + sem + futex (M29) + `SYS_SET_TLS` + `SYS_CLONE` present | mostly **OK** |
| Signals for safepoints | `SA_SIGINFO`, `siginfo_t`, `si_addr`, `sigaction`, sigprocmask present | **OK** |
| `mmap`/`mprotect` | present, incl. `PROT_EXEC`, `MAP_ANONYMOUS`, `MAP_FIXED`, `memfd_create`, `SYS_MPROTECT` | **OK for jitless** |
| `madvise` / `mincore` / `MAP_NORESERVE` | **missing** (no syscall, no libc, flag not defined) | hard (V8 memory mgr leans on these) |
| `sigaltstack` | **missing** (no syscall, no libc) | medium (V8 installs a guard-stack handler) |
| Disk / RAM for build | 79 GB free on `/home`; V8-only tree is ~10–15 GB (not the full ~100 GB Chromium) | OK |

Net: the **OS-primitive** side is in surprisingly good shape (most of what
jitless V8 needs at runtime is already there). The wall is the **build-system
port**: making V8's GN graph and `base/platform/platform-*.cc` understand a
brand-new OS that upstream has never heard of.

---

## 1. Build-system reachability

**Host inventory (this dev box, Arch Linux x86_64):**

```
ninja      1.13.2                          ✅
node       v20.20.2 (nodejs-lts-iron)      ✅ (V8 d8 host fallback, not the b1nix build)
clang      22.1.6                          ✅
python3    3.14.5                          ✅
gn         ❌ no Chromium gn; only /usr/lib/qt6/gn (6.11.1.qtwebengine) — Qt's fork
depot_tools❌ absent (no fetch/gclient)
system V8  ❌ no d8, no libv8
cross g++  ✅ build/toolchain_build/x86_64-b1nix/cross/bin/x86_64-b1nix-g++ (GCC 13.2.0)
```

So step zero is: install `depot_tools`, `fetch v8`, sync (this alone pulls
several GB and a vendored toolchain). The Qt6 `gn` is a *fork* and is not a
drop-in for V8's `gn` revision — V8 pins a specific `gn` from `depot_tools`.

**Is adding b1nix a config tweak or a from-scratch port? — From-scratch.**
Upstream V8 has no `is_b1nix`. Adding a new OS is not a flag; it is:

1. `v8/build/config/BUILD.gn` + `//build/config/<os>/` — a new OS dir with
   compiler/linker/sysroot config, the `target_os == "b1nix"` branches, default
   libs, and crt handling. (`//build` is the shared Chromium build module V8
   vendors; touching it is exactly the "add b1nix as a GN target" work the
   chromium-assessment flags as multi-month — V8 is the *small* member of that
   family but still drags the same `//build`.)
2. A GN **toolchain** definition pointing at `x86_64-b1nix-gcc/g++/ar/ld` with
   the right flags, plus a separate **host** toolchain (Linux clang) so
   `mksnapshot`/`torque`/`bytecode_builtins_list` build *for the host* and run
   during the build to emit the snapshot + generated builtins for the target.
3. V8's own `BUILD.gn` `v8_current_cpu`/`v8_target_os` plumbing and the
   `//v8/src/base/platform` selection (see §3).
4. Dozens of `if (is_linux)` / `is_posix` sites across `//build` and V8 that
   silently assume Linux-isms; b1nix is POSIX-ish but not Linux, so the common
   trick is to alias b1nix to the `is_posix && !is_linux` path and then patch
   each spot that still reaches for a Linux-only header/syscall.

This is the same shape of work as M61 ("Add b1nix to GN/Ninja") in the roadmap,
just scoped to the V8 subtree — realistically **weeks of GN plumbing before a
single object file of V8 proper compiles**, then more weeks chasing
platform-abstraction breaks.

---

## 2. Toolchain fit

**Good news:** V8 compiles cleanly with GCC (Linux distros build it with GCC);
the clang/libc++ expectation is a *default*, not a hard requirement. b1nix's
`x86_64-b1nix-g++` is GCC 13.2 and supports **both C++17 and C++20** (verified),
and M55 delivered libstdc++ with exceptions, RTTI, thread-safe statics,
`std::thread`, iostream and `std::filesystem`. That clears what historically was
the #1 blocker for any C++ engine on a hobby OS.

**Gaps that V8 will hit at compile/link time** (independent of the GN work):

- **`madvise` + `MADV_*`**: not declared in `<sys/mman.h>` and no syscall.
  V8's `OS::DiscardSystemPages`, `OS::DecommitPages`, and the page allocator use
  `madvise(MADV_FREE/MADV_DONTNEED)`. Stubbing as no-ops is *functionally*
  possible (memory just is not returned to the OS) but each call site must be
  patched in `platform-posix.cc`.
- **`MAP_NORESERVE`**: not defined. V8 reserves large address ranges with
  `PROT_NONE + MAP_NORESERVE` and commits lazily. b1nix `mmap` would need a
  no-reserve semantic (or the reservation path rewritten to reserve-then-mprotect
  in chunks). This touches the kernel VMM, not just libc.
- **`sigaltstack`**: absent. V8 installs a stack-overflow / signal handler and
  expects an alternate signal stack on POSIX. Needs a kernel syscall + libc
  wrapper, or the handler-install path patched to skip it.
- **`mincore`**: absent — used in a few diagnostics; low priority, stub.

**Present and adequate:** `pthread_key_create/setspecific` (TLS), `pthread_cond_*`
(incl. `timedwait`), `sem_*`, `SYS_FUTEX` (M29), `SYS_SET_TLS`, `SYS_CLONE`,
`clock_gettime(CLOCK_MONOTONIC)`, `nanosleep`/`clock_nanosleep`,
`gettimeofday`, `<sys/ucontext.h>`.

---

## 3. Runtime blockers — jitless specifically

Jitless V8 (`--jitless`, Ignition bytecode interpreter, no TurboFan/Sparkplug)
is the right scope because it **removes the hardest OS requirement**: no
executable code generated at runtime → **no W^X executable mmap, no RWX→RX
re-protect dance, no code-space `mprotect(PROT_EXEC)` churn.** That is genuinely
the thing b1nix would struggle with, and jitless sidesteps it.

What jitless **still needs**, mapped to b1nix:

| Need | b1nix today | Note |
|---|---|---|
| Large reserved heap + GC | `mmap`/`munmap`/`mprotect` ✅; `MAP_NORESERVE` ❌; `madvise` ❌ | The reservation + lazy-commit + page-discard pattern needs the §2 gaps closed (or patched out, at a memory-efficiency cost). **This is the #1 runtime blocker.** |
| Signal-based interrupt / safepoint | `sigaction`+`SA_SIGINFO`+`si_addr` ✅, `sigprocmask` ✅ | Adequate. `sigaltstack` missing but the guard-stack handler can be skipped. |
| Threads (background GC/compile, even jitless has concurrent marking) | pthreads + futex (M29) ✅ | OK. (`--single-threaded` exists as a further fallback.) |
| Snapshot blob | built on host by `mksnapshot` (host toolchain), embedded as data ✅ | No runtime W^X needed — blob is *data*, deserialized into the (non-exec) heap in jitless. OK. |
| Time / monotonic clock | `clock_gettime(CLOCK_MONOTONIC)` ✅ | OK. |
| `getrandom` for hash seed | `SYS_GETRANDOM` ✅ | OK. |

So the runtime story for **jitless** is: **one real blocker (the
NORESERVE/madvise large-heap memory model) plus a couple of small stubs.** That
is far better than the "whole subsystems missing" framing for full Chromium.

---

## 4. Cheap smoke-of-the-probe — first hard blocker, empirically

I did **not** sink hours into a sync+gen. The first hard blocker is reachable by
inspection and is unambiguous, so capturing it precisely is enough:

**First hard blocker (infra): `gn` + `depot_tools` are not present, and even
once installed, `gn gen` would fail immediately** because V8's `BUILD.gn`
dispatches on `target_os`/`current_os` and there is **no `b1nix` value** — the
build config in `//build/config/BUILD.gn` has no `is_b1nix`, so a
`target_os="b1nix"` arg is rejected before any compile begins. This is the
"upstream supports no such target" wall, made concrete:

- `which gn` → nothing; only `/usr/lib/qt6/gn` (a Qt fork, wrong revision).
- `fetch`/`gclient` → absent (no depot_tools).
- Adding a new OS to GN is a source edit across `//build/config`, the toolchain
  defs, and V8's `src/base/platform/` — i.e. the port itself, not a flag.

Spending host-hours to *reproduce* a `gn gen` error message would only confirm
what the source layout already proves. The blocker is structural, not a missing
package.

---

## Ordered hard-blocker list (with severity + effort)

1. **Add b1nix as a GN/V8 OS target** (`//build/config`, host+target toolchains,
   `v8_target_os`, the `is_posix && !is_linux` aliasing, and every Linux-only
   site it exposes). *Severity: blocking. Effort: ~3–6 weeks just to get V8
   proper + `mksnapshot` to link.*
2. **Large-heap memory model** — `MAP_NORESERVE` semantics + `madvise`
   (`MADV_FREE/DONTNEED`) in kernel VMM + libc, or patch every V8 page-allocator
   call site to a reserve-then-commit fallback. *Severity: blocking (runtime).
   Effort: ~1–2 weeks (kernel VMM touch).*
3. **Host/target snapshot split** — build `mksnapshot`/`torque`/
   `bytecode_builtins_list` with the host (Linux/clang) toolchain, run them, feed
   the snapshot + generated sources into the b1nix target build. *Severity: hard.
   Effort: ~1 week once the GN toolchains exist.*
4. **`sigaltstack` + minor POSIX gaps** (`mincore`, a few `is_linux` headers).
   *Severity: medium. Effort: days.*
5. **Bring up `d8` and run a `print("hello")` under `--jitless`**, then chase the
   inevitable next round of platform breaks (file I/O, embedder API). *Severity:
   integration. Effort: ~1–2 weeks.*

**Realistic total: 2–3 months of focused work for a single engineer** to get
`d8 --jitless` printing JS output on b1nix — consistent with the
chromium-assessment's "milestone-sized port" wording, now with line items.

---

## Is any blocker a cheap independent win worth doing now?

Yes — two, both useful beyond V8:

- **`madvise(MADV_FREE/MADV_DONTNEED)` + `MAP_NORESERVE`** in the b1nix VMM/libc.
  This is a real POSIX gap (the libc header even notes "we do not provide
  madvise"), benefits any large-heap allocator (jemalloc-style, Mesa, future
  ports), and is a self-contained kernel+libc task. **Do this now if anything.**
- **`sigaltstack`** — small, standard POSIX, helps robust crash handling
  generally. Low risk.

These are worth landing as small POSIX-coverage wins **regardless of whether V8
is ever attempted**, and they de-risk M58 if it is.

## Pragmatic alternative (recommended)

b1nix **already runs Duktape** (it is compiled into the NetSurf browser, M54 —
`content_handlers_javascript_duktape_*.o` are built in the tree). If the
underlying want is "execute JavaScript on b1nix," exposing Duktape as a tiny
standalone `/bin/js` ELF (it is single-file, ANSI C, no GN, no snapshot, no
madvise) is a **1–2 day** task that delivers a working JS REPL today. V8 jitless
buys ECMAScript-2023 conformance and speed, not "JS at all."
