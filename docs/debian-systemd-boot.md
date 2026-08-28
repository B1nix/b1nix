# Booting Debian with systemd as PID 1

[`docs/debian-glibc-boot.md`](debian-glibc-boot.md) got a Debian glibc userspace
running under sysvinit and ended by listing what systemd would need. This is
that milestone.

Everything here runs unmodified Debian binaries — systemd 252.39, dbus 1.14.10,
util-linux's agetty, Debian's own units. Nothing in the guest is patched to work
around a kernel defect; where the guest failed, the kernel was fixed.

## Running it

```sh
make systemd-image     # downloads debian:bookworm-slim once, builds debian-systemd.ext4
make systemd-smoke     # boots it and checks the markers
```

`tests/systemd-smoke.sh` does **not** rebuild the image — run
`PROFILE=systemd sh tools/images/mk-debian-image.sh` first or you will measure a
stale guest.

`tools/images/mk-debian-image.sh PROFILE=systemd` resolves the **dependency
closure** of `systemd systemd-sysv dbus procps` against the suite's `Packages`
index, using the base layer's own `dpkg` status as "already installed" — 23
packages, none hand-listed, so a point release does not break the recipe. It
then makes the configuration `dpkg`'s maintainer scripts would have made: a
machine-id, an fstab, `console-getty.service` enabled, and one unit of ours
(`b1nix-smoke.service`) that runs the marker harness after `multi-user.target`.

The image boots the normal b1nix ISO with

```
root=LABEL=b1nix-systemd init=/sbin/init systemd.unit=multi-user.target
```

## Where it got to

| Stage | Result | Evidence |
|---|---|---|
| 1. systemd runs as PID 1 | yes | `systemd 252.39-1~deb12u2 running in system mode … default-hierarchy=unified`; `/proc/1/comm` = `systemd` |
| 2. cgroup v2 mounted, units placed in it | yes | `ok cgroup2 (/system.slice/b1nix-smoke.service)`, `controllers=pids` |
| 3. API filesystems mounted | yes | /proc, /sys, /dev (devtmpfs), /run, /dev/shm, /dev/pts, /sys/fs/cgroup |
| 4. generators run, default target queued | yes | `Queued start job for default target multi-user.target.` |
| 5. `systemctl` answers | yes | `ok systemctl-state` |
| 6. journald active, `journalctl` reads back | yes | `ok unit-active systemd-journald.service`, `ok journalctl` |
| 6b. systemd-udevd runs and stays up | yes | `ok unit-active systemd-udevd.service`; udevd forks a worker per device and applies its rules |
| 7. sysinit / basic / multi-user targets | reached | `ok unit-active sysinit.target`, `basic.target`, `multi-user.target` |
| 7b. graphical.target, started explicitly | active | the boot asks for `multi-user.target`, so the manager's job never included it |
| 8. dbus system bus | active | `dbus.service loaded active running` |
| 9. transient unit over the bus | yes | `systemd-run --wait --pipe` → `ok systemd-run` |
| 10. login prompt on the serial console | yes | `Debian GNU/Linux 12 b1nix console` / `b1nix login:` |
| 11. `.device` units activate | yes | `sys-block-vda.device loaded active plugged` |
| 12. socket activation, timers, `Type=notify`, `PrivateTmp=`, `Restart=on-failure` | yes | one check each |

`multi-user.target` is reached ~1.4 s into userspace. `systemd-analyze blame`
attributes the rest of "userspace" to `b1nix-smoke.service` — the harness — not
to the boot.

## The kernel defects this found

Each entry is what a real program did, what this kernel did wrong, and what
broke. They are listed so they are not re-investigated.

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
   filesystem somewhere else: every API filesystem systemd mounted landed on top
   of **/sys**. The link is resolved at read time from the owner's live fd
   table, and a closed descriptor is `ENOENT`.

3. **`statfs` only worked on a mount root, and the synthetic filesystems had
   none at all** — everything else got `ENOSYS`, which userspace reads as "this
   kernel has no statfs". systemd decides whether the machine has a unified
   cgroup hierarchy by `statfs("/sys/fs/cgroup")`. `statfs` now resolves the
   node's mount and asks its root, and procfs/sysfs/cgroup2 report their real
   magic numbers.

4. **No cgroup v2 filesystem.** Implemented: `kernel/fs/cgroup.c`, see below.

5. **`devtmpfs` was an alias for tmpfs.** Mounting it on /dev — which systemd
   does early — replaced every device node in the machine with an empty
   directory, and PID 1 lost `/dev/console` mid-boot. The devtmpfs mount now
   publishes its root and asks the VFS to lay the device nodes down inside it
   (`vfs_populate_dev`).

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
   LOCKED bits enforced and the flags really changing what `cred_refresh_caps`
   does on a uid transition), `GET/SET_KEEPCAPS`, `CAPBSET_READ/DROP`,
   `GET_NAME`, `GET_PDEATHSIG`, `GET/SET_DUMPABLE`, `GET/SET_TIMERSLACK`.

8. **`timerfd` ignored `TFD_TIMER_ABSTIME` and overflowed.** systemd arms its
   "the wall clock was stepped" watch for `TIME_T_MAX` seconds; multiplying that
   by the tick rate wrapped a `u64` back to a small number, so the timer that
   must never fire fired at once and every pass of the event loop rebuilt it:
   `Looping too fast. Throttling execution a little.`, forever. Absolute
   deadlines are now measured against the descriptor's own clock, the conversion
   saturates, and `CLOCK_BOOTTIME` and the `_ALARM` clocks are accepted.

### Stopping individual services

9. **`PR_CAPBSET_DROP` also cleared the effective set.** The bounding set is a
   ceiling on what may be *gained*; dropping from it must not take a capability
   away from the running process. As soon as systemd's bounding-set loop reached
   `CAP_SETPCAP` it lost the privilege the loop needs, and every remaining drop
   returned `EPERM`. `capset(2)` was lowering the bounding set too, which it
   does not do on Linux.

10. **`capget(2)` did no version negotiation.** Linux answers a header whose
    version it does not know with `EINVAL` *and* its preferred version written
    back; that pair is how a caller learns which layout to use. libcap-ng opens
    with `capget(&hdr, NULL)` and `hdr.version = 0`, and marks itself
    permanently broken otherwise — `Failed to drop capabilities: Success`.

11. **A newly created file had `st_nlink == 0`.** `create(2)` never set it.
    systemd-journald refuses to append to a journal whose link count is zero
    (`EIDRM`), so it created the journal, wrote its header, decided the file was
    already gone and removed it, on every start. `mkdir` and `symlink` had the
    same hole.

12. **`/proc/sys/kernel/random/boot_id` did not exist.** journald stamps every
    entry with the boot id, and `sd_id128_get_boot()` failing is `Failed to open
    runtime journal: No such file or directory` — on which journald exits 1 on
    every start, taking every unit that logs with it. Added, with `uuid` and
    `entropy_avail`, plus `/proc/sys/kernel/cap_last_cap` and `threads-max`
    (systemd reads the last to turn `DefaultTasksMax=15%` into a number, and
    reports its absence as `Failed to run 'start' task` naming no file).

13. **`/proc/<pid>/oom_score_adj` did not exist**, so journald failed at step
    `OOM_ADJUST`. It reads back what was written. Nothing consults it: this
    kernel has no OOM killer to rank victims for.

14. **`SO_TIMESTAMP` was unsupported.** journald sets it on its native socket
    and treats the failure as fatal. Implemented, and the arrival stamp really
    is attached as an `SCM_TIMESTAMP` control message. `SO_PASSCRED` was refused
    on anything but AF_UNIX (journald's audit socket is AF_NETLINK);
    `SO_RCVBUFFORCE`, `SO_SNDBUFFORCE` and `SO_PASSSEC` were missing.

15. **`fchownat` read its flags from the wrong argument.** The flags are the
    fifth argument; the code read the fourth, which is the *group id* and never
    has `AT_EMPTY_PATH` set. Every `fchownat(fd, "", u, g, AT_EMPTY_PATH)` — how
    systemd copies ownership onto a temporary file — was `ENOENT`.
    systemd-sysusers died on it while writing `/etc/gshadow`.

16. **`symlinkat` went through the generic `(dirfd, path)` resolver**, but its
    first two arguments are the symlink's *content* and the descriptor. It read
    a user string from the address `0xffffff9c` (`AT_FDCWD`) and always returned
    `EFAULT`.

17. **`inotify_add_watch` treated an `ERR_PTR` as a valid node.** A missing path
    installed a watch whose "node" was the encoded errno, so the next failed
    lookup with the same errno matched that watch and handed back its
    descriptor. systemd keys a hash map on the descriptor: two units collided.

18. **`split_path()` re-parsed a long component as two.** It peeled components
    into a 64-byte buffer while `VFS_NAME_MAX` is 256, and set `*rest` into the
    **middle** of the name. `systemd-private-<32 hex>-<unit>-XXXXXX` is 78
    characters and was looked up as a 63-character directory containing a
    15-character one, so `mkdtemp()` succeeded through a route that stored the
    name whole and the `mkdir()` inside its result answered ENOENT. Every unit
    with `PrivateTmp=` failed at `start` with `Result=resources`, systemd-logind
    among them — where its D-Bus activation made it look like an activation
    problem. The buffer is `VFS_NAME_MAX` and an over-long component is
    `ENAMETOOLONG`. A probe for this must use a component of the real length:
    the first one used a 22-character name and certified the area healthy.

19. **`rename(2)` cut a long name in half.** The rename path copied the
    destination name into a 64-byte field while every other creation path stores
    `VFS_NAME_MAX-1`, so a destination longer than 63 characters was silently
    renamed to a shorter, different name. "Write to a temporary, rename over the
    target" is how systemd, dpkg and glibc all replace a file.

20. **A directory's mtime never moved, and timestamps had whole-second
    resolution.** `systemctl start` re-scans the unit directories only when
    their mtimes differ from its last scan. Creating, removing or renaming an
    entry did not mark the containing directory modified (POSIX requires it),
    and `st_mtim.tv_nsec` was always zero, so two writes inside one second were
    indistinguishable — the symptom was `Unit b1nix-tick.timer not found` for a
    file sitting in `/run/systemd/system`. Inodes carry `atime_nsec`,
    `mtime_nsec` and `ctime_nsec`, filled from one clock read. Every tool that
    decides whether to re-read a directory by its mtime had the same blind spot.

### Corrupting userspace

21. **The `clone(2)` child got a zeroed register file.** Linux hands the child
    the caller's registers with only the return value and stack changed, and
    libcs depend on it: glibc's `__clone3` keeps the thread function in `%r9`
    and its argument in `%r8` across the syscall. Threads started with a NULL
    argument or called through a NULL pointer — dbus-daemon, systemd-logind and
    systemd's own workers all died at `rip=0`.

22. **`accept4` handed the caller's pointers straight to the VFS.** The peer
    address was written with no regard for the size the caller declared, and the
    `socklen_t` beside it as a 64-bit word over a 32-bit field. dbus-daemon
    accepts with a 16-byte `struct sockaddr` on its stack: `*** stack smashing
    detected ***` on the first connection it served. `accept`, `getsockname` and
    `getpeername` now truncate to what the caller offered and report the
    untruncated length, as Linux does.

23. **`epoll`'s recursion counter was a `static __thread int` in kernel code.**
    On x86-64 TLS is addressed through `%fs`, and in ring 0 the FS base is still
    the *user* thread's TLS pointer — so every read and write of that counter
    went to user memory, and to a page fault for a thread with no TLS.

24. **`/dev/log` copied a kernel buffer as if it were a user pointer.** Every
    caller bounces the payload in before reaching the socket layer, so the
    second copy failed the user-range check and every syslog datagram came back
    `EFAULT`. That is where dbus-daemon reports why it is exiting.

### Making the machine tell the time

25. **`CLOCK_REALTIME` walked backwards, continuously.** The seconds came from
    the 100 Hz tick counter and the nanoseconds from the TSC; the two are not in
    phase, so the composite went backwards by up to a second all boot long.
    systemd-journald noticed — `Time jumped backwards, rotating` — and threw its
    journal away each time. There is now one wall clock (`rtc_now_unix_nanos`)
    and every reader takes both halves from it.

26. **Every seconds-to-ticks conversion multiplied by 100 while the LAPIC timer
    runs at 1 kHz.** `alarm(2)`, `setitimer(2)`, the POSIX timers,
    `sigtimedwait` and the tick fallback in `clock_gettime` all fired ten times
    early. `timeout(1)` arms a POSIX timer, so `timeout 10 sleep 2` reported a
    timeout for a command that finished in two seconds: **every harness that
    bounds a command was reporting false timeouts on work that had succeeded.**
    They go through one saturating helper that reads `sched_tick_hz()`.
    `sched.h` already warned about this and provides `SCHED_MS_TO_TICKS`.

27. **A signal never woke a sleeping task.** Every kill path promoted a target
    that was `TASK_BLOCKED` or `TASK_STOPPED` and left a `TASK_SLEEPING` one
    alone, so a signal sent to a task in `nanosleep` sat in `pending_signals`
    until the sleep's own deadline: `timeout 3 sleep 60` returned 124 on time
    and took sixty seconds. A bound that reports the right answer while bounding
    nothing corrupts every measurement made through it.
    `sched_wake_for_signal()` is now the one place that promotes a task out of
    an interruptible wait, used by all five kill paths, and the sleeping wake is
    gated on the signal actually doing something to the target — waking for a
    signal the target ignores would turn `sleep 60` into `sleep 0.1`.

### Making the bus work

28. **`read(2)` on a non-blocking AF_UNIX socket blocked.** `recvmsg` had been
    fixed for this; plain `read` had not, and dbus-daemon reads its connections
    with `read()`. It parked in the middle of serving systemd's first method
    call until the caller gave up 25 seconds later.

29. **An unconnected AF_UNIX datagram send ignored its destination.** `sendmsg`
    with `msg_name` and no `connect` — which is exactly `sd_notify(3)` — was
    answered `ENOTCONN`, so no `Type=notify` unit could report itself started
    and systemd killed each one on its start-up timeout. AF_UNIX `SOCK_DGRAM`
    also had no message boundaries: two datagrams queued back to back arrived as
    one.

30. **A socket had no address family until it was bound.** sd-bus asks
    `sd_is_socket(fd, AF_UNIX, …)` before offering descriptor passing and
    concluded the connection could not carry descriptors, so `systemd-run
    --pipe` failed with `EOPNOTSUPP` before sending anything. An accepted socket
    also inherited no address from its listener.

31. **Running out of ancillary-data slots was reported as `ENOBUFS`.** With
    `SO_PASSCRED` set, which is what a bus daemon does to every connection,
    every message takes one, and a bus client treats a failed write as a dead
    peer.

32. **Zero-length datagrams were dropped in three places**, which Linux
    delivers: `sendmsg`/`recvmsg` refused an iovec of total length zero with
    `EINVAL`; `write`/`send`/`sendto` of zero bytes returned 0 without sending;
    and a queued zero-length AF_UNIX message never made the socket readable,
    because readability was gated on bytes in the ring rather than messages in
    the queue. Ancillary data is the whole content of some such messages.

33. **AF_UNIX `SOCK_SEQPACKET` connect took the datagram path.**
    `unix_connect` tested for `SOCK_STREAM` alone, so a seqpacket connect never
    checked that the peer was listening, never queued an endpoint for
    `accept()`, and pointed the client at the **listening** socket itself: the
    client's message landed in the listener's own ring, and the listener
    reported POLLIN for ever while `accept()` answered EAGAIN. That is
    `udevadm control --ping`.

34. **A message sent before its endpoint was accepted carried no credentials.**
    Linux's `maybe_add_creds()` attaches them when either end asked *or the
    receiving endpoint has no socket yet* — the case for every one-shot
    protocol, because the client writes immediately after `connect(2)`. Ours
    required the flag, so systemd-udevd answered `No sender credentials
    received, ignoring message`. Credentials the kernel attaches on its own are
    reported only to a receiver that asked (`cred_implicit`), so a caller with a
    small control buffer does not get an unasked-for `SCM_CREDENTIALS` that
    truncates its ancillary data.

35. **A TCP connection whose peer had closed was no longer there to accept.**
    "Is there a connection to accept" was re-derived on every poll and every
    `accept` from `state == TCP_ESTABLISHED`, so a client that connects, writes
    and closes — every one-shot client, and what socket activation is driven by
    — left the completed connection invisible to both `poll` and `accept`, and
    leaked the slot. Acceptance is a latch now (`accept_pending`), a passively
    opened connection is marked as such so a listener cannot be handed the
    caller's own outbound connection, and an unaccepted connection is reclaimed
    after a minute.

36. **A dual-stack listener refused the IPv4 connections it must accept.** An
    `AF_INET6` listener that is not `IPV6_V6ONLY` — the default, and what
    `ListenStream=<port>` with no address produces — must report an IPv4 peer as
    `::ffff:a.b.c.d`. The passive-open path matches a SYN on the port alone, so
    the connection was created as `AF_INET` and `accept` then refused it for
    having the wrong family while `poll` went on reporting the listener
    readable.

### Making the device tree work

37. **`name_to_handle_at` reported mount id 0 for every path**, and no
    mountinfo row is numbered 0. systemd's `dev_is_devtmpfs()` asks for the
    mount id of `/dev` and scans mountinfo for it; failing that test,
    `device_monitor_new_full()` **disables the device monitor for the rest of
    the boot** (the container case), so **no `.device` unit could activate
    whatever udev did**. Visible in one line of `b1nix.trace-uevent`:
    `groups=0x1 pid=1` before, `groups=0x2 pid=1` after — group 2 is the one
    udevd broadcasts on. `vfs_mount_id_for_path()` answers the mount's position
    in the visible list, +1, exactly how `r_mountinfo` numbers its rows.

38. **`/proc/<pid>/fdinfo` did not exist.** `pos`, `flags` and `mnt_id` per
    descriptor; the last is the documented fallback for a mount id when
    `name_to_handle_at` is unavailable, and systemd uses it that way.

39. **A uevent carried no DEVTYPE.** An `sd_device` built from a netlink message
    is **sealed**: libsystemd answers `sd_device_get_devtype()` out of the
    message and never falls back to the `uevent` file, so
    `block_device_is_whole_disk()` got ENOENT for a device whose sysfs `uevent`
    said `DEVTYPE=disk`. Publishing `/sys/block/<disk>/queue` did not change the
    message, because the question is not asked of sysfs at all. `uevent_post()`
    takes a devtype; block devices send `disk`/`partition`.

40. **The netlink source address named a process, not a socket.**
    `sockaddr_nl.nl_pid` is a **port id**: it identifies a socket, and a task
    holding several netlink sockets has several. The address a receiver read
    back was filled in from the sender's *process* id, so a udev worker checking
    the message's source against `getsockname` on the manager's monitor
    (`device_monitor_allow_unicast_sender`) discarded every device it was
    handed: `Unicast netlink message ignored`. Each worker processed exactly the
    one device it inherited across the fork, udevd forked up to its child limit,
    and the queue stopped. Each queued message records the port id of the socket
    that sent it and `recvmsg`'s `msg_name` reports it; `SCM_CREDENTIALS` still
    carries the real pid — they are different questions.

41. **`recvmsg` on a netlink socket left `msg_name` zeroed**, and systemd's
    device monitor requires the source to say `AF_NETLINK` with `nl_pid == 0`
    before it will look at a message — so udevd read every coldplug event and
    discarded it, which from outside is indistinguishable from a kernel that
    sends none. Getting this wrong is worse than leaving it blank: nearly every
    netlink message a task receives is a reply the kernel builds in that task's
    own context, so crediting it to the running task turns `nl_pid` from 0 into
    the caller's pid, and iproute2 then discards its own answers and hangs. **A
    message is from the kernel unless a task's own `write(2)` put it there.**

42. **A groupless netlink send was refused with `EINVAL`.** A udev worker's
    monitor is a `NETLINK_KOBJECT_UEVENT` socket in **no** multicast group, and
    the manager sends it one device at a time by port id: every worker reported
    `did not accept message, killing the worker: Invalid argument`. Unicast
    delivery is implemented, a netlink socket is a possible destination whatever
    groups it joined, and two sockets in one task no longer share a port id.

43. **`/sys/class/tty/*/uevent` was read-only.** A coldplug replay is `udevadm
    trigger` writing "add" to every `uevent` file; every VT answered `Permission
    denied`, so no terminal was re-announced to a manager that started after the
    kernel did. They are 0644 and the write really re-announces the device.

### Mounts and namespaces

44. **A file bound onto itself renamed itself to `/`.** `vfs_get_node_path()`
    walks a node's parents, stepping from a filesystem root to the node it is
    mounted over. `ReadOnlyPaths=`, `ProtectHostname=` and
    `ProtectKernelTunables=` all bind a file **onto itself**, so both ends of
    that step were the same node, the walk stepped from the node to itself until
    its budget ran out, and the path came out as `/`. Since
    `/proc/<pid>/fd/<n>` is rendered from that walk and systemd mounts through
    it, every later mount target resolved to the root directory — and a mount
    whose two ends agreed then landed a unit's private tree on top of `/`. The
    seam now applies only to a node with no parent and never steps to the node
    it started from.

45. **`/proc/<pid>/fd/<n>` re-walked a path instead of naming the file.** A node
    reached through a bind mount lives in two places and remembers only the
    first. A descriptor now remembers the path it was opened at
    (`vfs_handle::open_path`), which is what Linux's `(mount, dentry)` pair
    encodes, and resolution follows a **magic link**
    (`vfs_inode::magic_link_cb`) straight to the open file's node. A descriptor
    with no file behind it still falls back to the `pipe:[7]` text.

46. **A mount was filed under the node's name, not the caller's.**
    `/proc/self/mountinfo` showed `/proc/sys/kernel/domainname` for a mount made
    at `/run/systemd/unit-root/proc/sys/kernel/domainname`; systemd re-reads
    mountinfo after each bind, did not find it, bound again 32 times and failed
    the unit with `EBUSY` — the "Device or resource busy" that
    `systemd-udevd.service` reported for weeks. A mount records the resolved
    path the caller named, and `MS_REMOUNT` and the propagation calls
    canonicalise the same way.

47. **A read-only mount refused only the second half of the operation.**
    `write`, `create`, `mkdir`, `unlink`, `rename` and `ftruncate` checked
    `MS_RDONLY`; `open(2)` did not — so `open(path, O_WRONLY)` on a read-only
    mount handed back a descriptor, and `O_TRUNC` emptied the file with no check
    at all. `link` and `symlink` were unchecked too. The mount lookup also
    answered with the *oldest* mount at a node: a bind mount records its source
    as the mount's root, so binding a path onto itself leaves two entries rooted
    at one node and the scan returned the one that was there before the
    read-only remount. Mounts carry a monotonic sequence number and the newest
    wins. The fallback for a node under no mount at all is deliberately
    unchanged — that is a different question, and answering it by recency
    changed which filesystem's flags an unattached node was judged by.

### Terminals

48. **`TIOCSCTTY` on `/dev/console` was rejected because its argument is a
    value, not a pointer.** A blanket "no argument means EINVAL" refused it, so
    systemd could not acquire the terminal for a getty and no login prompt was
    printed. `TIOCSCTTY` now really claims the console for the caller's session,
    and `TIOCNOTTY`, `TIOCVHANGUP`, `TCFLSH`, `TCSBRK`, `TIOCEXCL` and
    `TIOCNXCL` are answered.

49. **`prctl(PR_SET_NAME)` was accepted and ignored**, so `/proc/1/comm` said
    `init`, not `systemd`.

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
set it. `cgroup.controllers` lists what this kernel can actually do, and systemd
logs the rest as unavailable — the same thing it does on a Linux kernel built
without those controllers.

## Mount propagation

Recorded per mount and reported in `/proc/self/mountinfo` as `shared:N` /
`master:N` / `unbindable`. A mount namespace here is a copy of the mount table,
so "the copies are peers" is given the only meaning it can have: a mount made
under a shared mount is made under every peer's namespace too
(`vfs_mount_propagate`). Slave propagation is recorded but does not forward
events.

## The device tree userspace walks

Three views of the same devices were missing, and each made devices invisible to
a different consumer. All three publish only what the kernel already knows.

### `/sys/block/<disk>/queue` — how systemd tells a disk from a partition

`block_get_whole_disk()` looks for `<dev>/queue` and calls the device a whole
disk if it is there, failing that for `<dev>/partition`. `vda` had neither, so
it was neither. The directory now carries the values the block layer has:
`logical_block_size`, `physical_block_size`, `hw_sector_size`,
`minimum_io_size`, `max_sectors_kb`, `max_hw_sectors_kb`, `max_segments`,
`nr_requests`, `rotational`, `scheduler` (really `none` — there is no I/O
scheduler to name) and `discard_granularity` (zero when the driver never offered
the command). `optimal_io_size` and `write_cache` are **absent rather than
guessed**: a wrong number there is one a filesystem lays itself out around.

### `/sys/dev/char` — how libudev finds a device by its number

The `dev` directory carried only `block`, so every character device was
invisible to anything that identifies devices the way udev does rather than by
path. It is populated from the character devices under `/dev`, **recursively** —
a DRM card is `dri/card0` and a pty slave is `pts/N`, and a walk of `/dev`'s
immediate children would have found neither while looking like a working fix.
`DEVNAME` is the path relative to `/dev`, as Linux writes it. Entries for major
226 are left to the DRM layer, whose entries are links to the card's minor
directory and carry the `device` link libdrm follows; a plain directory of the
same name would shadow that.

`/dev/null`, `/dev/zero`, `/dev/console` and `/dev/ptmx` reported `st_rdev == 0`
— device files that are no device. They carry the numbers the Linux ABI fixes
for them (1:3, 1:5, 5:1, 5:2). Those are not ours to choose.

### `/sys/devices/pci0000:00` — the parent chain udev matches rules against

`/sys/devices` held `system` and nothing else. `pci_sysfs_publish_all()`
publishes every function the enumeration finds: `vendor`, `device`, `class`,
`revision`, `irq`, `subsystem_vendor`, `subsystem_device` and a Linux-shaped
`uevent`, with `subsystem -> ../../../bus/pci` and the
`/sys/bus/pci/devices/<addr>` links, every value read out of config space at
publish time. Three deliberate limits:

- **Subsystem ids only on a type 0 header.** A bridge's `0x2C` is its secondary
  bus numbers; reading it as a subsystem vendor would publish a number that
  means something else. Bridges get the `sv*sd*` wildcards Linux writes.
- **No `driver` link.** This kernel records no binding between a function and
  the code driving it, and a `driver` link is read as a claim that something has
  claimed the device.
- **`uevent` is writable and really re-announces the device.** With the file
  read-only, `udevadm trigger` reached no device at all; accepting the write and
  doing nothing would be the "writable file that silently discards writes" the
  sysfs registry's own contract warns against.

`kernel/dev/drm.c` used to fill an unidentified card's attributes with an Intel
id written into its source; it no longer does, so such a card shows no
`vendor`/`device` at all. The PCI node is where a real identity comes from, and
an absent attribute is an honest "not known" where a borrowed one was an answer
nothing could check.

What this does not give is a `device` link with a *bus address* for a DRM node
on a machine that has one — the chain exists and the PCI node is real, but the
headless test image has no DRM card, so it cannot prove it.

## What is still missing

- **`systemd-update-utmp`, `e2scrub_reap` and `sys-kernel-debug.mount` fail.**
  utmp and debugfs are simply absent.
- **Slave/unbindable mount propagation is recorded, not enforced.** A unit with
  `PrivateTmp=` gets a private mount table copy, which is the important half,
  but events do not propagate from master to slave.
- **No `SO_PEERSEC`, no audit netlink, no BPF.** systemd logs each as
  unavailable and continues.
- **`hwdb.bin` is not built into the image**, so udev's hardware database is
  empty. Nothing in this boot depends on it.
- **`Type=notify` has been flaky rather than failing.** When it fails the
  manager says `Cannot find unit for notify message of PID N, ignoring`: it
  received the `sd_notify` datagram and could not attribute it to a unit,
  because the sender is `systemd-notify`, a short-lived grandchild, and PID 1
  resolves the sender's unit from the cgroup of the pid in `SCM_CREDENTIALS` —
  a pid that may already have exited. Whether that is ours (delivery latency, or
  a `/proc/<pid>` that disappears sooner than Linux's) is not measured.

## Diagnostics

All off unless asked for on the kernel command line.

| Flag | What it prints |
|---|---|
| `b1nix.trace-mount` | every `mount(2)` that names no filesystem — bind, remount, move, propagation — with its flags, its result, and the path the mount was recorded under |
| `/proc/b1nix-tasks` | reading it prints the scheduler's task dump — which syscall every task is in — at a chosen moment, instead of paying for one every ten seconds |
| `b1nix.trace-uevent` | every uevent broadcast: group, length, how many sockets were listening, how many got it; and each uevent socket as it registers |
| `b1nix.trace-errno=<n>` | the number of every syscall that returned that errno, the pid that made it, and — for a call that takes one — the path it was given. "errno 2 from syscall 83" says a mkdir failed and not which directory, and the directory is the whole answer |
| `b1nix.trace-errno-pid=<pid>` | narrows the above to one task. Unfiltered ENOENT is thousands of lines through a serial console that is already the machine's bottleneck |
| `b1nix.sysd-debug` | (guest, not kernel) the systemd harness's deep diagnostics: the timer and loopback probes, the udev and logind evidence, and a `/proc/b1nix-prof` bracket around `daemon-reload` |

## Coverage

`tests/systemd-smoke.sh`, 32 checks, all of them markers the guest prints only
after the named thing really happened. The socket-activation check runs twice,
once over an AF_UNIX listener and once over loopback TCP, because the manager's
half of socket activation is the same either way while what sits underneath is
not. `SMOKE_DEBIAN=1 sh tests/smoke.sh` still runs the sysvinit image; the two
are independent.

The kernel-side fixes are covered in the main suite as well, so a regression is
caught without a Debian image:

| Check | What it holds |
|---|---|
| `M46-SMOKE: dir-mtime-create`, `dir-mtime-subsecond`, `dir-mtime-unlink` | a directory's mtime moves when an entry is created, removed or renamed, with sub-second resolution |
| `M46-SMOKE: rename-long-name` | a rename keeps a name longer than 63 characters |
| `M46-SMOKE: empty-datagram-cred` | a zero-length datagram is delivered and carries its credentials |
| `M46-SMOKE: rdonly-mount-open` | `open(O_WRONLY)` and `O_TRUNC` are refused on a read-only mount |
| `M46-SMOKE: signal-ends-sleep`, `M42-W5PRE: kill-sleeping-child` | a signal reaches a task asleep in `nanosleep` at once, asserted in elapsed time — a status check alone is satisfied by a child that slept to the end and was signalled afterwards |
| `UNIX-SMOKE: seqpacket-accept`, `seqpacket-ctl-roundtrip` | a SOCK_SEQPACKET connect queues a real connection for `accept`, and udev's control protocol works end to end |
| `UNIX-SMOKE: dgrampair-epoll-wake`, `dgrampair-scm-credentials` | a datagram wakes a task already parked in `epoll_wait`, and carries `SCM_CREDENTIALS` naming the sender |
| `TCP-SMOKE: accept-after-peer-close`, `accept-after-peer-close-data` | a completed connection survives its peer closing, client reaped first, and its bytes belong to the accepted socket |
| `NET-SMOKE: dual-stack-accept` | an IPv4 connection to a dual-stack listener is accepted |
| `M109-UEVENT: netlink-source-portid` | the source address of a uevent names the sending socket |
| `M109-UEVENT: uevent-trigger`, `uevent-trigger-tty` | a coldplug write re-announces a device, carries DEVTYPE, and works on a device that is not a disk |

## Harness notes

- One log **per run**, written straight to its own path
  (`smoke_run/b1nix-systemd-boot-<tag>-<time>.log`, `-last.log` symlinked): a
  run that is killed used to leave no evidence, and two runs at once destroyed
  each other's.
- Every marker goes to `/dev/kmsg`. `console-getty.service` is `Type=idle`, so
  agetty claims `/dev/console` — and `vhangup()`s it — while the harness is
  still running, and writes after that point go nowhere. `/dev/kmsg` reaches the
  same serial line and nobody can take it away.
- Every unit file written at runtime is followed by a `daemon-reload`. A unit
  the manager has not read is "not found", which is the right answer and not a
  kernel failure — six checks were once recorded as kernel failures for that.
- The journal-index check writes one line of its own to stdout through
  `StandardOutput=journal`: `/dev/kmsg` is filed as a *kernel* message belonging
  to no unit, so `journalctl -u b1nix-smoke.service` otherwise matched whatever
  noise landed there last.
- The `.device` check polls for twenty seconds instead of sampling once: the
  unit appears when two asynchronous programs have finished talking, and asking
  once measures the scheduler.
- Every command the harness runs is bounded by `timeout`, and the unit's
  `TimeoutStartSec=` is looser than both the harness's own bounds and the
  runner's: at 90 s systemd killed the harness partway through and every later
  check was reported missing rather than failed.
- `sh -n` on the staged harness at image-build time. One unbalanced quote in a
  diagnostic killed a whole run and read exactly like a kernel that had stopped
  answering.
- The deep diagnostics do not leave the manager's log level at debug for more
  than one device: at debug level it writes faster than the serial console
  drains, and a machine that slow times out its own udev workers — which looks
  precisely like the defect being investigated.
