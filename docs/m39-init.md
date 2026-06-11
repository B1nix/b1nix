# M39: Configurable init system

b1nix PID 1 (`/bin/init`, the in-kernel `init_main`) is now driven by
`/etc/inittab` instead of a hardcoded boot program list. Runlevels and a
`telinit` client let the running system reconfigure which services are up.

## /etc/inittab

BusyBox/SysV-flavoured, one entry per line:

```
id:runlevels:action:process
```

- **runlevels** — digit string `0`–`6` the entry applies to (empty = all).
- **action** — one of:
  - `sysinit` — run once at boot, init waits for it (used for `/etc/rc`).
  - `wait` — run on entering the runlevel, init waits.
  - `once` — run on entering the runlevel, init does not wait.
  - `respawn` — keep running; restart when it exits (the console shell, getty).
  - `initdefault` — selects the default runlevel.
  - `ctrlaltdel` / `shutdown` — recorded (hooks for those events).

The shipped inittab makes GNU **bash** the console terminal and runs a real
login getty on the serial line:

```
id:3:initdefault:
si::sysinit:/etc/rc
console:2345:respawn:/bin/bash
ttyS0:2345:respawn:/bin/getty -L 115200 ttyS0 vt100
ca::ctrlaltdel:/bin/reboot
```

## Supervisor

`init_supervise()` (in `kernel/user/programs.c`) runs the `sysinit` entries,
enters the `initdefault` runlevel (starting its `wait`/`once`/`respawn`
entries), then loops: reaps children with `waitpid(WNOHANG)` and restarts
`respawn` entries valid in the current runlevel.

The respawn storm guard is rate-based (SysV-style): a respawn child that
exits within 2 seconds of its spawn 5 times **in a row** gets the entry
disabled with `init: <id>: respawning too fast` (a runlevel switch un-parks
it). A long-lived child resets the streak, so a getty that respawns after
every normal logout is never silenced.

Because the kernel exec path has no shebang support, a script entry (the
sysinit `/etc/rc`) is retried through `/bin/sh` when the direct spawn fails —
the same way the legacy init path always ran rc.

The legacy rc + respawn-shell path is preserved as a fallback when
`/etc/inittab` is absent or a boot override (`init=`, `single`, `login`, `ui`)
is requested.

## telinit and /run/initctl

PID 1 is an in-kernel task and cannot install a userspace signal handler, so
`telinit` (`userspace/bin/telinit.c`, at `/sbin/telinit` and `/bin/telinit`)
communicates through a control file: it writes the requested runlevel to
`/run/initctl` and the supervisor polls + consumes it each loop. Runlevel `0`
halts, `6` reboots, others stop the `respawn` entries no longer valid and start
the ones that now are.

```
telinit 5      # switch to runlevel 5
telinit 0      # halt
telinit 6      # reboot
```

## getty and the serial ttys

`/bin/getty` and `/sbin/getty` are the upstream BusyBox `getty` applet, used by
`respawn` inittab entries for serial/tty login sessions.

The serial lines are real, independent tty devices (`kernel/dev/serial_tty.c`):
`/dev/ttyS0` (COM1, always) and `/dev/ttyS1` (COM2, when the UART probe finds
one). Each has its own input ring, canonical line discipline (ICANON / ECHO /
ISIG / VEOF / VERASE / ICRNL), termios, winsize, and foreground-pgrp/session
state — fully separate from the merged VGA+COM1 boot console. Opens are
intercepted in `vfs_open_flags` and return raw handles with custom file ops
(the pty model), so the fds flow through normal read/write/poll/ioctl/fork.
Supported ioctls: `TCGETS`/`TCSETS`, `TIOCGPGRP`/`TIOCSPGRP`, `TIOCSCTTY`,
`TIOCNOTTY`, `TIOCGWINSZ`/`TIOCSWINSZ`.

**COM1 ownership rule:** while `/dev/ttyS0` is open it owns the COM1 receive
side (the BSP timer tick drains the UART through the line discipline into the
tty's ring and wakes pollers); the boot console only falls back to COM1 input
when no one holds the device. Kernel log output still mirrors to COM1 — same
property as a Linux `console=ttyS0` system running a getty on the same port.

The full chain works end-to-end: `getty → /bin/login → bash` as a login shell
on the serial line, with the console bash session running independently on the
VGA/keyboard side. `tools/build-busybox.sh` patches BusyBox's `pw_encrypt()`
to defer `$b1$` settings to the libc `crypt()` (the system-wide scheme used by
/etc/shadow, dropbear, and the native su/passwd) while the builtin crypt keeps
handling `$1$/$5$/$6$` for `cryptpw`/`chpasswd`; without the deferral, `login`
died with `bad salt` on b1nix shadow entries.

## Verification

`M39-INIT` in the boot self-test exercises the parser, the `initdefault`
runlevel, runlevel matching, the `telinit → /run/initctl` round-trip, getty
applet presence, and the serial tty layer (open, termios/pgrp independence
from the console, canonical + raw + VEOF reads, ISIG VINTR interception,
TIOCSCTTY, a real TX marker out COM1, and the close-releases-claim rule).

`tests/serial-getty.sh [x86|x86_64]` is the end-to-end host test: it boots a
NORMAL-mode ISO, answers the getty prompt on QEMU's serial stdio as `root`,
verifies an interactive bash login session on `/dev/ttyS0`, logs out, and
checks that init respawns getty with a fresh login prompt.

`tools/inguest/run-build.py` performs the same root login handshake before
driving in-guest builds (normal boots now land on a serial getty, not a raw
shell).
