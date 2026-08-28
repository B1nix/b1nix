# Handoff: finish Plasma, then run Debian on this kernel

Paste this into a new chat. It is written to be read once, from the top.

---

## Where the tree stands

`main`, version **0.114.21**, working tree clean. Main smoke suite:
**1363 passed / 0 failed / 0 blocked**. Build a KDE image with

```sh
B1NIX_KDE=1 KERNEL_CMDLINE="b1nix.kde" make -j6 iso
flock smoke_run/.qemu.lock sh tools/run-kde.sh <tag>
```

Read `CLAUDE.md` first and query the codebase-memory MCP before opening source.
That is a standing project rule and it is cheaper than grepping.

---

## Task 1 — close Plasma completely

A desktop has already been photographed once (`smoke_run/kde9-shot.png`, 1280x720,
11576 colours: wallpaper, panel with clock, a `foot` window with KDE decorations)
but that was **nested inside sway**. The goal is Plasma on the real DRM path,
with no host compositor underneath, photographed from outside the guest with the
QEMU monitor's `screendump` so nothing in the guest can fake it.

### The chain, and exactly where it stops

Every one of these was a real defect, found and fixed in order. They are listed
so you do not re-investigate them:

1. `/var/run` did not exist → dbus bound its socket in `/run/dbus` while clients
   connected to `/var/run/dbus/system_bus_socket`; the bus ran and was
   unreachable at the same time. **Fixed** (symlink in the overlay).
2. No PAM login → no logind session. **Fixed**: `util-linux-login` staged as
   `/sbin/login-pam`, `runuser -l` with `XDG_SEAT=seat0 XDG_VTNR=1` registers the
   session the way a display manager does.
3. `/sys/class/tty/tty0/active` absent → logind concluded the seat had no VTs and
   refused a session whose VT number is not 0. **Fixed**.
4. A blocking device read held the inode's **write** lock, so `open("/dev/tty1")`
   never returned and logind's `TakeControl` timed out with no reply. **Fixed**.
5. `KDSKBMODE(K_OFF)` was refused with EINVAL → sd-bus turns that into
   `InvalidArgs` → "Failed to take control of session. Maybe another compositor
   is running?". **Fixed**.

**Where it stops now**: `TakeControl` succeeds; `TakeDevice(226, 1)` answers
`System.Error.ENODEV`, and logind never reaches an `open()` of the node — a
kernel trace of the whole boot (`b1nix.trace-sysfs`) shows no compositor and no
logind touching `/dev/dri`.

`/sys/dev/char/226:1` **does** exist and resolves correctly:

```
226:1 -> /sys/devices/pci0000:00/0000:00:03.0/drm/card1
uevent = MAJOR=226 MINOR=1 DEVNAME=dri/card1 DEVTYPE=drm_minor
```

So logind rejects it while parsing the device rather than while opening it.
`sd_device_new_from_devnum` and what it demands next — `subsystem`, `ID_SEAT`,
the udev database entry under `/run/udev/data/c226:1` — is the place to look.
Note `udevadm test` creates `/run/udev/data/b8:0` for block devices, so the
rules work; whether anything ever tags the DRM node is unverified.

### Requirements for "closed"

- Plasma running on DRM with no nested compositor, photographed from the host.
- The picture shows contents (a blank frame is 2 unique colours; a real desktop
  was 11576). Assert on that, not on a marker.
- `kwin_wayland` visible in `b1nix.trace-sysfs` opening `/dev/dri/cardN`, or
  logind visibly doing it on its behalf. This is the one signal the harness
  cannot fake, and it is why it is required.
- The main suite still 1363+/0/0, and the KDE path exercised by a check that
  fails when the desktop does not draw.
- Roadmap updated; the KDE entry currently describes the nested path only.

### Traps that cost hours today

- **`tools/kde-refresh.sh` builds from `build/x86_64/rootfs` as it stands.** A
  plain `make iso` (no `B1NIX_KDE=1`) prunes KDE out of that tree, so a refresh
  afterwards silently produces an image with no kwin in it. After any full smoke
  run, rebuild with `B1NIX_KDE=1` before a KDE run. The build agent recommended
  retiring this script; it is now slower and less correct than plain `make`.
- **Every launch in `kde.sh` is `timeout N prog &`, so `$!` is the timeout, not
  the program** — and `timeout` here exits on its own while its child runs.
  Three separate markers reported healthy programs as dead. Use `prog_alive`.
- **Markers lie more often than the system does.** Of the "causes" identified
  during that session, more than half were defects in the harness: a stale
  wayland socket read as success, a bus judged working because a socket file
  existed, a process searched for by a name it does not use, a header deletion
  "proved" safe by an incremental build that rebuilt nothing.
- **A flaky panic in `net_proto_reset` still exists**, roughly 1 boot in 6 in the
  IOMMU instance. It colours runs at random and will tempt you to blame your last
  change. It already caused one wrong attribution. Do not conclude anything from
  three runs.

### Instruments that already exist

`b1nix.trace-sysfs` (every `/dev/dri` and `/sys` open with the task name),
`b1nix.drm-debug` (refused DRM opens with flags and errno), `b1nix.trace-open`,
`b1nix.trace-ioctl` (every ioctl refused as ENOTTY, with request and fd),
`b1nix.trace-errno=<n>` and `b1nix.trace-errno-pid=<pid>`, `b1nix.gfx-prof`
(per-frame copy/transfer/flush plus spin and park counts), `b1nix.task-watch`
and `/proc/b1nix-tasks` (a blocked task's syscall number and wait channel; when
the channel is an inode it names the lock holder and where it was taken),
`tools/boot-timeline.sh` (where a boot's seconds went).

The single most effective move all session was **asking the program instead of
reasoning about it**: elogind with `ELOGIND_LOG_LEVEL=debug` logs its whole D-Bus
traffic, and calling a method by hand with `dbus-send` gives the error the client
swallows. Two of the five fixes came straight from that.

---

## Task 2 — Debian on this kernel, as an agent

Run this as a separate agent on its own branch. It is independent of Task 1 and
should not share a worktree with it.

### The goal

Take a stock Debian VM image, replace its Linux kernel with `kernel.elf` from
this tree, boot it, and report how far systemd gets. Not a port: a swap, to find
out what a real distribution's PID 1 demands that we do not provide.

`tools/images/mk-debian-image.sh` and `tests/systemd-smoke.sh` already exist and
boot a Debian userspace on this kernel with a harness of 29 checks; 15 pass. Use
them rather than starting over.

### What is already known

- **The 90 s `daemon-reload`** fails with `Transport endpoint is not connected`.
  Refuted, with measurements, and recorded in `docs/`: it is not per-unit cost
  (61% more units moved the time 1.2%), not a hanging generator (all twelve run
  by hand in ~1.2 s), not work of any kind (203 `openat` in 91 s), not a
  kernel-side disconnect (one AF_UNIX teardown in the window, the client's, after
  the call returned), and not a lost listener edge (the same socket served 38
  connections). What remains is delivery of one request on an established
  connection. Do not re-test the refuted five.
- `udevadm control --ping` times out for real — it survived the clock fix.
- No `.device` unit activates even though udev now processes and tags the device.
- `loop0`–`loop6` are still not processed.
- `timeout(1)` does not kill its child. This is ours and it corrupts any
  measurement bounded by it.

### Requirements

- Report how far a stock Debian boots, by target, not by impression:
  `systemd-analyze critical-chain` if it gets that far, or the last unit reached.
- Every claim measured. If a number came through `timeout`, say so.
- Retract in the report anything that turns out wrong; the previous agent's
  retractions were the most useful part of its output.
- Keep the diagnostic logs per run — the harness used to write every run to one
  path and destroyed its own evidence.
- Do not chase Debian-specific packaging. The question is what the **kernel**
  fails to provide.

### Why this is worth doing

Every defect found today came from a foreign program asking the kernel something
by the standard and getting a different answer: device numbers of zero, terminal
ioctls refused as "not a terminal", `/sys` views that did not exist, `/var/run`
missing, a keyboard mode we had never heard of. Our own 1363 checks pass because
they ask what we already answer. A real distribution's PID 1 asks systematically.
That is the point of the swap, and it will find more than another suite of ours
would.
