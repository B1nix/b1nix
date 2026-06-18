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
- `wl_seat` (pointer + keyboard + **touch**, capabilities = 7), `wl_pointer`,
  `wl_keyboard`, `wl_touch` (down / up / motion / frame from
  `/dev/input/event2`), `wl_output` (v2, with `mode`/`scale`/`done`).
- `xdg_wm_base`, `xdg_surface`, `xdg_toplevel` (title, app_id, configure/ack,
  close, **move, resize, maximize/unmaximize, fullscreen/unfullscreen**),
  `xdg_wm_base.ping`/`pong` hung-client detection.
- `wl_data_device_manager` clipboard (selection copy/paste over `SCM_RIGHTS`)
  **and drag-and-drop**: `start_drag` opens a server-side drag grab keyed to the
  pointer button; the surface under the pointer gets `data_device`
  enter/motion/leave + `drop`, each carrying a server-made `data_offer` that
  mirrors the source MIME and `source_actions`; the source sees
  `dnd_drop_performed` / `dnd_finished` / `cancelled`; `data_offer`
  accept/finish/set_actions are handled.
- `wp_viewporter` (`set_source` ignored at scale 1, `set_destination` stored as
  the on-screen extent), `wl_subcompositor` (`get_subsurface` / `set_position`,
  composited relative to the parent), `wp_presentation` (advertises
  `clock_id` = `CLOCK_MONOTONIC`, replies `presented` to a feedback request).
- `zxdg_decoration_manager_v1`: answers `server_side` so clients skip their own
  (client-side) title bars — displayd draws them.
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

**`set_minimized` now unmaps the window and surfaces it as a taskbar button** in
the top panel (between the menu bar and the clock). Clicking a button restores +
raises + focuses the window; clicking a non-minimized window's button raises it.

### Absent globals / protocols

- **`zwp_linux_dmabuf_v1`**: advertised (it announces the `ARGB8888` /
  `XRGB8888` formats and accepts `create_params`), but buffer import is
  **honestly rejected** — `create()` replies `failed` and `create_immed()` is
  dropped, because b1nix has no GEM/dmabuf importer. GPU buffers reach the host
  via `/dev/virtio-gpu` outside Wayland (see M53). This is a real limitation, not
  a fake stub: a conforming client sees the rejection and falls back to SHM.
- **HiDPI / transform**: fixed at scale 1, normal transform. `set_buffer_scale`,
  `set_buffer_transform`, `set_opaque_region`, `set_input_region`, surface
  `offset` are accepted and ignored. `wp_viewport.set_source` (crop) is likewise
  a recognised no-op (no scaling at 1x).
- **Subsurface stacking / sync**: `wl_subsurface.place_above` / `place_below` /
  `set_sync` / `set_desync` are recognised no-ops — a subsurface composites in
  commit order above its parent, which is enough for a single overlay child.

### Fixed capacities

`MAX_CLIENTS` / `MAX_SURFACES` / `MAX_BUFFERS` = 32, `MAX_TOPLEVELS` = 16,
`MAX_WOBJECTS` = 256, `MAX_WPOOLS` = 32. Raise the `#define`s if a workload needs
more; nothing else assumes the value.

## Architecture notes

These are deliberate, evaluated design choices — both were investigated as
possible rewrites and both came out **net-negative** (they would degrade a
green, working compositor for zero functional gain). Keep them.

- **`wl_shm` lives in displayd, not in the ported `libwayland-server`.**
  Investigated: not cleanly feasible and not worth it. The real blockers (the
  old "pthread-TLS / SIGBUS ABI" note is stale — M29 delivered pthreads + TLS):
  (1) b1nix has no **SIGBUS-on-beyond-EOF** semantics — a file/memfd mapping
  past `i_size` silently zero-fills rather than faulting, and the upstream
  `wl_shm` keys its crash-guard on `SIGBUS` + `si_addr`; (2) `wayland-shm.c` is
  not even compiled into the ported `libwayland-server.a`, and adopting it pulls
  in the whole `wl_display`/`wl_event_loop`/`wl_resource` object model — a
  ~2,400-LOC rewrite of the standalone wire server. displayd's own
  `wl_create_buffer` already validates buffer extent against `pool->size`, so it
  does not need the SIGBUS net for correctness. (Adding SIGBUS-on-EOF is a
  worthwhile *independent* kernel mm task if other ported software ever needs
  it — decoupled from Wayland.)
- **Hand-rolled wire marshalling, not `wayland-scanner` codegen.** Investigated:
  `wayland-scanner` only emits the `wl_resource`/listener model, so adopting its
  generated glue means adopting the full `libwayland-server` runtime (same as
  above). Measured cost: ~9,200 generated LOC + a codegen build step + vendored
  protocol XML + a `libwayland-server.a` link + high rewrite-regression risk —
  for zero functional gain. The hand-rolled dispatch keeps the server one tiny
  file. The only real downside is opcodes documented by comment rather than a
  generated header; if that ever matters, generating *just* an enum/opcode
  header (no runtime) is the cheap fix — the full dispatch rewrite is not.
