# M52 — Mesa and Accelerated OpenGL

Goal: run an EGL/OpenGL Wayland application on b1nix, software-rendered, with a
path toward VirGL hardware acceleration.

## What shipped (software EGL/OpenGL)

The verifiable core of the milestone — an unmodified EGL/OpenGL app rendering
into a Wayland surface with a software renderer — is done and green on both
arches.

- **TinyGL** (C-Chads fork, software OpenGL 1.1 subset) ported as a freestanding
  static `libTinyGL.a`. Built by `tools/build-tinygl.sh`, the same curated-subset
  / freestanding-clang pattern as the M51 libraries (no CMake/Make from the
  port). Renders into an in-memory `ZBuffer` (`ZB_MODE_RGBA`, packed
  `0x00RRGGBB`). Multithreaded raster paths disabled (single-threaded smoke, no
  thread-runtime dependency).
- **b1nix EGL 1.4 shim** (`userspace/libegl/b1egl.c`, header
  `userspace/include/EGL/egl.h`) → `libEGL.a`. A thin, real implementation (not a
  stub) binding EGL windowing to TinyGL + b1gui:
  - `eglCreateWindowSurface(win)` opens a `ZBuffer` sized to the b1gui window.
  - `eglMakeCurrent` binds the single global TinyGL context (`glInit`).
  - `eglSwapBuffers` blits TinyGL's framebuffer into the live `wl_shm` buffer
    (adding opaque alpha), `b1gui_present`s it, and waits for displayd's frame
    callback — a synchronous swap.
  - `eglB1nixAccelerated()` reports software vs GPU path.
- **End-to-end app** `userspace/bin/m52_gl_smoke.c`: sets up an EGL window
  surface on a b1gui window, clears to blue, draws a red triangle through the
  real GL pipeline, swaps, then **inspects the presented `wl_shm` buffer** and
  requires ≥100 blue-background and ≥100 red-triangle pixels before emitting any
  `ok`. No marker is faked — every `ok` is gated on real rasterized pixels.

Markers (checked by `tests/smoke.sh`): `M52-GFX: ok egl`, `ok tinygl`,
`ok gl-triangle`, `ok path-software`.

The software-rendered frame reaches a real GPU when displayd's framebuffer is
the VirtIO-GPU 2D scanout (`kernel/dev/virtio_gpu.c`, from M50:
RESOURCE_CREATE_2D / ATTACH_BACKING / SET_SCANOUT / TRANSFER_TO_HOST_2D /
RESOURCE_FLUSH).

## VirGL 3D acceleration (kernel transport — implemented)

`kernel/dev/virtio_gpu.c` drives the VirtIO-GPU 3D / VirGL command set against a
virglrenderer-backed host device. On init it inspects the device feature bits
and, when `VIRTIO_GPU_F_VIRGL` (bit 0) is offered, negotiates it and runs a
selftest that performs a real GPU render and reads the result back:

1. `GET_CAPSET_INFO` / `GET_CAPSET` — confirms the host virglrenderer backend is
   live and reports the VIRGL capset (`capset_id=1`, `max_size=308` here).
2. `CTX_CREATE` — a 3D context.
3. `RESOURCE_CREATE_3D` — a 64×64 `B8G8R8A8_UNORM` render target
   (`RENDER_TARGET | SAMPLER_VIEW`), `RESOURCE_ATTACH_BACKING` of guest pages,
   `CTX_ATTACH_RESOURCE`.
4. `SUBMIT_3D` — a hand-built virgl command stream, byte-compatible with what
   Mesa's `src/virtio/virtio-gpu/virgl_protocol.h` emits: `CREATE_OBJECT`
   (`VIRGL_OBJECT_SURFACE`) wrapping the resource, `SET_FRAMEBUFFER_STATE`
   binding it, and `CLEAR` to `(R,G,B,A) = (0.25, 0.5, 0.75, 1.0)`. Float clear
   colours are passed as precomputed IEEE-754 bit patterns — the kernel uses no
   FPU.
5. `TRANSFER_FROM_HOST_3D` — copies the GPU-rendered surface back to guest
   memory; the read-back BGRA pixel is `0xff4080bf` (B=191, G=128, R=64,
   A=255), the exact requested colour, proving the host GPU executed the clear.

Markers: `M52-GFX: ok virgl-negotiate / ok virgl-capset / ok virgl-3d-clear /
ok path-accelerated`. This is the accelerated *transport* — a guest virgl
command stream rendered by host GL on real hardware — verified end-to-end.

### Host requirement (per-host capability, not a b1nix limit)

VirGL needs a QEMU built `--enable-virglrenderer` with a GL-capable display
backend. The Linux/KVM dev host has all of it:

```
qemu-system-x86_64 -device help    | grep virtio-gpu-gl   # virtio-gpu-gl-pci
qemu-system-x86_64 -display help   | grep egl-headless     # egl-headless
ls /dev/dri/renderD128                                      # AMD Radeon render node
libvirglrenderer.so.1                                       # virglrenderer 1.3.0
```

`tests/smoke.sh` auto-detects this (gl device + `egl-headless` + a DRM render
node) and gives only the graphics smoke instance
`-device virtio-gpu-gl-pci -display egl-headless`. On a host without it (e.g.
Homebrew QEMU on macOS, built without virglrenderer) the graphics instance falls
back to the plain 2D `virtio-gpu-pci`, the kernel selftest is a clean no-op
(emits no markers), and the smoke records the VirGL check as a **skip** — a real
host limitation, not a fake pass. The software OpenGL/Mesa path is the verified
path there.

## Real Mesa (OSMesa softpipe) + GLSL shaders — shipped

Real upstream **Mesa 24.0.9** (OSMesa + Gallium **softpipe**, no LLVM) is ported
via a meson cross-build (`tools/build-mesa.sh`). Two demos exercise it:

- `m52_osmesa` — drives the unmodified OSMesa API through softpipe, renders a 3D
  triangle off-screen, pixel-verifies it, and presents to displayd:
  `M52-GFX: ok mesa-context / mesa-render / mesa`.
- `m52_glsl` — compiles and links a `#version 120` vertex+fragment shader pair
  through Mesa's real GLSL compiler, draws an interleaved VBO
  (`glBindAttribLocation` + `glDrawArrays`), and pixel-verifies the Gouraud
  triangle softpipe produces: `M52-GFX: ok shader-compile / shader-link /
  shader-render / glsl`.

Both are green on both arches (x86_64 and i686) under KVM. The remaining layer
is the Mesa gallium `virgl` driver itself (Mesa rendering *through* the VirGL
transport above, via a libdrm/virtgpu winsys) — that gives full
hardware-accelerated OpenGL/GLES inside b1nix and is the natural M53 follow-on.
