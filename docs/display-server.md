# Display Server Plan (M47–M49)

Decision record and phased plan for graphics/windowing in b1nix.

## Decision

Write our **own userspace display server speaking our own protocol** (M47),
but shape the protocol after Wayland's core concepts so that real Wayland
compatibility (M49) is a short follow-up, not a rewrite. Porting was rejected:

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

Missing (checked 2026-06: no hits in `kernel/`): `sendmsg`/`recvmsg`,
`SCM_RIGHTS` fd passing, `memfd_create`. These are the M48 prerequisites for
real libwayland, and independently useful POSIX features (dbus, privilege
separation in daemons).

## M47: Userspace Display Server

### Phase 1 — kernel substrate (LANDED, reclaim deferred)

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
- Deferred: console *reclaim* when the last fb0 user exits (needs displayd
  lifecycle, Phase 2); `compositor.c` currently just stops flushing once
  fb0 is claimed.
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

`b1display` protocol v1 over a UNIX socket (`/run/display.sock`).
**Wayland-shaped on purpose** — these constraints are load-bearing for M49:

- Wire format: little-endian 8-byte header (object id u32, opcode u16,
  size u16) + arguments — exactly Wayland's framing.
- Object model: client-allocated object ids, requests (client→server) and
  events (server→client), explicit destructors.
- Buffers are **always client-allocated**; the server never hands out
  drawing surfaces. v1 transport: SysV SHM key + offset (M48 switches this
  to fd passing; the transport sits behind one small abstraction).
- Surface lifecycle: `attach(buffer)` → `damage(x,y,w,h)` → `commit`;
  nothing is visible until commit (atomic updates, like `wl_surface`).
- `frame` callback for client-driven redraw pacing (no server-side timers
  per client).
- `seat` abstraction for input: enter/leave + focus, keyboard events with
  serials, pointer events in surface-local coordinates.
- Toplevel role (`xdg_toplevel` analog): title, move/resize initiated via
  input serials, close event.

`displayd` server (userspace, single-threaded poll loop):

- Damage-driven compositing of SHM surfaces into the mmap'd `/dev/fb0`,
  then `FB_FLUSH` of the dirty union.
- Software cursor, focus-follows-click, alt-tab, server-side decorations
  (1-px border + title bar is enough for v1).
- Started from `/etc/inittab` as a service; exits cleanly back to the kernel
  console.

### Phase 3 — client library + apps + smoke

- `libb1gui`: connection handling, object marshalling, SHM pool helper,
  damage/commit/frame helpers, simple event dispatch.
- Demo clients: `gclock` (animated, exercises frame callbacks), `gterm`
  (terminal emulator on the existing pty layer), `gpaint` or similar
  (exercises pointer input).
- Smoke: extend `tests/graphics-smoke.sh` (QEMU with `-device
  virtio-gpu-pci`, headless). Pixel correctness is verified server-side: the
  server checksums the composited framebuffer region and emits markers —
  no screenshot needed over serial. Markers (`M47-GFX:`): `ok fb-mmap`,
  `ok fb-flush`, `ok input-event`, `ok connect`, `ok shm-attach`,
  `ok commit-damage`, `ok frame-callback`, `ok two-clients`,
  `ok focus-switch`, `ok server-restart` (console reclaim works).

## M48: FD Passing and Anonymous Shared Memory

Standalone POSIX milestone; the display stack is the first consumer but not
the only one (dbus-style daemons, dropbear privilege separation).

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
- Switch the `b1display` buffer transport from SysV SHM keys to
  memfd + `SCM_RIGHTS` (the abstraction from M47 Phase 2 makes this a
  contained change). Markers: `M48-FDPASS:` `ok scm-rights`,
  `ok scm-refcount-close`, `ok memfd`, `ok display-fd-buffers`.

## M49: Wayland Protocol Compatibility (planned)

With M47's Wayland-shaped core and M48's fd passing in place:

- Port libwayland (client + server); needs only UNIX sockets + cmsg + an
  epoll-shim over poll. Run `wayland-scanner` on the host at build time.
- Teach `displayd` the real protocol: `wl_display`, `wl_registry`, `wl_shm`,
  `wl_compositor`/`wl_surface`, `wl_seat`, and an `xdg-shell` subset —
  each maps 1:1 onto a `b1display` concept by construction.
- Keyboard: minimal keymap shim or an xkbcommon port (xkbcommon is
  self-contained C, no X dependency — the realistic choice).
- Success criterion: one stock SHM-based Wayland client runs unmodified
  (e.g. weston-simple-shm class).
- Parked alternative for X11 apps: TinyX/kdrive `Xfbdev` on the same
  `/dev/fb0` + input substrate.
