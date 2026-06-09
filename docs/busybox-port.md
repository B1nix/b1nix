# Porting Upstream BusyBox 1.36.1 to b1nix (Stage 1)

This document describes the initial phase of porting upstream BusyBox 1.36.1
as a static, multicall ELF to the `b1nix` operating system.

The upstream binary is intentionally kept separate from the native b1nix
utilities. It is installed as `/opt/busybox/bin/busybox` and does not create
links in `/bin`.

Build the standalone package with:

```sh
make ARCH=x86_64 busybox-package
```

Embed and test it without replacing native commands with:

```sh
UPSTREAM_BUSYBOX=1 make ARCH=x86_64 smoke
```

## Enabled Applets

The isolated package currently configures **86 applets**. Migration wave 1
added `cmp`, `cut`, `env`, `id`, `ls`, `printenv`, `tee`, `tr`, `whoami`,
`seq`, `which`, `clear` and `hexdump`. Wave 2 added `stat`, `realpath`,
`mktemp`, `find`, `grep`, `sed`, `awk`, `xargs`, `diff`, `cksum`, `md5sum`
and `sha256sum`. Wave 2b added `dd`, `du`, `df`, `tar`, `gzip`, `gunzip`,
`bzip2`, `bunzip2`, `unxz` and `xzcat`. Wave 3 added `ps`, `top`, `free`,
`uptime`, `pidof`, `pgrep`, `pkill` and `dmesg`. Wave 4 added `mount`,
`umount`, `nslookup`, `lsof`, `netstat`, `route`, `ifconfig`, `blkid` and
`fdisk`. Wave 4b added `ping`, `losetup` and `ip`:

- **Logic & Flow Control**: `true`, `false`, `yes`
- **Output & Print**: `echo`, `printf`, `pwd`, `printenv`, `tee`
- **File Utilities**: `cat`, `head`, `tail`, `wc`, `mkdir`, `rmdir`, `rm`, `cp`, `mv`, `ln`, `readlink`, `touch`, `chmod`, `chown`, `ls`, `cmp`, `cut`, `stat`, `mktemp`, `find`, `diff`, `dd`
- **Path Manipulation**: `basename`, `dirname`, `realpath`
- **System Utilities**: `sync`, `sleep`, `date`, `uname`, `kill`, `id`, `whoami`, `env`, `which`, `clear`, `hexdump`, `du`, `df`
- **Process & Diagnostics**: `ps`, `top`, `free`, `uptime`, `pidof`, `pgrep`, `pkill`, `dmesg`, `lsof`
- **Storage & Mount**: `mount`, `umount`, `blkid`, `fdisk`
- **Networking**: `nslookup`, `netstat`, `route`, `ifconfig`
- **Archive & Compression**: `tar`, `gzip`, `gunzip`, `bzip2`, `bunzip2`, `unxz`, `xzcat`
- **Text & Sequences**: `tr`, `seq`, `grep`, `sed`, `awk`, `xargs`
- **Checksums**: `cksum`, `md5sum`, `sha256sum`
- **Shell Builtins & Pipelines**: `test`, `[` (alias of test), `sort`, `uniq`

`tar`, `gzip`/`bzip2` compress and decompress; `xz` is **decompress-only**
(`unxz`/`xzcat`) because upstream BusyBox ships no xz compressor — `tar -J`
create is therefore unavailable, while `tar -z` (gzip) create/extract and
`xz` extraction both work. `df` reads `/proc/mounts`, which was added to the
kernel procfs for this wave.

## What b1nix Already Provides

The port does not start from a minimal kernel. The following foundations are
already implemented and smoke-tested on both `x86_64` and `i686`:

- Processes: `fork`, `execve`, `waitpid`, credentials, supplementary groups,
  sessions and process groups.
- Signals: handlers, masks, delivery, process-group signals and job-control
  stop signals.
- Terminal support: console termios, PTYs, `openpty`, `forkpty`, controlling
  PTYs, foreground process groups and window-size ioctls.
- VFS: files, directories, hard links, symbolic links, rename, permissions,
  ownership, timestamps, file locking, `statfs`, mounts and unmounts.
- Filesystems: initramfs, ext2, ext3, ext4, FAT32, exFAT, ISO9660 and partial
  Btrfs support.
- Networking: IPv4, IPv6, TCP, UDP, Unix sockets, DNS, DHCP, NDP, `poll` and
  `select`.
- Runtime state: `/proc` process directories, memory/CPU information and a
  small `/sys` tree.
- Storage: block-device registry, MBR/GPT partition discovery, loop devices,
  AHCI, NVMe and virtio block devices.

Therefore the remaining work is not "implement the Linux ABI". BusyBox needs
specific missing contracts exposed through normal libc/POSIX interfaces or,
where the operation is inherently OS-specific, through a small b1nix BusyBox
backend.

## Concrete Remaining Interfaces

### Completed for migration waves 1 and 2

- Added builtin `alloca`, `strsep`, `getgrouplist`, `endgrent`, `hstrerror`,
  `fseeko` and `ftello` declarations and implementations.
- Added `getsid()` across the userspace ABI, syscall dispatcher and scheduler.
- Made `off_t` match the architecture ABI while remaining 64-bit.
- Marked `longjmp()` as `noreturn`, matching its implementation and removing
  the upstream `test` applet warning.
- Updated the kernel/userspace `struct stat` ABI to expose POSIX `st_atim`,
  `st_mtim` and `st_ctim` timestamps while retaining the traditional
  `st_atime`, `st_mtime` and `st_ctime` aliases.
- Added `strcasestr`, `mkdtemp`, `popen` and `pclose`, and made `rand()` use
  the declared 31-bit `RAND_MAX`.
- Added a compact libc BRE/ERE engine with literals, groups, alternation,
  bracket and named character classes, anchors, backreferences, `*`, `+`,
  `?`, captures, `REG_ICASE` and `REG_NEWLINE`. This is sufficient for the
  enabled Wave 2 workflows but is not yet full POSIX regex conformance.

### Required libc/POSIX semantics

- Implement atomic `sigsuspend()` in the scheduler. The current libc correctly
  returns `ENOSYS`; `ash` must not be enabled until this is real.
- Implement `alarm()`; the current function is a no-op and cannot deliver
  `SIGALRM`.
- Implement `getrlimit()` and make `setrlimit()` report actual state instead of
  an unconditional success.
- Replace the remaining placeholder `dup()`, `access()` and `ftruncate()`
  implementations with fd-table, VFS permission and truncate operations.
  (`isatty()` is done — it now queries `tcgetattr`, so a redirected stdio fd is
  correctly reported as a non-tty; this was required for the wave 2b
  gzip/bzip2/xz applets.)
- Complete `fnmatch()` support for `FNM_PATHNAME`, `FNM_PERIOD` and bracket
  expressions.
- Complete POSIX regex intervals (`{m,n}`), error reporting and edge-case
  semantics beyond the Wave 2 BRE/ERE subset.
- Complete `utimensat`, `futimens` and `lchown`, including symlink and
  nanosecond semantics.
- Implement `fchdir`, `mknod` and `chroot`.
- Add userspace wrappers and headers for `mount`, `umount`, mount enumeration
  and mount flags. The kernel syscalls already exist.
- Add `getopt_long` only for external software compatibility. BusyBox itself
  primarily uses its internal `getopt32`.
- Replace the no-op syslog functions with `/dev/log`, a kernel log interface,
  or an explicitly documented b1nix logging backend.

### Required kernel-visible state

- Extend `/proc/<pid>/stat` to the field layout expected by upstream `ps` and
  `top`; the current file only exposes pid, name, state and parent pid.
- Add `/proc/<pid>/fd` and the required `/proc/net/*` files. (`/proc/mounts`
  is implemented — it backs upstream `df` in wave 2b.)
- Extend `/sys/class/block` with device name, size, partition relationship and
  read-only state.
- Extend `/sys/class/net` with interface name, flags, MTU, MAC and addresses.
- Expose block size/capacity through a userspace ioctl or a b1nix-specific
  query syscall. The information already exists in `struct block_device`.
- Add loop-device attach/detach/status operations for upstream `losetup`.
- Add interface enumeration/configuration operations for `ifconfig`, `ip` and
  `route`. The current `struct ifreq` header exists, but socket ioctls do not.
- Add raw ICMP sockets, or adapt upstream `ping` to `SYS_NET_PING`. TCP and UDP
  sockets are already sufficient for `nc` and most of `wget`.
- Complete controlling-terminal ownership for the physical console. PTYs
  support `TIOCSCTTY`, but the console path currently relaxes the POSIX session
  check and does not implement terminal reassignment.

## Replacement Plan

The replacement target is the command set currently dispatched by
`kernel/user/busybox.c`, not every optional BusyBox applet.

### Wave 1: low-risk utilities

Enable and test:

`ls`, `whoami`, `id`, `clear`, `env`, `printenv`, `cut`, `tr`, `tee`,
`cmp`, `seq`, `which` and `hexdump`.

Most kernel support already exists. `hexdump` first needs `fseeko`; identity
applets need the passwd/group API to be warning-clean.

### Wave 2: text and traversal

Enabled and smoke-tested:

`stat`, `realpath`, `mktemp`, `grep`, `find`, `sed`, `awk`, `xargs`, `diff`,
`cksum`, `md5sum` and `sha256sum`.

The applets remain isolated under `/opt`; recursive traversal, regular
expressions, substitutions, field processing, argument batching, checksums and
error exit statuses are covered by the optional BusyBox smoke suite. The full
suite, including SMP, passes 425/425 on both `x86_64` and `i686`.

### Wave 2b: larger file and archive utilities (done)

Enabled and smoke-tested: `dd`, `du`, `df`, `tar`, `gzip`, `gunzip`,
`bzip2`, `bunzip2`, `unxz` and `xzcat`. Coverage (`BB-W2B:` markers in the
posix smoke): byte-exact `dd` copy, `du` block accounting, `df` against the
new `/proc/mounts`, `tar` create/extract round trip, `tar -z` seamless gzip,
`gzip`/`gunzip` and `bzip2`/`bunzip2` round trips, `xz` decompression of an
embedded fixture, and a malformed-input negative test (`gunzip` on non-gzip
data returns nonzero). `xz` is decompress-only (no upstream xz compressor),
so `tar -J` create is unavailable. Kernel change: added `/proc/mounts`.

### Migration wave 3: process & system inspection (done)

Enabled and smoke-tested: `ps`, `top`, `free`, `uptime`, `pidof`, `pgrep`,
`pkill` (procps) and `dmesg` (util-linux). Coverage is the `BB-W3:` markers in
the posix smoke: `ps`/`top` enumerate `/proc`, `uptime` prints the load line
from `sysinfo()` + `/proc/uptime`/`/proc/loadavg`, `free` reports memory from
`sysinfo()` + `/proc/meminfo`, `dmesg` drains the kernel ring buffer, and
`pidof`/`pgrep`/`pkill` find and signal a backgrounded process by name.
Verified: `x86_64` **443/0** (full suite, single-CPU + `-smp 4`); `i686` all
eight `BB-W3:` markers green, suite at **442/1** — the only failure is the
pre-existing, i686-only `M37 e1000` ARP-receive-over-SLIRP timing test (green
on `x86_64`, introduced before this branch, untouched by W3 code).
Kernel/libc/build changes this wave needed:

- **`/proc/<pid>/stat`** grew from 4 fields to the full 24-field Linux layout
  BusyBox procps parses (state is a single `%c`; b1nix has no per-task CPU
  time/start ticks yet, so utime/stime/starttime are 0; vsize = heap span,
  rss = page count).
- **Process "comm" is now the executable basename**, not the full exec path
  truncated to 15 chars. `sys_spawn` truncated `path` directly, so
  `/opt/busybox/bin/busybox` became comm `/opt/busybox/bi` (basename `bi`) and
  `pidof`/`pgrep`/`pkill` could not match by name. It now takes the basename
  first; `/proc/<pid>/{stat,comm,status}` expose it (matching Linux's
  `TASK_COMM_LEN`).
- New **`SYS_SYSINFO`** syscall + `struct sysinfo` (`<sys/sysinfo.h>`) and a
  `sysinfo()` wrapper, filling totalram/freeram/procs/mem_unit from the PMM
  (`mem_unit = 1` byte). New **`klogctl()`** (`<sys/klog.h>`) mapping syslog(2)
  read actions onto `SYS_DMESG`. New **`SYS_GETPPID`** + `getppid()`, and a
  `usleep()` wrapper (via `nanosleep`).
- **Build:** the cross GCC predefines `__b1nix__`/`__unix__`, not `__linux__`,
  so procps `free`/`uptime`/`ps` skipped `<sys/sysinfo.h>`. `build-busybox.sh`
  idempotently widens those three include guards to also fire for `__b1nix__`.

`lsof` (needs a `/proc/<pid>/fd/` dynamic dir of readlink-able fd symlinks) is
deferred to a follow-up sub-wave.

### Migration wave 4: storage & networking (done)

Enabled and smoke-tested (`BB-W4:` markers), green on **both** arches (`x86_64`
and `i686`): `mount`, `umount`, `nslookup`, `lsof`, `netstat`, `route`,
`ifconfig`, `blkid`, `fdisk`. Coverage: a `mount`/`umount` round trip on
`sata0`; `nslookup` of a numeric address (deterministic, no live DNS);
`netstat -tln` finding the dropbear `:22` listener via `/proc/net/tcp`;
`route -n` showing the on-link `10.0.2.0/24` route; `ifconfig eth0` reading the
DHCP `10.0.2.15` via SIOCGIF* ioctls; `blkid /dev/sata0` identifying ext4;
`fdisk -l /dev/sata0` reading geometry via the BLK* ioctls; `lsof` listing open
files from `/proc/<pid>/fd/`.

Kernel/libc infrastructure this wave added:

- **`/proc/net/{tcp,tcp6,udp,unix}`** from the kernel socket tables, and
  **`/proc/net/route`** — netstat/route parse these. The scanf engine gained
  the **`%[...]` scanset** conversion they rely on (without it netstat reported
  "bogus data" and route a "read error").
- **`socket_file_ops.ioctl`** serving the `SIOCGIF*` interface queries for a
  single modelled `eth0` (ifconfig); `SIOCADDRT/DELRT` accepted as no-ops.
- **`/dev/<name>` block-device nodes** (byte-addressed cached read/write +
  read-modify-write) with the **BLK\* size ioctls** and **`/proc/partitions`**
  — blkid and fdisk read through these.
- **`/proc/<pid>/fd/`** fd symlinks (lsof); `<sys/mount.h>` + `mount()`/
  `umount()`; minimal `<resolv.h>` + `res_init()`; `getservbyport`, `strnlen`,
  `if_nametoindex`, `_IOC`/`_IOR` macros, `caddr_t`; headers `net/route.h`,
  `net/if.h` (full Linux `ifreq`), `net/if_arp.h`, `net/ethernet.h`.

`lsblk` is **not shipped by upstream BusyBox 1.36** — `blkid` and `fdisk -l`
cover the block-inspection role.

### Migration wave 4b: ping / losetup / ip (done)

Enabled and smoke-tested (`BB-W4B:` markers), green on **both** arches
(`x86_64` and `i686`): `ping`, `losetup`, `ip`. Each required a distinct new
kernel subsystem:

- **`ping`** — raw `SOCK_RAW`/ICMP sockets. A raw-socket registry in
  `kernel/net/socket.c` (`raw_sock_register/unregister`); `icmp_receive()`
  delivers every echo reply to registered raw sockets via
  `vfs_socket_push_raw_icmp()`, prepending a synthetic 20-byte IPv4 header.
  libc `recvfrom()` recovers the peer from that header; `sendto()` records the
  per-packet destination as the socket's connected peer.
- **`losetup`** — a loop-device ioctl surface. `kernel/dev/loop.c` registers 8
  `/dev/loopN` block devices plus `/dev/loop-control`, answering
  LOOP_CTL_GET_FREE, LOOP_SET_FD/CLR_FD and the status ioctls. `vfs_ioctl()`
  routes the `0x4C` ioctl group (and the `loop-control` node) to the loop
  handler **before** the `!arg` guard, since LOOP_CTL_GET_FREE takes no arg.
- **`ip`** — BusyBox `ip` speaks **rtnetlink** exclusively, so it needs an
  `AF_NETLINK` socket personality. `netlink_build_dump()` encodes
  RTM_NEWLINK/NEWADDR/NEWROUTE responses (nlmsghdr + rtattr TLVs) terminated by
  NLMSG_DONE for a single modelled `eth0`; `vfs_socket()`/`vfs_bind()` accept
  `AF_NETLINK`, and `getsockname()` reports `nl_pid 0`. libc `sendmsg`/`recvmsg`
  gained single-iov scatter-gather; **`recvmsg` zeroes `msg_name`** so the
  source `nl_pid` reads as 0 — BusyBox libnetlink rejects (and would hang on)
  any reply whose recvmsg source `nl_pid` is non-zero.

Headers this wave added: `linux/{netlink,rtnetlink,if_vlan,if_arp,neighbour,
loop,version,types}.h`, `asm/types.h`, `netinet/{ip,ip_icmp,if_ether}.h`,
`netpacket/packet.h`, `resolv.h`; `PF_PACKET`, `SIOCSIFHWBROADCAST`,
`RTNH_F_*`/`RTAX_*` constants.

### Wave 3: upstream `ash`

The existing shell proves that pipes, redirection, `fork`/`exec`, process
groups, PTYs and job-control signals work. Remaining blockers are:

- atomic `sigsuspend`;
- `getsid` and complete session validation;
- physical-console `TIOCSCTTY`;
- real resource-limit reporting;
- focused tests for `SIGCHLD`, stopped jobs, orphaned groups and interrupted
  waits.

Install `ash` as `/opt/busybox/bin/ash` first. Do not replace `/bin/sh` until
the complete POSIX and SSH PTY suites pass with it.

### Wave 4: process and diagnostics

**Mostly done** — see "Migration wave 3: process & system inspection (done)"
above. `ps`, `top`, `free`, `uptime`, `dmesg`, `pidof`, `pgrep` and `pkill` are
enabled and smoke-tested; `kill` already shipped in wave 1. The expanded
`/proc/<pid>/stat` layout, basename `comm`, `SYS_SYSINFO`/`sysinfo()` and
`klogctl()` that these needed are in place.

Still to do:

- `lsof` — needs a `/proc/<pid>/fd/` dynamic dir of readlink-able fd symlinks.
- `killall` — name-based bulk kill (the basename `comm` work makes this
  straightforward now).
- `sysctl` — read-only keys can be ported first; writable `sysctl` requires an
  explicit b1nix policy.

### Completed storage applets

Delivered across migration waves 4 and 4b:

`mount`, `umount`, `df`, `blkid`, `fdisk` and `losetup`.

These use the existing b1nix mount and block-device APIs, raw block-device I/O,
geometry ioctls and the loop-device interface. BusyBox 1.36 does not ship an
`lsblk` applet; `blkid` and `fdisk -l` cover the inspection role.

### Completed networking applets

Migration waves 4 and 4b delivered:

- interface query/configuration for `ifconfig`;
- route and address operations for `ip` and `route`;
- raw ICMP or a b1nix backend for `ping`;
- `/proc/net` data for `netstat`.

### Privileged login and account applets

Enable:

`su`, `login`, `getty`, `passwd`, account-management applets, `reboot`,
`poweroff`, `halt`, `chroot` and `mknod`.

Credentials, passwd/shadow parsing and reboot commands already exist.
`chroot` and device-node creation are still missing. BusyBox `init` and `mdev`
are intentionally excluded: B1NIX keeps its own `/bin/init` as PID 1 and will
develop `/etc/inittab`, runlevels, respawn policy and service supervision in
M39. BusyBox `getty` and `login` may be launched by that native init without
transferring ownership of the boot process to BusyBox.

### Migration wave 5: shell, login and accounts

This migration wave combines the earlier upstream-`ash` and privileged
login/account work:

- **`ash` is enabled and `/bin/sh` now points at it** (in progress). BusyBox is
  configured `CONFIG_SH_IS_ASH=y`; `/bin/sh` is a symlink to
  `/opt/busybox/bin/busybox` under `B1NIX_UPSTREAM_BUSYBOX`. The `BB-W5:` smoke
  markers cover applet listing, `-c`, variables + `test`, arithmetic, pipes,
  redirection and child wait, and the interactive `M32B-SSH: ok pty` test runs a
  command through `ash` over a remote PTY. Bringing it up exposed and fixed a
  latent kernel gap — `/dev/tty` had no `poll_cb`, so the first `poll()` on the
  console (which the old builtin shell never issued but `ash`'s line editor does)
  jumped through an uninitialised pointer and panicked. Fixed with `tty_poll`
  (`kernel/fs/vfs.c`) + `ps2_kbd_has_data()` (`kernel/dev/ps2_kbd.c`). Two more
  real fixes followed: a genuine **`/dev/null`** node (the builtin shell faked
  `2>/dev/null`; `ash` really opens it) and **`kill(pid,0)` existence-probe
  semantics** (the kernel rejected `sig 0`, breaking the sshd `status` arm's
  `kill -0`). `/bin/dropbear` was also **rebuilt** against the current libc (it
  predated the `4ab0fd3` `sa_restorer` fix and was crashing on SIGCHLD; dropbear
  builds with clang via `tools/b1nix-autotools-cc`, so no cross-GCC is needed).
  Upstream-BusyBox smoke went **197/283 → 478/2** on x86 and x86_64; `BB-W5` and
  `M32B-SSH` `dropbearkey`/`handshake`/`negauth`/`pty`/`service-status` are green.
- **One remaining SSH red — `M32B-SSH: service-lifecycle`:** after `stop`, the
  daemon is a zombie that nothing reaps (orphans are not reparented on parent
  exit, and test-mode `/bin/init` is the smoke driver, not a general reaper), so
  the test's `kill(pid,0)` "process gone" check fails. This was masked before by
  the broken `kill -0`. Tracked as `planned` in `docs/roadmap.md` (needs SMP-safe
  orphan reparenting + a real zombie reaper). A kernel-injected `sigreturn`
  trampoline (so delivery never trusts a userspace `sa_restorer`) is also tracked
  there as a forward-compatible hardening.
- enable `getty`, `login`, `su`, `passwd` and account-management applets behind
  a security-sensitive test gate (not yet done);
- leave `/bin/init`, `/etc/rc` and PID 1 service supervision owned by B1NIX.

BusyBox `init` is not a migration target. Replacing the current shell or login
commands remains independently reversible, while the native init evolves
separately according to M39 in `docs/roadmap.md`.

## Applet Promotion Rule

Track every upstream applet through these states:

1. `builds-warning-clean`
2. `runs-from-/opt`
3. `behavior-smoke-passed`
4. `error-paths-passed`
5. `x86_64-and-i686-passed`
6. `full-suite-and-SMP-passed`
7. `replaces-native`

Only the final state may create a `/bin/<applet>` link. Keep the native
implementation registered until the upstream applet has passed the full suite;
this makes every replacement independently reversible.

## Initial Libc Compatibility Work

Stage 1 added the following headers and functions. Items described as partial
below are sufficient for the currently enabled applets but are not considered
complete compatibility contracts.

### New Header Files
- `byteswap.h`, `endian.h`, `features.h`, `fnmatch.h`, `libgen.h`, `malloc.h`, `paths.h`, `poll.h`, `regex.h`
- `sys/sysmacros.h`, `sys/utsname.h`
- `net/if.h`

### Implemented Functions in `libb1nix`
- **String Manipulation**: `stpcpy`, `strndup`, `strchrnul`, `mempcpy`
- **I/O & Formatting**: `getline`, `vasprintf`, `asprintf`, `vdprintf`, `dprintf`
- **Temporary Files**: `mkstemp`, `mkdtemp`
- **Path Resolution & Links**: `symlink`, `readlink`, `libgen.h` functions
  (`basename`, `dirname`), and partial `fnmatch`
- **System Information**: `uname`, `sysconf` (for `_SC_CLK_TCK`)
- **Process & Signals**: `vfork` (implemented via `fork`)
- **File Status & Times**: `utimes`, plus partial `utimensat` and `lchown`
- **Multiplexing**: `poll`
- **Time Utilities**: `gmtime_r`, `localtime_r`, `strptime`
- **Fallback Stubs**: `mknod` (returns `ENOSYS`), `clock_settime` (returns `EPERM`), `futimens` (returns `ENOSYS`), `fchdir` (returns `ENOSYS`), `chroot` (returns `EPERM`), `settimeofday` (returns `EPERM`)
- **Option Parsing**: short-option `getopt` with grouped options and BusyBox's
  `optind = 0` reset convention. GNU permutation, optional arguments and
  `getopt_long` are not implemented.
- **Regex**: libc `regcomp`, `regexec`, `regerror` and `regfree` with the
  Wave 2 BRE/ERE subset described above.
- **Process I/O**: `popen` and `pclose` through `/bin/sh -c`.

## Key Integration and Bug Fixes

1. **getopt reset**: BusyBox resets the option parser using `optind = 0`. The custom libc `getopt` implementation was updated to handle `optind == 0` correctly (resetting internal state and setting `optind = 1`), preventing command-line arguments from getting mismatched.
2. **Trailing slash support in VFS**: `split_parent_path` in `kernel/fs/vfs.c` was updated to strip trailing slashes (except for `/`), allowing recursive creation of parent directories via BusyBox's `mkdir -p` (which internally executes `mkdir` calls on paths like `/tmp/`).

Native commands should be replaced one applet at a time only after the
upstream implementation passes the complete regression suite. Until the kernel
can atomically replace a signal mask and wait, `sigsuspend()` returns `ENOSYS`
instead of pretending to provide POSIX semantics.
