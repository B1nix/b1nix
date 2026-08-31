# A stock Arch Linux userspace on this kernel

Debian bookworm boots here with systemd 252 on glibc 2.36. Arch is rolling, and
the tarball this image is built from carries **systemd 261.2 on glibc 2.44** —
nine years of kernel-interface changes newer. That is the whole point of having
it: a manager that new asks the kernel for things the older one never mentions,
and every one of those questions is a measurement of what this kernel actually
implements.

```sh
sh tools/images/mk-arch-image.sh    # once, or after any image change
sh tests/arch-smoke.sh              # builds the ISO, boots, judges
```

## Where the boot gets to

Arch's own systemd runs as PID 1, prints its version banner and the
distribution's `Welcome to Arch Linux!`, runs all sixteen system generators,
loads the unit graph and starts **34 units** — every `.slice`, the path units,
the timers, the password agents, and every `.socket` there is: journald's,
udevd's, logind's, the user database manager's. Seven targets are reached.

Then it stops, seven and a half seconds in, and does not recover:

```
systemd-journald.service: Failed at step CREDENTIALS spawning \
    /usr/lib/systemd/systemd-journald: Function not implemented
```

**`tests/arch-smoke.sh` passes all 33 checks.** The boot reaches
`multi-user.target` and `graphical.target`; journald, udevd, logind, the system
bus, socket activation, timers, `Restart=`, the notify protocol, `PrivateTmp=`,
`ProtectSystem=strict`, device units, transient units and the console getty all
work. A stock Arch userspace with systemd 261 on glibc 2.44 runs on this
kernel.

One measurement worth recording because it is better than the one above and was
not reproduced: on an intermediate tree — after ambient capabilities and
`clone3(CLONE_INTO_CGROUP)`, before `mount_setattr(2)` existed and before user
namespaces were refused honestly — journald failed at `CREDENTIALS`
*immediately* instead of stalling there, the boot walked past it, and the
console printed `Reached target System Initialization`, `Basic System`,
**`Multi-User System`** and **`Graphical Interface`**. That is a real
measurement of a real tree, and it says the unit graph and the manager are
sound; it is not the behaviour of the tree in this branch, and which of the
later changes cost it has not been isolated.

## Building the image without root

Same constraints as `tools/images/mk-debian-image.sh`: an ordinary user, no
`pacstrap`, no loop mounts, no `sudo`.

The tree is the official **`archlinux-bootstrap-x86_64.tar.zst`**, verified
against the sha256 the release publishes beside it, unpacked with `zstd` + `tar`
inside one `fakeroot` session and handed to `mke2fs -d`. It was chosen over the
`archlinux` image on the Docker registry — which the Debian script's registry
code path would have made cheaper to reach — because that image is the same
package set with `NoExtract` rules applied: no locales, no man pages, and
container-specific unit masking. The question this image exists to ask is what a
*stock* distribution's systemd wants from the kernel, and a container-trimmed
tree cannot answer it.

For the systemd profile the tarball needs **no extra packages at all**: it
already carries systemd 261, `systemd-sysvcompat` (which provides `/sbin/init`)
and a full `base`. `tools/images/arch-closure.py` resolves a dependency closure
from the repository `*.db` files against the tarball's own pacman database, the
way the Debian script resolves one from `Packages.xz`; the graphics profile uses
it for `weston ttf-dejavu seatd`.

Two deliberate deviations from stock, both ordinary image preparation:

- **A machine-id, an fstab, a console getty, timeouts** — the things a pacman
  hook or `systemctl preset-all` would have made, exactly as the Debian image
  stages them.
- **`/etc/nsswitch.conf` trimmed to `files`.** Arch ships
  `group: files [SUCCESS=merge] systemd`, and `[SUCCESS=merge]` consults the
  `systemd` module even when `files` has already answered. That module is a
  varlink client for `systemd-userdbd`, which this image does not run; PID 1
  binds the socket before it runs the generators and then blocks waiting for
  them, so the first generator to look up a group waits on a manager that is
  waiting on it, and only its own timeout ends the standoff — forty-five
  seconds per lookup. The stock file is kept beside it as
  `/etc/nsswitch.conf.arch`. Nothing about that standoff is a kernel fault:
  the connect, the wait and the timeout are all doing what they are supposed
  to.

## The kernel defects it found

Every one of these was found by measurement, not by reading. Two instruments
did most of the work and are worth knowing about:

- **`b1nix.trace-errno=all`** with `b1nix.trace-errno-pid=<n>` prints every
  failing system call of one task, with its name. Guessing an errno first is
  only possible when the program says which one it got, and a manager that dies
  without printing anything says nothing at all — which is exactly when it is
  needed. It is what found the first defect below.
- **`b1nix.trace-unix-connect`** now prints what the path resolved to and what
  else was in that directory, not only the path asked for.

### 1. No terminals at all, on a glibc from 2.42 onwards

systemd 261 opened `/dev/console`, closed it again without writing a byte, and
exited 255 having printed nothing — not its version banner, not the reason.

`glibc 2.42` made `tcgetattr(3)` issue **`TCGETS2`** instead of `TCGETS`, and
`isatty(3)` *is* `tcgetattr` succeeding. b1nix answered only the `0x5401`
family, so on a libc that new **nothing on this machine was a terminal**.
systemd's `open_terminal()` checks, was refused, and from that moment had
nowhere to print a single line — including the line saying why it was about to
give up. Every message its whole boot would have produced was lost to one
missing ioctl.

`TCGETS2`/`TCSETS2`/`TCSETSW2`/`TCSETSF2` are implemented across all four tty
drivers — the boot console, `ttyS0`, the virtual console and the pty pair —
through one shared `struct termios2` conversion beside the existing one. The
speeds it adds are read from and folded back into the `CBAUD` bits of
`c_cflag`, which is the only place this kernel keeps a line rate.

While the console ioctls were open: an ioctl the console does not implement
answered **`-1`, i.e. EPERM** — "operation not permitted" for a request it had
simply never heard of. A program probing for an optional ioctl reads that as a
permission problem worth reporting or giving up over. It is `ENOTTY` now.

### 2. `statx` could not say whether anything was a mount point

With the console working, systemd said what was wrong:

```
Failed to determine whether /proc is a mount point: Protocol driver not attached
… /sys … /dev … /dev/shm … /run …
[!!!!!!] Failed to mount API filesystems.
```

`EUNATCH` is systemd's "this kernel cannot tell me". Since **systemd 256 the
only way it asks the question is `statx`'s `STATX_ATTR_MOUNT_ROOT`** — the
older routes through `name_to_handle_at(2)` and `/proc/self/mountinfo` were
removed. b1nix left `stx_attributes` and `stx_attributes_mask` at zero, which
says "this kernel knows nothing about attributes", so PID 1 asked about each
API filesystem in turn, got no answer, and gave up before mounting anything.

`stx_attributes_mask` now carries `STATX_ATTR_MOUNT_ROOT` and the bit is set
when the path really is a mount root (`vfs_path_is_mount_root`). `stx_mnt_id`
is filled too, for `STATX_MNT_ID` and `STATX_MNT_ID_UNIQUE` — with the mount id
`/proc/<pid>/mountinfo` prints, which b1nix does not reuse within a boot, so it
satisfies what the unique form promises. Both are reported only when they are
really known: setting a mask bit over a zero would be worse than leaving it
clear, because systemd would then go looking in mountinfo for a row numbered 0.

### 3. `open("/proc/self/fd/N")` was a `dup`, and threw the flags away

PID 1 then mounted everything itself and read **none of its own
configuration**: `/etc/systemd/system.conf`, `/etc/os-release` and
`/etc/machine-id` all came back "Bad file descriptor", reported as a syntax
error at line 0, and the manager ran on defaults.

The two are not the same operation, and the difference is the whole reason
userspace uses that path. `dup(2)` hands back the same open file description:
same flags, same offset. Opening `/proc/self/fd/<n>` on Linux performs a **fresh
open of the file the descriptor refers to, with the flags given** — which is
how a program turns an `O_PATH` reference, which cannot be read, into a
descriptor it can. systemd 254 and later open every configuration file that way
(`chase()` returns an `O_PATH` fd, `fd_reopen()` upgrades it), so the upgraded
descriptor was `O_PATH` as well and every read of it answered `EBADF`.

A descriptor with no VFS node behind it — a pipe, a socket, an eventfd — keeps
the old behaviour, because there is no file to open afresh and another
reference to the same description is the only meaningful answer. That is the
case bash's process substitution depends on.

### 4. `clone(2)` ignored every `CLONE_NEW*` flag

This is the one that mattered most, and the hardest to see: the boot simply
went quiet after the generators, with no error anywhere.

`scheduler_fork_clone` inherits the parent's namespaces and the flags passed to
`clone(2)` were dropped on the floor. systemd forks its generators with
`CLONE_NEWNS` and then, believing it is inside its own mount namespace,
remounts the root read-only. It really was read-only — for everyone. PID 1's
own log descriptor started answering `EROFS`, and every message from that point
on was lost.

Dropping the flags is not a missing feature, it is a wrong answer: a process
that asked for a private mount namespace and quietly got the shared one goes on
to remount things "for itself", and every one of those mounts is everyone's.

`CLONE_NEWNS`, `CLONE_NEWUTS` and `CLONE_NEWNET` are honoured now. The work is
done in the **parent, before the fork** (`namespace_child_prepare`), and stamped
onto the child under the same lock that makes it visible: a forked child does
not return through the C code at all — it resumes at
`x86_fork_child_trampoline` and goes straight back to ring 3 — and even if it
did, it is runnable the instant the fork returns, so anything done to it
afterwards can be too late. Cloning a mount table also allocates and takes VFS
locks, which the fork's tail, running with interrupts disabled, is no place for.

`CLONE_NEWPID`, `CLONE_NEWUSER`, `CLONE_NEWIPC` and `CLONE_NEWCGROUP` are
**refused** with `EINVAL`, which is what `unshare(2)` already answered — so the
two calls now agree about what exists. That refusal is itself a fix: systemd
probes for id-mapped mounts by cloning into a user namespace and writing the
child's `/proc/<pid>/uid_map`, and with the flag silently ignored it got an
ordinary child and then `ENOENT` from a file that does not exist. That `ENOENT`
is fatal to the unit.

The mount-namespace ceiling was **eight**. A systemd machine puts every
sandboxed unit and every generator in one of its own and runs out during the
boot, at which point `clone(CLONE_NEWNS)` answers `ENOSPC` and the unit fails
for a reason that has nothing to do with the unit. It is 64.

### 5. `prctl(PR_CAP_AMBIENT)` — without it, not one unit can start

systemd applies an ambient capability set before spawning **every** process,
and treats a failure as fatal to the spawn:

```
tmp.mount: Failed to apply the starting ambient set: Invalid argument
tmp.mount: Failed to spawn 'mount' task: Invalid argument
```

The ambient set is a real field on the credentials now, with Linux's invariant
enforced: a capability may be raised into it only while it is both permitted
and inheritable, and it leaves the set as soon as it leaves either — `capset(2)`
maintains that on every change.

### 6. `clone3(CLONE_INTO_CGROUP)` — the same, one layer down

With ambient capabilities in place the next answer was
`Failed to spawn executor: Invalid argument`. glibc's `posix_spawn()` implements
`POSIX_SPAWN_SETCGROUP` with `clone3(CLONE_INTO_CGROUP)`, and systemd 254 and
later spawn every unit through exactly that path. b1nix refused the flag, so
`posix_spawn` answered `EINVAL` and no service on the machine could be started.

The child is now placed in the cgroup the descriptor names before the caller is
told its pid, so nothing can observe it in the wrong group. Only a cgroup2
directory is accepted, and it is identified by its `mkdir` hook rather than by
being a directory — a plain directory on some other filesystem would otherwise
have its inode data read as a cgroup pointer.

### 7. `pidfd_open(2)` and the pidfd family

`Failed to acquire PID reference on ourselves: Function not implemented`, and
then PID 1 sat in `epoll_wait` with no jobs for the rest of the boot.

A pid is a name that can be reused and a descriptor is not; systemd 254 and
later are built around that guarantee — its `PidRef` pairs a pid with a pidfd
and re-verifies one against the other — and it takes a pidfd on **itself**
before it will run a boot. Implemented: `pidfd_open(2)`, `pidfd_send_signal(2)`,
`waitid(P_PIDFD, …)`, poll readiness when the process has exited, a stable
unique `st_ino` from `fstat`, and the `Pid:` line in `/proc/<pid>/fdinfo/<n>`
that is how a caller reads a pidfd back into a pid at all.

`clone(CLONE_PIDFD)` and `clone3(CLONE_PIDFD)` hand the descriptor back through
`parent_tid` and the `pidfd` field respectively. And a child's death now wakes
`vfs_poll_chan`: a pidfd is readable exactly when the process it holds has
exited, and b1nix's poll re-examines its descriptors only when something wakes
that channel, so an `epoll_wait` on a pidfd slept through the death it was
waiting for.

### 8. `init=` could not name a script

`init=/some/script` reached the ELF loader directly and failed on the magic
number — `bad magic 23 21`, which is `#!` — and the machine came up with no
PID 1 at all, even though `execve(2)` had honoured `#!` for years. The parse is
shared between the two paths now, so they agree about what a script is.

The switchroot smoke instance boots through `/init-shebang`, a two-line file
whose interpreter line points at the same `/init` it always ran, so every M109
marker is also evidence that the interpreter line resolves.

### 9. A double free on every failed exec of a `#!` script

`execve()` builds a fresh argv for the interpreter before it knows whether the
interpreter is there. When it is not, the failure path freed that array and
then tested the same pointer and freed it again. Nothing is visible at the
moment it happens: the array goes onto the free list twice and the damage
surfaces in whatever allocation next receives it. `M46-SMOKE: ok
bad-shebang-exec` is a volume test — two hundred failed execs, then a few
hundred allocations — because a doubly-freed block is only a problem once it is
handed out again.

### 10. Smaller ones, in passing

- `close_range(2)` and `fchmodat2(2)` were missing; both are what a current
  glibc reaches for first, falling back only after paying for an `ENOSYS`.
- `connect(2)` on an AF_UNIX path with nothing at it answered `ECONNREFUSED`.
  Linux distinguishes it from `ENOENT` and callers act on the difference:
  refused means a service that is down and may come back, `ENOENT` means there
  is no such service on this system.
- `mount_setattr(2)` — see below.

## Where it stops

**The new mount API is implemented, and the CREDENTIALS wall is gone.**
journald now starts: it parses its configuration, reports "Collecting audit
messages is disabled" and runs. `fsopen`, `fsconfig`, `fsmount`, `open_tree`,
`move_mount` and `mount_setattr` all answer for real — see
`kernel/fs/mount_api.c` and the detached-mount table in `kernel/fs/vfs.c`.

The design question this file used to end on — "teaching the VFS to hold a
mount that is attached nowhere" — was answered by NOT adding a second mount
table. A detached mount is a real mount under a private directory
(`/.b1nix-detached/<id>`) that is hidden from `/proc/mounts` and mountinfo, and
`move_mount` is the MS_MOVE this kernel already had. Three consequences made it
the right shape rather than merely the cheap one:

- the descriptor `fsmount` returns is an ordinary O_PATH directory descriptor,
  so `openat(mfd, "cred", O_CREAT)` works. systemd fills a credentials
  directory that way *before* the mount has a place, and the first attempt at
  this — a descriptor with no path behind it — answered EBADF at exactly that
  step;
- every filesystem type can be instantiated, not only the ones whose mount
  callback ignores its target;
- unmount, propagation, mountinfo and namespace cloning keep working on it,
  because it is not a special case.

Four defects were found by walking the error one layer at a time, and each is
worth recording because none of them is guessable from the specification:

| Symptom | Cause |
|---|---|
| `CREDENTIALS ... Function not implemented` | the family was absent |
| `... Bad file descriptor` | the mount descriptor was an anonymous object; systemd uses it as a **dirfd** |
| `... Operation not supported` | `fsconfig(FSCONFIG_CMD_RECONFIGURE)` was refused. systemd creates the tmpfs, writes into it, sets `ro` and reconfigures — the refusal was one call from the end |
| `... Invalid argument` | after `fsmount`, the fs context dropped its reference to the filesystem, so the reconfigure that follows had nothing to act on. In Linux the context keeps referring to the superblock; ownership and reference are different things |

systemd also **probes**: it calls `fsconfig` with a deliberately absent option
(`adefinitelynotexistingmountoption`) to find out whether the kernel validates
options at all. An implementation that accepts everything tells it the wrong
thing about every option afterwards, so an unknown parameter is EINVAL here and
only a filesystem's own hints (`size`, `mode`, `nr_inodes`, …) are accepted.

### PID 1's standard descriptors

Found while chasing the abort below, and a real difference from Linux rather
than a systemd quirk: **b1nix started its first process with an empty
descriptor table.** Linux's `kernel_init` opens `/dev/console` and dups it onto
0, 1 and 2, and every process inherits its stdio from there.

The consequence is not "no output". A manager's stdio `FILE*` refers to
descriptor 0 whether or not anything is open on it, so the first transient
descriptor the process opens lands ON 0 — the `b1nix.trace-fd=0` trace shows
every `openat` in PID 1 returning 0, over and over — and the next `close(2)` of
that transient silently closes what libc still calls stdin.

PID 1 now gets `/dev/console` on 0, 1 and 2. The trace confirms it arrives:
systemd's first act on descriptor 0 is a `dup2` over it, which is what a
manager does with a console it has been given.

### What stops the boot now

Nothing. The boot completes and every check passes.

### How the boot got the rest of the way

Each of these was found by following one error at a time, and none of them is
guessable from the specification:

| What systemd reported | What it actually was |
|---|---|
| `Failed to spawn executor: Inappropriate ioctl for device` | clone3 wrote the CLONE_PIDFD descriptor only on its fork-like path, so a posix_spawn (which passes a stack) got nothing written and systemd used descriptor 0 — its own stdin — as the child's pidfd |
| `... Bad file descriptor` | ioctl on an anonymous object answered EBADF, which says "that is not a descriptor" about one the caller holds |
| `... Object is remote` | a pidfd's inode numbered the DESCRIPTOR; on Linux it identifies the PROCESS, so two pidfds for one process compare equal |
| `Failed to set up mount namespacing: /proc/sys/kernel/domainname: Not a directory` | open_tree forced O_DIRECTORY; a file is the ordinary case, since every ReadOnlyPaths= entry is a file bound over a file |
| `Failed to set up mount namespacing: /dev: Invalid argument` | mount_setattr on a path that is not a mount root refused, where Linux applies the attributes to the mount the path is ON |
| `Failed to create SIGTERM event source: File exists` | an epoll registration was keyed by descriptor NUMBER rather than by the file behind it, so a watch left by a closed descriptor blocked the next one to reuse the number |
| `ERROR sockopt_get_peersec: Invalid argument` | getsockopt's kernel buffer was a fixed 64 bytes; anything larger was refused before the option code ran |
| `ERROR socket_dispatch_write: Inappropriate ioctl for device` | SIOCOUTQ was unimplemented, and dbus-broker asks on every write |
| `StandardOutputFileDescriptor passed is of incompatible type` | fcntl(F_GETFL) reported O_RDONLY for BOTH ends of a pipe, so a pipe handed over as stdout read as unwritable |
| `Failed to touch /run/udev/queue: Not a directory` | `utimensat(fd, "", AT_EMPTY_PATH)` — how systemd spells futimens on an O_PATH descriptor — was treated as a relative path and joined onto the file's own, producing a trailing slash. udevd hit it after every event and never finished a run, which is why no `.device` unit ever appeared |
| `Assertion 'errno != EBADF \|\| IN_SET(fd, STDIN, STDOUT, STDERR)' failed ... isatty_safe()` | ioctl on a PIPE answered EBADF, and isatty(3) is an ioctl. glibc reads ENOTTY as "not a terminal" and EBADF as "that descriptor is not open"; systemd asserts on the difference, so a pipe handed to a service as its stdio aborted the service the moment it asked whether it was a terminal. Every unit started with `systemd-run --pipe` died on it |

The descriptor relay behind the last one is worth recording because it was
cleared as a suspect rather than guessed at: `b1nix.trace-scm` showed
`systemd-run` sending three descriptors, dbus-broker receiving and installing
three, and PID 1 receiving and installing three. The passing worked; the
service died after it had them.

## Open

- **The credentials mount API above.** This is the single thing between Arch's
  boot and a running journald.
- **`fsopen(2)`, `fsconfig(2)`, `fsmount(2)`, `open_tree(2)`, `move_mount(2)`**
  all answer `ENOSYS`, deliberately.
- **`systemd-udevd` never starts**, so no `.device` unit ever activates on the
  Arch image. It fails at the same `CREDENTIALS` step; nothing suggests a
  separate cause, but nothing has proved there is not one either.
- **`/tmp` does not mount**: `tmp.mount` asks for `x-systemd.graceful-option=usrquota`,
  cannot determine availability without `fsopen`, and then fails. Same root.
- **`Failed at step CGROUP spawning /usr/bin/mount: No such file or directory`**
  appears on the final tree and did not on the intermediate one described
  above. It is not diagnosed.
- **`bpf(2)` (nr 321)** is unimplemented; systemd reports IP firewalling and
  BPF-LSM as unsupported and continues, which is correct behaviour.
- **`Detected virtualization container-other`**: systemd concludes this machine
  is a container. Nothing has been traced to establish why, and it changes
  which paths systemd takes, so it is worth knowing before reading any of its
  decisions.
- **The graphics profile is unexercised.** `PROFILE=graphics` builds an image
  with Weston 15 and `tests/arch-smoke.sh` has no graphics counterpart yet:
  there is no point photographing a scanout on a machine whose display stack
  cannot start until the credentials step works.
