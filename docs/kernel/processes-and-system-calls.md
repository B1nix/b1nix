# Processes and system calls

Milestones M4, M12–M13, M18–M20, M30, M35–M36, M40, M46, M48, M56, M73–M74,
M80, M92, M124, M125 and M126.

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
As of this milestone, measured in six parts at 4 GiB against the same harness
on the branch point:

| | before | after | after, on a boot that did not hit the defect below |
|---|---|---|---|
| pass | 89 | **106** | **116** |
| fail | 73 | 45 | 57 |
| skip | 48 | 28 | 31 |
| timeout | 7 | 13 | 13 |
| tests that ran | 217 | 192 | 217 |

Seventeen tests stopped skipping because the feature they probe for now exists.
The middle column is this tree measured as it stands: two of the six boots end
in the panic described below and lose the tests after it. The right-hand column
is an intermediate build of the same feature set whose boots happened not to
trip it, and is what the features are worth once the defect is fixed.

The timeouts went **up**, and that is worth saying plainly rather than hiding in
the total: `socket`, `send_recv`, `send_recvmsg`, `sendmsg_iov_clean` and
`recv-bundle-short-ooo` used to stop at a refused `IORING_OP_SOCKET` or a
refused setup flag and now get past it, only to reach a UDP `bind(port = 0)`
that this kernel does not answer with an ephemeral port — the same gap
`accept.t` names with `t_bind_ephemeral_port: Assertion 'addr->sin_port != 0'`,
and a networking one, not an io_uring one. The remaining failures are the
opcodes still absent, the syzkaller reproducers (they `mmap` at a hint below
4 GiB, which this kernel relocates), and tests for `IOSQE_IO_DRAIN` ordering.
`fio --ioengine=io_uring` is the second consumer, run by stage 15 with
`FIO=1 make debian-image`, plain and with `registerfiles=1 fixedbufs=1`.

**One defect liburing found is open, and it is worth the space.** Run the two
syzkaller reproducers `232c93d07b74` and `a0908ae19763` in one boot together
with the rest of their part of the suite, and the machine panics later — inside
`__ext4_new_inode`, with the heap allocator's poison in a register, or inside
`ext4_writepages` at the harness's closing `sync`. It needs **both**
reproducers; either one alone is clean, at 4 GiB and at 8 GiB alike, so it is
not simple exhaustion. It disappears if `IORING_OP_OPENAT` is refused — because
the reproducers then create no files at all — which places the trigger at file
creation under the process and memory churn two fork bombs make, and the defect
in the create path's error handling rather than in io_uring. It is recorded here
because io_uring is what made it reachable: before this milestone nothing could
ask the kernel to create a file from a fuzzed SQE. Not root-caused.

Two defects liburing found were not in io_uring at all and are fixed here: a
`fsync(2)` on an unlinked file on an imported filesystem answered `EINVAL`
(`lkpifs_fsync` now syncs through the superblock when the name is gone), and a
static `ET_EXEC` faults in its own image before `main()`, which is why the
suite is built `-static-pie`.

## Observability: perf_event_open (M126)

`kernel/perf/perf_event.c`. The ABI is Linux's, vendored into
`kernel/include/b1nix/perf_event_abi.h` for the same reason io_uring's is: the
distribution's own `perf` is compiled against `struct perf_event_attr`, the
sample-record layout and the mmap'd control page, and a field in the wrong
place is a class of bug no test names. The only edits to the header are the
three host includes and the ioctl numbers, which are spelled out because this
kernel has no `_IO`/`_IOW` macros.

The descriptor is a node handle over an anonymous `VFS_DEVICE` inode, as
io_uring's ring is. `read(2)` gives the count in whichever `PERF_FORMAT_*`
shape was asked for, `ioctl(2)` carries ENABLE/DISABLE/RESET/REFRESH/PERIOD/ID,
`poll(2)` reports the ring buffer readable, and `mmap(2)` maps one control page
plus a power of two of data pages — the length is what says how big, so the
buffer is allocated on the first mapping and not at open.

**What is counted is what this kernel already keeps**, and nothing is invented:

| counter | read from |
|---|---|
| `SW_TASK_CLOCK` | the task's own CPU time (M86 accounting) |
| `SW_CPU_CLOCK` | wall time while the counter is enabled |
| `SW_PAGE_FAULTS`, `_MIN`, `_MAJ` | a per-task pair counted in `vmm_handle_page_fault`; major means the page came from swap or a file |
| `SW_CONTEXT_SWITCHES` | the scheduler's `nvcsw + nivcsw` |
| `SW_DUMMY` | nothing, by definition — `perf` opens one only to get a ring buffer |

`PERF_TYPE_HARDWARE`, `HW_CACHE`, `RAW` and `TRACEPOINT` are **`EOPNOTSUPP` at
open**. There is no PMU driver here — nothing programs `IA32_PERFEVTSELx` and
there is no counter-overflow NMI — and a counter that read zero for ever would
be worse than an honest refusal. A `sample_type` bit whose field this does not
write, `PERF_FORMAT_GROUP`, `attr.inherit` and `attr.precise_ip` are refused for
the same reason: each would otherwise be a wrong number rather than none.

**A sample is taken from the timer tick**, on the CPU that took it, with the
register file of whatever it interrupted — there is no counter-overflow
interrupt to hang sampling off, so the tick *is* the sampling clock.
`attr.freq` asks for N samples a second and gets one every
`SCHED_TICKS_PER_SEC / N` ticks; an `attr.sample_period` on a clock counter is
charged the tick's worth of nanoseconds, and a period on an event counter is
refused rather than approximated. The period written into every
`PERF_RECORD_SAMPLE` says what that sample is worth, so the profile is
statistically what `perf record` produces, at the resolution the timer gives.

The user call chain is walked from the frame pointer **without faulting**: each
link is resolved through the page tables with `paging_user_frame` and a page
that is not resident simply ends the chain. A sample is taken in interrupt
context, so nothing on that path allocates, sleeps, or takes a lock another path
holds while it sleeps.

The buffer also carries `PERF_RECORD_COMM` and a `PERF_RECORD_MMAP2` per
file-backed mapping, written when the buffer is first mapped, and
`PERF_RECORD_EXIT` when a watched task goes — without them a sampled address is
a number with nothing to resolve it against. Every non-`SAMPLE` record carries
the `sample_id_all` trailer, whose field order is the ABI's and is parsed from
the *end* of the record: one field too many or too few makes every record after
it nonsense.

`/proc/sys/kernel/perf_event_paranoid` is real and enforced: 2 (the Linux
default) lets an unprivileged caller profile only tasks of its own thread group,
1 and 0 relax that, and anything machine-wide needs `CAP_SYS_ADMIN`.

What is **not** there: PMU hardware counters, tracepoints, eBPF programs
attached to an event, `PERF_EVENT_IOC_SET_OUTPUT` (redirecting one event's
records into another's buffer), `rdpmc` from userspace (`cap_user_rdpmc` stays
clear, so `perf` uses `read(2)`), and counters that follow `fork`. The
distribution's `perf record` has not been run against this.

`tests/programs/bin/smoke/m126_smoke.c` proves each counter by making the thing
it counts happen — 120 ms of spinning against the task clock, 256 fresh pages
against the fault counter, twenty sleeps against the switch counter — and then
maps a ring, samples 300 ms at 200 Hz and walks the records it finds.

## The vDSO

Every process gets a `[vdso]` and a read-only `[vvar]` page (`kernel/vdso/`).
REALTIME, MONOTONIC, MONOTONIC_RAW and BOOTTIME are then read without entering
the kernel: 32 ns per call against 262 ns for the system call on KVM. On
x86_64 the TSC is used only when it is invariant, calibrated and agrees on
every CPU; otherwise the vDSO falls back to the system call, as it does for
processes in a time namespace. `vdso_smoke` proves the fast path under a seccomp
filter that refuses the clock calls, and the Debian lane's `vdso-glibc` probe
does the same for glibc.


## Observability: perf, userfaultfd, fanotify and eBPF (M126)

Four ways for a program to see what the kernel is doing, and in two of the four
to change the answer.

### perf_event_open and the PMU

A counter is a descriptor: `read(2)` gives the count, `ioctl(2)` starts and
stops it, `mmap(2)` gives the ring buffer records are written into. The
software counters come from accounting the kernel already keeps; the hardware
ones come from the CPU (`kernel/perf/pmu_x86.c`), found through CPUID's
architectural performance-monitoring leaf.

Two details there are worth stating, because both were found by running the
distribution's own `perf` rather than by reading a manual:

* **The fixed counters matter.** A KVM guest may back `IA32_FIXED_CTR1`
  (unhalted core cycles) and not the same event on a general-purpose counter:
  cycles read as forty thousand over 120 ms of spinning through `IA32_PMCx`,
  and as 549 million through the fixed counter. Linux prefers the fixed
  counters for cycles, instructions and reference cycles, and so does this.
* **Attribution is by interval, not by counter.** The counters are read at
  every context switch and every tick, and the delta is credited to the task
  that ran over that interval — which is exact at the switch boundaries, since
  within a timeslice there is only one task to credit. A counter on a child
  blocked in `read(2)` sees twenty thousand instructions while its parent burns
  four hundred million.

`perf stat`, `perf record` and `perf report` from Debian work on this kernel.
Making them work needed `attr.inherit` (perf sets it on everything it opens),
`enable_on_exec` (a counter opened on a child before it execs must start when
the program does, not when the fork did), group reads, and `PERF_FORMAT_LOST`
— which is a Linux 6.0 field `perf record` asks for unconditionally, and whose
refusal made the whole recording fail before it started.

### userfaultfd

A fault in a registered range stops the faulting thread and puts a message on
a descriptor; a monitor decides what the page should hold and fills it in with
`UFFDIO_COPY` or `UFFDIO_ZEROPAGE`. The hook sits at the TOP of the fault
handler, before any case looks at the page: a fresh anonymous read would
otherwise be answered with the shared zero page and a write to a
write-protected page would quietly take the copy-on-write path, and in both
cases the monitor would never hear about the access it exists to see.

A monitor that goes away does not freeze its process: closing the descriptor
releases every waiter, and the fault falls through to the ordinary zero-fill.

### fanotify

inotify says what changed in a directory. fanotify says what the machine is
DOING to its files — a mark on a whole mount, an open descriptor per event, and
a permission event whose `FAN_DENY` makes the `open(2)` that triggered it
return EPERM. That is the shape an antivirus scanner or a file-integrity
monitor needs.

The trap worth remembering: an event carries a descriptor, and reading or
closing THAT descriptor is an access like any other, so it produces another
event, which produces another descriptor. A monitor doing its job feeds itself
for ever and the machine stops doing anything else. Linux marks those files
`FMODE_NONOTIFY`; this kernel marks the handle `no_notify`, which is what
`struct vfs_handle` grew that field for.

### eBPF

A program is verified, loaded and interpreted, and can be attached to a perf
event so that it runs on every sample. Maps are how it keeps state that
userspace reads while it runs: a profiler that counts into a map writes no
records at all.

The verifier is the part that matters. It walks every path with a model of
what each register holds — uninitialised, a number, the context, a stack
pointer at a known offset, a map, a map value that may be NULL, a map value
that has been tested — and refuses the moment an instruction could do
something the model cannot prove is in bounds. Backward jumps are refused
outright, which is what makes the walk terminate; Linux required the same
until bounded loops arrived.

What is missing is stated at load time rather than discovered later: no JIT,
no BTF (so no CO-RE), no kprobe or tracepoint attach points, and no program
types outside the tracing ones.
