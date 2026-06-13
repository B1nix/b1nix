# Display Server Plan (M47–M49)

Decision record and phased plan for graphics/windowing in b1nix.

## Decision

Write our own userspace compositor, then speak the real Wayland protocol.
M47 used a temporary Wayland-shaped protocol to validate the compositor;
M49 removed it rather than maintaining two wire formats.

- **Full X.org**: the server itself plus the dependency pyramid
  (pixman, libX11/libxcb, xkbcommon/xkbcomp, freetype/fontconfig) is an
  order of magnitude more porting work than everything ported so far, and
  yields a server with no clients until xterm/dwm-class apps are also ported.
- **weston/wlroots**: modern weston requires DRM/KMS + GBM/EGL (the fbdev
  backend was removed); wlroots additionally wants udev + libinput. Not
  realistic without a DRM emulation layer.
- **"Porting Wayland" itself is mostly a misconception**: Wayland is a
  protocol plus a small IPC library (libwayland); the compositor must be
  written from scratch either way. So the custom path *is* the Wayland path,
  minus the wire format.
- **TinyX/kdrive (`Xfbdev`)** remains a viable *parked* alternative for
  running real X11 apps later; it needs exactly the same kernel substrate
  (`/dev/fb0` + input devices) that M47 builds, so nothing is lost.

## Existing substrate

Already in the tree: `kernel/dev/virtio_gpu.c` (704 lines, resource +
transfer/flush path), `kernel/dev/video.c`, an in-kernel `compositor.c`
(~500 lines, becomes the console fallback), `ps2_kbd.c`/`ps2_mouse.c`,
SysV SHM, UNIX sockets, poll/select, pthreads, mmap.

M48 provides `sendmsg`/`recvmsg`, `SCM_RIGHTS`, `SCM_CREDENTIALS`, and
`memfd_create`. Wayland `wl_shm` uses memfd-backed buffers over
`/run/wayland-0`.

## M47: Userspace Display Server

### Phase 1 — kernel substrate (LANDED)

Status: implemented and green on both arches (`M47-GFX` markers in the main
smoke suite, single-CPU and `-smp 4`). What landed vs. the plan below:

- `/dev/fb0` (`kernel/dev/fb.c`): lazily allocated contiguous shadow buffer,
  `B1NIX_FBIOGET_INFO` / `B1NIX_FBIOFLUSH` (virtio-gpu present → bootfb row
  copy fallback). mmap goes through a new generic `vfs_inode.mmap_phys_cb`
  hook in `sys_mmap`: device frames are mapped `VMM_SHARED` with a pmm ref
  per page (the SysV-shm pattern), so fork shares them and munmap/teardown
  can never free the kernel-owned buffer. A new `vfs_inode.ioctl_cb` hook
  dispatches device ioctls ahead of the legacy name-based cases.
- `/dev/input/event0` (kbd) + `event1` (mouse) (`kernel/dev/input.c`):
  evdev-style 16-byte records, per-open-client queues with drop-oldest
  overflow, opens intercepted in `vfs_open_flags` (serial-tty pattern,
  `VFS_HANDLE_INPUT`), root-only (0600). PS/2 IRQ decoders push raw
  scancodes / REL+ABS+BTN bursts.
- Smoke: `userspace/bin/m47_smoke.c` — two fb mappings must alias, pixels
  must survive munmap+remap, EAGAIN/poll semantics, and a kernel-injected
  mouse burst received through a blocking read (`m47-inject` kthread in
  `programs.c`; the injector feeds `input_event_push` directly since a
  headless QEMU has no mouse to wiggle — i8042 decode is real-HW-only).
- Console reclaim: device-mmap VMA lifecycle hooks count mappings across
  fork/split/munmap/exec/exit. The last mapping returns scanout ownership and
  requests a full kernel-compositor redraw.
- Fixed en route: `userspace/include/sys/resource.h` used `id_t` without
  including `<sys/types.h>` (M46 regression) — broke every fresh autotools
  configure (`ac_cv_header_sys_resource_h=no` → bash build failure).

### Phase 1 — kernel substrate (original plan)

- `/dev/fb0`: mmap-able linear framebuffer char device on top of
  virtio-gpu/VBE. ioctls: get mode (width/height/stride/bpp), set mode
  (optional), `FB_FLUSH(x, y, w, h)` mapping to the virtio-gpu
  transfer+flush path. The kernel console/compositor keeps ownership until a
  userspace server opens the device (same handoff discipline as the serial
  tty COM1-ownership rule).
- `/dev/input/event0..N`: evdev-style unified input events
  (type/code/value + timestamp) for PS/2 keyboard and mouse; readable,
  pollable, blocking reads are signal-interruptible. Keyboard events carry
  raw keycodes; keymap handling lives in userspace.
- The in-kernel `compositor.c` is demoted to boot console / fallback; it must
  release the framebuffer when the userspace server claims it and reclaim on
  server exit (so a crashed server returns to a usable console).

### Phase 2 — protocol + server

Status: the compositor work landed in M47; its temporary protocol was removed
in M49. `displayd` is a single-threaded Wayland compositor with damage-driven
framebuffer updates, a software cursor, click-to-focus/raise, draggable title
bars, `Alt+Tab`, `Alt+F4`, and server-side decorations. It is available as an
inittab-supervised runlevel-5 service.

#### Desktop-integration fixes (interactive `make run-graphics`)

Bringing the real runlevel-5 desktop up (vs. the headless smoke, which uses a
synthetic input injector) surfaced four issues, all now fixed:

- **Kernel console fought displayd for the framebuffer.** `fb_console`
  (text + the blinking cursor) and the runlevel-5 `console` bash both drew
  straight into `/dev/fb0` while displayd composited there — a blinking line,
  windows getting half-erased, and a stray block. Fix: every `fb_console`
  draw path early-returns when `fb_dev_claimed()` (both arches), and the
  inittab `console` shell now runs at runlevels 2-4 only (not 5), so no shell
  scribbles on or steals keystrokes from the desktop. The serial getty stays
  at runlevel 5 as a rescue path.
- **The mouse never moved (`ps2_mouse: enable failed`).** The keyboard
  IRQ/timer-tick i8042 poll fallback drained the mouse's 0xFA ACK out of the
  shared output buffer mid-`ps2_mouse_init` (and dropped it, since
  `mouse_ready` was still 0), so init always timed out. Fix: the init
  handshake now runs with interrupts disabled, does a proper `0xFF` reset for
  presence detection, and tolerates late/stale ACKs. The "stray block" in the
  middle of the screen was simply displayd's cursor stuck at its start
  position because no motion events were arriving.
- **macOS-style top bar:** the panel now shows `b1nix` + the focused app's
  title on the left and a live `HH:MM` clock (RTC, repainted on the minute)
  flush right. System, active-app, File/Edit/View, and clock headers open
  server-rendered dropdowns; supported actions close/quit windows, dispatch
  Cut/Copy/Paste shortcuts, cycle focus, and raise the active window. Menus
  track pointer hover, switch while traversing the bar, and dismiss on
  click-away or Escape. Title-bar dragging is clamped so a window can't be
  lost under the bar or off-screen.

`displayd` server (userspace, single-threaded poll loop):

- Damage-driven compositing of SHM surfaces into the mmap'd `/dev/fb0`,
  then `FB_FLUSH` of the dirty union.
- Software cursor, focus-follows-click, alt-tab, server-side decorations
  (1-px border + title bar is enough for v1).
- Started from `/etc/inittab` as a service; exits cleanly back to the kernel
  console.

### Phase 3 — client library + apps + smoke

- Status: implemented and green on x86_64 and x86.
- `libb1gui`: Wayland connection/registry handling, object marshalling,
  `wl_shm` buffer helper, damage/commit/frame helpers, simple event dispatch.
- Demo clients: `gclock` (animated frame callbacks), `gterm` (shell over the
  existing `forkpty` layer with raw-key input), and `gpaint` (pointer-driven
  drawing).
- Smoke: `tests/graphics-smoke.sh` (QEMU with `-device
  virtio-gpu-pci`, headless). It verifies Wayland registry, xdg-shell,
  `wl_shm`, frame callbacks, `libb1gui`, console reclaim, and server restart.

## M48: FD Passing and Anonymous Shared Memory

Standalone POSIX milestone; the display stack is the first consumer but not
the only one (dbus-style daemons, dropbear privilege separation).

- Status: implemented and smoke-tested on x86_64 and x86.
- `sendmsg`/`recvmsg` syscalls with `msg_control` ancillary data on UNIX
  sockets.
- `SCM_RIGHTS`: fd transfer with correct refcounting across the existing
  fd-table machinery. **Caution:** fd-table lifetime is a known sharp edge
  (see the M46 shared-fd-table fixes and the `fdtable_other_refs` latent in
  the thread-exit notes) — in-flight fds pinned in a socket buffer must hold
  real references, including when the receiver dies before `recvmsg`.
- `SCM_CREDENTIALS` (cheap once the cmsg plumbing exists; lets the server
  identify clients).
- `memfd_create` (anonymous mmap-able file, no sealing needed for v1).
- Display buffers use memfd + `SCM_RIGHTS` through Wayland `wl_shm`.

## M49: Wayland Protocol Compatibility

With M47's Wayland-shaped core and M48's fd passing in place:

- `displayd` speaks `wl_display`, `wl_registry`, `wl_shm`,
  `wl_compositor`/`wl_surface`, `wl_seat`, and an `xdg-shell` subset on
  `/run/wayland-0`.
- `libb1gui` and all bundled GUI applications use Wayland exclusively.
- The temporary M47 protocol, socket, header, and smoke client are deleted.
- Upstream `libwayland-client` 1.25.0 is ported and covered by boot smoke;
  `libb1gui` and the native compositor also marshal the standard wire format
  directly.
- Upstream `libwayland-server` core is also ported with a poll-backed event
  loop; `displayd` stays native because replacing working compositor policy
  would add risk without changing protocol compatibility.
- Keyboard uses the standard `no_keymap` fd event, raw evdev keycodes, and
  repeat metadata; xkbcommon is unnecessary until layouts compose symbols.
- Smoke covers the stock SHM/xdg-shell protocol path and a `libb1gui` client
  on x86_64 and x86.
- Parked alternative for X11 apps: TinyX/kdrive `Xfbdev` on the same
  `/dev/fb0` + input substrate.

## Display resolution and absolute pointer (mouse)

- **Resolution is 1280x800.** The Multiboot2 framebuffer tag (`kernel/arch/*/boot.S`),
  the GRUB `gfxmode` (`boot/grub/grub.cfg`), and the virtio-gpu native scanout
  all agree at 1280x800x32. Keeping them identical avoids a dual-surface
  mismatch where the kernel framebuffer (GRUB VGA mode) and the virtio-gpu
  scanout disagreed and QEMU's presented size depended on init order.
- **Mouse — two input paths feed `/dev/input/event1`:**
  - **PS/2 relative mouse** (`kernel/dev/ps2_mouse.c`): works, but in a QEMU
    window relative motion only reaches the guest after you click to grab the
    pointer. The kernel maintains a framebuffer-clamped absolute position and
    emits both `EV_REL` and `EV_ABS`.
  - **virtio-tablet absolute pointer** (`kernel/dev/virtio_input.c`, a modern
    virtio-input driver): the cursor tracks the host 1:1 with no grab. The
    device's event virtqueue is drained from the timer tick; each
    `virtio_input_event` maps 1:1 onto the b1nix input layer (Linux-identical
    codes), with `EV_ABS` values scaled from the device's 0..32767 range to the
    live framebuffer resolution. `make run-graphics` attaches
    `-device virtio-tablet-pci`.
- **displayd** consumes both: `EV_REL` accumulates a delta; `EV_ABS` sets the
  cursor position directly (`userspace/bin/displayd.c`).
