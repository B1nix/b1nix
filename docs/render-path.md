# The accelerated render path

Hardware rendering is a path *beside* the software one, never a replacement.
Acceleration is claimed only when a render node opens, a Mesa DRI driver is
present, and `gl_probe` draws a shader triangle on it and reads the pixels back.
Otherwise the compositor starts on pixman.

## How it fits together

- **DRM node.** The virtio GPU is served by the imported DRM core
  (`kernel/lkpi/drm_b1nix_kms.c`). It reports the driver name `virtio_gpu` —
  Mesa's loader turns that into `virtio_gpu_dri.so`. Its parent is a real
  `struct pci_dev` carrying virtio-gpu's bus address and `1af4:1050`; code that
  publishes a PCI identity checks the parent really is a PCI function rather
  than `container_of`-ing whatever it is.
- **Master lease.** The shim's `capable()` asks b1nix's credentials, translating
  Linux capability numbers (`CAP_SYS_ADMIN` is 21 in Linux, 20 in b1nix —
  `kernel/lkpi/env.c`), so `DRM_IOCTL_SET_MASTER` works for root.
- **virtgpu adapter.** Mesa's virgl winsys speaks `DRM_IOCTL_VIRTGPU_*`
  (`GETPARAM`, `GET_CAPS`, `RESOURCE_CREATE`, `MAP`, `EXECBUFFER`,
  `TRANSFER_TO/FROM_HOST`, `WAIT`, `GEM_CLOSE`) with upstream's
  `virtgpu_drm.h` layouts. The adapter puts b1nix's own VirGL transport
  (`kernel/dev/virtio_gpu.c`, `B1NIX_VIRGL_*`) behind those ioctls, each
  resource backed by a GEM object. ABI details that bite:
  - `GETPARAM` writes a 4-byte `int` *through* `value` (a user pointer).
  - Capsets are looked up by id, not index (VIRGL2 is index 1).
  - Control requests may exceed a page (the buffer is 16 pages).
  - Submission is synchronous, so `VIRTGPU_EXECBUF_FENCE_FD_OUT` returns an
    already-signalled fence; unknown flags are refused.
  - A DRM-created resource does not own its frames (`vgpu_res_unref_id` frees
    only the character device's contiguous runs).

## Flags

| Flag | Effect |
|---|---|
| `b1nix.no-virgl-drm` | force the adapter off (otherwise on when `lkpi_virgl_available()` — a virtio-gpu without virglrenderer reports "no 3D" and falls back cleanly) |
| `b1nix.virgl-trace` | print each virtgpu ioctl and its answer |
| `b1nix.render-smoke` | run `/etc/render-smoke.sh` on a non-smoke boot |

## Tests

`M101T-DRM` (open, version, `pci-identity`, the session sequence
`stat`/`open(O_NOCTTY|O_NONBLOCK)`/set-master/drop-master, modeset ioctls) runs
in the ordinary suite. `/etc/render-smoke.sh` on the gfx instance proves each
composition path by having sway paint two colours and reading them back via
wlr-screencopy:

- `RENDER-SMOKE: ok software-frame` — pixman
- `RENDER-SMOKE: ok accel-frame` — GLES on the render node
- `RENDER-SMOKE: ok fallback-engaged` — acceleration forced off, still paints
- `RENDER-SMOKE: ok selection` — the runtime choice, recorded in `/run/render-selection`

The ordinary image carries no DRI driver (mesa-dri-gallium is 184 MB), so the
default suite reports `accel-frame` as a skip with its reason. To run the
accelerated path on a host GPU through virglrenderer:

```sh
make B1NIX_GPU_DRV=1 iso-gfx
B1NIX_GPU_DRV=1 SKIP_BUILD=1 SMOKE_INSTANCES=gfx GPU_DEVICE=virtio-gpu-gl-pci \
    GPU_DISPLAY=egl-headless sh tests/smoke.sh x86_64
```
