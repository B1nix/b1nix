# displayd Wayland conformance

Living reference for what `userspace/bin/displayd.c` implements of the Wayland
protocol, and what it deliberately leaves out. displayd is a small native
compositor that speaks the real Wayland wire format (object id / opcode / size
headers, `wl_fixed` 24.8 coordinates, `SCM_RIGHTS` buffer fds) — it is not a
shim. The list below is the honest delta from a full compositor.

## Implemented (core + xdg-shell subset)

- `wl_display`, `wl_registry`, `wl_callback` (sync), `wl_compositor`,
  `wl_surface` (attach / damage / frame / commit), `wl_region`.
- `wl_shm` + `wl_shm_pool` + `wl_buffer`, formats `ARGB8888` and `XRGB8888`.
- `wl_seat` (pointer + keyboard), `wl_pointer`, `wl_keyboard`, `wl_output` (v2,
  with `mode`/`scale`/`done`).
- `xdg_wm_base`, `xdg_surface`, `xdg_toplevel` (title, app_id, configure/ack,
  close, **move, resize, maximize/unmaximize, fullscreen/unfullscreen**),
  `xdg_wm_base.ping`/`pong` hung-client detection.
- `wl_data_device_manager` clipboard (selection copy/paste over `SCM_RIGHTS`).
- **Keyboard: a real `XKB_V1` keymap** (US/evdev, embedded in
  `userspace/bin/xkb_keymap_us.h`, sent over a `memfd`) plus
  `wl_keyboard.modifiers` (Shift/Ctrl/CapsLock/Alt/Super). Key events carry raw
  evdev keycodes; the client's own xkbcommon adds the +8 offset and translates.

## Deliberately not implemented

These are conscious scope choices, not missing-but-intended work. None of them
breaks a conforming client — the client simply finds the global absent, or its
request is accepted and ignored.

### Window management

The compositor runs its own WM (title-bar drag, click-to-focus / raise,
`Alt+Tab` cycle, `Alt+F4` close), *and* honours the client-driven
`xdg_toplevel` requests:

- `move` — starts a server-side drag of the window (same path as title-bar
  drag), ending on button release.
- `resize` — interactive edge/corner grab; each pointer step sends a
  `configure` with the new size and the `resizing` state, keeping the anchored
  edge fixed. The window repaints when the client commits the resized buffer.
- `set_maximized` / `unset_maximized` — `configure` to the work area
  (full width, below the top panel) with the `maximized` state; restores the
  saved floating position on unmaximize.
- `set_fullscreen` / `unset_fullscreen` — `configure` to the full screen
  (covering the panel) with the `fullscreen` state.

A floating toplevel's first `configure` uses width/height = 0 ("client picks
its size") plus the `activated` state, since a freshly-mapped window takes
focus.

**`set_minimized` is acknowledged but not acted on** — there is no taskbar to
restore from, so unmapping would strand the window. Add it together with a
taskbar/dock if one is built.

### Absent globals / protocols

- **Drag-and-drop**: only clipboard selection is implemented; `wl_data_device`
  `start_drag` and the DnD offer/action events are not.
- **Touch**: `wl_seat` advertises pointer + keyboard only (capabilities = 3).
- **HiDPI / transform**: fixed at scale 1, normal transform. `set_buffer_scale`,
  `set_buffer_transform`, `set_opaque_region`, `set_input_region`, surface
  `offset` are accepted and ignored.
- **Not advertised at all**: `xdg-decoration`, `wp_viewporter`,
  `wl_subcompositor`, `wp_presentation`, `zwp_linux_dmabuf`. (GPU buffers reach
  the host via `/dev/virtio-gpu` outside Wayland — see M53.)

### Fixed capacities

`MAX_CLIENTS` / `MAX_SURFACES` / `MAX_BUFFERS` = 8, `MAX_TOPLEVELS` = 8,
`MAX_WOBJECTS` = 64, `MAX_WPOOLS` = 8. Raise the `#define`s if a workload needs
more; nothing else assumes the value.

## Architecture notes

- **`wl_shm` lives in displayd, not in the ported `libwayland-server`.** M49
  ported `libwayland-server`'s core, but SHM compositing stays native until
  b1nix has the pthread-TLS / SIGBUS ABI the upstream SHM path expects.
- **Hand-rolled wire marshalling.** Opcodes are literal numbers in
  `handle_wayland_msg`, not `wayland-scanner`-generated stubs. This keeps the
  server tiny (one file, no codegen step) at the cost of the opcodes being
  documented by comment rather than by a generated header.
