# b1nix Mesa patches (applied by tools/build-mesa.sh)

Mesa is built from an upstream tarball extracted into `build/ports-src/` (a
git-ignored, regenerable tree). Any change to the Mesa source must live HERE,
in the b1nix repo, so it is reproducible on every host — never edit the
extracted tree directly (those edits vanish on re-extract / `make clean`).

`tools/build-mesa.sh` applies, right after extraction and idempotently:

- `*.patch` — unified diffs against the Mesa source root, applied with
  `patch -p1` (skipped automatically if already applied). Use for changes to
  existing upstream files (e.g. `meson.build` to enable the virgl driver/winsys).
- `files/...` — whole b1nix-owned files copied verbatim into the Mesa tree at
  the mirrored path. Use for NEW source (e.g. the b1nix `/dev/virtio-gpu` virgl
  winsys, which replaces the libdrm-based `virgl_drm_winsys.c`).

## M53 variant B (Mesa-through-VirGL) plan

The Mesa gallium `virgl` driver needs a winsys. Upstream's
`src/gallium/winsys/virgl/drm/virgl_drm_winsys.c` talks to a Linux DRM
virtio-gpu node via libdrm; b1nix has neither. The b1nix winsys (added here as
`files/...` + a meson `.patch`) maps the same operations onto the
`/dev/virtio-gpu` ioctls in `userspace/include/b1nix/virgl.h`:

  CONTEXT_INIT, GET_CAPS/GET_CAPS_DATA, GETPARAM, RES_CREATE, RES_INFO,
  SUBMIT (=EXECBUFFER), TRANSFER_TO_HOST, TRANSFER_FROM_HOST, WAIT, mmap.

A GEM "handle" maps 1:1 to a b1nix res_id; PRIME/dma-buf/blob paths are dropped.
Then build-mesa.sh flips `-Dgallium-drivers=swrast` -> include `virgl`, and
libEGL/OSMesa select the virgl pipe screen instead of softpipe.
