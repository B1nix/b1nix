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

## VirGL acceleration — environment constraint

VirGL 3D acceleration over VirtIO-GPU is **blocked by the host emulator**, not by
b1nix. The dev/CI QEMU here is Homebrew **QEMU 11.0.0 on macOS**, built without
virglrenderer:

```
$ qemu-system-x86_64 -device help | grep virtio-gpu
  name "virtio-gpu-pci"   # no virtio-gpu-gl / virtio-vga-gl
$ qemu-system-x86_64 -device virtio-gpu-pci,help | grep -i virgl
  (nothing — no virgl property)
$ qemu-system-x86_64 -display help
  none curses cocoa dbus   # no gtk/sdl gl=on, no egl-headless
```

VirGL needs a QEMU compiled `--enable-virglrenderer` plus a GL-capable display
backend (`egl-headless`, `gtk gl=on`, or `sdl gl=on`). With none available,
accelerated rendering cannot be exercised or verified on this machine, so — per
the project's NO-FAKE-PASSES rule — no green `virgl` marker is emitted.

What is honest and testable here: the EGL backend probes the
`VIRTIO_GPU_F_VIRGL` capset and **falls back to software** when it is absent (the
path that runs on this QEMU, verified by `ok path-software`). The 3D command
path (CTX_CREATE / RESOURCE_CREATE_3D / GET_CAPSET / SUBMIT_3D) is implemented
behind that probe and only activates on a virgl-capable QEMU.

To exercise acceleration, run under a QEMU with virgl, e.g.:

```
qemu-system-x86_64 -device virtio-vga-gl -display egl-headless ...
```

## Real Mesa (swrast/llvmpipe)

Planned. The TinyGL path already satisfies the milestone's "unmodified
EGL/OpenGL Wayland app, software fallback" requirement. Real Mesa adds the
GLES2/shader API surface via a meson cross-build (everything else in b1nix
bypasses meson, so this is the largest remaining piece).
