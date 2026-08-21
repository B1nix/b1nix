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

## What is still missing

- **No udev.** systemd's device monitor needs `SO_ATTACH_FILTER` (a cBPF socket
  filter), which this kernel does not implement, and the `udev` package is not
  in the image. Consequently **no `.device` unit ever becomes active**: anything
  ordered after one waits out its job timeout. That is why the image enables
  `console-getty.service` (agetty on /dev/console, no device dependency) rather
  than `serial-getty@ttyS0.service`, which is `BindsTo=dev-ttyS0.device`.
- **`systemd-logind` does not start** — it fails before exec with `ENOENT` from
  its sandboxing setup; not yet traced to a single call.
- **`systemd-update-utmp`, `e2scrub_reap` and `sys-kernel-debug.mount` fail.**
  utmp and debugfs are simply absent.
- **Slave/unbindable mount propagation is recorded, not enforced.** A unit with
  `PrivateTmp=` gets a private mount table copy, which is the important half,
  but events do not propagate from master to slave.
- **`/dev/log` is the kernel's syslog sink**, so journald's `dev-log` socket
  never receives anything: syslog traffic goes to the serial console instead of
  the journal.
- **No `SO_PEERSEC`, no audit netlink, no BPF.** systemd logs each as
  unavailable and continues.
- **An intermittent `SIGSEGV` at `rip=0` in a `pthread`-created thread** was
  seen once, in one run out of roughly twenty, before the `clone` register fix;
  it has not recurred since, but it is not proven gone.

## Coverage

`tests/systemd-smoke.sh`, 14 checks, all of them markers the guest prints only
after the named thing really happened. `SMOKE_DEBIAN=1 sh tests/smoke.sh` still
runs the sysvinit image; the two are independent.
