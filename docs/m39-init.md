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

The shipped inittab makes GNU **bash** the console terminal:

```
id:3:initdefault:
si::sysinit:/etc/rc
console:2345:respawn:/bin/bash
ttyS0:23:respawn:/bin/getty ttyS0
ca::ctrlaltdel:/bin/reboot
```

## Supervisor

`init_supervise()` (in `kernel/user/programs.c`) runs the `sysinit` entries,
enters the `initdefault` runlevel (starting its `wait`/`once`/`respawn`
entries), then loops: reaps children with `waitpid(WNOHANG)` and restarts
`respawn` entries valid in the current runlevel. A per-entry respawn cap
(`INITTAB_MAX_RESPAWNS`) stops a failing service from storming. The legacy
rc + respawn-shell path is preserved as a fallback when `/etc/inittab` is absent
or a boot override (`init=`, `single`, `login`, `ui`) is requested.

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

## getty

`/bin/getty` and `/sbin/getty` are the upstream BusyBox `getty` applet, used by
`respawn` inittab entries for serial/tty login sessions. b1nix's interactive
hardware is effectively a single console, so multi-tty getty is `initial`; the
applet and inittab plumbing are in place for serial getty on real hardware.

## Verification

`M39-INIT` in the boot self-test exercises the parser, the `initdefault`
runlevel, runlevel matching, the `telinit → /run/initctl` round-trip, and getty
applet presence.
