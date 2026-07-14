# Skia + Ganesh Port to b1nix — PORT-PLAN

## Overview

Port the full Skia graphics library (M132+) with the Ganesh GPU backend to
b1nix as a standalone static library (`libskia.a`). This is the "deferred
Software Skia (Ganesh) raster fallback" from M59 in the roadmap.

Skia is Google's 2D graphics library used by Chrome, Android, Flutter.
Ganesh is the legacy GPU-accelerated backend (the newer one is Graphite).

## Architecture

```
SkCanvas → SkGpuDevice → GrDirectContext → GrGLInterface → EGL/OSMesa softpipe
                    └→ SkRasterPipeline → CPU fallback (no EGL)
```

- **Raster path:** Software CPU rasterizer (always available, no GPU needed)
- **GPU path:** Ganesh GL backend via b1nix's EGL 1.4/1.5 over Mesa OSMesa
- **Build:** GN + Ninja (Skia's only supported build system)

## Key Technical Decisions

1. **Standalone build** (not inside Chromium). Like the V8 d8 port — own GN
   project, own toolchain file, produces `libskia.a`.

2. **Reuse `is_linux` alias** (Chromium Patch C1). Skia's `#ifdef __linux__`
   guards select POSIX threading, file I/O, and GL backend. The `__linux__`
   define from the Chromium `//build` already makes b1nix invisible to Skia.

3. **Ganesh GL via OSMesa softpipe.** b1nix has EGL 1.4/1.5 over Mesa OSMesa.
   Skia's `GrGLInterface` wraps GL function pointers resolved through
   `eglGetProcAddress`.

4. **Clang cross-compiler** (M89/M90). b1nix is now Clang-first. Skia builds
   best with Clang.

5. **Bundled third-party deps.** Use Skia's own zlib/libpng/libjpeg-turbo/expat
   to avoid header conflicts.

## Patches

| Patch | File | What |
|-------|------|------|
| S1 | `//build/config/BUILDCONFIG.gn` | Add `b1nix` toolchain dispatch |
| S2 | `//build/toolchain/b1nix/BUILD.gn` | GN toolchain definition (Clang cross) |
| S3 | `include/private/base/SkFeatures.h` | `SK_OS_B1NIX` / `SK_POSIX` detection |
| S4 | `BUILD.gn` | Force-disable Vulkan/Metal/D3D/X11/Wayland |
| S5 | `src/gpu/ganesh/gl/GrGLMakeNativeInterface.cpp` | b1nix EGL proc resolver |
| S6 | `src/ports/` | b1nix platform files |
| S7 | `third_party/zlib/` | Compat fixes |
| S8 | `gn/skia/BUILD.gn` | Disable tools/tests |

## GN Build Args

```gn
target_os = "b1nix"
target_cpu = "x64"
is_official_build = true
is_clang = true
skia_use_gl = true
skia_use_egl = true
skia_enable_gpu = true
skia_use_vulkan = false
skia_use_metal = false
skia_use_direct3d = false
skia_use_x11 = false
skia_use_wayland = false
skia_use_system_*=false   # bundled deps
skia_enable_tools = false
skia_enable_skottie = false
```

## Dependencies

- Mesa OSMesa/EGL (M52/M59)
- libc++ shared (M89)
- Clang cross-toolchain (M90)
- pthreads (M29)

## Verification

1. `build-skia.sh` produces `libskia.a`
2. Smoke test: raster draw + GPU draw via EGL/OSMesa
3. Full graphics-smoke.sh passes with M91-SKIA markers
