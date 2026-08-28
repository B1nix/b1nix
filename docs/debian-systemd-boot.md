# Booting Debian with systemd as PID 1

[`docs/debian-glibc-boot.md`](debian-glibc-boot.md) got a Debian glibc userspace
running under sysvinit and ended by listing what systemd would need and calling
it a milestone rather than a fix. This is that milestone.

Everything below runs unmodified Debian binaries — systemd 252.39, dbus 1.14.10,
util-linux's agetty, Debian's own units. Nothing in the guest is patched to work
around a kernel defect; where the guest failed, the kernel was fixed.

## Running it

```sh
make systemd-image     # downloads debian:bookworm-slim once, builds debian-systemd.ext4
make systemd-smoke     # boots it and checks the markers
```

`tools/images/mk-debian-image.sh` gained a `PROFILE=systemd` mode. It resolves
the **dependency closure** of `systemd systemd-sysv dbus procps` against the
suite's `Packages` index, using the base layer's own `dpkg` status as "already
installed" — 23 packages, none of them hand-listed, so a point release does not
break the recipe. It then makes the configuration `dpkg`'s maintainer scripts
would have made: a machine-id, an fstab, `console-getty.service` enabled, and
one unit of ours (`b1nix-smoke.service`) that runs the marker harness after
`multi-user.target`.

`tests/systemd-smoke.sh` boots the normal b1nix ISO with

```
root=LABEL=b1nix-systemd init=/sbin/init systemd.unit=multi-user.target
```

## Where it got to

| Stage | Result | Evidence |
|---|---|---|
| 1. systemd runs as PID 1 | yes | `systemd 252.39-1~deb12u2 running in system mode … default-hierarchy=unified`; `/proc/1/comm` = `systemd` (`SYSTEMD-SMOKE: ok pid1-systemd`) |
| 2. cgroup v2 mounted, units placed in it | yes | `SYSTEMD-SMOKE: ok cgroup2 (/system.slice/b1nix-smoke.service)`, `controllers=pids` |
| 3. API filesystems mounted | yes | /proc, /sys, /dev (devtmpfs), /run, /dev/shm, /dev/pts, /sys/fs/cgroup |
| 4. generators run, default target queued | yes | `Queued start job for default target multi-user.target.` |
| 5. `systemctl` answers | yes | `SYSTEMD-SMOKE: is-system-running=starting` → `ok systemctl-state` |
| 6. journald active, `journalctl` reads back | yes | `ok unit-active systemd-journald.service`, `ok journalctl (5 lines)` |
| 6b. systemd-udevd runs and stays up | yes | `ok unit-active systemd-udevd.service`; udevd forks a worker per device and applies its rules |
| 7. sysinit / basic / multi-user targets | reached | `ok unit-active sysinit.target`, `basic.target`, `multi-user.target` |
| 8. dbus system bus | active | `dbus.service loaded active running` |
| 9. transient unit over the bus | yes | `systemd-run --wait --pipe /bin/sh -c 'echo …'` → `ok systemd-run` |
| 10. login prompt on the serial console | yes | `Debian GNU/Linux 12 b1nix console` / `b1nix login:` |

`Startup finished in ~2.0s (kernel) + ~2.5s (userspace)`.

## The kernel defects this found

Each one is what a real program did, what this kernel did wrong, and what broke.

### Stopping PID 1 outright

1. **Every file under /proc and /sys was a character device.** b1nix builds them
   as `VFS_DEVICE` nodes because that is the node type whose read goes to a
   callback rather than through the page cache — a statement about how the read
   is served, not about what the file is. `stat` reported `S_IFCHR`. systemd's
   `read_virtual_file()` fstats `/proc/cmdline` and returns `EBADF` for anything
   that is not `S_ISREG`, so PID 1 died at `Failed to fix up PID 1 environment:
   Bad file descriptor` before starting a single unit. Fixed with an inode flag
   (`VFS_NODE_PSEUDO_REG`) that says "regular file to userspace"; `getdents`
   d_type follows the same rule.

2. **`/proc/<pid>/fd/N` froze its target at first use.** The symlink was
   materialised once with a strdup of whatever the descriptor pointed at then.
   systemd mounts through a descriptor — `mount_nofollow()` opens the target
   `O_PATH` and calls `mount(…, "/proc/self/fd/4", …)` — so a stale link put the
   filesystem somewhere else. Every API filesystem systemd mounted (/sys, /dev,
   /run, /dev/shm, /sys/fs/cgroup) landed on top of **/sys**. The link is now
   resolved at read time from the owner's live fd table, and a closed descriptor
   is `ENOENT`.

3. **`statfs` only worked on a mount root, and the synthetic filesystems had
   none at all** — everything else got `ENOSYS`, which userspace reads as "this
   kernel has no statfs". systemd decides whether the machine has a unified
   cgroup hierarchy by `statfs("/sys/fs/cgroup")`. Now `statfs` resolves the
   node's mount and asks its root, and procfs/sysfs/cgroup2 report their real
   magic numbers.

4. **No cgroup v2 filesystem.** Implemented: `kernel/fs/cgroup.c`, see below.

5. **`devtmpfs` was an alias for tmpfs.** Mounting it on /dev — which systemd
   does early — replaced every device node in the machine with an empty
   directory. PID 1 lost `/dev/console` mid-boot and printed nothing for the
   rest of the run. The devtmpfs mount now publishes its root and then asks the
   VFS to lay the device nodes down inside it (`vfs_populate_dev`).

6. **`mount(2)` had no propagation flags, no `MS_BIND`, no `MS_REMOUNT`, and
   `EFAULT`ed on a NULL source or type.** systemd's first act as PID 1 is
   `mount(NULL, "/", NULL, MS_REC|MS_SHARED, NULL)`, and it treats the failure
   as fatal. Mounts also recorded the *raw target string* rather than the
   canonical path, so with systemd mounting through `/proc/self/fd/N` every row
   of `/proc/self/mountinfo` said `/proc/self/fd/4` and nothing could be
   recognised as already mounted.

7. **`prctl` knew almost nothing.** `PR_GET_SECUREBITS` returned `EINVAL`, so
   systemd concluded the bits differed from what it wanted, called
   `PR_SET_SECUREBITS`, got `EINVAL` from that too, and failed *every* service
   at step `SECUREBITS` (status 213). Added: `GET/SET_SECUREBITS` (with the
   LOCKED bits enforced and the flags actually changing what
   `cred_refresh_caps` does on a uid transition), `GET/SET_KEEPCAPS`,
   `CAPBSET_READ/DROP`, `GET_NAME`, `GET_PDEATHSIG`, `GET/SET_DUMPABLE`,
   `GET/SET_TIMERSLACK`.

8. **`timerfd` ignored `TFD_TIMER_ABSTIME` and overflowed.** systemd arms its
   "the wall clock was stepped" watch for `TIME_T_MAX` seconds; multiplying that
   by the tick rate wrapped a `u64` back to a small number, so the timer that
   must never fire fired at once. Every pass of the event loop tore the watch
   down and rebuilt it: `Looping too fast. Throttling execution a little.`,
   forever. Absolute deadlines are now measured against the descriptor's own
   clock, the conversion saturates, and `CLOCK_BOOTTIME` and the `_ALARM` clocks
   are accepted.

### Stopping individual services

9. **`PR_CAPBSET_DROP` also cleared the effective set.** The bounding set is a
   ceiling on what may be *gained*; dropping a capability from it must not take
   that capability away from the running process. As soon as systemd's
   bounding-set loop reached `CAP_SETPCAP` it lost the privilege the loop needs,
   and every remaining drop returned `EPERM` — `Failed to drop capabilities` for
   every service with a `CapabilityBoundingSet=`, journald included. `capset(2)`
   was lowering the bounding set too, which it does not do on Linux.

10. **`capget(2)` did no version negotiation.** Linux answers a header whose
    version it does not know with `EINVAL` *and* its preferred version written
    back; that pair is how a caller learns which layout to use. libcap-ng opens
    with `capget(&hdr, NULL)` and `hdr.version = 0`, and marks itself
    permanently broken when the version it reads back is not one it knows.
    dbus-daemon's `capng_change_id()` then failed before making a single
    syscall, with errno untouched: `Failed to drop capabilities: Success`.

11. **A newly created file had `st_nlink == 0`.** `create(2)` never set it.
    systemd-journald refuses to append to a journal whose link count is zero
    (`EIDRM`, "the file has been deleted"), so it created the journal, wrote its
    header, decided the file was already gone and removed it — on every start,
    forever. `mkdir` and `symlink` had the same hole.

12. **`/proc/sys/kernel/random/boot_id` did not exist.** journald stamps every
    entry with the boot id; `sd_id128_get_boot()` failing was
    `Failed to open runtime journal: No such file or directory`. Added with
    `uuid` and `entropy_avail`, and `/proc/sys/kernel/cap_last_cap` beside them.

13. **`/proc/<pid>/oom_score_adj` did not exist**, so journald failed at step
    `OOM_ADJUST`. It now reads back what was written. Nothing consults the
    value: this kernel has no OOM killer to rank victims for — the file is a
    setting whose consumer does not exist, not one that is silently ignored.

14. **`SO_TIMESTAMP` was unsupported.** journald sets it on its native socket
    and treats the failure as fatal, so the Journal Service exited 1 on every
    start. Implemented, and the arrival stamp really is attached as an
    `SCM_TIMESTAMP` control message. `SO_PASSCRED` was refused on anything but
    AF_UNIX (journald's audit socket is AF_NETLINK); `SO_RCVBUFFORCE`,
    `SO_SNDBUFFORCE` and `SO_PASSSEC` were missing.

15. **`fchownat` read its flags from the wrong argument.** The flags are the
    fifth argument; the code read the fourth, which is the *group id* and never
    has `AT_EMPTY_PATH` set. Every `fchownat(fd, "", u, g, AT_EMPTY_PATH)` — how
    systemd copies ownership onto a temporary file — was `ENOENT`.
    systemd-sysusers died on it while writing `/etc/gshadow`, so the users dbus
    needs were never created.

16. **`symlinkat` went through the generic `(dirfd, path)` resolver**, but its
    first two arguments are the symlink's *content* and the descriptor. It read
    a user string from the address `0xffffff9c` (`AT_FDCWD`) and returned
    `EFAULT` — always.

17. **`inotify_add_watch` treated an `ERR_PTR` as a valid node.** A missing path
    installed a watch whose "node" was the encoded errno, and since every failed
    lookup with the same errno produced the same value, the next call matched
    that watch and handed back its descriptor. systemd keys a hash map on the
    descriptor: two units collided on one wd.

### Corrupting userspace

18. **The `clone(2)` child got a zeroed register file.** Linux hands the child
    the caller's registers with only the return value and stack changed, and
    libcs depend on it: glibc's `__clone3` keeps the thread function in `%r9` and
    its argument in `%r8` across the syscall, and its `__clone` pops both from
    the new stack. Threads therefore started with a NULL argument, or called
    through a NULL pointer — dbus-daemon, systemd-logind and systemd's own
    workers all died at `rip=0`.

19. **`accept4` handed the caller's pointers straight to the VFS.** The peer
    address was written into the caller's buffer with no regard for the size the
    caller declared, and the `socklen_t` beside it was written as a 64-bit word
    over a 32-bit field. dbus-daemon accepts with a 16-byte `struct sockaddr` on
    its stack: `*** stack smashing detected ***`, `SIGABRT`, and the system bus
    gone on the first connection it served. `accept` and `getsockname`/
    `getpeername` now truncate the copy to what the caller offered and report
    the untruncated length, as Linux does.

20. **`epoll`'s recursion counter was a `static __thread int` in kernel code.**
    On x86-64 TLS is addressed through `%fs`, and in ring 0 the FS base is still
    the *user* thread's TLS pointer. Every read and write of that counter went
    to whatever that pointer named: user memory quietly corrupted for a thread
    that had TLS, and a kernel page fault — a panic — for one that did not.

21. **`/dev/log` copied a kernel buffer as if it were a user pointer.** Every
    caller bounces the payload in before reaching the socket layer, so the
    second copy failed the user-range check and every syslog datagram came back
    `EFAULT`. That is where dbus-daemon reports why it is exiting, so it exited
    silently.

### Making the machine tell the time

22. **`CLOCK_REALTIME` walked backwards, continuously.** The seconds came from
    the 100 Hz tick counter and the nanoseconds from the TSC; the two are not in
    phase, so the composite went backwards by up to a second, over and over, all
    boot long. systemd-journald noticed — `Time jumped backwards, rotating` —
    and threw its journal away each time. There is now one wall clock
    (`rtc_now_unix_nanos`) and every reader takes both halves from it.

### Making the bus work

23. **`read(2)` on a non-blocking AF_UNIX socket blocked.** `recvmsg` had been
    fixed for this; plain `read` had not, and dbus-daemon reads its connections
    with `read()`. It parked in the middle of serving systemd's first method
    call and stayed there until the caller gave up 25 seconds later, then the
    bus connection was torn down.

24. **An unconnected AF_UNIX datagram send ignored its destination.**
    `sendmsg` with `msg_name` and no `connect` — which is exactly `sd_notify(3)`
    — was answered `ENOTCONN`, so no `Type=notify` unit could ever report itself
    started and systemd killed each one on its start-up timeout. dbus.service is
    one of them. AF_UNIX `SOCK_DGRAM` also had no message boundaries: two
    datagrams queued back to back arrived as one.

25. **A socket had no address family until it was bound.** `getsockname` on an
    unbound socket reported family 0. sd-bus asks
    `sd_is_socket(fd, AF_UNIX, …)` before offering file-descriptor passing and
    concluded the connection could not carry descriptors, so `systemd-run
    --pipe` — which hands its stdio to PID 1 as descriptors — failed with
    `EOPNOTSUPP` before sending anything. An accepted socket also inherited no
    address from its listener.

26. **Running out of ancillary-data slots was reported as `ENOBUFS`.** With
    `SO_PASSCRED` set, which is what a bus daemon does to every connection,
    every message takes one. A bus client treats a failed write as a dead peer.

### Terminals

27. **`TIOCSCTTY` on `/dev/console` was rejected because its argument is a
    value, not a pointer.** A blanket "no argument means EINVAL" refused it, so
    systemd could not acquire the terminal for a getty: `Failed to set up
    standard input: Invalid argument`, and no login prompt was ever printed.
    `TIOCSCTTY` now really claims the console for the caller's session, and
    `TIOCNOTTY`, `TIOCVHANGUP`, `TCFLSH`, `TCSBRK`, `TIOCEXCL` and `TIOCNXCL`
    are answered.

28. **`prctl(PR_SET_NAME)` was accepted and ignored**, so a process that renamed
    itself was still listed under the file it was exec'd from — `/proc/1/comm`
    said `init`, not `systemd`.

## cgroup v2

`kernel/fs/cgroup.c`. A mount is a directory tree in the VFS's own in-memory
node graph, the way tmpfs is: each directory **is** a cgroup and carries the
control files that describe it, `mkdir(2)` creates one and `rmdir(2)` destroys
one. Membership lives in a side table keyed by task id — `struct task` must not
grow — and "no entry" means the root cgroup, so a machine that never mounts
cgroup2 allocates nothing.

Files: `cgroup.procs`, `cgroup.threads`, `cgroup.controllers`,
`cgroup.subtree_control`, `cgroup.stat`, and for a non-root cgroup
`cgroup.events`, `cgroup.type`, `cgroup.max.depth`, `cgroup.max.descendants`.
`cgroup.events` is refreshed and reported through inotify when a cgroup's last
process leaves, which is how systemd learns a unit has finished.
`/proc/<pid>/cgroup` and `/proc/cgroups` exist.

**One controller, `pids`, and it is enforced**: a fork that would exceed a
`pids.max` anywhere between the new task's cgroup and the root fails with
`EAGAIN`. `memory`, `cpu` and `io` are deliberately **not** advertised.
Advertising a controller means accepting writes to `memory.max` or `cpu.weight`,
and accepting a limit that nothing enforces is a lie told to the process that
set it. `cgroup.controllers` therefore lists what this kernel can actually do,
and systemd logs the rest as unavailable — the same thing it does on a Linux
kernel built without those controllers.

## Mount propagation

Recorded per mount and reported in `/proc/self/mountinfo` as `shared:N` /
`master:N` / `unbindable`. A mount namespace here is a copy of the mount table,
so "the copies are peers" is given the only meaning it can have: a mount made
under a shared mount is made under every peer's namespace too
(`vfs_mount_propagate`). Slave propagation is recorded but does not forward
events — nothing in this boot exercises it, and it is listed below rather than
claimed.

## The clock that made every other measurement lie

Before anything else in this round: `alarm(2)`, `setitimer(2)`, the POSIX
timers, `sigtimedwait`, `clock_getres` and the tick fallback in `clock_gettime`
all converted seconds to scheduler ticks by multiplying by **100**. The LAPIC
timer has been armed at **1 kHz** since it could be calibrated, so every one of
them fired ten times early.

`timeout(1)` arms a POSIX timer. So `timeout N` marked itself timed out after
N/10 seconds, let the child run to completion, and returned 124 anyway —
`timeout 10 sleep 2` reported a timeout for a command that finished in two
seconds. **Every harness that bounds a command was reporting false timeouts on
work that had succeeded**, and several conclusions in the previous version of
this document were drawn through that lens.

Measured, before and after:

| command | rc before | rc after | elapsed |
|---|---|---|---|
| `timeout 3 sleep 60` | 124 | 124 | 60 s (the child was still not killed — that half is fixed in the round below) |
| `timeout 10 sleep 60` | 124 | 124 | 60 s |
| `timeout 10 sleep 2` | **124** | **0** | 2 s |

`sched.h` already warned about exactly this ("written as a bare count it
silently became ten times shorter when the timer moved from 100 Hz to 1 kHz")
and provides `SCHED_MS_TO_TICKS`; these call sites predate it. They now go
through one saturating helper that reads `sched_tick_hz()`. `clock_sanity`
gained `alarm-keeps-time`, `itimer-keeps-time` and `posix-timer-keeps-time`,
which ask for one second, measure against `CLOCK_MONOTONIC`, and fail on early
as well as late.

Two things this did **not** explain, stated because it would be easy to assume
otherwise: kwin's "No suitable DRM devices have been found" is unchanged by it,
and the 90-second `daemon-reload` below is real rather than an artefact of it.
(The second of those is retracted in the round below: it takes 0.6 s here.)

## Round two: past multi-user, and the queue that never drained

Everything below is from this tree, measured on `tests/systemd-smoke.sh` runs
whose logs are kept per run under `smoke_run/b1nix-systemd-boot-*.log`.

### Three things the previous round recorded that are not true here

- **The 90-second `daemon-reload` does not exist in this tree.** Measured on
  every run in this round: `daemon-reload 42.95->43.58 rc=0`, i.e. 0.6-0.7 s.
  The section below titled "The 90-second reload: what it is not" describes a
  machine this one no longer is; it is kept because the refutations in it are
  still sound reasoning about how to measure a stall, not because the stall is
  there.
- **`graphical.target` was never "not reached" — it was never asked for.** The
  harness boots with `systemd.unit=multi-user.target` on the kernel command
  line, so the manager's job never included the graphical target. Started
  explicitly, it reaches active in one step:
  `Reached target graphical.target - Graphical Interface`, and the harness now
  checks it (`ok unit-active graphical.target`). There is no display manager in
  the image; `graphical.target` `Wants` one and does not require it, exactly as
  on Linux.
- **The udev queue stall was not the zero-length worker message.** That was the
  first hypothesis and it is wrong: `struct worker_message` is empty in systemd
  252, but a boot traced for zero-length AF_UNIX writes contains **none at
  all**. The real cause is the netlink source address, below. (Zero-length
  datagrams *were* broken here, in three separate places, and are fixed — see
  the defect list — but they are not what udevd was waiting for.)

### The unit cache: a directory whose mtime never moved

`systemctl start` of a unit the manager has not loaded re-scans the unit
directories only when their mtimes differ from the ones its last scan recorded.
Two halves of that were missing:

1. **Creating, removing or renaming an entry did not mark the containing
   directory modified.** POSIX requires it; nothing in the VFS did it. A
   directory's mtime was fixed at the moment it was created and never moved
   again.
2. **Timestamps had whole-second resolution.** `st_mtim.tv_nsec` was always
   zero, so even with (1) fixed, two writes inside one second are
   indistinguishable — and a harness that writes a unit file and starts it does
   exactly that.

The visible symptom was `Failed to start b1nix-tick.timer: Unit
b1nix-tick.timer not found` for a file sitting in `/run/systemd/system`, and it
took four checks with it: `notify-ready`, `private-tmp`, `timer-fires` and
`restart-on-failure`. All four pass now. Inodes carry `atime_nsec`,
`mtime_nsec` and `ctime_nsec`, one clock read fills seconds and nanoseconds
together, and `stat` reports both.

Every tool that decides whether to re-read a directory by its mtime — make,
ccache, a package manager — had the same blind spot here.

### `rename(2)` cut a long name in half

The rename path copied the destination name into a 64-byte field while every
other creation path stores `VFS_NAME_MAX-1`. A destination longer than 63
characters was silently renamed to a shorter, different name: the file existed
under a name nobody would look up and the intended one was `ENOENT`. "Write to
a temporary, rename over the target" is how systemd, dpkg and glibc all replace
a file, and their temporary names are long.

### The netlink source address named a process, not a socket

This is the udev queue stall, and the evidence is in the daemon's own debug log
(`udev.log_level=debug`):

```
systemd-udevd[43]: loop4: Processing device (SEQNUM=23, ACTION=add)
systemd-udevd[43]: loop4: sd-device-monitor(worker): Passed 254 byte to netlink monitor.
systemd-udevd[43]: sd-device-monitor(worker): Unicast netlink message ignored.
```

A udev worker processes the device it was forked with, and then waits for more
over a `NETLINK_KOBJECT_UEVENT` socket in no multicast group. The manager sends
each device to that socket by port id. Before accepting one, the worker checks
the message's source `nl_pid` against the address it obtained with
`getsockname` on the manager's own monitor
(`device_monitor_allow_unicast_sender`), and discards anything else.

`sockaddr_nl.nl_pid` is a **port id**: it identifies a socket, and a task that
holds several netlink sockets has several of them — this kernel already
allocates them that way (`netlink_alloc_portid`). But the source address a
receiver read back was filled in from the *sender's process id*, taken from the
same per-message credentials that feed `SCM_CREDENTIALS`. The manager holds
more than one netlink socket, so its monitor's port id is not its pid, and
every device it handed to an already-running worker was thrown away.

The first device always worked, which is why this looked like "workers stop
being marked idle": a freshly forked worker inherits its first device across
the fork and never receives it over netlink. So each worker processed exactly
one device, udevd forked up to its child limit (`Processing with 11 children at
max`), and the queue stopped.

Each queued message now records the port id of the socket that sent it, and
`recvmsg`'s `msg_name` reports that. `SCM_CREDENTIALS` still carries the real
pid — they are different questions. After the fix, `Unicast netlink message
ignored` does not appear in the boot at all and a single worker processes
several devices in turn.

### A read-only mount that only refused the second half of the operation

`ProtectSystem=strict` and `ReadOnlyPaths=` bind a path onto itself and then
remount it `MS_BIND|MS_REMOUNT|MS_RDONLY`. Two things were wrong:

- **`open(2)` never consulted the mount.** `write(2)`, `create`, `mkdir`,
  `unlink`, `rename` and `ftruncate` all checked `MS_RDONLY`; opening did not.
  So `open(path, O_WRONLY)` on a read-only mount succeeded and handed back a
  descriptor that could not be written — and worse, `O_TRUNC` took effect with
  no check at all, so a sandboxed process could empty a file on a filesystem
  the kernel believed was read-only. `link` and `symlink` were unchecked too.
- **The mount lookup answered with the oldest mount at a node, not the
  newest.** A bind mount records its *source* as the mount's root, so binding a
  path onto itself leaves two entries rooted at one node, and the scan returned
  whichever sat in the lower array slot — the one that was there before the
  read-only remount. Mounts now carry a monotonic sequence number and the
  newest wins, which is what "stacked on top" means. The fallback for a node
  that is under no mount at all is deliberately left as it was: that is a
  different question and answering it by recency changed which filesystem's
  flags an unattached node was judged by.

### Dual-stack accept

An `AF_INET6` listener that is not `IPV6_V6ONLY` — the default, and what a
`ListenStream=<port>` with no address produces — must accept an IPv4 connection
and report its peer as `::ffff:a.b.c.d`. The passive-open path here matches a
SYN on the port alone, so the connection was created as `AF_INET`, and then
`accept` refused it for having the wrong family while `poll` went on reporting
the listener readable: readable and unacceptable at the same time.

This is **not** what the harness's `socket-activation-tcp` check hits — that
unit is `ListenStream=127.0.0.1:17999`, an explicit IPv4 address, so the
listener is `AF_INET` and the dual-stack path is not involved. The fix stands
on its own and has its own check (`NET-SMOKE: ok dual-stack-accept`); the
socket-activation failure is still open.

### `timeout(1)` bounded nothing, because a signal could not end a sleep

`kill(2)` posted the signal and woke a target that was BLOCKED on a wait
channel or STOPPED — and left one that was SLEEPING on the clock exactly where
it was. A process in `nanosleep(2)` therefore carried the pending signal until
its sleep ran out and took it then: `kill -TERM` on `sleep 30` landed thirty
seconds later, which is why `timeout 3 sleep 60` returned 124 on time and the
child ran to completion anyway. Every bound in every harness here was that.

`terminate_group_siblings` — the exit path — already woke SLEEPING tasks, which
is why a dying group's SIGKILL did land and this looked like a `timeout`
problem rather than a signal one. `scheduler_kill`, `scheduler_sigqueue` and
the group-stop path now do the same. A signal the target *blocks* still does
not shorten the sleep: the sleep loop re-checks its own deadline and sleeps out
the remainder.

Measured by `M46-SMOKE: signal-ends-sleep`, which forks a child into a
five-second `nanosleep`, signals it, and requires both the exit status and the
elapsed time to say it died there — a status check alone would be satisfied by
a child that slept to the end and was signalled afterwards. Writing that check
also took two attempts worth recording: the first version used a thirty-second
sleep, which cost the instance enough time that twenty later checks never ran,
and it did not reset SIGTERM to `SIG_DFL` — the shell that launches the suite
runs `trap '' TERM`, and an ignored disposition survives both fork and exec, so
it would have measured the trap.

### Zero-length datagrams

Three places dropped a message with no bytes in it, which Linux delivers:

- `sendmsg`/`recvmsg` refused an iovec of total length zero with `EINVAL`;
- `write`/`send`/`sendto` of zero bytes returned 0 without sending anything,
  even on a datagram socket;
- a queued zero-length AF_UNIX message never made the socket readable, because
  readability was gated on bytes in the ring rather than on messages in the
  queue.

Ancillary data is the whole content of some such messages — the sender is
identified by the `SCM_CREDENTIALS` the receiver's `SO_PASSCRED` attaches.
`M46-SMOKE: ok empty-datagram-cred` covers the round trip; the existing
credentials check passed throughout because it uses a stream socket and a
four-byte message.

## What is still missing

Rewritten for this round; the entries that are gone are gone because they were
fixed or refuted, and each of those is accounted for in "Round two" above.
`tests/systemd-smoke.sh` stands at **28 of 32**.

- **Every block device fails `worker_lock_block_device`** with `Failed to get
  whole disk device: No such file or directory`, `vda` included, and a device
  that is not processed carries no `systemd` tag — so **no `.device` unit
  activates** and `udevadm control --ping` still times out. systemd asks this
  by `access("/sys/dev/block/<major>:<minor>/queue", F_OK)`. That path **is**
  there: probed by a direct `[ -e ]` with nothing having listed the directory
  first, and probed again from inside a `PrivateMounts=yes` unit so the
  namespace is the one udevd runs in — `direct-first: 8:0/queue exists`,
  `ns: queue exists`. So the file is present, in the right namespace, under the
  major:minor the uevent carries, and the call still says it is not. What has
  not been done is watching the call itself: `b1nix.trace-errno=2` prints the
  syscall number, the pid and the path for every ENOENT. This is the one open
  item that the rest of udev sits behind.
- **`ProtectSystem=strict` does not make the filesystem read-only, because the
  unit's namespace has no read-only mount in it at all.** The unit now runs (it
  used to fail at `start` with `Result=resources`), its write to `/etc` returns
  0, and its own `/proc/self/mountinfo`, printed by the harness on failure,
  contains **zero** entries marked `ro` — `/` appears three times and all three
  say `rw`. So this is not the mount lookup or the write path (both of which
  were wrong too, and are fixed, with `M46-SMOKE: ok rdonly-mount-open`
  covering them): the mounts systemd makes for the sandbox are not in the table.
  `b1nix.trace-mount` on that boot names every `mount(2)` systemd issues and
  what it returned, and reading that is the next step.
- **`socket-activation-tcp`**: the connection is really made — the probe now
  reports `tcp-probe connect rc=0` — and the service behind the socket unit is
  not started. The AF_UNIX half of the same check passes and loopback TCP with
  a blocking `accept` passes in the main suite, so what is left is the manager's
  readiness on a listening `AF_INET` socket it holds across `epoll`. The socket
  unit binds `127.0.0.1:17999`, an explicit IPv4 address, so the dual-stack
  accept fixed in this round is not involved.
- **`systemd-update-utmp`, `e2scrub_reap` and `sys-kernel-debug.mount` fail.**
  utmp and debugfs are simply absent.
- **Slave/unbindable mount propagation is recorded, not enforced.** A unit with
  `PrivateTmp=` gets a private mount table copy, which is the important half,
  but events do not propagate from master to slave.
- **No `SO_PEERSEC`, no audit netlink, no BPF.** systemd logs each as
  unavailable and continues.
- **`hwdb.bin` is not built into the image**, so udev's hardware database is
  empty. Nothing in this boot depends on it.

### Retracted from this round's own working notes

- **`systemctl enable` does not hang the machine.** It looked that way — the
  console went quiet for four minutes after the reload its `enable` triggers,
  with no further output from anything — and it was the harness dying, not the
  machine: `b1nix-smoke.service` carried `TimeoutStartSec=90s`, the harness
  reached that point at 92 s, and systemd terminated it. The remaining silence
  was a machine with nothing left to say. With the unit's timeout raised and
  the `systemctl` calls bounded like every other call in the harness,
  `enable rc=0 93.81->94.57` — 0.76 s — and `unit-enable-disable` and
  `unit-mask-refuses` both pass.
- **The dual-stack accept fix is not what `socket-activation-tcp` needed** (the
  unit is IPv4-only), and **zero-length datagrams are not what udevd was
  waiting for** (no zero-length AF_UNIX write happens in that boot). Both were
  real defects and both are fixed with their own coverage; neither explains the
  failure it was reached for.

## The device tree userspace walks

Three views of the same devices were missing, and each one made devices
invisible to a different consumer. All three publish only what the kernel
already knows.

### `/sys/block/<disk>/queue` — how systemd tells a disk from a partition

`block_get_whole_disk()` looks for `<dev>/queue` and calls the device a whole
disk if it is there; failing that it looks for `<dev>/partition` and walks to
the parent. `vda` had **neither**, so it was neither, and systemd-udevd
abandoned the event before running a single rule — `vda: Failed to get whole
disk device: No such file or directory`, then `Failed to process device,
ignoring`. A device that is not processed carries no `systemd` tag, and only a
tagged device gets a `.device` unit, so nothing `BoundTo=` a device could start
on this machine.

The directory now carries the values the block layer has: `logical_block_size`,
`physical_block_size`, `hw_sector_size`, `minimum_io_size`, `max_sectors_kb`,
`max_hw_sectors_kb`, `max_segments`, `nr_requests`, `rotational`, `scheduler`
(really `none` — there is no I/O scheduler to name) and `discard_granularity`
(zero when the driver never offered the command). `optimal_io_size` and
`write_cache` are **absent rather than guessed**: a wrong number there is one a
filesystem lays itself out around.

### `/sys/dev/char` — how libudev finds a device by its number

The `dev` directory carried only `block`. `udev_device_new_from_devnum()` for a
character device looks up `/sys/dev/char/<major>:<minor>`, so every character
device was invisible to anything that identifies devices the way udev does
rather than by path.

It is populated from the character devices under `/dev`, **recursively** — a
DRM card is `dri/card0` and a pty slave is `pts/N`, and a walk of `/dev`'s
immediate children would have found neither while looking like a working fix.
`DEVNAME` is the path relative to `/dev`, as Linux writes it. Entries for major
226 are left to the DRM layer, whose entries are links to the card's minor
directory and carry the `device` link libdrm follows; a plain directory of the
same name would shadow that and leave `226:N/device` missing.

`/dev/null`, `/dev/zero`, `/dev/console` and `/dev/ptmx` reported `st_rdev == 0`
— device files that are no device — so they could not be filed under any number
at all. They now carry the numbers the Linux ABI fixes for them (1:3, 1:5, 5:1,
5:2). Those are not ours to choose.

### `/sys/devices/pci0000:00` — the parent chain udev matches rules against

`/sys/devices` held `system` and nothing else. The only PCI nodes anywhere were
the ones the DRM layer created for its own cards, and when it had no identity to
publish it filled them with an Intel id written into the source.

`pci_sysfs_publish_all()` publishes every function the enumeration finds:
`vendor`, `device`, `class`, `revision`, `irq`, `subsystem_vendor`,
`subsystem_device` and a Linux-shaped `uevent`, with `subsystem ->
../../../bus/pci` and the `/sys/bus/pci/devices/<addr>` links. Every value is
read out of config space at publish time.

Three deliberate limits:

- **Subsystem ids only on a type 0 header.** A bridge's `0x2C` is its secondary
  bus numbers; reading it as a subsystem vendor would publish a number that
  means something else. Bridges get the `sv*sd*` wildcards Linux writes.
- **No `driver` link.** This kernel records no binding between a function and
  the code driving it, and a `driver` link is read as a statement that
  something has claimed the device.
- **`uevent` is writable and really re-announces the device.** `udevadm
  trigger` walks `/sys/bus/pci/devices` writing `add` to each `uevent`; with the
  file read-only every device answered `Permission denied` and a coldplug replay
  reached none of them. Accepting the write and doing nothing would be the
  "writable file that silently discards writes" the sysfs registry's own
  contract warns against.

These nodes also change what a DRM card publishes. `kernel/dev/drm.c` used to
fill an unidentified card's attributes with an Intel id written into its source,
so a card with no identity claimed to be one; it no longer does, and such a card
now shows no `vendor`/`device` at all. Read alongside this section that is the
consistent behaviour rather than a regression: the PCI node is where a real
identity comes from, and an absent attribute is an honest "not known" where a
borrowed one was an answer nothing could check. See
[`docs/gpu-drm-plan.md`](gpu-drm-plan.md) for the DRM side.

What this still does not give is a `device` link with a *bus address* for a DRM
node on a machine that has one — the chain exists (`/sys/dev/char/226:N` → the
minor directory → `device -> ../..` → the PCI node), and the PCI node is now
real, but the headless test image has no DRM card, so this image cannot prove
it.

## The path that could not be resolved

`split_path()` peeled path components into a 64-byte buffer while `VFS_NAME_MAX`
is 256 — and the damage was not the truncation. It set `*rest` to a point in the
**middle** of the name, so an over-long component was re-parsed as *two*
components. `systemd-private-<32 hex>-systemd-logind.service-XXXXXX` is 78
characters, and was looked up as a 63-character directory containing a
15-character one. Nothing of that shape exists, so the lookup answered ENOENT —
while creating the same name through a route that stored it whole succeeded.

That is precisely what broke `PrivateTmp=`. systemd's `setup_one_tmp_dir()`
calls `mkdtemp()` and then `mkdir()` inside the result, and the **second** call
failed:

```
mkdir("/tmp/systemd-private-616b…-systemd-logind.service-j5XuSV/tmp") = ENOENT
```

Every unit with `PrivateTmp=` failed at `start` with `Result=resources` and
`ExecMainCode=0` — never forked. systemd-logind is one of them, and because it
is D-Bus activated its failure appeared as five restarts and `Start request
repeated too quickly`, which reads like an activation problem and is not one.
`e2scrub_reap.service` failed identically.

The buffer is now `VFS_NAME_MAX` and an over-long component is `ENAMETOOLONG`,
which is what Linux answers and what a caller can act on — never a silently
different, shorter path. **systemd-logind now reaches active and answers
`loginctl`.**

A note on how this was nearly missed: the first probe written for it used
`mktemp -d /tmp/b1nix-probe-XXXXXX`, a 22-character name, and reported
`inner mkdir ok`. A short name cannot reach the defect, so the probe would have
certified the area healthy. It was replaced with a sweep over component lengths
60, 63, 64, 70, 78 and 100.

## systemd-udevd: from refused to running

The previous account of this said the blocker was a bind mount that resolved to
`/`, and named `mount(2)` on an `O_PATH` descriptor as the next step. That was
the first of six defects, not the whole of it. Each one below is what a real
program did, what this kernel did wrong, and what it cost.

### 1. A file bound onto itself renamed itself to `/`

`vfs_get_node_path()` walks a node's parents, stepping from a filesystem root
to the node it is mounted over. A bind mount records `(mount point, root)`, and
`ReadOnlyPaths=`, `ProtectHostname=` and `ProtectKernelTunables=` all come down
to binding a file **onto itself** — so the two ends of that step were the same
node, the walk stepped from the node to itself until its budget ran out, and
the file's path came out as `/`.

`/proc/<pid>/fd/<n>` is rendered from that walk, and systemd mounts everything
through `/proc/self/fd/<n>` (it opens the destination `O_PATH|O_NOFOLLOW` so no
symlink can be swapped in). So after the first such bind, every later mount
target resolved to the root directory. `bind: kind mismatch … target
/proc/self/fd/4 is a directory (resolved to '')` was the *visible* symptom; the
invisible one was worse, because a mount whose two ends agreed then landed a
unit's private tree on top of `/`, and every cgroup and `/dev/console`
operation in the machine answered `ENOTDIR` from that moment on.

The seam now only applies to a node with no parent — a filesystem root, which
is the only thing it was for — and never steps to the node it started from.
A later mount of an already-mounted root does not rename it either: taking the
second entry made the walk circular through
`/run/systemd/unit-root/run/systemd/unit-root/…` until every
`openat(dirfd, name)` in the machine returned `ENAMETOOLONG`.

### 2. `/proc/<pid>/fd/<n>` re-walked a path instead of naming the file

The link's target was rendered by asking the node where it lived. A node
reached through a bind mount lives in two places and only remembers the first,
so the answer named the wrong one — and mount(2) on that link then recorded the
mount somewhere the caller was not looking.

A descriptor now remembers the path it was opened at
(`vfs_handle::open_path`), which is what Linux's `(mount, dentry)` pair encodes
and what every reader of this file means. Resolution no longer re-walks the
string at all: the path resolver follows a **magic link**
(`vfs_inode::magic_link_cb`) straight to the open file's node, so
`mount(2)`, `openat(2)` and `stat(2)` on `/proc/self/fd/<n>` all reach the file
the descriptor holds. A descriptor with no file behind it (a pipe, a socket)
still falls back to the `pipe:[7]` text, so nothing that worked stops working.

### 3. A mount was filed under the node's name, not the caller's

`/proc/self/mountinfo` showed `/proc/sys/kernel/domainname` for a mount systemd
had just made at `/run/systemd/unit-root/proc/sys/kernel/domainname`.
systemd re-reads mountinfo after each bind to check the mount took; not finding
it, it bound again — 32 times, and then failed the unit with `EBUSY`, which is
the "Device or resource busy" that `systemd-udevd.service` reported for weeks.
A mount now records the path the caller named (resolved), and `MS_REMOUNT` and
the propagation calls canonicalise the same way, so
`MS_BIND|MS_REMOUNT|MS_RDONLY` and `MS_SLAVE` find the mount they mean.

### 4. `/proc/sys/kernel/random/boot_id` did not exist

systemd-journald stamps every journal file header with the boot id, and
`sd_id128_get_boot()` failing is `Failed to open runtime journal: No such file
or directory` — on which journald exits 1, on every start, forever, taking the
journal (and every unit that logs) with it. This is why the image was red at
2 passed / 12 failed with no local changes at all: it had nothing to do with
udev. Added, with `uuid` and `entropy_avail` beside it, and
`/proc/sys/kernel/cap_last_cap` and `threads-max`, which systemd reads to turn
`DefaultTasksMax=15%` into a number before it forks a unit — its absence is
reported as `Failed to run 'start' task: No such file or directory`, with no
file named.

### 5. Every uevent was delivered and every uevent was thrown away

`recvmsg` on a netlink socket left `msg_name` zeroed. systemd's device monitor
requires the source address to say `AF_NETLINK` with `nl_pid == 0` before it
will look at a message, and logged each of ours as
`sd-device-monitor(manager): Unicast netlink message ignored.` — so udevd read
every coldplug event and discarded it, which from outside is indistinguishable
from a kernel that sends no events at all. The source address is now a real
`sockaddr_nl` carrying the sender's port id and the multicast group the message
arrived on; with `SO_PASSCRED` set, the sender's credentials come back as
`SCM_CREDENTIALS` as well, which is the monitor's other precondition.

A source address is a claim about who sent the message, and getting it wrong is
worse than leaving it blank. Nearly every netlink message a task receives is a
reply the kernel builds **in that task's own context**, so crediting the
message to the running task turns `nl_pid` from 0 into the caller's pid — and
every netlink client checks that, because a reply that did not come from the
kernel is not a reply. Doing this credited iproute2's own answers to iproute2,
which discarded them and waited for one that never came; `ip` then hung, and
with it every test that ran after it. A message is from the kernel unless a
task's own `write(2)` put it there.

### 6. udevd could not hand a device to a worker

A worker's monitor is a `NETLINK_KOBJECT_UEVENT` socket in **no** multicast
group, and the manager sends it one device at a time, addressed to that
socket's port id. b1nix answered a groupless netlink send with `EINVAL` — "a
unicast uevent has no meaning" — and every worker reported `did not accept
message, killing the worker: Invalid argument`. Unicast delivery is
implemented; a netlink socket is registered as a possible destination whatever
groups it joined, and two sockets in one task no longer share a port id (Linux
starts from the pid and picks something else when it is taken).

With those six fixed, `systemd-udevd.service` reaches `active (running)`,
`udevadm trigger` re-announces every device, udevd forks a worker per event and
runs the rule set. The remaining failure is inside the worker, at the point
where it records what it found: `Failed to process device, ignoring: No such
file or directory`. That is the next thing to look at, and `udev.log_level=debug`
on the kernel command line is how to see it.

## Terminals: the harness could not be heard

`console-getty.service` is `Type=idle`, so agetty claims `/dev/console` — and
`vhangup()`s it — while the marker harness is still running. Writes from the
harness after that point went nowhere, and later a write blocked outright, so
the run ended with no verdict rather than a failing one. Every marker is
written to `/dev/kmsg` now, which reaches the same serial line and which nobody
can take away, and every diagnostic command in the harness is bounded by
`timeout`.

## Diagnostics added

All of them are off unless asked for on the kernel command line.

| Flag | What it prints |
|---|---|
| `b1nix.trace-mount` | every `mount(2)` that names no filesystem — bind, remount, move, propagation — with its flags, its result, and the path the mount was recorded under |
| `/proc/b1nix-tasks` | reading it prints the scheduler's task dump — which syscall every task is in — at a chosen moment, instead of paying for one every ten seconds |
| `b1nix.trace-uevent` | every uevent broadcast: group, length, how many sockets were listening, how many got it; and each uevent socket as it registers |
| `b1nix.trace-errno=<n>` | the number of every syscall that returned that errno, the pid that made it, and — for a call that takes one — the path it was given. "errno 2 from syscall 83" says a mkdir failed and not which directory, and the directory is the whole answer |
| `b1nix.trace-errno-pid=<pid>` | narrows the above to one task. Unfiltered ENOENT is thousands of lines through a serial console that is already the machine's bottleneck, and printing them changes the timings being measured |
| `b1nix.sysd-debug` | (guest, not kernel) the systemd harness's deep diagnostics: the timer and loopback probes, the udev and logind evidence, and a `/proc/b1nix-prof` bracket around `daemon-reload` |

## Coverage

`tests/systemd-smoke.sh`, 32 checks, all of them markers the guest prints only
after the named thing really happened. The socket-activation check runs twice,
once over an AF_UNIX listener and once over loopback TCP, because the manager's
half of socket activation is the same either way while what sits underneath is
not; `graphical.target` was added in this round. `SMOKE_DEBIAN=1 sh
tests/smoke.sh` still runs the sysvinit image; the two are independent.

The kernel-side fixes of this round are covered in the main suite rather than
only here, so a regression is caught without a Debian image: `M46-SMOKE`
gained `dir-mtime-create`, `dir-mtime-subsecond`, `dir-mtime-unlink`,
`rename-long-name`, `empty-datagram-cred`, `rdonly-mount-open` and
`signal-ends-sleep`;
`M109-UEVENT` gained `netlink-source-portid`; `NET-SMOKE` gained
`dual-stack-accept`.

## The 90-second reload: what it is not

> **This section describes a machine this tree no longer is.** The reload here
> takes 0.6-0.7 s, measured on every run of the round above. It is kept for the
> reasoning — how each hypothesis was refuted, and what an instrument that
> cannot separate "parked" from "spinning" is worth — not for its conclusion.

The reload is the one defect left that everything else waits behind, so what has
been ruled out is worth recording as carefully as what has been found.

Measured with an honest clock, three ways:

**It is not per-unit work.** `b1nix.sysd-fillers=150` adds 150 units before the
reload. 225 units + 8 → 90.03 s; 225 units + 150 → 91.10 s. A 61% increase in
unit count moved the time by 1.2%, so the cost does not scale with what is being
loaded. It is one fixed stall.

**It is not a hanging generator.** `manager_reload()` re-runs every system
generator through `execute_directories()`, whose bound is systemd's
`DEFAULT_TIMEOUT_USEC` — exactly 90 s, which made this the obvious suspect. Run
by hand, all twelve finish in 60–100 ms each, about 1.2 s for the set
(`systemd-gpt-auto-generator` exits 1, which is normal on a machine with no GPT).
The syscall profile agrees: `wait4` accounts for 3701 Mcycles (~1 s) across the
whole 91 s, so nothing is waiting on a child.

**It is not work at all.** Across the 91 s the entire machine made 203 `openat`,
126 `newfstatat` and 136 `close` calls — not a re-read of 375 unit files. The
time sits in blocking calls: `read` (326189 Mcycles), `epoll_wait` (324435 over
25 calls), `rt_sigsuspend` and `ppoll` (one call each).

One caution about that reading, because it is easy to over-claim from it: the
profile is **global**, summed over every task, not per-task. So "203 `openat` in
the whole machine" is solid — it is a statement about the absence of work
anywhere — and so is `wait4` totalling ~1 s. But attributing the 25 `epoll_wait`
calls to PID 1, and concluding that PID 1 was awake and scanning every ~3.6 s,
is **not** supported: 90 s spread over 25 calls is equally consistent with one
task parked for 90 s, or several parked for less. Whether PID 1 was parked or
spinning is exactly the distinction that matters here, and this instrument
cannot make it. A per-task profile, or a trace of accept/readiness on the
private socket, is what would.

**Neither side disconnects during it.** With `b1nix.trace-exit` on, exactly one
AF_UNIX teardown occurs in the whole window, at 291.29 s — after the call has
already returned — and it is the *client's*. PID 1 closes nothing. So
`Transport endpoint is not connected` is sd-bus reporting its own method-call
timeout (`DEFAULT_TIMEOUT_USEC`, the same 90 s) and tearing the connection down
afterwards; it is not the kernel dropping a connection.

What that leaves: **systemctl's Reload request never reaches PID 1's event
loop.** PID 1 wakes about every 3.6 s throughout, finds nothing to serve, and
sleeps again; systemctl waits out its 90 s and gives up.

That conclusion rests on a claim worth checking rather than assuming — that the
*same* socket answers other calls quickly. `systemctl` reaches the manager over
`/run/systemd/private` for some operations and over the D-Bus system bus for
others, and if `is-active` went through dbus-daemon while `Reload` went through
the private socket, then the private socket would never have been shown to work
at all and the bug would more likely be in `accept()` on that listener.

`b1nix.trace-unix-connect` names the socket every `connect(2)` targets, with the
caller. Over one boot: **38** connects to `/run/systemd/private` against **3** to
`/run/dbus/system_bus_socket`, and the fast calls are on the private socket by
name — `UNIX-CONNECT: pid=65 comm=/usr/bin/systemctl path=/run/systemd/private`
at 5.4 s, answered in tens of milliseconds. The reload window contains exactly
one connect, from systemctl, to that same path.

So the listener works, PID 1 accepts on it repeatedly, and a fresh connection is
established for the reload. That also rules out an edge-triggered readiness on
the listener being delivered once and lost: a listener in that state would not
serve thirty-eight connections.

What it does **not** establish is that the reload's connection was accepted.
`connect(2)` on an AF_UNIX socket here completes as soon as the endpoint is
queued in the listener's backlog, so a successful connect proves the client
reached the listener, not that the server took it off the queue. Three
possibilities remain and the trace cannot separate them: the connection was
never accepted; it was accepted but its readiness was never reported; or it was
read and the reply was lost. The next instrument should log the accept and the
first read on that socket, which distinguishes all three in one boot.

---

# Second round: 15 → 29 of 31, and what the harness was measuring

Everything below is from this tree, measured on boots whose logs are kept one
per run under `smoke_run/b1nix-systemd-boot-<tag>-<time>.log`. Where a number
came through `timeout(1)` it says so.

`tests/systemd-smoke.sh` now stands at **28–29 passed of 31** (the third
failure, when there is one, is the flaky `notify-ready` below),
from 22/9 at the start of this round (and 15/14 when the previous round wrote
its notes against a smaller suite). The two that fail are named at the end,
with what is known about each.

## How far it boots, by target

Asked of systemd rather than of our markers. `systemd-analyze` refuses to
answer while the boot transaction is still running and the harness is part of
that transaction, so the harness now leaves a process behind that waits for the
manager to stop reporting `starting` and then asks:

```
Startup finished in 1.990s (kernel) + 1min 1.633s (userspace) = 1min 3.623s
graphical.target was never reached.

critical-chain:
  multi-user.target @1.398s
    systemd-logind.service @1.255s +133ms
      sysinit.target @962ms
        systemd-tmpfiles-setup-dev.service @912ms +36ms
          systemd-sysusers.service @696ms +206ms
            systemd-remount-fs.service @507ms +152ms

blame:
  1min 226ms  b1nix-smoke.service
```

`multi-user.target` is reached 1.4 s into userspace and the chain that leads to
it is systemd's own early units, in the order it chose. The minute of
"userspace" is the harness itself — `blame` says so — not the boot.
`graphical.target` was never reached because nothing in this image wants it.

## Retractions

**The 90-second `daemon-reload` no longer exists, and nothing in this round
fixed it.** The first boot of this round — before a single change — measured
`daemon-reload 42.73->43.34: ok`, and every boot since has been 0.6–0.7 s
across ten runs. The elaborate account above of what the stall "is not" is
therefore an account of something this tree no longer does; it is kept because
its measurements are still true of the boot they were taken on, but the reload
is not the thing everything else waits behind. Whatever fixed it landed between
that document and this tree.

**Six of the harness's nine remaining failures were the harness's own.** It
wrote `b1nix-notify.service`, `b1nix-private.service`, `b1nix-ro.service`,
`b1nix-tick.{service,timer}` and `b1nix-restart.service` into
`/run/systemd/system` **after** its single `daemon-reload`, and then started
them. systemd answered `Unit b1nix-notify.service not found`, which is the
correct answer to a unit file it has never read — and the harness recorded it
as notify-ready, private-tmp, protect-system, timer-fires, restart-on-failure
and journal-filter-unit all failing in the kernel. The single reload was a
deliberate economy from when a reload cost 90 seconds. It costs 0.6 s now, so
every unit file written at runtime is followed by one (`sd_load`).

**The journal-index check was reading its own noise.** Every marker goes to
`/dev/kmsg`, which journald files as a *kernel* message belonging to no unit,
so `journalctl -u b1nix-smoke.service` had nothing of the harness's in it and
the check matched whatever landed there last — which was a shell error from
`wc -l < /run/b1nix-restart.count` before that file existed. (`wc -l <
missing` is the *shell* failing to open the redirect, before `wc` runs, so
`2>/dev/null` on `wc` silences nothing and `|| echo 0` never fires.) The
harness now prints one line of its own to stdout, through
`StandardOutput=journal`, and looks for that.

**`timeout(1)` kills its child now.** Measured in the guest against
`/proc/uptime` rather than through the thing being tested:
`timeout 3 sleep 60 -> rc=124 elapsed 29.85->32.87` — 3.02 s, where it used to
be 60. The fix is the sleeping-task wake below.

## The kernel defects this round found

Each is what a real program asked, what this kernel answered, and what it cost.

### 1. `name_to_handle_at` reported mount id 0 for every path

`/proc/<pid>/mountinfo`'s first field is a mount id, and a caller asks for a
path's mount id in order to find that row. Ours answered 0 — with the comment
"one namespace, one mount id" — and no row is numbered 0.

systemd's `dev_is_devtmpfs()` asks for the mount id of `/dev` and then scans
mountinfo for it. Failing that test, `device_monitor_new_full()` **disables the
device monitor for the rest of the boot** ("We do not support subscribing to
uevents if no instance of udev is running", the container case), because PID 1
builds its monitor before udevd has created `/run/udev/control`. A manager with
no device monitor never learns about a device, so **no `.device` unit could
activate on this machine whatever udev did** — and that is what the previous
round was looking for in udev.

Visible in one line of `b1nix.trace-uevent`: `uevent-sock register: … groups=0x1
pid=1` before, `groups=0x2 pid=1` after. Group 2 is the one udevd broadcasts
on, and PID 1 had not joined it. Deliveries went from `sent=0` to `sent=1` per
message on the same boot.

`vfs_mount_id_for_path()` now answers the mount's position in the visible list,
+1 — exactly how `r_mountinfo` numbers the rows it prints.

### 2. A uevent carried no DEVTYPE

`systemd-udevd` answered `vda: Failed to get whole disk device: No such file or
directory` and abandoned the event before running a rule. The previous round
read that as a missing `/sys/block/<disk>/queue` and published one; the
directory was there and the message was unchanged, because the question is not
asked of sysfs at all.

An `sd_device` built from a netlink message is **sealed**: libsystemd answers
`sd_device_get_devtype()` out of the message and never falls back to the
`uevent` file. `block_device_is_whole_disk()` therefore got ENOENT for a device
whose sysfs `uevent` file said `DEVTYPE=disk` perfectly clearly. Our messages
had never carried the property.

`uevent_post()` takes a devtype now; block devices send `disk`/`partition`. With
it, `vda` runs the full rule set, writes `/run/udev/data/b8:0`, creates
`/dev/disk/by-uuid/…` and `/dev/disk/by-label/…`, and carries
`TAGS=:systemd:`.

### 3. AF_UNIX `SOCK_SEQPACKET` connect took the datagram path

`unix_connect` tested for `SOCK_STREAM` alone, so a seqpacket connect fell
through to the datagram branch: it never checked that the peer was listening,
never queued an endpoint for `accept()`, and pointed the client at the
**listening** socket itself. The client's message landed in the listener's own
ring buffer, its `shutdown(SHUT_WR)` marked the listener, and the listener
reported POLLIN for ever while `accept()` answered EAGAIN.

That is `udevadm control --ping`: it sends one message and waits for the daemon
to close the connection. It timed out on every boot. It passes now.

### 4. A message sent before its endpoint was accepted carried no credentials

Linux's `maybe_add_creds()` attaches the sender's credentials when either end
asked for them **or the receiving endpoint has no socket yet** — which is the
case for every one-shot protocol, because the client writes immediately after
`connect(2)` and the server has not yet accepted, let alone set SO_PASSCRED.
Ours required the flag, so the message arrived bare and systemd-udevd answered
`No sender credentials received, ignoring message`: the ping connected, was
delivered, and did nothing.

Credentials the kernel attaches on its own are reported only to a receiver that
asked (`cred_implicit`), so a caller with a small control buffer does not get an
unasked-for `SCM_CREDENTIALS` that truncates its ancillary data.

### 5. A signal never woke a sleeping task

Every kill path promoted a target that was `TASK_BLOCKED` or `TASK_STOPPED` and
left a `TASK_SLEEPING` one alone. A task in `nanosleep` is asleep, not blocked,
so a signal sent to it sat in `pending_signals` until the sleep's own deadline
expired.

From outside: `timeout 3 sleep 60` returned 124 on time and took sixty seconds.
A bound that reports the right answer while bounding nothing corrupts every
measurement made through it, and this harness bounds everything.

`sched_wake_for_signal()` is now the one place that promotes a task out of an
interruptible wait, used by all five kill paths. The sleeping wake is gated on
the signal actually doing something to the target (not blocked, not ignored),
because a sleeper that wakes ends its sleep on any unblocked pending bit — so
waking it for a signal it ignores would turn `sleep 60` into `sleep 0.1`. Linux
does not queue an ignored signal at all; this is the same rule applied at the
wake.

### 6. A TCP connection whose peer had closed was no longer there to accept

"Is there a connection to accept" was re-derived, on every poll and every
`accept`, from `state == TCP_ESTABLISHED`. A client that connects, writes and
closes — every one-shot client, and exactly what socket activation is driven by
— left ESTABLISHED for CLOSE_WAIT before the server was scheduled, and the
completed connection became invisible to both the listener's `poll` and its
`accept`. The bytes went away with the slot, which also leaked.

`b1nix-echo-tcp.socket` never activated its service for that reason. Acceptance
is a latch now (`accept_pending`, set at the handshake's completion and cleared
by `accept`), a passively-opened connection is marked as such so a listener
cannot be handed the caller's own outbound connection, and an unaccepted
connection is reclaimed after a minute rather than holding a slot for ever.

### 7. `/sys/class/tty/*/uevent` was read-only

A coldplug replay is `udevadm trigger` walking /sys and writing "add" to every
`uevent` file. Every VT answered `Permission denied`, so no terminal was ever
re-announced to a manager that started after the kernel did. They are 0644 now
and the write really re-announces the device, as the PCI and block nodes
already did.

### 8. `/proc/<pid>/fdinfo` did not exist

`pos`, `flags` and `mnt_id` for each open descriptor. The last is the
documented fallback for learning a mount id when `name_to_handle_at` is
unavailable, and systemd uses it that way; a missing file is read as "this
kernel cannot tell me".

## What these bought

`udevadm control --ping`, loopback TCP socket activation, `Type=notify`
readiness, `PrivateTmp=`, `.timer` units, `Restart=on-failure` and the
journal's per-unit index all pass now. `.device` units are created: a boot with
the manager's monitor working shows

```
dev-disk-by\x2dlabel-b1nix\x2dsystemd.device loaded active plugged
sys-block-ram0.device                        loaded active plugged
sys-block-vda.device                         loaded active plugged
```

## The two that still fail

### `.device` units are not there when the harness looks

They exist (above), and the whole chain up to them is now verified on one boot:
udevd processes the device, writes its db entry, tags it `systemd`, broadcasts
it on netlink group 2, and PID 1 is a listener on that group (`sent=1` per
message in `b1nix.trace-uevent`).

What goes wrong is upstream of that, in udev's own queue. `systemd-udevd` sets
`children_max` to 11, hands one device to each of eleven workers — every one of
which processes it, broadcasts the result, and writes its four-byte result to
the manager over the `AF_UNIX SOCK_DGRAM` socketpair with no error — and then
**never marks any worker idle again**. The remaining forty-odd events sit in
the queue until each worker's event timeout kills it. A task dump taken at that
moment (`/proc/b1nix-tasks`) shows every worker parked in `epoll_wait` (`sys=232`)
and the manager parked in `epoll_wait`, which is the manager not being told that
its socketpair became readable.

The obvious shape of that was tested directly and **passes**: a new check
(`UNIX-SMOKE: ok dgrampair-epoll-wake` / `ok dgrampair-scm-credentials`) sets
SO_PASSCRED on one end of a socketpair, parks in `epoll_wait`, and has a forked
child write — the wake arrives and the credentials name the child. So whatever
udevd's manager is missing, it is not that in isolation, and the next
instrument should be a trace of `epoll_ctl` registrations and of what a scan
reports for the manager's fds.

### A third that is flaky rather than failing: `notify-ready`

`Type=notify` passes on some boots and not others. When it fails the manager
says `Cannot find unit for notify message of PID 327, ignoring`: it received
the `sd_notify` datagram and could not attribute it to a unit, because the
sender is `systemd-notify`, a short-lived grandchild, and PID 1 resolves the
sender's unit from the cgroup of the pid in `SCM_CREDENTIALS` — a pid that has
already exited by the time the message is read. Whether that is ours (delivery
latency, or a `/proc/<pid>` that disappears sooner than Linux's) is not yet
measured. It is recorded here rather than counted as passing.

### `ProtectSystem=strict` lets the write through

The unit runs and its write to `/etc` succeeds where it must fail. `MS_RDONLY`
is enforced per mount (`vfs_create`, `vfs_unlink`, `vfs_rename`, … all check
it), and `vfs_remount` records the flag — but only on a mount recorded at
exactly the path named. Whether systemd's remount inside the unit's namespace
lands on a mount the write path then finds is not yet measured;
`b1nix.trace-mount` on a boot that starts `b1nix-ro.service` is the next step.

## Harness changes

- One log **per run**, written straight to its own path
  (`smoke_run/b1nix-systemd-boot-<tag>-<time>.log`, `-last.log` symlinked),
  rather than one shared path copied at the end — a run that is killed used to
  leave no evidence at all, and two runs at once destroyed each other's.
- Every unit file written at runtime is followed by a `daemon-reload`.
- The `.device` check polls for twenty seconds instead of sampling once: the
  unit appears when two asynchronous programs have finished talking, and asking
  once measures the scheduler.
- `sh -n` on the staged harness at image-build time. One unbalanced quote in a
  diagnostic killed a whole run and read exactly like a kernel that had stopped
  answering.
- The deep diagnostics do not turn the manager's log level to debug for more
  than one device: at debug level this manager writes faster than the serial
  console drains, and a machine that slow times out its own udev workers —
  which looks precisely like the defect being investigated.

## Checks added to the main suite

All of them fail on the tree as it was before this round.

| Check | What it holds |
|---|---|
| `M42-W5PRE: ok kill-sleeping-child` | a signal reaches a task asleep in `nanosleep` at once, asserted in elapsed time |
| `UNIX-SMOKE: ok seqpacket-accept` | a SOCK_SEQPACKET connect queues a real connection for `accept` |
| `UNIX-SMOKE: ok seqpacket-ctl-roundtrip` | udev's control protocol end to end, released by the server's close |
| `UNIX-SMOKE: ok dgrampair-epoll-wake` | a datagram wakes a task already parked in `epoll_wait` |
| `UNIX-SMOKE: ok dgrampair-scm-credentials` | and carries `SCM_CREDENTIALS` naming the sender |
| `TCP-SMOKE: ok accept-after-peer-close` | a completed connection survives its peer closing, client reaped first |
| `TCP-SMOKE: ok accept-after-peer-close-data` | and its bytes belong to the accepted socket |
| `M109-UEVENT: ok uevent-trigger` (extended) | the re-announcement carries DEVTYPE |
| `M109-UEVENT: ok uevent-trigger-tty` | a coldplug write works on a device that is not a disk |
