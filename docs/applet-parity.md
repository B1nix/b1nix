# Alpine applet parity — and the kernel subsystems it needed

Milestones **M107** (applets blocked on missing kernel subsystems), **M108**
(base tools handed to BusyBox) and **M109** (parity itself). The measure is not
how many applets build but how many *work*: every one is exercised through
`/bin` in the smoke suite.

**283 of Alpine's 321 applets** are built and proved.

## What the kernel had to grow first (M107)

- **Netlink route sockets**: link, address, route and neighbour queries
  (`ip`, `route`, `arp`, `netstat`).
- **Virtual terminals**, console fonts and keymaps (`chvt`, `openvt`,
  `deallocvt`, `setfont`, `loadkmap`, `dumpkmap`).
- **Loop devices** with offsets, size limits and a write path (`losetup`).
- **`/proc` extended**: full paths in `fd/`, named map regions, per-process
  memory (`lsof`, `fuser`, `pmap`).
- **A structured kernel log ring** with syslog, `/dev/kmsg` and `/proc/kmsg`
  — every reader gets its own cursor (`syslogd`, `klogd`, `logread`).
- **inotify** move cookies, attribute changes and self deletion (`inotifyd`).
- **RTC** read and write plus watchdog ioctls with a real reset deadline.
- **SMBus i2c** on the host controller, reporting what it cannot do.
- Block-backed device nodes carry `S_IFBLK`, `SIOCGIF*` honours the interface
  name, IPv6 neighbours and interface up/down state are administered for real.

## Base tools handed to BusyBox (M108)

- `su`, `passwd`, `login`, `id`, `whoami` and `groups` are BusyBox applets; the
  local ELFs are deleted.
- The setuid bit lives on a *second* copy of the multicall binary
  (`/opt/busybox/bin/busybox-suid`, mode 4755 root), reachable by exactly three
  names — the `BB_SUID_REQUIRE` applets `su`, `passwd` and `login`. With
  `CONFIG_FEATURE_SUID=y`, libbb's `check_suid()` drops euid for every other
  applet, so even a hand-made symlink from `sh` to the setuid copy yields no
  privilege.
- One shadow format is written and read end to end: SHA-512, shared with PAM.
- BusyBox init is PID 1 from `/sbin/init`, with OpenRC driving the runlevels;
  the inittab getty respawn is exercised by killing it and requiring PID 1 to
  replace it. Three forged init markers were deleted from the smoke hook so the
  real paths report.
- `execve` publishes post-exec credentials in the auxv and refreshes
  capabilities and fsuid.
- **`/etc/shadow` locking under concurrent password changes**, confirmed and
  then fixed: BusyBox `update_passwd` opened the file *before* taking its
  `<file>+` `O_EXCL` lock, so a losing racer rewrote a stale snapshot over the
  winner's update — and both exited 0. The read now happens under the lock
  (`tools/patches/busybox/b1nix-config.sh`). The kernel primitives it rests on
  were already sound and are now proved: four simultaneous `passwd` runs all
  land, and `M108-SMOKE: ok shadow-lock-excl` shows `O_CREAT|O_EXCL` admitting
  exactly one racer per round and `F_SETLK` blocking, naming and releasing its
  holder.

## What parity itself needed (M109)

- `/dev/zero`, `/dev/urandom` and `/dev/random`, unblocking `shred`, `who` and
  `cpio`.
- **`AF_PACKET`** (`kernel/net/packet.c`): SOCK_RAW and SOCK_DGRAM, bound to an
  interface and/or ethertype, with taps on both the receive and the transmit
  path so a socket sees its own outgoing frames the way `tcpdump` does. Gated on
  CAP_NET_RAW.
- **`pivot_root(2)` and `mount(MS_MOVE)`**: a move retargets a mount and every
  mount nested in it, so `umount` and `/proc/mounts` follow the tree, and an
  initramfs boot hands the machine over to the real root (`switchroot` smoke
  instance).
- **Filesystem UUID and label probing** (`blk_probe_uuid`/`blk_probe_label`) for
  ext2/3/4, FAT and exFAT, exposed at `/sys/block/<dev>/{uuid,label,fstype}` and
  used by `root=UUID=`, `findfs` and `blkid`. Readdir merges a directory's
  in-memory children over its on-disk entries, so `/dev` lists the nodes those
  tools scan for.
- **Virtual network devices**: 802.1Q VLAN, a learning bridge, active-backup
  bonding and gretap tunnels — ipip carries no ethernet header, so it does not
  fit the device model.
- **Namespaces**, all four kinds — see [`namespaces.md`](namespaces.md).
- **A uevent channel for `mdev`** (`kernel/dev/uevent.c`):
  `NETLINK_KOBJECT_UEVENT` messages on device registration and removal, and a
  `/sys/dev/block` tree carrying `dev`/`uevent`, so `mdev -s` populates `/dev`
  and `mdev -d` maintains it against a device that appears after boot.
- **Inode attribute flags** for `chattr`/`lsattr`: `FS_IOC_GET/SETFLAGS` over
  ext4's on-disk `i_flags`, with immutable and append-only enforced in the
  write, truncate, rename and unlink paths (the other six are stored only).
- **Discard** for `blkdiscard` and `fstrim`: `BLKDISCARD`/`BLKZEROOUT` down to
  virtio-blk DISCARD, NVMe DSM Deallocate and ATA TRIM, plus a `FITRIM` walk of
  a mounted ext4's free bitmaps. No command on the device, no pretending —
  `EOPNOTSUPP`, never a software fallback that writes the I/O it saves.
- **I/O priorities** for `ionice`: `ioprio_set`/`ioprio_get` drive the block
  layer's admission gate, which hands a busy device to the best-priority waiter
  and ages waiters so no class starves another.

## M114 — the layers built for the applets that had none

Counted from the applet tables inside the two binaries rather than from build
options: Alpine 3.20's BusyBox carries **304** applets, ours carried **287**,
and 33 of the difference were absent here. Most were missing because the kernel
had nothing under them, so the subsystems were written and the applets enabled.
Ours now carries **295**.

| applet | what had to exist first |
|---|---|
| `nsenter`, `unshare` | nothing new — the syscalls and `/proc/<pid>/ns/*` were already there (M109) |
| `readahead` | `readahead(2)`, a genuine prefetch into the cache |
| `setconsole` | `TIOCCONS`, kernel console output redirected to a terminal |
| `raidautorun` | software RAID: striping and mirroring as a block device |
| `nbd-client` | a network block device over the existing TCP stack |
| `eject`, `volname` | the ATAPI packet read path, so a CD-ROM is a block device |
| `flash_eraseall`, `flashcp` | MTD over CFI NOR flash, which QEMU gives x86_64 through `-drive if=pflash` |

Three of these are worth their own note.

**The console cannot write to a device from where it prints.** `console_write`
runs under a spinlock with interrupts disabled and is called from interrupt
handlers and from the panic path; a device write may sleep. So the console
pushes bytes into a ring and returns, and a kernel thread drains the ring into
the terminal. Output arrives a moment late. If the ring overflows the loss is
reported rather than swallowed.

**The RAID superblock is ours, and is not Linux's.** There is no mdadm here to
write a Linux superblock and none to verify one against, so claiming that
format would be a promise no test could keep. `/bin/mdcreate` writes the
documented b1nix format; `raidautorun` assembles from it. A mirror has no
resynchronisation: a member that fails stays failed until the array is stopped,
because a member that silently rejoined would be serving stale blocks.

**Flash is real hardware here, and getting it needed a choice.** QEMU offers
x86_64 raw flash only through `-drive if=pflash`, and pflash unit 1 requires
unit 0 — the machine's firmware. With OVMF there the kernel does not load at
all (`multiboot2: Could not find viable load address`), so a UEFI firmware
would have traded flash for the ability to boot. SeaBIOS in unit 0 keeps the
ordinary boot and leaves unit 1 for the chip:

```
-drive if=pflash,format=raw,readonly=on,file=/usr/share/qemu/bios-256k.bin,unit=0
-drive if=pflash,format=raw,file=smoke_run/flash-blk.img,unit=1
```

The driver probes with a CFI query and reads the size and erase-block layout
out of the chip's own table, so it is discovery rather than a constant. The
probe WRITES a command to a physical address, so it happens only when
`b1nix.mtd` asks for it: on real hardware that space is the firmware's.

One property cannot be proved here and is recorded rather than asserted. Real
NOR clears bits and never sets them, which is why an erase exists; QEMU's
pflash model stores the byte outright. The test prints what the device did and
checks what is checkable — that the program command sequence completes.

NAND stays out. `nanddump` and `nandwrite` exist for out-of-band data, which
NOR does not have; the ioctls that ask for it answer "not supported" rather
than returning zeroes a NAND tool would read as real spare data.

**A handed-over socket is held by reference.** `nbd-client` connects, completes
the handshake and gives the kernel its descriptor. An fd number belongs to one
process and can be closed or reused underneath the kernel, so the handle itself
is referenced instead.

What the tests prove, rather than what they run: the namespace checks compare
the namespace's answer against the parent's; the RAID check writes through the
array and then reads **each member directly**, which is the only thing a mirror
exists for; the network device is required to return no bytes at all while
empty, since a successful read there would be handing out unrelated memory.

## Deliberately not done

- **UBI and the NAND tools** stay unbuilt: UBI is a whole volume layer on top
  of MTD, and `nanddump`/`nandwrite` exist for out-of-band data that NOR has
  none of. MTD itself is no longer on this list — the firmware `pflash` the
  note dismissed turned out to be usable as a second chip (see above), so the
  layer was built on emulated hardware rather than on a RAM pretence.
- **rfkill and floppy**: no radio and no controller. CD-ROM, RAID and the
  network block device were in this list until M114 built the layers under
  them. Serial configuration, by
  contrast, is real — termios baud/parity/stop bits on the 16550, `TIOCM*`,
  `TIOCGSERIAL`.
- **`i2ctransfer`** needs raw I2C that an SMBus controller cannot issue.
- The **shadow-suite names** `useradd`/`userdel`/`groupadd` do not exist: the
  account tools moved to BusyBox's own `adduser`/`deluser`/`addgroup`/
  `delgroup`, and a symlink from a shadow-suite name to an applet that rejects
  its options would be a rename rather than a handover.

## An open flake, recorded rather than closed

`M13-JC-SMOKE: ok wuntraced` — waitpid reporting a child stopped by SIGTSTP —
failed once in five isolated runs of the posix instance on this branch, and
also stalled that instance during a full suite run while another emulator was
busy on the same host. The base commit passed it three times out of three.
Those numbers do not distinguish "this branch introduced it" from "it was
always there and shows under load", and they are recorded here rather than
rounded to whichever answer is more comfortable.

What was changed is only the test's patience: it polled for about half a
second, which is generous on an idle machine and tight when the host is running
several emulators. The assertion is untouched — the child must still report
`WIFSTOPPED` — so a kernel that drops the notification still fails, merely
later. What was NOT done is a fix to the stop-notification path, because no
defect in it has been demonstrated yet.
