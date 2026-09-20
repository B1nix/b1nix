# Processes and system calls

Milestones M4, M12–M13, M18–M20, M30, M35–M36, M40, M46, M48, M56, M73–M74,
M80, M92, M124 and M125.

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
`statx` and `inotify` round out modern I/O (M73), and io_uring is below (M125).

## Debugging

A crashing process leaves an ELF core dump, and the kernel symbolises its own
addresses through kallsyms (M35). A GDB stub and a function tracer are there for
kernel work (M36). `ptrace(2)` is complete enough for gdb and strace: regset
access (XSAVE on x86_64, FPSIMD on AArch64), the Yama `ptrace_scope` rules,
`/proc/<pid>/task`, and crash capture for a process that faults (M80).

## System calls Linux added later (M124)

These live in `kernel/syscall/linux_modern.c`, `linux_keys.c` and
`kernel/fs/landlock.c`. Each has a probe in the Debian lane (stage 12 of
`tools/image/debian-stage.sh`) that checks results and errno values, not just
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

## io_uring (M125)

`io_uring_setup`, `io_uring_enter` and `io_uring_register` are in
`kernel/fs/io_uring.c`. The ABI is not retyped: `kernel/include/b1nix/io_uring_abi.h`
is Linux 6.18.51's `include/uapi/linux/io_uring.h` verbatim, because liburing is
compiled against those structures and a 64-byte SQE with one field in the wrong
place is a class of bug no test names.

The ring descriptor is a node handle over an anonymous `VFS_DEVICE` inode whose
`mmap_handle_page_phys_cb` resolves the magic offsets, which is the only route
`sys_mmap` has to kernel-owned physical pages. Both rings live in one region
(`IORING_FEAT_SINGLE_MMAP`) and the SQEs in a second.

**A request is issued in the context of the task that submitted it.** There is
no thread per request and no io-wq pool. A file that cannot block is read or
written inline. A file that can — a socket, a pipe, a tty — is asked for its
readiness first with its own `->poll` op; not ready means the request is armed
and left on the ring, and `io_uring_enter`'s wait loop re-tests it every time
`vfs_poll_chan` is woken, which every ISR and every socket receive path already
does (M70). That is what `IORING_FEAT_FAST_POLL` describes, and it is why
submitting a read on an empty pipe returns at once instead of parking the
submitter.

The limit that follows is worth stating rather than discovering: an armed
request makes progress while its owner is inside `io_uring_enter`. Every
liburing and `fio` submission pattern comes back there, because that is what
`IORING_ENTER_GETEVENTS` is for; a program that submits and then blocks on
something else entirely does not get its completion until it returns. SQPOLL,
which needs a kernel thread of its own, is **refused** rather than faked.

Refusing is the theme, and it is the lesson `kernel/fs/mount_api.c` records for
the new mount API: userspace probes for io_uring and switches strategy
wholesale. So `io_uring_setup` answers `EINVAL` to every `IORING_SETUP_*` flag
this kernel cannot honour — SQPOLL, IOPOLL, SQE128, CQE32, CQE_MIXED, NO_MMAP,
ATTACH_WQ, DEFER_TASKRUN, REGISTERED_FD_ONLY, NO_SQARRAY, HYBRID_IOPOLL —
`params->features` advertises only what is true, an opcode that is not
implemented completes with `EINVAL`, and `IORING_REGISTER_PROBE` reports that
same set, which is the mechanism the ABI provides for the question.

What works:

- **Opcodes:** NOP, READ, WRITE, READV, WRITEV, READ_FIXED, WRITE_FIXED, FSYNC,
  SYNC_FILE_RANGE, POLL_ADD, POLL_REMOVE, ACCEPT, CONNECT, SEND, RECV, TIMEOUT,
  TIMEOUT_REMOVE, LINK_TIMEOUT, ASYNC_CANCEL, FILES_UPDATE and CLOSE. An offset
  of `-1` means the descriptor's own position (`IORING_FEAT_RW_CUR_POS`), and
  `O_APPEND` beats any offset the caller passes, as it does on Linux.
  A vectored transfer over a descriptor that can block asks it again between
  vectors and stops when it is no longer ready, which is what a short read is
  for — running on into the next vector parked the submitting task in an empty
  pipe. The iovec array itself is copied at submission, which is what
  `IORING_FEAT_SUBMIT_STABLE` promises.
- **Links.** `IOSQE_IO_LINK` and `IOSQE_IO_HARDLINK` run a chain one request at
  a time; a failure cancels the rest with `ECANCELED` unless the link is hard.
  A chain is collected before its head is issued — issuing the head as it
  arrives lets it complete, and be freed, before the next SQE of the same batch
  is read. A request that is doomed at submission (a bad descriptor, a flag
  combination this kernel refuses) is still made into a request and still put
  on its chain, because the chain has to see the failure.
- **Registration.** `REGISTER_FILES`, `FILES2` (including the sparse form),
  `FILES_UPDATE`, `FILES_UPDATE2`, `UNREGISTER_FILES`, `REGISTER_BUFFERS`,
  `UNREGISTER_BUFFERS`, `REGISTER_EVENTFD`, `UNREGISTER_EVENTFD`,
  `ENABLE_RINGS` and `PROBE`. A registered file is held by reference, so it
  keeps working through `IOSQE_FIXED_FILE` after the descriptor that registered
  it is closed — which is the whole point of registering one, and which is why
  `vfs_read`, `vfs_write`, `vfs_pread`, `vfs_pwrite`, `vfs_fsync`, `vfs_accept`
  and `vfs_connect` grew handle-taking halves.
- **Completions are never dropped** (`IORING_FEAT_NODROP`): one that does not
  fit is parked on a backlog, `IORING_SQ_CQ_OVERFLOW` is raised, and it is
  flushed into the ring as soon as userspace consumes one.
- **`IORING_ENTER_EXT_ARG`**, so `io_uring_wait_cqe_timeout` has a deadline. A
  signal mask passed with it is refused rather than ignored.

What is not there: SQPOLL and IOPOLL, provided-buffer rings
(`IOSQE_BUFFER_SELECT`, `REGISTER_PBUF_RING`), multishot poll and recv,
`SENDMSG`/`RECVMSG`, zero-copy send, `URING_CMD`, `MSG_RING`, personalities,
restrictions, NAPI, ring resizing, and the filesystem opcodes (`OPENAT`,
`STATX`, `RENAMEAT`, `UNLINKAT`, the xattr family). Each is absent from the
probe rather than half-present.

`userspace/bin/smoke/m125_smoke.c` drives the rings by hand in the posix lane —
the head/tail protocol, the layout and the mmap offsets, not liburing's view of
them.

liburing's own suite is the other half of the proof, and it is opt-in because
it is 217 static programs and about 180 MiB of image:
`tools/images/fetch-liburing.sh` builds them (pinned at 2.12, static-PIE),
`LIBURING=1 make debian-image` stages them at `/opt/liburing`, and stage 14 of
`tools/images/debian-stage.sh` runs each under `timeout` and reports its exit
status verbatim — 0 passed, 77 skipped, anything else failed. `b1nix.liburing=`
runs a named subset and `b1nix.liburing-part=N/M` runs a share of the list,
which is how the tail of it is measured in a guest the head has not worn out.
As of this milestone: **89 pass, 48 skip, 72 fail, 8 hang**. Almost every
failure is a test for something refused at setup — SQPOLL, IOPOLL, SQE128,
DEFER_TASKRUN — which those tests treat as a failure rather than a skip; the
rest are opcodes that are not implemented, the syzkaller reproducers (they
`mmap` at a hint below 4 GiB, which this kernel relocates), and the socket
tests that need an ephemeral TCP bind. `fio --ioengine=io_uring` is the second
consumer, run by stage 15 with `FIO=1 make debian-image`, plain and with
`registerfiles=1 fixedbufs=1`.

Two defects liburing found were not in io_uring at all and are fixed here: a
`fsync(2)` on an unlinked file on an imported filesystem answered `EINVAL`
(`lkpifs_fsync` now syncs through the superblock when the name is gone), and a
static `ET_EXEC` faults in its own image before `main()`, which is why the
suite is built `-static-pie`.

## The vDSO

Every process gets a `[vdso]` and a read-only `[vvar]` page (`kernel/vdso/`).
REALTIME, MONOTONIC, MONOTONIC_RAW and BOOTTIME are then read without entering
the kernel: 32 ns per call against 262 ns for the system call on KVM. On
x86_64 the TSC is used only when it is invariant, calibrated and agrees on
every CPU; otherwise the vDSO falls back to the system call, as it does for
processes in a time namespace. `vdso_smoke` proves the fast path under a seccomp
filter that refuses the clock calls, and the Debian lane's `vdso-glibc` probe
does the same for glibc.
