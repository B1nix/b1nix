# Processes and system calls

Milestones M4, M12–M13, M18–M20, M30, M35–M36, M40, M46, M48, M56, M73–M74,
M80, M92 and M124.

## The Linux ABI

Every system call enters through the Linux ABI with Linux's numbers: about 230
calls translated in M40, and since M121 there is no other interface. Each
architecture has its own number table (asm-generic on AArch64). A call nobody
maps answers ENOSYS and logs its name once. When a program reports an error
without saying where it came from, `b1nix.trace-errno=<n>` or `=all` names the
call, the pid and the path. `b1nix.strace-pid=<pid>` prints every call one task
makes.

## Programs and processes

The ELF loader handles PT_LOAD, PT_INTERP and PIE, builds the argument, auxv and
environment layout Linux programs expect, and rejects malformed files (M4, M18,
M30). musl is the default libc (M92), and glibc runs in the Debian lane.
`/proc/<pid>/exe` is a magic link to the file that was executed, so it still
works after that file is unlinked or its mount is gone.

Processes are created with `fork`, `vfork`, `clone` and `clone3`. File
descriptor tables are per process and honour close-on-exec, and cwd and umask
are inherited (M12, M19). Signals follow POSIX: handlers, masks, `sigaltstack`,
restartable calls, real-time signals with queued values, and POSIX timers (M74).
The kernel owns the signal trampoline. Job control works, and a terminal applies
its line discipline, control keys and controlling-terminal rules (M13, M20).
The fixes from the VFS/POSIX audit also landed here: `exit_group`, `SIGHUP` to
orphaned process groups, and `waitpid` corner cases (M46).

## Descriptors that are not files

Descriptors can be passed over AF_UNIX sockets with `SCM_RIGHTS`, and
credentials with `SCM_CREDENTIALS` (M48). A message may carry descriptors and no
data. `memfd_create` supports seals, and `epoll`, `eventfd`, `timerfd` and
`signalfd` make up the event-loop primitives (M56). `sendfile`, `splice`,
`statx` and `inotify` round out modern I/O (M73); io_uring is M125.

## Debugging

A crashing process leaves an ELF core dump, and the kernel symbolises its own
addresses through kallsyms (M35). A GDB stub and a function tracer are there for
kernel work (M36). `ptrace(2)` is complete enough for gdb and strace: regset
access (XSAVE on x86_64, FPSIMD on AArch64), the Yama `ptrace_scope` rules,
`/proc/<pid>/task`, and crash capture for a process that faults (M80).

## System calls Linux added later (M124)

These live in `kernel/syscall/linux_modern.c`, `linux_keys.c` and
`kernel/fs/landlock.c`. Each has a probe in the Debian lane (stage 12 of
`tools/images/debian-stage.sh`) that checks results and errno values, not just
that the call no longer answers ENOSYS.

- **`openat2`** resolves each path component itself, honouring every
  `RESOLVE_*` flag: no cross-device, no magic links, no symlinks, beneath, in
  root and cached.
- **`pidfd_getfd`** copies a descriptor after a ptrace access check. **`kcmp`**
  compares files, address spaces and descriptor tables.
- **futex2:** `futex_wait`, `futex_wake` and `futex_waitv` on 32-bit futexes,
  with absolute timeouts.
- **Memory:** `process_madvise` takes the advice as hints, `process_mrelease`
  works only on a process being killed, `cachestat` counts cached and dirty
  pages, and `remap_file_pages` is emulated the way Linux does.
- **`sched_setattr` / `sched_getattr`** handle `SCHED_OTHER` with nice.
- **Memory policies** (`mbind` and friends) have single-node semantics.
- **`statmount` / `listmount`** use mount ids that are never reused.
- **The key retention service** (`add_key`, `request_key`, `keyctl`) supports
  keyring, user and logon keys. There is no upcall, so a miss is ENOKEY.
- **Landlock ABI 3** confines filesystem access in layered rulesets, and
  symlinks cannot escape.

- **Protection keys** (x86 PKU, `kernel/arch/x86_64/pkeys.c`) where the CPU has
  them: each thread's PKRU travels with it across a context switch, a signal
  handler starts from the initial rights and `sigreturn` puts back the
  interrupted ones, `pkey_alloc`/`pkey_free` keep a per-address-space
  allocation map that `fork` copies and `execve` clears, and `pkey_mprotect`
  writes the key into the page-table entries — including the lazy ones a page
  is faulted in from. A `PROT_EXEC`-only mapping goes under the address
  space's execute-only key, so its code runs and cannot be read. An access the
  keys refuse is `SIGSEGV` with `SEGV_PKUERR` and `si_pkey`; the kernel's own
  copies to and from a refused page are `EFAULT`, as they are on Linux. A CPU
  without the feature answers as Linux does there: `pkey_alloc` is `ENOSPC`.
- **`memfd_secret`** (`kernel/mm/secretmem.c`) hands out pages that are in no
  kernel mapping at all: they are taken, 2 MiB at a time, out of the direct
  map, the kernel-image window and every address space's identity window, and
  the removal is verified before a page is handed over. The file is mapped
  shared or not at all, its size is set once, `read`/`write` on the descriptor
  are `EINVAL`, and `/proc/<pid>/mem`, `process_vm_readv` and
  `PTRACE_PEEKDATA` cannot reach the pages. They count against
  `RLIMIT_MEMLOCK`, are never swapped, never dumped in a core, and are wiped
  before the memory goes back to the allocator.
- **`quotactl` / `quotactl_fd`** are Linux's own `fs/quota/quota.c`, imported
  whole (`kernel/lkpi/fs_quotactl.c`): b1nix names the filesystem and the
  command handlers are upstream's. See
  [filesystems-and-storage.md](filesystems-and-storage.md).

The robust futex list is kept per thread. When a thread exits or execs, every
robust mutex it still holds is marked `FUTEX_OWNER_DIED` and a waiter is woken,
the way Linux does it; priority-inheritance entries are skipped.

## The vDSO

Every process gets a `[vdso]` and a read-only `[vvar]` page (`kernel/vdso/`).
REALTIME, MONOTONIC, MONOTONIC_RAW and BOOTTIME are then read without entering
the kernel: 32 ns per call against 262 ns for the system call on KVM. On
x86_64 the TSC is used only when it is invariant, calibrated and agrees on
every CPU; otherwise the vDSO falls back to the system call, as it does for
processes in a time namespace. `vdso_smoke` proves the fast path under a seccomp
filter that refuses the clock calls, and the Debian lane's `vdso-glibc` probe
does the same for glibc.
