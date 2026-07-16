# M92: musl libc Port — Full Analysis

> Parent milestone: [`roadmap.md`](roadmap.md) §M92.

## Overview

musl is a lightweight, standards-conformant C standard library. Porting it to
b1nix enables running a wide range of Linux userspace software without the
complexity of glibc. musl compiles as a single static library with no external
dependencies, making it ideal for embedded/bare-metal targets.

**Key architectural insight:** b1nix already has a Linux ABI translation layer
(`kernel/syscall/linux_abi.c` + `kernel/include/b1nix/linux_abi.h`) that
translates Linux x86_64 syscall numbers to b1nix native numbers for processes
with `PERSONALITY_LINUX`. Since musl compiles against Linux x86_64 headers, we
don't need to modify musl — only extend the kernel's Linux ABI layer.

---

## 1. Syscall Gap Analysis

### Currently mapped in Linux ABI (76 syscalls)

The existing `lx_table[]` covers these Linux x86_64 syscall numbers:
read(0), write(1), open(2), close(3), stat(4), fstat(5), lstat(6), poll(7),
lseek(8), mmap(9), mprotect(10), munmap(11), brk(12), rt_sigaction(13),
rt_sigprocmask(14), rt_sigreturn(15), ioctl(16), access(21), pipe(22),
select(23), sched_yield(24), dup(32), dup2(33), nanosleep(35), alarm(37),
getpid(39), socket(41), connect(42), accept(43), sendto(44), recvfrom(45),
shutdown(48), bind(49), listen(50), getsockname(51), getpeername(52),
setsockopt(54), getsockopt(55), clone(56), fork(57), execve(59), exit(60),
wait4(61), kill(62), uname(63), fcntl(72), fsync(74), ftruncate(77),
getcwd(79), chdir(80), fchdir(81), rename(82), mkdir(83), rmdir(84),
link(86), unlink(87), symlink(88), readlink(89), chmod(90), fchmod(91),
chown(92), fchown(93), umask(95), getuid(102), getgid(104), setuid(105),
setgid(106), geteuid(107), getegid(108), setpgid(109), getppid(110),
getpgrp(111), setsid(112), getpgid(121), getsid(124), setpriority(141),
sync(162), gettid(186), getdents64(217), clock_gettime(228), exit_group(231),
getrandom(318).

Plus special-cased (struct translation): stat/fstat/lstat(4-6), uname(63),
getdents64(217), arch_prctl(158), tkill(200), tgkill(234).

### Missing syscalls musl requires (~30)

**M92: partial.** All ~30 missing syscalls are implemented in the Linux ABI
layer (`kernel/syscall/syscall.c`) with entries in `linux_abi.c`. Compiled but
not verified without a musl-compiled binary.

#### Critical (musl won't compile/link/run without these)

| Linux NR | Name | Implementation strategy |
|----------|------|------------------------|
| 257 | openat | New handler or map to SYS_OPEN with AT_FDCWD dirfd |
| 262 | newfstatat | Map to SYS_STAT/SYS_FSTAT with dirfd |
| 263 | unlinkat | Map to SYS_UNLINK |
| 258 | mkdirat | Map to SYS_MKDIR |
| 316 | renameat2 | Map to SYS_RENAME (ignore flags) |
| 265 | linkat | Map to SYS_LINK |
| 266 | symlinkat | Map to SYS_SYMLINK |
| 267 | readlinkat | Map to SYS_READLINK |
| 268 | fchmodat | Map to SYS_CHMOD |
| 260 | fchownat | Map to SYS_CHOWN |
| 269 | faccessat | Map to SYS_ACCESS |
| 293 | pipe2 | Wrapper: pipe() + fcntl(F_SETFL) for flags |
| 292 | dup3 | Wrapper: dup2() + fcntl(F_SETFD) for O_CLOEXEC |
| 271 | ppoll | Map to SYS_POLL with sigset_t conversion |
| 270 | pselect6 | Map to SYS_SELECT with sigset_t conversion |
| 288 | accept4 | Map to SYS_ACCEPT + fcntl for flags |
| 230 | clock_nanosleep | Map to SYS_SLEEP with clock_id/flags handling |
| 202 | futex | Extend existing: WAIT_BITSET, WAKE_BITSET, REQUEUE, CMP_REQUEUE |
| 435 | set_tid_address | New: store clear_child_tid, return old value |

#### Important (pthread won't work without these)

| Linux NR | Name | Implementation strategy |
|----------|------|------------------------|
| 300 | set_robust_list | Stub: store pointer, return 0 |
| 301 | get_robust_list | Stub: return -ENOSYS |
| 434 | prlimit64 | Map to SYS_GETRLIMIT/SYS_SETRLIMIT |

#### Already handled (no action needed)

| Linux NR | Name | Status |
|----------|------|--------|
| 231 | exit_group | Mapped to SYS_EXIT |
| 186 | gettid | Mapped to SYS_GETTID |
| 200 | tkill | Special-cased in dispatcher |
| 234 | tgkill | Special-cased in dispatcher |
| 332 | statx | Direct via SYS_STATX |
| 302 | splice | Direct via SYS_SPLICE |
| 158 | arch_prctl | Special-cased (ARCH_SET_FS/GET_FS) |
| 318 | getrandom | Mapped to SYS_GETRANDOM |

---

## 2. Struct Layout Compatibility

**M92: partial.** Both struct translations are implemented in the Linux ABI
layer. Compiled but not fully verified without musl.

### Already translated in Linux ABI layer

| Struct | b1nix size | Linux size | Translation |
|--------|-----------|-----------|-------------|
| struct stat | 112 B | 144 B | linux_stat_from_b1nix() |
| struct dirent | 272 B | ~1048 B | sys_linux_getdents64() |
| struct utsname | 160 B | 390 B | linux_utsname_from_b1nix() |

### Matches Linux ABI (no translation needed)

struct timespec (16 B), struct timeval (16 B), struct rlimit (16 B),
struct rusage (144 B), struct sysinfo (112 B), struct tms (32 B),
struct epoll_event (12 B, packed), struct itimerspec (32 B),
struct msghdr (56 B), struct cmsghdr (16 B),
struct sockaddr/sockaddr_in/sockaddr_in6, Elf64_auxv_t (16 B).

### Requires new translation

| Struct | b1nix layout | Linux layout | Issue |
|--------|-------------|--------------|-------|
| struct sigaction | 32 B (sa_mask=u64) | 152 B (sa_mask=__sigset_t=128 B) | Field order matches but sa_mask size differs |
| struct termios | 48 B (c_cc[32], no c_line) | 44 B (c_line, c_cc[19], c_ispeed/ospeed) | Missing fields, different sizes |

---

## 3. ELF Auxiliary Vector Gaps

Current b1nix ELF loader populates only 4 entries:
- AT_NULL(0) — terminator
- AT_PHDR(3) — **hardcoded to 0** (bug)
- AT_ENTRY(9) — entry point address
- AT_B1NIX_DSO_INIT(0x1000) — custom DSO init table

**M92: FIXED.** The loader now populates 15 standard auxv entries:
- AT_NULL(0), AT_PHDR(3), AT_PHENT(4), AT_PHNUM(5), AT_ENTRY(9),
  AT_PAGESZ(6), AT_BASE(7), AT_CLKTCK(17), AT_UID(11), AT_EUID(12),
  AT_GID(13), AT_EGID(14), AT_RANDOM(25), AT_HWCAP(16), AT_SECURE(23),
  AT_EXECFN(31), plus the custom AT_B1NIX_DSO_INIT(0x1000).

AT_PHDR is now correct (was hardcoded to 0). AT_RANDOM provides 16 bytes of
kernel entropy (rdrand + xorshift64* fallback) for stack canary initialization.

musl requires (minimum):

| Entry | Value | Purpose in musl |
|-------|-------|----------------|
| AT_PHDR | phdr VA | dl_iterate_phdr, dynamic linker |
| AT_PHENT | sizeof(Elf64_Phdr)=56 | Program header entry size |
| AT_PHNUM | e_phnum | Number of program headers |
| AT_PAGESZ | 4096 | Page size for mmap alignment |
| AT_BASE | 0 (static) | ELF interpreter base address |
| AT_CLKTCK | 100 | sysconf(_SC_CLK_TCK) |
| AT_UID | task->cred->uid | getuid() at startup |
| AT_EUID | task->cred->euid | geteuid() at startup |
| AT_GID | task->cred->gid | getgid() at startup |
| AT_EGID | task->cred->egid | getegid() at startup |
| AT_RANDOM | stack ptr to 16 bytes | Stack canary initialization (CRITICAL) |
| AT_HWCAP | 0 | Hardware capabilities |
| AT_SECURE | 0 | Secure execution mode |
| AT_EXECFN | program filename | /proc/self/exe equivalent |

**AT_RANDOM is the showstopper:** without it, musl's stack canary is
uninitialized → first printf with format args crashes on stack_chk_fail.

---

## 4. clone() Flag Gaps

**M92: partial.** The Linux ABI clone dispatcher translates the Linux argument
layout and adds CLONE_PARENT_SETTID/CLONE_CHILD_SETTID flags when
parent_tid/child_tid pointers are provided. Compiled but not verified without
musl pthread.

| Flag | Value | Status | Impact |
|------|-------|--------|--------|
| CLONE_VM | 0x100 | Supported | Thread shares address space |
| CLONE_FS | 0x200 | Supported | Thread shares cwd/umask |
| CLONE_FILES | 0x400 | Supported | Thread shares fd table |
| CLONE_SIGHAND | 0x800 | Supported | Thread shares signal handlers |
| CLONE_THREAD | 0x10000 | Supported | Thread group membership |
| CLONE_SETTLS | 0x80000 | Supported | Set TLS base from arg |
| CLONE_CHILD_CLEARTID | 0x200000 | Supported | Futex wake on exit |
| CLONE_PARENT_SETTID | 0x100000 | **MISSING** | Write TID to parent location |
| CLONE_CHILD_SETTID | 0x1000000 | **MISSING** | Write TID to child location |

CLONE_PARENT_SETTID: musl writes `*parent_tid = child_pid` so pthread_join can
find the thread. CLONE_CHILD_SETTID: musl writes `*child_tid = child_pid` so
the thread knows its own TID. Without these, pthread_join may deadlock.

---

## 5. Futex Operation Gaps

**M92: partial.** All futex operations musl requires are implemented as
special-case handlers in the Linux ABI dispatcher. Compiled but not verified
without musl pthread.

| Op | Name | Purpose |
|----|------|---------|
| 9 | FUTEX_WAIT_BITSET | pthread_cond_wait with CLOCK_MONOTONIC |
| 10 | FUTEX_WAKE_BITSET | pthread_cond_signal |
| 4 | FUTEX_REQUEUE | pthread_cond_broadcast (efficient) |
| 8 | FUTEX_CMP_REQUEUE | pthread_cond_broadcast (safe, checks val) |
| 0x80 | FUTEX_PRIVATE_FLAG | Optimization hint (no-op on b1nix) |

FUTEX_WAIT_BITSET differs from FUTEX_WAIT in that the `val3` argument is a
bitmask of allowed signal numbers (typically ~0UL for "any"). b1nix can treat
it identically to FUTEX_WAIT since signals are not a concern for the wait.

FUTEX_REQUEUE wakes `val` waiters on uaddr and requeues `val2` additional
waiters from uaddr to uaddr2. This is critical for efficient pthread_cond_broadcast.

---

## 6. Build Strategy

### musl cross-compilation for b1nix

musl supports custom targets via `configure --host=`. For b1nix:

```sh
# From the musl source tree
CC=x86_64-b1nix-gcc \
AR=x86_64-b1nix-ar \
RANLIB=x86_64-b1nix-ranlib \
CFLAGS="-ffreestanding -nostdlib -fPIC" \
./configure --host=x86_64-b1nix --prefix=/usr --disable-shared

make -j$(nproc) lib/libc.a
```

musl's `config.sub` recognizes `b1nix*` targets (already patched via
`tools/patches/`), and its internal arch support for x86_64 is complete.

### Integration into b1nix build

1. Build musl as `lib/libc.a` in `build/x86/musl/`
2. Install headers to `userspace/include/musl/` (keep separate from b1nix libc)
3. Add `tools/b1nix-musl-cc` wrapper for musl-targeted compilation
4. Link test programs: `b1nix-musl-cc test.c -o test -static -nostdlib -l musl`
5. Embed in initramfs as usual

---

## 7. Effort Estimate (actual)

| Task | Estimated | Actual | Priority |
|------|-----------|--------|----------|
| Linux ABI: *at() syscall mappings | 1-1.5d | ✅ done (Phase 1) | P0 |
| Linux ABI: pipe2/dup3/ppoll/pselect6/accept4 | 0.5d | ✅ done (Phase 1) | P0 |
| Linux ABI: clock_nanosleep/set_tid_address/prlimit64 | 0.5d | ✅ done (Phase 1) | P0 |
| Linux ABI: set_robust_list/get_robust_list stubs | 0.25d | ✅ done (Phase 1) | P1 |
| Futex expansion (WAIT_BITSET/WAKE_BITSET/REQUEUE) | 1d | ✅ done (Phase 1) | P0 |
| clone: PARENT_SETTID + CHILD_SETTID | 0.5d | ✅ done (Phase 1) | P0 |
| ELF auxv: populate 12+ missing entries | 0.5d | ✅ done (pre-existing) | P0 |
| struct sigaction Linux ABI translation | 0.5d | ✅ done (Phase 1) | P1 |
| struct termios Linux ABI translation | 0.25d | ✅ done (Phase 1) | P1 |
| musl cross-compile + build integration | 1d | ✅ done (Phase 2) | P0 |
| Kernel bugfixes (AT_PHDR, vm_find_free_area, fork TLS, set_thread_area) | — | ✅ done (Phase 2) | P0 |
| Smoke test: write/malloc/fork/signal/pipe/open/clock/env | 1-2d | ✅ done (Phase 2) | P0 |
| **Phase 1+2 total** | **~3-4d** | **done** | |
| pthread_create+join | — | ❌ remaining | P0 |
| Socket smoke test | — | ❌ remaining | P1 |
| Dynamic musl (libc.so.1) | — | planned (Phase 3) | P0 |

### Phase 1 (done): Linux ABI

All syscall mappings, struct translations, futex expansion, clone flags.

### Phase 2 (done): musl smoke test

musl cross-compile, b1nix-musl-cc wrapper, 4 kernel bugfixes, 8/9 tests pass.

### Phase 2 remainder: pthread

pthread_create+join crashes in CLONE_VM child thread. Needs investigation of
thread trampoline, TLS setup, and mmap address space in clone.

### Phase 3 (working): real userspace ld.so

Replaces the in-kernel eager linker with a genuine userspace dynamic linker
for musl binaries, per `docs/ldso-migration-and-unix-parity-plan.md`. **A
binary now boots end-to-end through a real userspace ld.so, with zero
in-kernel dynamic-linking involvement — `M92-LDSO: done (ok=4 fail=0)`.**

**Kernel (`kernel/user/process.c`):** a binary whose `PT_INTERP` names
`/lib/ld-musl-x86_64.so.1` gets a real interpreter load. The kernel loads
only the interpreter's own `PT_LOAD` segments (unrelocated — it
self-relocates via its own `R_X86_64_RELATIVE` entries, exactly like on
Linux), sets `AT_BASE`/`AT_ENTRY` correctly, and jumps to the interpreter's
entry point instead of running the in-kernel eager linker for that binary.
Every other `PT_INTERP` value keeps the existing eager-linker path unchanged
— confirmed with a full smoke run (864/5, all 5 failures pre-existing and
unrelated: 1 b1cc corpus test + 4 M55 iostream tests, neither exercising the
musl interpreter path at all).

**`tools/b1nix-musl-cc -ldso`:** new link mode. Produces a PIE with a real
`-Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1`. One post-link fixup is
required and applied here (not in musl, not a test hack — see root cause
below): the `DT_NEEDED` string for musl's own `libc.so` is rewritten from its
embedded `DT_SONAME` (`ld-musl-x86_64.so.1`) to the canonical `libc.so`,
byte-for-byte in place in `.dynstr` (verified NUL-delimited so it can never
touch the unrelated `.interp` string), restoring the exact self-reference
name real musl-gcc toolchains produce.

**Root cause of the crash chased down (not a musl bug):** `ld.so`'s
`load_library()` recognizes a self-referential `DT_NEEDED` two ways — a
short hardcoded prefix list (`libc.`, `libpthread.`, ...) or an exact string
match against `ldso.name` (which gets overwritten to the app's own
`PT_INTERP` path, e.g. `/lib/ld-musl-x86_64.so.1`, once a real interpreter is
in play). Our `-ldso` binaries were linked directly against the `.so` file,
so lld recorded `DT_NEEDED` from its embedded `DT_SONAME`
(`ld-musl-x86_64.so.1`) — which matches *neither* check (not `lib*`-prefixed,
and not textually equal to `/lib/ld-musl-x86_64.so.1`). So `ld.so` fell
through to genuinely re-open and re-map itself as a "new" dependency, where
it then crashed inside `find_sym`/`sysv_lookup` deduping against itself. Real
musl-gcc never hits this because it links via `-lc`, producing
`DT_NEEDED=libc.so` — which matches the hardcoded prefix list immediately, no
reopen. The `.dynstr` post-link fixup above reproduces that exact upstream
convention.

**`userspace/bin/m92_musl_ldso_test.c`** + `tests/musl-smoke.sh`: builds and
embeds an `M92-LDSO` smoke binary (write, malloc, fork+waitpid, getenv — all
resolved through the real ld.so, zero kernel involvement) alongside the
existing eager-linker one.

**Known pre-existing, unrelated regression:** `M92-MUSL-DYN` (the *old*
eager-in-kernel-linker dynamic smoke test) currently SIGSEGVs with the
current `libc.so` build — this predates and is untouched by the ld.so work
above (that binary has no `PT_INTERP` at all, so it takes the exact same code
path as before this session). Not investigated further here; superseded in
intent by the real-ld.so path.

**Not yet touched:** step 1.3 of the plan (deleting the in-kernel eager
linker) — correctly deferred until more of the shipped ports (NetSurf, Mesa,
V8, rustc, ...) are migrated to real interpreters too; they all still depend
on the eager linker today.

---

## 8. Kernel Bugs Found During M92 Port

These were discovered while getting musl to run on b1nix. All are fixed.

### 8a. AT_PHDR for ET_EXEC binaries (`process.c`)

**Bug:** `phdr_vaddr = load_base + e_phoff` computed 0x40 for ET_EXEC (file
offset), but the actual VA is `first_segment_vaddr + (e_phoff - segment_offset)`.
For a binary loaded at 0x400000, the program headers are at 0x400040, not 0x40.

**Impact:** musl's `__init_tls` iterates program headers via `AT_PHDR`. Reading
from address 0x40 (unmapped) causes SIGSEGV.

**Fix:** Walk LOAD segments to find the one containing `e_phoff`, compute the
correct VA. Both `user_load_elf64` and `user_load_elf32` paths updated.

### 8b. `vm_find_free_area` unsorted VMA list (`scheduler.c`)

**Bug:** The VMA list is in reverse-insertion order (newest first), but
`vm_find_free_area` assumed sorted order. When it encountered a VMA below the
search start (0x100000000), it set `current_addr = vma->end` (a low address),
eventually returning an address within the program's .text section.

**Impact:** mmap returned addresses overlapping program code. mallocng's metadata
stored at page boundaries was corrupted, causing `get_meta` assertion failures
in `free()`.

**Fix:** Skip VMAs whose `end <= current_addr` (they are below the search start).

### 8c. Fork TLS inheritance (`scheduler.c`)

**Bug:** `scheduler_fork_current` copied `g_task_altstack_sp`, `g_task_nice`,
`g_task_tgid`, `g_task_ctty_type/index`, `g_task_rlimits` from parent to child,
but forgot `g_task_tls_base`. Child started with FS base = 0.

**Impact:** Any TLS access in the child (musl's `_Fork` cleanup reads `%fs:0x0`)
crashes with SIGSEGV.

**Fix:** Added `g_task_tls_base[c_idx] = g_task_tls_base[p_idx]`.

### 8d. `set_thread_area` (NR 205) unmapped (`syscall.c`)

**Bug:** musl's `__set_thread_area` calls `SYS_set_thread_area` (Linux NR 205)
during `__init_tls`. b1nix doesn't map this syscall, so it returns -ENOSYS.
musl's `__init_tp` checks the return and calls `a_crash()`.

**Impact:** musl binaries crash during TLS initialization before `main()`.

**Fix:** Added no-op handler that returns 0 (b1nix uses `arch_prctl(ARCH_SET_FS)`
instead; the GDT-based TLS is not needed on x86_64).

---

## 9. Risks and Mitigations

| Risk | Likelihood | Mitigation |
|------|-----------|-----------|
| musl's internal assumptions about Linux vDSO | Low | musl falls back to syscall instruction if AT_SYSINFO_EHDR missing |
| Signal delivery differences (b1nix signo != Linux signo) | Medium | Linux ABI layer already remaps signo in rt_sigaction/rt_sigprocmask |
| futex timeout granularity (b1nix uses 10ms ticks) | Low | musl tolerates imprecise sleeps; add a comment |
| struct sigaction sa_mask size mismatch | Medium | Translation layer handles 8B→128B zero-extension |
| stack layout assumptions (red zone, alignment) | Low | musl assumes 16-byte aligned stack, b1nix provides this |
| pthread_create CLONE_VM child thread crash | High | musl's thread trampoline at high mmap address; needs TLS/stack investigation |
