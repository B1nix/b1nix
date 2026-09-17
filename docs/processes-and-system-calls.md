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

Three calls give the answers of a system without the feature. Protection keys
answer as on a CPU without PKU. `quotactl` validates its target and then says
no filesystem here has quotas. `memfd_secret` does not exist yet, because pages
cannot be removed from the kernel direct map.

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
