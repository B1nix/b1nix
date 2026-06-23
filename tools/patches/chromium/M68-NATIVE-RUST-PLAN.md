# M68 — Native Rust compiler that RUNS ON b1nix (rustc + cargo self-host)

The Rust analog of M26 (native GCC self-host): a `rustc` (and `cargo`) whose
**host** triple is `x86_64-unknown-b1nix`, i.e. ELF binaries that execute inside
the b1nix OS and compile Rust there.

Branch: **m68-native-rust** (off `rust-port`). **Not merged to main** until it
actually runs in QEMU and compiles real code (same isolation rule as rust-port).

Depends on **M67** (the Rust → b1nix CROSS-toolchain, branch `rust-port`):
reproduced and verified working — a real std program (Vec/String/HashMap/
std::thread) cross-compiles + links into a clean static b1nix ELF
(`0x2000000` base, `nm -u` empty). See `RUST-PORT-PLAN.md`. Repro:
`sh tools/build-rust-toolchain.sh`.

---

## What "native rustc" requires (the bootstrap matrix)

Rust's bootstrap (`x.py`) builds rustc in stages:

| stage | built by | runs on | links |
|-------|----------|---------|-------|
| stage0 | (prebuilt CI nightly) | build host (linux) | CI LLVM |
| stage1 | stage0 | build host (linux) | CI LLVM |
| stage2 | stage1 | **b1nix host** | **LLVM-for-b1nix** |

To produce a rustc that *runs on b1nix*, stage2 must be built **for the b1nix
host**, which needs three things:

1. **b1nix as a valid Rust HOST target** (`host_tools = true`) — NOT just a
   `--target`. Done: a built-in target
   `compiler/rustc_target/src/spec/targets/x86_64_unknown_b1nix.rs`
   (committed as `tools/patches/rust/x86_64_unknown_b1nix.rs`, staged by
   `tools/patches/rust/apply.sh`). It mirrors the M67 JSON spec (os=linux,
   env=musl, static ET_EXEC, no-PIE, `-Ttext-segment=0x2000000`, panic=abort,
   linker `x86_64-b1nix-gcc`) but sets `host_tools: Some(true)`.

2. **LLVM cross-built FOR b1nix** — rustc links libLLVM. There is no CI LLVM for
   b1nix, so the bundled `src/llvm-project` submodule (X86 target only) is
   cross-built with the b1nix cross g++
   (`build/toolchain_build/x86_64-b1nix/cross/bin/x86_64-b1nix-g++` +
   wchar-enabled libstdc++). The enormous part (millions of LOC, tens of GB).
   The **build host** keeps using prebuilt CI LLVM (`download-ci-llvm=true`).

3. **rustc's crates + cargo cross-compiled to b1nix** — and every `std::sys`/host
   gap resolved (process spawn, fs, threads — mostly covered by M67's unix-PAL
   reuse + libc shims + futex bridge).

Driver: `tools/build-rust-native.sh` → `x.py build --stage 2` with
`bootstrap.toml` host=`["x86_64-unknown-b1nix"]`.

---

## HARD architectural limit (honest scope)

b1nix is **static-only**: no dynamic loader, `dlopen`/`dlsym` are no-ops
(`userspace/libc/stdlib.c`). rustc loads **proc-macro** crates as dylibs via
dlopen at compile time. Therefore a native rustc on b1nix **cannot compile any
crate that uses proc-macros** (serde derive, etc.) — it can only compile Rust
that does not depend on proc-macros (hello-world, core/alloc/std-only code,
hand-written generics). This is the exact analog of native GCC compiling plain C
without the full host ecosystem. Lifting it would require a static proc-macro
mechanism (compile proc-macros into the rustc binary, or a separate exec-based
proc-macro server) — a large research project, out of M68's first scope and
documented as the principal remaining limitation.

Other inherited M67 limits: panic=abort only (no unwinder); futex bridge is
WAIT/WAKE only; ms-granular timeouts.

---



## Bootstrap plan CONFIRMED (x.py dry-run)

`x.py build --stage 2 --dry-run` with `host=[x86_64-unknown-b1nix]` plans exactly
the M68 deliverable — the human-readable step headers include:
- `Building stage1 library artifacts (... -> stage1:x86_64-unknown-b1nix)` (std for b1nix, M67-proven)
- **`Building LLVM for x86_64-unknown-b1nix`** (the long pole — cmake+ninja LLVM cross-build with the b1nix cross g++)
- `Building stage2 compiler artifacts (stage1:x86_64-unknown-linux-gnu -> stage2:x86_64-unknown-b1nix)` (rustc cross-compiled FOR the b1nix host, linking the b1nix LLVM)
- `Building stage2 rustdoc_tool_binary (... -> stage2:x86_64-unknown-b1nix)`

So the host=b1nix configuration produces a real rustc-for-b1nix; the build-host
(linux) stage1/stage2 are built first as scaffolding (they use the prebuilt CI
LLVM). The b1nix LLVM is built from source from the bundled
`src/llvm-project` (rust-lang/llvm-project @ ec9ab9d).

## Verified prerequisites for the LLVM-for-b1nix cross-build

The b1nix cross g++ (gcc 13.2.0, `build/toolchain_build/x86_64-b1nix/cross`) is
LLVM-capable: a threaded C++17 program (`std::thread`+`std::mutex`+`std::atomic`
+`std::vector`/`std::string`+`<cwctype>`/`towlower`) both **compiles and STATIC-
links** into a clean b1nix ELF at `0x2000000` (only a benign
`.note.GNU-stack`/executable-stack linker warning). So libstdc++ here already
has threads, atomics, and the wide-char family — the M67-era `<cwctype>` wchar
gap is resolved in the current toolchain. The principal LLVM-cross risks are now
scale (time/disk) and any specific POSIX/host API LLVM assumes that b1nix lacks,
not the C++ runtime.


## LLVM-for-b1nix cross-build: concrete gaps fixed (this run)

Reached "Building LLVM for x86_64-unknown-b1nix" and chased the first wave of
b1nix-not-recognized gaps (b1nix defines __b1nix__/__unix__, not __linux__, and
LLVM/its cmake key on __linux__/known-OS):

1. `CMAKE_SYSTEM_NAME=Generic` (triple matched no known OS) → `LLVM_ON_UNIX`
   unset → `file_status::getSize` and the POSIX paths compiled out (errors in
   FileSystem.h / CachePruning.cpp). **Fix:** bootstrap `llvm.rs` maps b1nix →
   `CMAKE_SYSTEM_NAME=Linux`.
2. `llvm/ADT/bit.h` → `#include <machine/endian.h>` (absent). **Fix:** add
   `userspace/include/machine/endian.h` (ships in sysroot).
3. Source `#if defined(__linux__)` branches not taken. **Fix:** bootstrap
   `llvm.rs` injects `-D__linux__=1` into the b1nix LLVM C/CXX flags (scoped — a
   global `llvm.cflags` is rejected as incompatible with `download-ci-llvm`).
4. `llvm/Support/ExitCodes.h` → `#include <sysexits.h>` (absent; `LLVM_ON_UNIX`
   path `#error`s without it). **Fix:** add `userspace/include/sysexits.h`.

After these, the LLVM cmake configure succeeds and ninja compiles the 3898-object
LLVM tree with the b1nix cross g++. (Optional deps Backtrace/OCaml/zlib/pygments/
yaml are reported missing but are non-fatal.) The compile is the long pole;
further gaps (if any) will surface as individual LLVMSupport/`*.cpp` errors and
are appended here.


### LLVM Unix/*.inc POSIX libc gaps (next wave, fixed)

LLVM's `lib/Support/Unix/{Path,Process}.inc` need POSIX APIs b1nix's libc lacked:
- `struct winsize` + `TIOCGWINSZ` — b1nix had them only in <termios.h>; glibc
  exposes them via <sys/ioctl.h> (which LLVM includes). **Fix:** also define them
  in `userspace/include/sys/ioctl.h` (guarded with `_STRUCT_WINSIZE_DEFINED` so
  including both headers is safe; termios.h guarded to match).
- `_SC_GETPW_R_SIZE_MAX` sysconf constant. **Fix:** added to `userspace/include/unistd.h`.
- `getpwnam_r` / `getpwuid_r` (reentrant passwd). **Fix:** declared in
  `userspace/include/pwd.h` + real impl in `userspace/libc/pwd.c` (wrap the
  non-reentrant lookups, pack strings into the caller buffer, ERANGE if small).
- **pwd.h had no `extern "C"`** → C++ callers (LLVM) mangled the calls →
  link-time `undefined reference to getpwnam_r(...)`. **Fix:** added the
  `extern "C"` guard (real bug for any C++ including pwd.h).

These are genuine b1nix userspace/libc changes that ship in the ISO sysroot.

### Toolchain header/lib staging gotcha (workflow)

The LLVM cross-build invokes the bare cross g++, which searches its OWN baked-in
`cross/x86_64-b1nix/include` and `sysroot/usr/include` (NOT just the rustc-link
sysroot `sysroot/include`). A libc/header change must be staged into ALL of
those paths or LLVM compiles/links against stale copies. `build-rust-native.sh`
now stages libc + headers into every search path. Also note there are TWO
`userspace/` source trees — the main checkout and this worktree (they share
`build/` via symlink but not source) — build the WORKTREE's userspace.


### LLVM Unix/*.inc POSIX gaps — wave 3 (fixed)

`Path.inc`/`Process.inc`/`Program.inc` further needed:
- `msync` + `MS_SYNC`/`MS_ASYNC`/`MS_INVALIDATE` → added to `<sys/mman.h>`; libc
  `msync` is a no-op returning 0 (b1nix shared mmaps are write-through via the
  page cache, so explicit flush is implicit).
- `getpagesize()` → declared in `<unistd.h>` + libc impl = `sysconf(_SC_PAGESIZE)`.
- `wait4`/`wait3` → declared in `<sys/wait.h>` + libc impls (zero the rusage, delegate to waitpid).
- `_SC_ARG_MAX` (sysconf) + `_POSIX_ARG_MAX` (limits.h) → added.

All real POSIX-completeness additions, verified compile+link for b1nix.

### LLVM Support gaps — wave 4 (fixed, commit 0f6d836)

LLVMSupport (objects ~166-173) needed two more APIs:
- `Signals.inc` re-raises a fault on itself with
  `syscall(SYS_rt_tgsigqueueinfo, getpid(), gettid(), Sig, Info)` (unconditional
  under `__linux__`, which we force-define). b1nix had no such syscall.
  **Fix:** added `SYS_RT_TGSIGQUEUEINFO = 162` (both syscall.h enums +
  `<sys/syscall.h>` lowercase alias) + a kernel handler that re-raises the
  signal to the calling process via `scheduler_kill(self, sig)`. The siginfo
  payload is dropped — identical to `raise(3)`, the documented fallback. Real,
  not a fake: b1nix genuinely re-delivers the signal so the default fault action
  runs.
- `Threading.inc` reads the thread name via `pthread_getname_np`. b1nix doesn't
  track per-thread names (`pthread_setname_np` is a no-op). **Fix:** declared
  `pthread_getname_np` in `<pthread.h>` + libc impl returning an empty name (0)
  — the honest answer; ERANGE only if buffer is NULL/zero.

Verified: Signals/Threading/Program/Process all compile clean for
x86_64-unknown-b1nix after this + correct header staging.

### Staging-clobber RACE (root-caused, hardened — commit 9718764)

Symptom: even after wave 2/3 committed `_SC_ARG_MAX`/`TIOCGWINSZ`/etc. to the
worktree headers, an LLVM run re-failed on those exact symbols. Cause: `build/`
in this worktree is a **symlink to the main checkout's `build/`**, so
`build/toolchain_build` is shared with the co-tenant Chromium port — which
re-stages the *main checkout's older* `userspace/include` into the same cross
toolchain, silently overwriting our staged headers (confirmed by md5: staged
unistd.h = main's `7ffd...`, not the worktree's `8c7b...`). **Fix:**
`build-rust-native.sh` now `cmp`-verifies each search-path `unistd.h` against the
worktree source after staging and aborts loudly if clobbered. If a clobber
recurs mid-build, re-stage (re-run the script — it resumes ninja) when the
Chromium build is not concurrently staging.

### LLVM ExecutionEngine/Orc shm_open gap — wave 5 (fixed, v0.64.15)

The LLVM compile reached object 1604/3892 and stopped on
`llvm/lib/ExecutionEngine/Orc/TargetProcess/ExecutorSharedMemoryMapperService.cpp:81`:
`'shm_open' was not declared`. The Orc cross-process JIT shared-memory mapper
uses the POSIX named-shm API — `shm_open()` → fd, then `ftruncate()`,
`mmap(MAP_SHARED)`, `close()`, and `shm_unlink()` in the deallocate path.
b1nix's `<sys/mman.h>` declared none of `shm_open`/`shm_unlink`.

**Fix (honest, real):**
- Declared `shm_open(const char*, int, mode_t)` and `shm_unlink(const char*)`
  in `userspace/include/sys/mman.h` — that's where glibc declares them — inside
  the existing `extern "C"` guard (same C++-mangling lesson as the wave-2 pwd.h
  fix; LLVM is C++). Added `#include <sys/types.h>` for `mode_t`.
- Implemented in `userspace/libc/unistd.c` on top of the **existing real
  anonymous-shared-memory primitive `memfd_create()`** — which is already a
  fully-backed VFS node supporting `ftruncate()` + `mmap(MAP_SHARED)` (used by
  M48/M56). `shm_open()` strips the leading `/` from the POSIX name, uses it as
  the memfd label, and returns the memfd fd (`MFD_CLOEXEC`). This is the honest
  path: b1nix has **no `/dev/shm` tmpfs namespace**, so the glibc
  `open("/dev/shm/"+name,...)` impl is impossible, but memfd gives an identical
  usable object (real fd + ftruncate + MAP_SHARED). Because there is no
  persistent namespace, each `shm_open()` yields a fresh unnamed object and
  `shm_unlink()` is a no-op success (the object is reclaimed when its last
  ref/mmap drops — an already-unlinked / O_TMPFILE semantics). This satisfies
  the only in-tree caller, which creates/ftruncates/mmaps/closes the fd within
  one process. NOT a fake: real fd, real ftruncate, real shared mmap.
- The `cmp`-guard in `tools/build-rust-native.sh` (commit 9718764) now also
  verifies `sys/mman.h` (not just `unistd.h`) in all three cross g++ include
  search paths, since this fix lives in mman.h.

**Verified before restart:** worktree libc rebuilt; `libb1nix.a` exports
`T shm_open`/`T shm_unlink`; the exact failing TU compile command (from
`compile_commands.json`, with `-D__linux__=1 -fno-exceptions -fno-rtti`) recompiles
**rc=0**; the object references `U shm_open`/`U ftruncate`/`U mmap` (declarations
picked up, resolved at link).

## Status (this run)

- M67 cross-build reproduced ✅ (clean static b1nix ELF).
- Full rust-lang/rust source pinned at the nightly commit
  `01dfd79246f1b2d5f146616deff08223a840a9ae` (rustc 1.98.0-nightly,
  LLVM 22.1.7) in `build/rust-native/rust-src-full` (gitignored).
- b1nix built-in **host** target added + registered; `x.py check --dry-run`
  **passes** — bootstrap recognizes `stage2:x86_64-unknown-b1nix` and plans the
  full stage1-std → stage2-rustc/cargo pipeline. ✅
- `bootstrap.toml` configured: build=linux (CI LLVM), host=b1nix, cross g++/ar
  for the b1nix LLVM, X86-only LLVM targets.
- Real `x.py build --stage 2` LAUNCHED (j4 to avoid starving the co-tenant
  Chromium ninja). <fill in: how far it got>.

## Next steps (precise)

1. Drive the LLVM-for-b1nix cross-build to completion (the long pole). Watch for:
   the b1nix cross g++ / libstdc++ gaps (threads, `<cwctype>` wchar — needs the
   `toolchain-wchar` 051909e libstdc++; rebase if it bites), and any
   posix/host-tool assumption LLVM makes that b1nix lacks (mmap flags, etc.).
2. stage1 std for b1nix (already proven to compile via M67's build-std).
3. stage2 rustc + cargo link for b1nix — resolve unresolved-symbol / host-tool
   gaps as they surface (record each exact error here).
4. **Proof**: run the produced `rustc` ELF inside b1nix (QEMU) to compile a
   hello-world. Wire an `M68-RUST` smoke marker mirroring M67. Claim "native
   rustc works" ONLY after this passes.
5. Bump `B1NIX_VERSION_STR` only once something real ships in the ISO + verified.

## Repro

```sh
sh tools/build-toolchain.sh                      # x86_64-b1nix cross gcc/g++
sh tools/build-rust-toolchain.sh                 # M67 cross + libc sysroot
# clone rust-lang/rust @ the nightly commit into build/rust-native/rust-src-full
tools/patches/rust/apply.sh build/rust-native/rust-src-full   # b1nix host target
# edit build/rust-native/rust-src-full/bootstrap.toml cross paths
sh tools/build-rust-native.sh                    # x.py build --stage 2
```
