# Browser Platform — Implementation Notes

Detailed implementation notes for M52 (Mesa/OpenGL) and M53 (NetSurf Browser).
High-level status is in [`roadmap.md`](roadmap.md).

## M52: Mesa and Accelerated OpenGL

Software EGL/TinyGL plus Mesa OSMesa softpipe, GLSL shaders, and VirGL 3D
hardware acceleration (host virtio-gpu-gl). VirGL needs a host with
virglrenderer; the smoke auto-detects it and honestly skips elsewhere.

- [x] `done` Software OpenGL + EGL: ported TinyGL (software GL 1.1 subset) as
  `libTinyGL.a` and a b1nix `libEGL.a` shim (`userspace/libegl/b1egl.c`,
  `userspace/include/EGL/egl.h`). `eglCreateWindowSurface` targets a b1gui
  wl_shm window; `eglSwapBuffers` blits the rendered framebuffer and commits to
  displayd. End-to-end app `m52_gl_smoke` clears + draws a 3D triangle through
  the real GL pipeline and verifies the presented pixels.
  `M52-GFX: ok egl/tinygl/gl-triangle/path-software`. Both arches green.
- [x] `done` Software renderer presented through the VirtIO-GPU 2D scanout
  (`kernel/dev/virtio_gpu.c`, RESOURCE_CREATE_2D / SET_SCANOUT /
  TRANSFER_TO_HOST_2D / RESOURCE_FLUSH, from M50) via displayd.
- [x] `done` VirGL 3D acceleration over VirtIO-GPU. `kernel/dev/virtio_gpu.c`
  negotiates `VIRTIO_GPU_F_VIRGL` with a virglrenderer-backed host device,
  queries the VIRGL capset, creates a 3D context + render-target resource, and
  submits a hand-built virgl command stream (CREATE_OBJECT SURFACE +
  SET_FRAMEBUFFER_STATE + CLEAR — byte-compatible with Mesa's
  `virgl_protocol.h`) via `SUBMIT_3D`. It then `TRANSFER_FROM_HOST_3D`s the
  GPU-rendered pixels back to guest memory and verifies them: the host GPU
  renders the clear and the read-back BGRA pixel matches the requested colour
  exactly (`0xff4080bf`). `M52-GFX: ok virgl-negotiate/virgl-capset/
  virgl-3d-clear/path-accelerated`. Exercised on the Linux/KVM host
  (QEMU 11 `-device virtio-gpu-gl-pci -display egl-headless`, virglrenderer
  1.3.0, AMD Radeon via `/dev/dri/renderD128`); the smoke harness auto-detects
  the virgl-capable device and the selftest is a clean no-op (recorded as a
  skip, software path still verified) on hosts without it (e.g. macOS QEMU
  built without virglrenderer). Full Mesa-on-virgl (gallium `virgl` driver +
  libdrm winsys) is the remaining layer above this transport.
- [x] `done` Port **real upstream Mesa** (OSMesa + Gallium **softpipe**, no
  LLVM) via a meson cross-build (`tools/ports/build-mesa.sh` + `b1nix-mesa-cc` +
  `enable-cxx-toolchain.sh`). `m52_osmesa` drives the unmodified OSMesa API
  (`OSMesaCreateContext`/`MakeCurrent`) through the softpipe rasterizer, renders
  a 3D triangle off-screen, pixel-verifies it, and presents to displayd:
  `M52-GFX: ok mesa-context/mesa-render/mesa`. **Verified on both arches**
  (x86_64 625/0, i686 624/0, single-CPU + `-smp 4`) under KVM.
  - Required converting **both kernels to higher-half** so the ~12 MB Mesa demo
    (embedded in the initramfs) no longer pushes the kernel image over the
    0x2000000 userspace base and into userspace's address range — see the
    higher-half kernel item under M2 (Memory Management). Plus an ELF
    loader shared-page fix, main-thread TLS, a virtio-gpu TSC-bounded wait, and
    a libc/toolchain round (memalign, gthr-posix libstdc++, open_memstream, ...).
- [x] `done` Programmable GLSL shader pipeline on top of Mesa. `m52_glsl`
  compiles and links a `#version 120` vertex+fragment shader pair through the
  real Mesa GLSL compiler, draws an interleaved VBO with
  `glBindAttribLocation`/`glDrawArrays`, and pixel-verifies the resulting
  Gouraud (per-vertex-interpolated) triangle rendered by softpipe before
  presenting to displayd: `M52-GFX: ok shader-compile/shader-link/
  shader-render/glsl`. Verified on both arches (x86_64 629/0, i686 628/0).
  (Full EGL + GLES2 ES contexts need Mesa's EGL/gbm stack, which rides on the
  VirGL/libdrm work; desktop GLSL shaders are the verifiable surface today.)

## M53: Browser Platform

Target browser: **NetSurf** (small, modular, pure-C, framebuffer/Wayland
frontend). Building the prerequisite codec + runtime stack first, each with a
no-fake-pass smoke test.

- [x] `done` Image + video-keyframe codecs (NetSurf loader dependencies), all
  freestanding-compiled against the b1nix userspace ABI and verified by
  encode/decode roundtrips with pixel checks (`tests/smoke.sh`):
  - **zlib** 1.3.1 (`tools/ports/build-zlib.sh`) — one-shot + streaming deflate/inflate
    + crc32. `M53-ZLIB: ok compress/uncompress/roundtrip/crc32/stream`.
  - **libpng** 1.6.43 (`tools/ports/build-libpng.sh`, over zlib + libm) — PNG
    encode→decode, pixels byte-for-byte identical. `M53-PNG: ok
    encode/decode-header/decode`.
  - **libjpeg** (IJG v9f, `tools/ports/build-libjpeg.sh`) — JPEG encode→decode, pixels
    within tolerance. `M53-JPEG: ok encode/decode-header/decode`.
  - **libwebp** 1.4.0 (`tools/ports/build-libwebp.sh`) — WebP lossless encode→decode
    byte-identical; WebP lossy is a VP8 intra (keyframe) decoder, the WebM video
    bitstream family. `M53-WEBP: ok encode/info/decode`.
  - Added the standard `*_10_EXP` macros to `userspace/include/float.h` (libpng
    gamma math needs them).
- [x] `done` Full-motion video codec: **libvpx** VP8 decode (the WebM / browser
  video codec). `tools/ports/build-libvpx.sh` runs libvpx's own configure for the
  portable `generic-gnu` target to generate its `vpx_config.h` + `*_rtcd.h`
  headers, then recompiles the VP8-decode C sources with the b1nix toolchain.
  `m53_libvpx_smoke` cross-verifies against libwebp: it encodes a lossy WebP
  (which *is* a VP8 keyframe), pulls the raw VP8 bitstream from the RIFF "VP8 "
  chunk, decodes it with libvpx to an I420 frame, and checks the luma plane
  reproduces the original within tolerance. `M53-VPX: ok
  webp-vp8-frame/decode-init/decode/luma`.
- [x] `done` Mesa **through VirGL** — host-GPU-accelerated OpenGL.
  - [x] `done` Kernel exposes the VirGL 3D transport (M52) to userspace via
    `/dev/virtio-gpu` (ioctls: GET_CAPS, RES_CREATE + mmap window, SUBMIT a virgl
    command stream, TRANSFER_FROM_HOST; a single implicit 3D context). `m53_virgl_smoke`
    creates a render target, submits a virgl CLEAR **from userspace**, and reads
    the GPU-rendered pixel back through the mmap: `M53-VIRGL: ok
    caps/resource/submit/path-accelerated`. Same kernel/userspace split a Mesa
    virgl winsys uses; auto-skips on non-virgl hosts.
  - [x] `done` A Mesa gallium `virgl` winsys on this ABI + Mesa rebuilt with
    `-Dgallium-drivers=swrast,virgl`, so the full OpenGL API runs on the host
    GPU (vs softpipe). The b1nix winsys
    (`tools/patches/mesa/files/src/gallium/winsys/virgl/b1nix/`, installed into
    the Mesa tree by `tools/ports/build-mesa.sh`) implements `struct virgl_winsys`
    over the `/dev/virtio-gpu` ioctls instead of libdrm — a b1nix res_id IS the
    host resource id (no GEM layer), SUBMIT/TRANSFER are synchronous so fences
    are trivial. `build-mesa.sh` drops the driver's vestigial libdrm dep, stubs
    the build-id/disk-cache + vl-video/driconf bits the minimal build omits, and
    enables the virgl gallium driver. `m53_mesa_virgl` drives the gallium pipe
    API (screen → context → render target → `set_framebuffer_state` + `clear` →
    `texture_map`); Mesa encodes the virgl command stream, submits it to the
    host virglrenderer over `/dev/virtio-gpu`, and the cleared pixels read back
    exactly: `M53-GFX: ok gl-accelerated`. The demo now also drives the **full
    draw pipeline** — vertex/fragment TGSI shaders, a vertex buffer, vertex
    elements, rasteriser/blend/DSA state and `draw_vbo` rasterise a triangle on
    the host GPU (`M53-GFX: ok gl-triangle`, centre pixel red, corner black).
    This required `RES_UNREF` + screen/resource teardown (the winsys frees the
    device's 16 res slots, no leak), raising the kernel SUBMIT staging buffer
    (1 page → 64 KiB) for the larger shader/draw streams, and building the demo
    with `-DHAVE_FUNC_ATTRIBUTE_PACKED=1` so its `pipe_draw_info` layout matches
    Mesa's (the enum-packing ABI). Both arches (x86 749/0, x86_64 750/0).
- [ ] `planned` Runtime gaps: robust pthread, futex, TLS (mostly done in M29),
  real dynamic loading (`dlopen` of `.so` — currently a stub), ICU.
- [x] `done` Port NetSurf's own libraries and the framebuffer frontend, and
  **render a real page**. The full dependency chain is ported and freestanding-
  compiled against the b1nix ABI, each with a no-fake-pass smoke:
  **libwapcaplet** (string internment, `M53-WAPCAPLET`), **libparserutils**
  (input + bundled charset codecs, `M53-PARSERUTILS`), **libhubbub** (HTML5
  tokeniser, `M53-HUBBUB`), **libcss** (CSS parse + cascade/selection,
  `M53-LIBCSS`), **libdom** (DOM via the hubbub binding, `M53-LIBDOM`),
  **libnsutils/libnsgif/libnsbmp/libnslog** (`M53-NSUTILS/NSGIF/NSBMP/NSLOG`),
  and **libnsfb** (framebuffer surface + plotters). The complete **NetSurf
  framebuffer browser** is then cross-built for b1nix with its native build
  system driven by the b1nix cross-gcc (`tools/ports/build-netsurf-fb.sh`), packaged
  into the initramfs with its resources + a test page
  (`tools/images/gen_netsurf_initramfs.sh`), and a headless `-T` render self-test loads
  a local `file://` HTML page (styled text + a PNG image), lays it out
  (extents 782x552) and paints it into a framebuffer (117849 non-background
  pixels verified). `M53-NS: ok load/redraw/render`. Runtime note: the file://
  fetcher uses the `fread()` path (`HAVE_MMAP` off — b1nix can't mmap an
  initramfs object).
- [x] `done` **Web access** — NetSurf fetches pages over a real TCP/HTTP
  connection through libcurl, not just `file://`. libcurl is enabled with
  genuine cookie, zlib (gzip) and MIME support (no stubbed/skipped options) and
  staged into the build via a `libcurl.pc`; `NETSURF_USE_CURL=YES`. A minimal
  in-VM loopback HTTP server (`m53_httpd`) serves a styled page, NetSurf fetches
  `http://127.0.0.1:8080/` over the network stack and renders it
  (`M53-HTTPD: ready`, `M53-WEB: has-content=1 / ok render`). The render
  self-test advances time with `nanosleep` (kernel tick sleep) rather than
  spinning on `gettimeofday`.
- [x] `done` **HTTPS** — NetSurf fetches over a real TLS 1.2 connection with
  certificate verification. A loopback HTTPS server (`m53_httpsd`, mbedTLS, using
  the M32 test PKI with SAN IP:127.0.0.1) serves a styled page; NetSurf fetches
  `https://127.0.0.1:8443/` with `--ca_bundle` pointed at the test CA, so
  libcurl (mbedTLS) verifies the server certificate. `M53-HTTPS: has-content=1 /
  ok render`. libcurl is built with genuine cookie + zlib + MIME support.
- [x] `done` **On-screen frontend** — NetSurf draws straight to the real
  hardware framebuffer, not just an off-screen buffer. A b1nix `/dev/fb0` libnsfb
  surface (`-f b1nix`) opens the M47 fb device, mmaps it and flushes damage via
  `B1NIX_FBIOFLUSH`; NetSurf lays out and paints a page at the real screen
  resolution (1280x800) and presents it on the virtio-gpu display.
  `M53-FB: ok render`.
- [x] `done` **Public-internet HTTPS** — NetSurf fetches a real public website
  (`https://example.com/`) over off-link TLS and renders it, verifying the cert
  against the shipped Mozilla CA bundle (the same libcurl/mbedTLS path M32's
  `ext-https` uses). It is optional: it skips cleanly (`M53-EXT-HTTPS:
  unsupported`) when the usernet has no off-link route, so the offline smoke
  stays green; with outbound enabled it fetches and paints the full page.
- [x] `done` **TCP zero-window stall fixed** — a multi-host HTTPS page (e.g.
  google.com) used to take ~20 s because each fresh connection whose TLS cert
  flight overflowed the 4 KiB receive buffer advertised a zero window and then
  sat through the peer's persist-timer backoff (~5 s per connection). The kernel
  TCP stack now sends an unsolicited window-update ACK when the app drains a
  throttled receive buffer (`kernel/net/tcp.c`), and the receive buffer/window
  grew 4 KiB → 16 KiB. Measured against a real page: a fresh-host TLS handshake
  dropped 5.5 s → 60 ms and full google.com load ~20 s → ~1.1 s.
- [x] `done` **Heavy-page memory robustness.** A real google.com tab with JS can
  exhaust RAM; that exposed kernel OOM-path bugs. Fixed: (1) `freelist_pop`
  validates each node before dereferencing it, so a freed-frame corruption
  degrades to a logged recovery (bitmap scan) instead of a GP-fault **panic**;
  (2) the page-eviction/swap path issues a cross-CPU **TLB shootdown**
  (`tlb_shootdown_page`) after marking a page swapped — the local `invlpg` left
  another CPU's stale TLB mapping a freed frame (the use-after-free behind the
  panic); (3) a last-resort **OOM-killer** SIGKILLs the userspace task demanding
  the memory (sparing kernel threads and init) instead of returning ENOMEM into
  a console-flooding retry storm; (4) the OOM diagnostic is throttled to once per
  pressure episode. `make run` now defaults to `-m 1024` (QEMU's 128 MB default
  OOM'd on heavy pages) — google.com loads and renders at 1 GB. Verified: forced
  256 MB cleanly OOM-kills the memory-heavy tests (`[OOM-KILL] killing ...`) with
  no panic; both arches stay green.
- [x] `done` **SVG images + JavaScript + public-suffix list.** SVG is decoded by
  **libsvgtiny** (`tools/ports/build-libsvgtiny.sh`, over libdom's expat XML binding)
  and `NETSURF_USE_NSSVG=YES`; the framebuffer frontend's `plot->path` (an
  upstream no-op stub) is given a real polygon-fill/stroke implementation so SVG
  actually paints. **JavaScript** runs via the compiled-in Duktape engine with
  the `enable_javascript` option flipped on. The **public-suffix list**
  (**libnspsl**, `tools/ports/build-libnspsl.sh`, `NETSURF_USE_NSPSL=YES`) scopes
  cookies to a registrable domain. The render self-test loads a page with an
  `<img>` SVG (solid green block) and a script that paints a solid blue block,
  and asserts all three colours appear in the framebuffer: `M53-NS: ok svg /
  ok js / ok jxl`.
  Also enabled: **RISC-OS sprite** decoding (**librosprite**,
  `tools/ports/build-librosprite.sh`, `NETSURF_USE_ROSPRITE=YES`), **utf8proc**
  (`tools/ports/build-libutf8proc.sh`, `NETSURF_USE_UTF8PROC=YES`) for IDNA Unicode in
  `utils/idna.c`, and **JPEG-XL** (**libjxl** 0.11.1 + highway + brotli + skcms,
  `tools/ports/build-libjxl.sh` — a CMake cross-build with the b1nix C++ toolchain,
  decode-only, `NETSURF_USE_JPEGXL=YES`). Enabling libjxl required completing the
  C++ toolchain: `userspace/include/math.h` filled out to full C99 (erf/lgamma/
  tgamma/long-double variants, double_t/float_t) with the classification
  helpers made C++-safe, and `enable-cxx-toolchain.sh` turning on libstdc++'s
  C99 math/stdint/fenv feature macros (the toolchain had been built against an
  empty sysroot) — HarfBuzz and Mesa re-verified against the updated toolchain.
  A real `<img>` JXL (magenta block) is decoded and asserted: `M53-NS: ok jxl`.
  Still off: **OpenSSL** (redundant — mbedTLS is the TLS backend); **PDF export**
  (the **libharu** library *is* ported — `tools/ports/build-libharu.sh` builds
  `libhpdf.a` — but NetSurf 3.11's PDF glue is upstream bit-rot: `font_haru.c`
  needs the removed `desktop/font.h` and a `struct font_functions` model that
  `print_make_settings` no longer uses, so enabling it needs that dead code
  revived, not just a flag); and **video** (GStreamer — a whole multimedia
  framework on GLib/GObject with a dlopen plugin model, not a self-contained
  codec; the realistic path is a custom libvpx-based content handler). All
  *browsing* formats (every image codec incl. JPEG-XL, JS, HTTPS, Unicode, PSL)
  are now enabled.
- [x] `done` **Wayland frontend** — NetSurf runs as a windowed client of the
  b1nix display server (displayd) over the b1display/libgui (Wayland-shaped)
  protocol. A new libnsfb "displayd" surface gives NetSurf a compositor window
  to paint into, presents damage with b1gui_present, and translates compositor
  pointer/keyboard events to libnsfb events. `M53-WL: ok render`.
- [x] `done` **Interactive input** — the framebuffer frontend is interactive:
  the /dev/fb0 libnsfb surface reads /dev/input/event* (keyboard + mouse) and
  feeds pointer motion, mouse buttons and keysyms into NetSurf's event loop. A
  self-test (-I) confirms synthesized move + click + key events all arrive
  (`M53-INPUT: ok mouse-move/mouse-click/key`). Both libnsfb surfaces (`b1nix`
  and `displayd`) share one scancode→keysym map (`libnsfb-b1keymap.h`) that is
  shift-aware and resolves the editing/navigation keys NetSurf text fields need
  — backspace, delete, the arrows, home/end and page up/down — so typing,
  deletion and caret movement in the address bar all work. gterm grew the
  matching arrow/nav→ANSI-escape handling, so line editing and shell history
  work in the terminal too. SysV `SHMMAX` was raised 1 MB → 32 MB so a single
  shared segment can hold a full-screen graphics/framebuffer buffer.
  Seven render/interaction paths green both arches (x86_64 739/0, x86 738/0):
  file:// (off-screen + on-screen /dev/fb0 + windowed displayd), loopback HTTP,
  loopback HTTPS, public-internet HTTPS, and keyboard/mouse input.
- [ ] `stubbed` Use that port to assess Chromium; add Vulkan only when a
  browser or another real application requires it. Assessment in progress and
  tracked in [`chromium-assessment.md`](chromium-assessment.md) — current
  verdict: blocked on whole subsystems (sandbox/namespaces, GPU EGL/GBM/Vulkan,
  full C++ runtime), not pursued; revisit conditions listed there.
