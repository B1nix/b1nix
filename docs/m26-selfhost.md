# M26 — Toolchain & Self-Hosting: Progress & Handoff

Status as of 2026-05-27. Branch: **`m26-selfhost`** (off `main`).

This document is the working record for M26 ("Full Toolchain and Self-Hosting"):
compile b1nix programs — and ultimately the b1nix kernel — using the natively
ported GCC, inside b1nix.

---

## 1. TL;DR — where we are

- The cross **and** native GCC 13.2.0 + Binutils 2.41 toolchains build, including
  a **target `libstdc++`** (this was the blocker that stopped the native GCC from
  building at all).
- **The native `gcc` runs inside b1nix and compiles C to an object file**
  (`cc1` + `as` both work in-guest): `gcc -S` emits correct assembly and
  `gcc -c` produces a real `.o`.
- **The b1nix kernel now builds with GCC** on the host:
  `make ARCH=x86 TOOLCHAIN=gcc all` produces `kernel.elf` with zero errors.
- The **GCC-built kernel boots** and passes the M12/M13/M14/M15/M25(TCC)/M16/M22
  smoke modules, then hits one **GCC-codegen `#GP`** (see §6).
- The **clang build is unchanged and still passes the full smoke suite, 214/0**,
  so all the kernel-side fixes below are regression-free.

What is NOT done yet:
- Fix the GCC-codegen `#GP` so the GCC-built kernel is fully clean (§6).
- Build the kernel **in-guest** (need a build driver + the `#GP` fixed) (§7).
- Full in-guest **link** of a user program (`gcc hello.c -o hello`): needs
  `crtbegin.o`/`crtend.o` and a prefix fix (§7).

---

## 2. Build environment (important)

b1nix builds inside **WSL Arch Linux**, NOT Git Bash. None of the build tools
(clang, ld.lld, qemu, mke2fs, grub) exist in Git Bash. Run builds via
`wsl <cmd>` / `wsl bash <script>`.

- WSL `HOME` = `/root`. Repo is at `/mnt/c/Users/Dmytro Manko/Documents/GitHub/b1nix`.
- The toolchains live at `/root/b1nix-toolchain/` (the build scripts redirect
  there because the repo path has spaces). `build/toolchain_build` is a symlink
  to it. Layout: `cross/` (host→b1nix), `native_root/` (b1nix→b1nix), `src/`
  (gcc-13.2.0, binutils-2.41, build trees), `sysroot/` → `build/x86/rootfs`.
- `rsync` is absent here — build scripts use `tar`/`cp`.
- PowerShell→WSL quoting mangles `$(...)`, `(`, nested quotes — put commands in a
  `smoke_run/*.sh` script and run `wsl bash "/mnt/c/.../script.sh"`.
- Helper/probe scripts from this work are in `smoke_run/_m26_*.sh` (git-ignored).

---

## 3. The toolchain

### Cross (host → x86_64-b1nix): `tools/build-toolchain.sh`
Now also builds **target `libstdc++-v3`** (step 6 added). Without it the native
GCC build failed at `libcpp` with `fatal error: new: No such file or directory`,
because GCC 13 is C++ and cross-compiling it for b1nix needs target C++ headers +
`libstdc++.a`. Output includes `cross/x86_64-b1nix/{lib/libstdc++.a,
include/c++/13.2.0}`.

### Native (b1nix → b1nix): `tools/build-native-toolchain.sh`
Produces `native_root/{bin/gcc,as,ld,ar, libexec/.../cc1, ...}` — b1nix ELF
binaries that run in-guest. Installed into the rootfs by
`make install-native-toolchain` and staged with the kernel source by
`make install-kernel-source` (now via `tar`, not `rsync`).

### Re-linking the native toolchain after a libc change
`cc1`/`gcc`/`as`/`ld` statically link `libb1nix.a` (and `crt0.o`). `make` does
not track those as dependencies, so to pick up a libc/crt0 change you must
**delete the final build-tree binaries** to force a link-only pass. See
`smoke_run/_m26_relink_native.sh` (removes `native_root` + the final binaries in
`build-native-{gcc,binutils}` and re-installs; reuses cached `.o`, ~minutes).
Then `make root-image` re-stages them into the 512 MB image.

---

## 4. Fixes made this session (all on `m26-selfhost`, committed)

In dependency order — each was a real gap the GCC port exposed:

1. **`tools/build-toolchain.sh`** — build/install target `libstdc++-v3`.
2. **`userspace/include/{time.h,math.h,ctype.h}` + new `userspace/libc/ctype.c`
   + `userspace/Makefile`** — close libc gaps that blocked libstdc++:
   - `time.h`: guard the C++11 `B1nixTimePtr(decltype(nullptr))` ctor under
     `__cplusplus >= 201103L` (libstdc++ compiles some TUs `-std=gnu++98`).
   - `math.h`: add `fabsf/sqrtf/fabsl/sqrtl` (libstdc++ math stubs reference
     them from `hypotf`/`hypotl`). Do NOT add transcendental f/l variants — the
     stubs define those, so it would be a duplicate definition.
   - `ctype.h` + `ctype.c`: newlib `_ctype_[257]` classification table that
     libstdc++'s generic (newlib) locale config reads as `_ctype_ + 1`.
3. **`userspace/libc/stdlib.c`** — replace the 16 MB static-pool/no-op-`free()`
   allocator with an mmap-backed explicit-free-list + boundary-tag allocator,
   16-byte aligned (host stress-tested).
4. **`kernel/fs/vfs.c`** — add the `/persist` mountpoint. `main.c` mounts
   `virtio-blk0` at `/persist`, but `vfs_mount()` requires the dir to exist and
   it was never created, so the persistent root image (and the toolchain it
   carries) never mounted.
5. **`kernel/fs/ext4.c` (+ `vfs.h`)** — `ext4_populate_vfs` created symlinks as
   plain `VFS_FILE` nodes; now creates `VFS_SYMLINK` nodes with the target (so
   `/persist/usr/include` etc. resolve). `VFS_NODE_OWNS_DATA` moved to
   `<b1nix/vfs.h>`.
6. **`kernel/arch/x86/fpu.S` (new) + `sched.h` + `scheduler.c` + `process.c` +
   `Makefile`** — save/restore FPU/SSE (`fxsave64`/`fxrstor64`) across context
   switches + clean FPU init on `execve`. (A real kernel bug — the kernel
   enabled SSE for userspace but never preserved XMM/MXCSR/x87. It was NOT the
   cc1 crash cause, but it is a correctness fix.) Follow-up: APs don't enable
   `OSFXSR` in `ap_trampoline.S` — add before cross-CPU scheduling.
7. **`userspace/crt/crt0.S` + `userspace/linker.ld`** — run the ELF
   `.init_array` constructors before `main`. This was THE cc1 crash fix: cc1 is
   C++ with 44 static ctors (one tied to its et-forest dominance structures);
   skipping them left a codegen global NULL → `et_splay(NULL)` → `cr2=0x8`. With
   this, in-guest `gcc -S` works.
8. **`userspace/libc/stdio.c`** — `vsnprintf` only understood a single `l`. GCC
   prints `HOST_WIDE_INT` with `%lld`, so cc1 emitted a literal `%ld` into its
   assembly (`.cfi_def_cfa_offset %ld`) and `as` rejected it. Now parses
   flags/width/precision and `ll`/`z`/`j`/`t`/`L`/`h`/`hh`. With this, in-guest
   `gcc -c` produces a real `.o`.
9. **`Makefile`** — `install-kernel-source` uses `tar` instead of `rsync`.
10. **`Makefile`** — opt-in `TOOLCHAIN=gcc` kernel build mode (see §5).

Dead-ends ruled out along the way (do not re-investigate): the cc1 crash was
NOT malloc, NOT FPU/SSE, NOT qsort/memcpy/memmove, NOT the ELF loader (it
correctly zeroes BSS, `process.c:546`). It was crt0 + printf.

---

## 5. Building the kernel with GCC

```sh
# host build with the cross GCC (default build stays clang):
make ARCH=x86 TOOLCHAIN=gcc all                 # -> build/x86/kernel.elf
make ARCH=x86 TOOLCHAIN=gcc KERNEL_CMDLINE="b1nix.test=1" iso
```
`TOOLCHAIN=gcc` sets `CC=x86_64-b1nix-gcc`, `LD=x86_64-b1nix-ld`, drops the
clang-only `--target`, and drops the lld-only `--image-base 0` on the AP
trampoline flat link. `kernel/arch/x86/linker.ld` is GNU-ld compatible as-is.

GOTCHA: the FIRST target in the Makefile is `analyze` (the clang static
analyzer), which is therefore the default goal. **Always pass an explicit
target** with `TOOLCHAIN=gcc` (e.g. `all`/`iso`), or bare `make` runs the
clang-only analyzer and errors on `-Xclang`. (Worth fixing: add
`.DEFAULT_GOAL := all` or move `all` first.)

Build is clean of errors. Remaining warnings under GCC `-Wall -Wextra`:
`-Wtype-limits` in `kernel/mm/kheap.c:88,168` and
`kernel/arch/x86/fb_console.c:122`; pre-existing `-Wunused-*` dead code in
`vfs.c`, `ext2/3/4.c`, `mc.c`, `lapic.c`. Clean these for a zero-warning GCC
build.

---

## 6. THE remaining blocker: GCC-codegen `#GP` after fork

The GCC-built kernel boots and runs M12/M13/M14/M15/M25(TCC)/M16/M22, then:

```
POSIX compliance check: foreground pgrp passed
paging_clone_address_space: ...                 <- a fork
EXCEPTION: general protection fault
rip: 0xff894c00001073e8                          <- NON-CANONICAL instruction pointer
```

The low 32 bits (`0x001073e8`) are a valid kernel `.text` address (~1.07 MB);
the **upper 32 bits are corrupted** (`0xff894c00`). The CPU jumped/returned to a
non-canonical address → `#GP`. It happens immediately after a `fork`
(`paging_clone_address_space`), so a return address / function pointer in the
fork-return path has its top half clobbered (pointer truncation).

This is a **GCC-vs-clang code-generation difference**, NOT the FPU/struct
changes: the **clang** kernel with all the same fixes passes the full smoke
suite 214/0, including this exact test.

The `[PANIC] unhandled CPU exception` that follows (rip `0x147d01`) is a
*secondary* fault: `arch_backtrace` itself `#GP`s while unwinding the bad frame
(`kernel/arch/x86/interrupts.c`) — its frame-pointer validation only checks an
upper bound, not canonical-ness/lower-bound. Hardening it would surface the real
first fault instead of masking it.

Where to look next:
- The fork path: `kernel/sched/scheduler.c` `scheduler_fork_current` and the
  fork **assembly trampolines** (`kernel/arch/x86/*.S`) that return the child to
  user/kernel. An asm trampoline written against clang's codegen/ABI
  assumptions may mishandle a 64-bit value under GCC (e.g. a `movl` where a
  `movq` is needed, or a clobbered callee-saved reg).
- `addr2line` the intended `0x1073e8` in a GCC `kernel.elf` to identify the
  function the child was returning into.
- Compare GCC vs clang disassembly of `scheduler_fork_current` and the trampoline.

---

## 7. Remaining work toward full in-guest self-host

1. **Fix the GCC-codegen `#GP`** (§6) — gate for a clean GCC kernel.
2. **In-guest full link** (`gcc hello.c -o hello`): native build only did
   `all-gcc`, so `crtbegin.o`/`crtend.o` were never built and are absent from
   the rootfs → any gcc-driven *link* fails. Fix: add `all-target-libgcc` +
   `install-target-libgcc` to `tools/build-native-toolchain.sh`. Also the gcc
   driver's compiled-in prefix is `/` but the toolchain is mounted at `/persist`
   → pass `-B/persist/... --sysroot=/persist`, or add `/lib /libexec /include
   /bin` symlinks → `/persist/...`, or rebuild native gcc with `--prefix=/persist`.
   NOTE: a freshly in-guest-linked program lands at **0x400000** (binutils
   default script) — do NOT pass `userspace/linker.ld` (it forces 0x2000000).
   `as`+`ld` by hand already work today (Path A): `as x.s -o x.o &&
   ld /persist/lib/crt0.o x.o -L/persist/lib -lc -o x`.
3. **In-guest build driver**: `nmake` (`kernel/user/nmake.c`) is too limited for
   the real Makefile (no variables/automatic vars, only searches `/bin` not
   `/persist/bin`, no mtime check). Either extend it or port GNU make, or drive
   the kernel build with a flat script.
4. Then build the kernel in-guest from `/persist/usr/src/b1nix` with
   `TOOLCHAIN=gcc`, and flip `can_build_kernel_inside_b1nix` to 1 in
   `kernel/syscall/syscall.c` (only when it actually works — no fake passes).
5. Add an `M26-SMOKE` test module + boot-log markers for the self-host path.

### Latent bugs noticed (not blockers, worth tracking)
- `kernel/mm/pmm.c:288` — inverted swap-evict retry check (OOM recovery never retries).
- `kernel/sched/scheduler.c` `vm_find_free_area` assumes an ascending-sorted VMA
  list, but `user_run_elf_image` prepends VMAs (harmless for mmap-only patterns).
- `arch_backtrace` frame-pointer validation is incomplete (see §6).

---

## 8. How to reproduce the in-guest gcc demo

```sh
# (toolchains already built in /root/b1nix-toolchain)
make ARCH=x86 iso                 # clang kernel, interactive shell
make root-image                   # 512MB ext4 with native gcc + source at /persist
# boot QEMU with root.ext4 as virtio-blk + >=1G RAM, drive the serial shell:
#   /persist/bin/gcc -c /tmp/m.c -o /tmp/m.o     -> works (cc1 + as)
# see smoke_run/_m26_run_validate.sh for the scripted version.
```
