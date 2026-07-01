# M89 — LLVM libc++ (shared) port

Implementation notes for migrating the b1nix C++ stack to a **shared LLVM
libc++**. Summary lives in [`roadmap.md`](roadmap.md#m89-migrate-the-c-standard-library-to-llvm-libc-shared);
this file holds the detail. Shipped in **v0.75.3**.

## What ships

Cross-built for `x86_64-b1nix` from the LLVM 18.1.8 source tree and embedded in
the initramfs:

- `/lib/libc++.so.1` — the C++ standard library. `NEEDED: libc++abi.so.1, libc.so.1`.
- `/lib/libc++abi.so.1` — Itanium C++ ABI **with the libunwind DWARF unwinder
  folded in** (one unwinder instance). `NEEDED: libc.so.1`.

`tools/toolchain/bin/b1nix-c++` defaults to libc++ (auto-detected when built) and
links it dynamically. The GCC libstdc++ path is unchanged and still selectable
with `B1NIX_CXX_STDLIB=libstdc++`.

## Build pipeline

1. `tools/toolchain/build-llvm-runtimes.sh` — compiler-rt builtins (and a
   standalone libunwind; ASM enabled so the register save/restore `.S` files
   compile). Picks the arch-matching `libclang_rt.builtins-<arch>.a`.
2. `tools/toolchain/build-libcxx.sh` — **unified** LLVM `runtimes` build of
   `libunwind;libcxxabi;libcxx` (the standalone libc++abi build is deprecated in
   LLVM 18 and hard-errors without libunwind in `LLVM_ENABLE_RUNTIMES`). PIC
   static archives. The libunwind objects are then **deleted from `libc++.a`** so
   the unwinder is not duplicated between the two shared objects (two `_Unwind_*`
   registries crash on the first throw). The complete libunwind.a is installed
   over the standalone (incomplete) one.
3. `tools/toolchain/build-libcxx-shared.sh` — links the two `.so` with `ld.lld`
   from the PIC archives (clang emits `.init_array`, so lld's default `-shared`
   gives a proper `DT_INIT_ARRAY` the M75 loader runs). compiler-rt builtins are
   resolved into each `.so`. `__register_frame`/`__deregister_frame` are localized
   out of `libc++abi.so.1`'s dynsym (see "frame registration" below).

## Real fixes required (not flag flips)

### libc / headers
- `userspace/include/elf.h`: added `PT_GNU_EH_FRAME` and the other standard
  `PT_*` constants (libunwind reads `PT_GNU_EH_FRAME`).
- `userspace/include/bits/alltypes.h` (new): musl-style `__NEED_*` provider; the
  `_LIBCPP_HAS_MUSL_LIBC` path pulls `mbstate_t` from here.
- `userspace/include/stdlib.h`: `strtoll_l`/`strtoull_l` guarded out under
  `_LIBCPP_HAS_MUSL_LIBC` (libc++'s musl shim defines them — avoid the clash).
- `userspace/include/unistd.h`: `_POSIX_TIMERS` (libc++ `<chrono>` gates
  `steady_clock` on it for the `clock_gettime(CLOCK_MONOTONIC)` backend).
- `__cxa_thread_atexit_impl` in `userspace/libc/pthread.c`: per-thread LIFO list
  of C++11 `thread_local` destructors, run at thread exit and (for the main
  thread) from `exit()`.

### libc++ build config
- `LIBCXX_HAS_MUSL_LIBC=ON` (b1nix libc is musl-like: default rune table +
  C-locale `*_l` shims).
- `LIBCXX_ENABLE_TIME_ZONE_DATABASE=OFF` (no IANA tzdata).
- `.deplibs` (`LLVM_DEPENDENT_LIBRARIES`) stripped from the archives — b1nix folds
  `dl`/`pthread`/`rt` into libc, so the autolink hints would fail `ld.lld` with
  "unable to find library from dependent library specifier: dl".

### Cross-DSO exceptions
libc++ unwinds with **libunwind**, which finds every object's FDEs through
`dl_iterate_phdr` + each module's `PT_GNU_EH_FRAME` (`.eh_frame_hdr`) — the
standard ELF mechanism, not libgcc's `__register_frame` registry. Two things were
needed:

- **`userspace/linker-libcxx.ld`** (new, libc++-only): unlike the GCC
  `linker-cxx.ld` it (a) maps the program headers — `. = base + SIZEOF_HEADERS`
  puts the ELF header + phdrs inside the first `PT_LOAD`, so libunwind's
  `findUnwindSectionsByPhdr` can read `dlpi_phdr` without faulting — and (b)
  keeps `.eh_frame_hdr` (linker-cxx.ld discards it). It uses **no explicit
  `PHDRS`** so lld auto-emits the full segment set (`PT_PHDR`/`PT_INTERP`/
  `PT_DYNAMIC`/`PT_TLS`/`PT_GNU_EH_FRAME`).
- crt0's libgcc-model `__register_frame(__EH_FRAME_BEGIN__)` must not bind to
  libunwind's `__register_frame` (different contract — single FDE vs section
  start), so it is localized out of `libc++abi.so.1`. The b1nix in-kernel loader
  still binds crt0's weak ref to a local symbol, so the real guarantee is that no
  loaded `.so` exports it; cross-DSO unwinding then goes entirely through
  `dl_iterate_phdr`.

### `*at` emulation — `std::filesystem::remove_all`
libc++'s `remove_all` opens the parent directory and recurses with
`openat(dirfd, <relative>, …)` / `unlinkat(dirfd, <relative>, …)`. b1nix had no
per-fd-base path resolver, so those returned `ENOSYS`/operated on the wrong path.
Added a real path resolver:

- Kernel: `SYS_FD_PATH(fd, buf, size)` → `vfs_fd_abspath()` walks `node->parent`
  (under the VFS tree read lock) to build the fd's absolute path.
- libc: `resolve_at_path()` in `userspace/libc/unistd.c` turns a `(dirfd,
  relative)` pair into an absolute path via `SYS_FD_PATH`, used by `openat`/
  `unlinkat`.

## Not migrated (remaining M89 work)

d8/V8, Mesa, and NetSurf/litehtml still link the shared **GCC** `libstdc++.so.6`
(path A) through their own build scripts. They run correctly there; moving each
to libc++ is a separate per-port reverify.
