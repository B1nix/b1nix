# M95: POSIX/Unix-Compatible ABI Port — Comprehensive Plan

## Goal

Make b1nix's kernel ABI **Linux x86_64 compatible** — signal numbers, struct layouts, and syscall conventions all match Linux x86_64 — so stock musl (and eventually glibc) binaries compiled for Linux run without a translation layer.

## Motivation

Currently b1nix uses its own in-house syscall numbers (SYS_WRITE=1, SYS_MEM=2, etc.), non-standard signal numbering (SIGABRT=1, SIGUSR1=19), and non-standard struct field orders (struct stat, struct utsname). A `linux_abi.c` translation layer maps Linux syscall numbers to b1nix handlers, but this adds complexity and limits compatibility with stock binaries.

**Important clarification:** POSIX (ISO/IEC 9945) defines the *API* — which fields must exist in structs and which functions must be available — but **does NOT define the ABI** — exact field order, byte sizes, and memory padding. Different Unix systems (Linux, FreeBSD, macOS) have different binary layouts for the same POSIX API. For example, `struct stat` has different field orders on Linux vs FreeBSD vs macOS, even though all three implement the POSIX `stat()` function.

Signal numbers are closer to universal (SIGHUP=1, SIGINT=2, SIGKILL=9 are the same on Linux, FreeBSD, macOS, OpenBSD) due to historical Unix heritage, but there are exceptions (e.g. `SIGINFO` in BSD/macOS differs from Linux, RT signal ranges vary).

**Our actual target is Linux x86_64 ABI compatibility**, not generic POSIX compatibility. We align with Linux because musl and glibc target Linux — they are compiled with `__linux__` defined and use Linux syscall numbers and struct layouts.

## Scope

This milestone covers **kernel-side changes only** — making the kernel natively POSIX-compatible so the translation layer can be eliminated. The musl libc is already cross-compiled and working; it will need its syscall table rewritten to use b1nix numbers instead of Linux numbers.

---

## Part 1: Signal Numbers

### Current State
b1nix signal numbers are non-standard:
```
SIGABRT=1,  SIGFPE=2,  SIGILL=3,  SIGSEGV=4,  SIGBUS=5
SIGTERM=7,  SIGUSR1=19, SIGUSR2=20, SIGCHLD=17
_NSIG=31,  RT signals=32..63
```

### Target (POSIX/Linux x86_64)
```
SIGHUP=1,  SIGINT=2,   SIGQUIT=3,  SIGILL=4,   SIGTRAP=5
SIGABRT=6, SIGBUS=7,   SIGFPE=8,   SIGKILL=9,  SIGUSR1=10
SIGSEGV=11, SIGUSR2=12, SIGPIPE=13, SIGALRM=14, SIGTERM=15
SIGCHLD=17, SIGCONT=18, SIGSTOP=19, SIGTSTP=20, SIGTTIN=21
SIGTTOU=22, SIGURG=23,  SIGXCPU=24, SIGXFSZ=25, SIGVTALRM=26
SIGPROF=27, SIGWINCH=28, SIGIO=29,  SIGPWR=30,  SIGSYS=31
RT signals=32..63,  _NSIG=65
```

### Changes Required

**`kernel/include/b1nix/sched.h`** (L68-109):
- Replace all `#define SIG*` with POSIX signal numbers
- Change `#define _NSIG 31` → `_NSIG 65`
- Change `sigactions[31]` → `sigactions[65]` (struct task, L231)
- Change bitmask type from `u64` to two `u64`s (pending_signals, blocked_signals — already u64, just widen the sigactions array)

**`userspace/include/signal.h`**:
- Same signal number definitions as kernel

**`kernel/syscall/syscall.c`** (signal delivery):
- Verify signal mask bit positions match the new signal numbers
- Verify `sigset_t` handling in `sys_rt_sigprocmask`, `sys_rt_sigaction`
- Verify `sigaltstack` works with the new _NSIG

**Impact on M74 RT signals**: RT signal range shifts from 32..63 to 32..63 (unchanged — RT signals are always > 31). The side-table allocation logic for RT signals is unaffected.

---

## Part 2: Struct Layout Changes

### 2a. `struct stat`

**Current b1nix field order** (`kernel/include/b1nix/posix.h` L123-146):
```c
struct stat {
  u64 st_dev, st_ino, st_nlink;
  u32 st_mode, st_uid, st_gid, st_rdev;
  u64 st_size;
  struct { long tv_sec; long tv_nsec; } st_atim, st_mtim, st_ctim;
  u64 st_blksize, st_blocks;
};
```

**Linux x86_64 field order**:
```c
struct stat {
  u64 st_dev, st_ino, st_nlink;
  u32 st_mode, st_nlink (again!), st_uid, st_gid, st_rdev;
  u64 __pad1; // 0 on 64-bit
  u64 st_size, st_blksize;
  i64 st_blocks;
  struct timespec st_atim, st_mtim, st_ctim;
  u64 st_ino (again!);
};
```

**Action**: Add `linux_stat_from_b1nix()` and `b1nix_stat_from_linux()` conversion functions in `kernel/syscall/linux_abi.c` (already exists). Then add **native** `sys_stat`/`sys_fstat`/`sys_lstat` handlers in `kernel/syscall/syscall.c` that return Linux-compatible struct stat directly — no translation layer needed.

**Keep**: `struct b1nix_stat` in `kernel/include/b1nix/posix.h` for internal use (VFS returns b1nix_stat). Add new `struct posix_stat` for the native ABI.

### 2b. `struct utsname`

**Current b1nix**: `char field[32]` per field (5 fields = 160 bytes)
**Linux**: `char field[65]` per field (5 fields = 325 bytes)

**Action**: Widen `struct b1nix_utsname` from 32 to 65 bytes per field, or define a new `struct posix_utsname` and update `sys_uname()` to fill it.

### 2c. `struct termios`

**Current b1nix** (`kernel/include/b1nix/posix.h` L148-154):
```c
struct b1nix_termios {
  u32 c_iflag, c_oflag, c_cflag, c_lflag;
  u8 c_cc[32];
};
```

**Linux x86_64**:
```c
struct termios {
  u32 c_iflag, c_oflag, c_cflag, c_lflag;
  u8 c_line;     // <-- missing in b1nix
  u8 c_cc[19];   // <-- b1nix has [32]
};
```

**Action**: Add `c_line` field and shrink `c_cc` from [32] to [19], or keep [32] and add padding. The key is the **struct size and layout** must match what musl expects.

### 2d. `struct dirent`

Need to verify the kernel's `getdents64` implementation returns Linux-compatible `struct linux_dirent64`. If it does, no change needed.

---

## Part 3: Syscall Number Renumbering

### Current b1nix numbers (`kernel/include/b1nix/syscall.h`)
```
SYS_WRITE=1, SYS_MEM=2, SYS_SPAWN=3, SYS_LIST=4, SYS_READ_FILE=5
SYS_OPEN=7, SYS_READ=8, SYS_CLOSE=9, SYS_LSEEK=10, SYS_STAT=11...
```

### Target: Keep b1nix numbers, but...
The approach is: **keep b1nix native syscall numbers** in the kernel, but update **musl's `bits/syscall.h.in`** to map Linux x86_64 numbers to b1nix syscall numbers. This means:
- Kernel continues using its own numbering internally
- musl's `__NR_*` macros map to b1nix numbers, not Linux numbers
- No linux_abi.c translation layer needed for musl

### What changes in musl
`build/src/musl/x86_64-b1nix/musl-1.2.5/arch/x86_64/bits/syscall.h.in`:
- Currently contains Linux x86_64 numbers (because musl was compiled with `-D__linux__`)
- Must be rewritten to contain b1nix syscall numbers

### What changes in musl build
`tools/ports/build-musl.sh`:
- Remove `-D__linux__` flag from CFLAGS (currently forces musl to think it's Linux)
- Let musl use b1nix's own bits headers directly

### What changes in b1nix-cc
`tools/b1nix-musl-cc`:
- Currently patches ELF header EI_OSABI to ELFOSABI_LINUX — keep this for compatibility
- Or remove if musl's own headers set the right OSABI

---

## Part 4: New Syscall Handlers (Kernel)

Musl requires ~74 syscalls that b1nix doesn't have. Most can be stubbed with ENOSYS. The critical ones that must be implemented:

### Must Implement (musl startup/runtime depends on these)
| Syscall | Priority | Notes |
|---------|----------|-------|
| `SYS_set_tid_address` | High | musl pthread uses `set_tid_address(pd->tid)` for thread cleanup |
| `SYS_set_robust_list` | High | musl pthread uses for robust futex list |
| `SYS_futex` | High | Already stubbed, needs real impl for pthreads |
| `SYS_rt_sigprocmask` | High | Already exists, verify Linux compat |
| `SYS_rt_sigaction` | High | Already exists, verify Linux compat |
| `SYS_ioctl` | High | Already exists |
| `SYS_openat` | High | musl prefers `openat` over `open` |
| `SYS_statx` | High | Already exists |
| `SYS_getrandom` | Medium | libc needs entropy |
| `SYS_clock_gettime` | Medium | Already exists |

### Can Stub with ENOSYS (74 total)
Most are Linux-specific (io_uring, perf events, cgroups, seccomp-notify, etc.) that musl only calls if explicitly requested by the application.

---

## Part 5: Implementation Order

### Phase 1: Signal Numbers (kernel + userspace)
1. Update `kernel/include/b1nix/sched.h` — new signal numbers, _NSIG=65
2. Update `userspace/include/signal.h` — matching signal numbers
3. Update `kernel/syscall/syscall.c` — verify signal delivery with new numbers
4. Update `kernel/syscall/signal.c` — verify sigset_t bit operations
5. Run smoke tests — signal-related tests (M12-SMOKE) must still pass

### Phase 2: Struct Layouts
1. Add `struct posix_stat` to `kernel/include/b1nix/posix.h`
2. Add native `sys_stat`/`sys_fstat`/`sys_lstat` in `kernel/syscall/syscall.c`
3. Update `struct b1nix_utsname` field sizes from [32] to [65]
4. Update `struct b1nix_termios` to include `c_line` and shrink `c_cc` to [19]
5. Run smoke tests — M12-SMOKE stat tests must pass

### Phase 3: musl Syscall Table Rewrite
1. Rewrite `build/src/musl/x86_64-b1nix/musl-1.2.5/arch/x86_64/bits/syscall.h.in`
2. Remove `-D__linux__` from `tools/ports/build-musl.sh`
3. Update `tools/b1nix-musl-cc` (keep EI_OSABI patch or remove)
4. Rebuild musl, verify linking
5. Run M12-SMOKE, M13-SMOKE — musl tests must pass

### Phase 4: New Syscall Handlers
1. Implement `sys_set_tid_address` in `kernel/syscall/syscall.c`
2. Implement `sys_set_robust_list` in `kernel/syscall/syscall.c`
3. Verify `sys_futex` is sufficient for musl pthreads
4. Stub remaining 71 syscalls with `-ENOSYS`
5. Run full smoke suite

### Phase 5: linux_abi.c Removal (Optional, after verification)
1. Verify all musl programs work with new native ABI
2. Remove `linux_abi.c` translation table
3. Remove `LINUX_SYS_*` definitions
4. Remove linux personality detection in `kernel/user/process.c`
5. Run full smoke suite — must be 100% pass

---

## Part 6: Files to Modify

| File | Change |
|------|--------|
| `kernel/include/b1nix/sched.h` | Signal numbers, _NSIG=65, sigactions[65] |
| `kernel/include/b1nix/posix.h` | struct utsname [65], struct stat (new), struct termios |
| `kernel/include/b1nix/syscall.h` | No change (keep b1nix numbers) |
| `kernel/syscall/syscall.c` | New native stat/utsname handlers |
| `kernel/syscall/signal.c` | Verify signal delivery with new numbers |
| `kernel/syscall/linux_abi.c` | Remove after Phase 5 |
| `kernel/user/process.c` | Remove personality detection (eventually) |
| `userspace/include/signal.h` | POSIX signal numbers |
| `build/src/musl/.../bits/syscall.h.in` | b1nix syscall numbers |
| `tools/ports/build-musl.sh` | Remove `-D__linux__` |
| `tools/b1nix-musl-cc` | Verify/adjust EI_OSABI patch |
| `docs/roadmap.md` | Add M95 milestone |

---

## Part 7: Verification

After each phase, run the full smoke suite:
```sh
make ARCH=x86 KERNEL_CMDLINE="b1nix.test=1" iso
sh tests/smoke.sh x86
```

Key test modules:
- **M12-SMOKE**: Syscalls, signals, process management — must pass after Phase 1-2
- **M13-SMOKE**: musl libc, stdio, execve — must pass after Phase 3
- **M13-JC**: Job control, SIGTSTP/SIGCONT — must pass after Phase 1
- **Full suite**: 864/0 baseline — must remain 864/0 after all phases

---

## Part 8: Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Signal mask bit positions change → signal delivery breaks | High | Test each signal individually; verify sigset_t bit ops |
| Struct stat size mismatch → userspace reads garbage | High | Add compile-time sizeof assertions |
| musl build fails without `-D__linux__` | Medium | Keep `-D__linux__` as fallback if needed |
| Existing tests break due to signal number changes | Medium | Run smoke after each phase, fix incrementally |
| RT signal side-table allocation breaks | Low | RT signals are > 31, range unchanged |

---

## Part 9: Success Criteria

- [ ] Signal numbers match POSIX/Linux x86_64 (SIGHUP=1..SIGSYS=31, RT=32..63)
- [ ] `_NSIG = 65`, `sigactions[65]` in struct task
- [ ] `struct stat` matches Linux x86_64 layout
- [ ] `struct utsname` uses 65-byte fields
- [ ] `struct termios` includes `c_line` and `c_cc[19]`
- [ ] musl `bits/syscall.h.in` uses b1nix syscall numbers
- [ ] musl builds without `-D__linux__` (or with it, as fallback)
- [ ] `linux_abi.c` translation layer removed
- [ ] All smoke tests pass: 864/0
- [ ] Stock musl programs (M12-SMOKE, M13-SMOKE) pass without translation

---

## Version

This is a **minor version bump** (0.94.0 → 0.95.0) because it changes the kernel ABI — a breaking change for any existing binaries compiled against the old b1nix ABI. All native userspace binaries (m12_smoke, m13_smoke, etc.) must be recompiled against the new headers.
