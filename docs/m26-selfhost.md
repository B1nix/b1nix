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
- The **GCC-built kernel boots and now passes the FULL test suite** to
  `B1NIX-TEST: done` with **zero exceptions/panics** — the post-fork `#GP` is
  fixed at its root cause (kernel-fork rbp-chain relocation, see §6).
- The **clang build is unchanged and still passes the full smoke suite, 214/0**,
  so all the kernel-side fixes below are regression-free.

What is NOT done yet:
- Build the kernel **in-guest** (need a build driver + crt objects) (§7).
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
11. **`kernel/sched/scheduler.c`** — kernel-mode fork: relocate the *entire*
    saved-rbp chain in the child's copied stack (not just the first frame) so the
    child unwinds on its own stack. Fixes the post-fork `#GP` (see §6). Was a real
    latent bug, exposed by GCC; clang happened to survive it.
12. **`kernel/arch/x86/interrupts.c`** — `arch_backtrace` now validates frame
    pointers for canonical-ness + mapped range (`fp_is_safe`), so a corrupt frame
    can't double-fault and mask the original exception.
13. **`Makefile`** — `install-native-toolchain` now also stages the cross-built
    `crtbegin.o`/`crtend.o` (+ `libgcov.a`/variants) into
    `rootfs/lib/gcc/x86_64-b1nix/13.2.0/`. This is what makes an in-guest
    gcc-driven *link* succeed (§7.2).
14. **`kernel/user/programs.c`** — raised the shell argv cap (`args[16]`→32) and
    line buffers (256→`SH_LINE_MAX`=512 in `readline`/`sh_run_script`/`sh -c`) so
    long toolchain command lines aren't truncated (§7.3). Host-clean; not yet
    re-verified in-guest.
15. **`Makefile`** — `install-kernel-source` now stages the generated
    `build/x86/*.inc` files into the in-guest source tree so `kernel/fs/initramfs.c`
    can compile during an in-guest kernel build (§7.4).

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

## 6. RESOLVED: `#GP` after a kernel-mode fork

**Fixed.** The GCC-built kernel now boots and runs the full test suite
(M12/M13/M14/M15/M25(TCC)/M16/M22/M24/NET) to `B1NIX-TEST: done` with **zero
exceptions/panics**, and the clang build is still **214/0**.

### Symptom (before the fix)
```
POSIX compliance check: foreground pgrp passed
paging_clone_address_space: ...                 <- a fork
EXCEPTION: general protection fault
rip: 0xff894c00001073e8                          <- NON-CANONICAL instruction pointer
```
The faulting fork is the one in `m22_check_posix_compliance` (TIOCSPGRP session
test, `kernel/user/programs.c:2033`). `addr2line 0x1073e8` on the GCC kernel
maps inside `scheduler_fork_current`; the `0xff894c00` upper half is stale stack
garbage (not real code — confirmed by searching `.text`).

### Root cause (NOT a codegen/truncation bug)
This fork is **kernel-mode**: `/bin/init` runs as a builtin (`init_main`), which
calls `m22_smoke_main` → `m22_check_posix_compliance` directly, so the fork takes
the `is_user == false` branch in `scheduler_fork_current` and resumes the child
via `x86_fork_kernel_trampoline` (`leave; ret` = `mov %rbp,%rsp; pop %rbp; ret`).

That trampoline unwinds by **following the saved-rbp chain**. `scheduler_fork_current`
`memcpy`s the parent's kernel stack into the child and relocated only the *first*
frame pointer (`child->context.rbp = current_rbp + stack_offset`). Every *saved*
rbp deeper in the copied stack still pointed into the **parent's** stack. After
the first `leave`, the child ran on the parent's stack, read a stale slot as a
return address, and `#GP`'d (and corrupted the parent's blocked frames). This
path is exercised exactly **once** in the whole suite; clang's frame layout
happened to survive it, GCC's did not — it was never a GCC codegen difference.

### The fix (`kernel/sched/scheduler.c`, kernel-fork branch)
After setting `child->context.rbp`, walk the copied stack's rbp chain and rebase
every saved rbp by `stack_offset`, so the child unwinds entirely on its own
stack. Bounded to 64 frames and to the child stack range. The user-fork branch
(iframe + `x86_fork_child_trampoline` + `sysretq`) was already correct and is
untouched.

### Secondary fault hardening (`kernel/arch/x86/interrupts.c`)
`arch_backtrace` only bounded rbp from above, so a non-canonical rbp (in the
`[2^47, 0xffff800000000000)` hole) passed the check and the dereference `#GP`'d
inside the exception handler, masking the real first fault. Added `fp_is_safe()`
(canonical + mapped-range + `fp+16` bound) and gated both unwind phases on it.

---

## 7. Remaining work toward full in-guest self-host

1. **Fix the GCC-codegen `#GP`** — **DONE** (§6).
2. **In-guest full link** (`gcc hello.c -o hello`) — **DONE & verified**.
   In-guest `/persist/bin/gcc /tmp/h.c -o /tmp/h` returns 0 and the binary runs
   (e.g. `return 42` → wait status 10752 = 42<<8). The fix was *only* staging the
   crt objects: `crtbegin.o`/`crtend.o` now copied into
   `rootfs/lib/gcc/x86_64-b1nix/13.2.0/` by the Makefile `install-native-toolchain`
   target (alongside the already-copied `libgcc.a`). **Correction to the earlier
   plan:** do NOT add `all-target-libgcc` to `tools/build-native-toolchain.sh` —
   that's the *native* (host=b1nix) build and can't run a b1nix `gcc` on the host.
   The crt objects come from the **cross** build (`cross/lib/gcc/.../crt*.o`),
   which already builds them. **No `-B`/`--sysroot`/symlink/prefix hack is
   needed**: the gcc driver relocates its prefix *and* target sysroot from
   `argv[0] = /persist/bin/gcc`, so it finds `cc1` at `/persist/libexec/...`,
   `crtbegin/crtend/libgcc` at `/persist/lib/gcc/...`, and `crt0.o` at the
   relocated sysroot `/persist/lib/crt0.o`. (One harmless warning remains:
   `crtend.o: missing .note.GNU-stack section implies executable stack` — it's
   the cross-built crtend.o; cosmetic, link still RC=0.)
   NOTE: a freshly in-guest-linked program lands at **0x400000** (binutils
   default script) — do NOT pass `userspace/linker.ld` (it forces 0x2000000).
3. **In-guest compilation of kernel sources — VERIFIED.** With the kernel flags
   (`-std=c11 -ffreestanding ... -mcmodel=kernel -mno-sse ... -I.../kernel/include`)
   the in-guest gcc compiles real kernel TUs to `.o`: `kernel/lib/string.c` → RC 0
   (5080-byte `.o`), and the larger `kernel/sched/scheduler.c` → RC 0 too. So
   `-mcmodel=kernel`, the kernel headers, and a big TU all work in-guest.
   Two **shell limits** were blocking long toolchain command lines and are now
   fixed (`kernel/user/programs.c`):
   - argv cap was `char *args[16]` → raised to 32 (matches `USER_MAX_ARGS`). 16
     silently dropped trailing args (a kernel-flag gcc line has ~20 tokens), e.g.
     `-o file` became just `-o` → "missing filename after -o".
   - input/script line buffer was 256 → `SH_LINE_MAX` (512) for `readline`,
     `sh_run_script`, and `sh -c`. 256 truncated a ~260-char gcc line mid-arg
     (e.g. `-o /tmp/sched.o` → `-o /tmp/sch`). (Mindful of the 16 KB kthread stack
     + 6.6 KB `syscall_dispatch_impl` frame; 512 is a safe bump.)
   - **Still UNVERIFIED in-guest** (compiles clean on host; needs a guest run).
4. **In-guest build driver + full build (REMAINING).** `nmake`
   (`kernel/user/nmake.c`) is too limited for the real Makefile (no
   variables/automatic vars, only searches `/bin` not `/persist/bin`, no mtime
   check). Simplest path: a **flat shell script** generated from the Makefile's
   `KERNEL_SOURCES`/`ASM_SOURCES`/`ARCH_SOURCES` + the gcc flags, run from the
   interactive shell. Prereqs now in place:
   - The generated `.inc` files (`build/x86/*.inc`) are now staged into
     `/persist/usr/src/b1nix/build/x86/` by `install-kernel-source` (kernel/fs/
     initramfs.c `#include`s them via `../../build/x86/*.inc`; can't be
     regenerated in-guest without `xxd`).
   - The AP trampoline needs a flat-binary link + an `xxd -i`-style `.inc`; since
     that `.inc` is now staged, the driver can skip regenerating it and just
     compile/link the kernel objects.
   - Expect this SLOW in QEMU: ~70 TUs each forking the 38 MB cc1 on an emulated
     single CPU — many minutes, well past the 120 s smoke timeout. Run it from the
     interactive shell, not the smoke runner.
   Then flip `can_build_kernel_inside_b1nix` to 1 in `kernel/syscall/syscall.c`
   (ONLY when an in-guest `kernel.elf` actually builds — no fake passes).
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
