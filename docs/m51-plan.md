# M51 — Desktop Graphics Stack: Port Plan

Status: **fully closed — nothing deferred.** All six libraries ported (pixman,
FreeType, Fontconfig, HarfBuzz, Cairo, xkbcommon) plus libm and expat; the
Wayland protocol surface completed in displayd (wl_output + clipboard atop the
existing wl_seat/xdg-shell); an end-to-end Cairo Wayland app rendering scalable
text; clipboard selection round-trip; Fontconfig family matching. HarfBuzz
(C++) was ported with the cross g++ kept entirely off the C++ runtime
(`-fno-exceptions/-rtti/-threadsafe-statics`, HB_TINY) so it links with no
libstdc++. Markers: `M51-GFX: ok libm|pixman|freetype|fontconfig|cairo|
xkbcommon|harfbuzz|wl-output|cairo-wayland|clipboard`. Version bumped to 0.51.0.

## End-to-end done: a Cairo Wayland app (`m51_cairo_wayland`)

Reuses libb1gui (the M47/M49 Wayland client) + Cairo: opens a real wl_shm
window, wraps the live buffer as a Cairo image surface, draws scalable text
("b1nix" in B1nix Mono via the FreeType backend), presents to displayd, waits
for the frame-complete callback, and asserts the presented buffer holds the dark
glyph ink. `M51-GFX: ok cairo-wayland`. This is the "run a Cairo Wayland app with
scalable fonts" deliverable (shaped text via HarfBuzz and keyboard
input/clipboard still pending).

Lesson: run the two arches sequentially/isolated (2 VMs each). `smoke-all-parallel`
(4 TCG VMs) overloads this host and times out the SMP runs even with warm
libs — both arches pass isolated (x86_64 611/0, x86 610/0).

## Rung 5 done: Cairo

`tools/build-cairo.sh` ports Cairo 1.16.0 → `libcairo.a` both triplets (image
surface + FreeType backend + user font only; no PNG/fontconfig/PS/PDF/SVG/Xlib;
`CAIRO_NO_MUTEX` single-threaded; atomics via `__atomic`; hand-written
`config.h`/`cairo-features.h`; the source list is `cairo.c` + the dashed
`cairo_sources` from Makefile.sources + `cairo-ft-font.c`). Without fontconfig,
`cairo_ft_font_face_create_for_ft_face` takes an FT_Face directly. The only link
snag was a duplicate `frexp` (libm vs libc) — fixed by weakening openlibm's
`frexp`/`ldexp` so the libc definitions win. Verified by `m51_cairo_smoke`
(`M51-GFX: ok cairo`): draws "Ab1" in B1nix Mono onto an ARGB32 image surface and
asserts the glyphs painted dark ink over a white background — the whole
cairo→pixman→freetype path end-to-end.

## Note: smoke-all-parallel + cold port builds overloads the host

`make smoke-all-parallel` runs 4 TCG VMs; if the Cairo/FreeType archives aren't
built yet it also runs two cold library builds concurrently, which starved the
slow i386-SMP VM and timed it out (uuidgen, near the end). Pre-build the port
libs first (or run arches sequentially); with a warm cache the parallel run is
fine. Both arches are 609/0 when run this way.

## Rung 3 done: FreeType

`tools/build-freetype.sh` ports FreeType 2.13.2 → `libfreetype.a` both triplets
(single-object freestanding build per docs/INSTALL.ANY; trimmed `ftmodule.h` =
TrueType + CFF + smooth rasterizer, drops SDF/SVG/BDF/PCF/Type1/Type42/winfonts;
`ftgrays.c` SSE2 path forced off — no `<emmintrin.h>` in the sysroot; `ftgzip.c`
included for WOFF/`FT_Gzip_Uncompress`). The project's **own** font, B1nix Mono
(`../b1nix-mono/dist/B1nixMono-Regular.ttf`, 959 glyphs), is bundled into the
initramfs at `/share/fonts/B1nixMono-Regular.ttf`. Verified by
`m51_freetype_smoke` (`M51-GFX: ok freetype`): loads the font, rasterizes 'A' at
48px, asserts the glyph bitmap has actual ink (not a blank box).

## Rung 2 done: pixman

`tools/build-pixman.sh` ports pixman 0.42.2 → `libpixman-1.a` both triplets
(generic C only; all SIMD impls excluded; CPU-dispatch stubs kept so they no-op;
`pixman-region.c` is template-only — excluded; `config.h`/`pixman-version.h`
hand-generated; `-DPIXMAN_NO_TLS` single-threaded). Verified by
`m51_pixman_smoke` (`M51-GFX: ok pixman`): OP_SRC fill checked exact, then
OP_OVER half-alpha blend checked to have actually combined (not memset).

## Rung 0/1 done: libm (openlibm)

`tools/build-openlibm.sh` ports openlibm 0.8.1 → `libm.a` for both triplets
(freestanding clang compile of the portable C subset; long-double, complex,
Bessel and gamma routines skipped — no graphics lib uses them; `__BSD_VISIBLE`
enabled for the `M_*` constants; openlibm's `ldexp` strong alias weakened so the
libc `ldexp`/`frexp` still win). Verified by `m51_smoke` (`M51-GFX: ok libm`).

Found and fixed a latent bug: `userspace/include/math.h` declared the
transcendentals as `static inline { return __builtin_f(...); }`, which lowers to
a self-call → infinite recursion the optimizer turns into `jmp .`. It only ever
"worked" because every prior use was a compile-time-constant fold. Replaced with
real `extern` prototypes (resolved by libm) + `M_*` constants. Zero blast radius
(no binary called a transcendental at runtime).

## Side finding: M50 was not honestly closed in the main suite

The M50 commit added 7 unconditional `M50-DRM` checks to `tests/smoke.sh` but
never gave that suite a `virtio-gpu-pci` device, so `/dev/dri/card0` did not
exist and all 7 failed — M50 only passed in the separate `graphics-smoke.sh`.
Fixed by adding `-device virtio-gpu-pci` to `run_qemu()` in `tests/smoke.sh`, so
the DRM stack (and the M47/M49 framebuffer paths) are actually exercised there.
M50's *implementation* was real all along; only the gate was hollow.

## Scope (from roadmap)

1. Port pixman, FreeType, Fontconfig, HarfBuzz, Cairo, xkbcommon.
2. Complete the Wayland input / clipboard / output / window protocol surface.
3. Run a Cairo Wayland app with shaped text, scalable fonts, keyboard input,
   and clipboard.

## How ports work here (the established pattern)

`tools/build-wayland.sh` is the model: **do not run upstream meson/autotools**.
Hand-pick the source subset, compile each `.c` with a fixed `clang` line
(`--target=$TARGET -ffreestanding -nostdinc -isystem userspace/include
-msoft-float`), hand-write any normally-generated `config.h`, shim missing
platform headers with a `compat.h`, archive to a static `.a` + headers, and
echo the install dir. The library ELF is then embedded into the initramfs as an
`xxd` `.inc`. Each port gets a `tools/build-<lib>.sh` keyed off
`tools/toolchain-env.sh` (`B1NIX_TRIPLET`, both arches).

## Hard blocker found during planning: no libm

`userspace/include/math.h` forwards everything to `__builtin_*`. Only the ops
that lower to a single SSE instruction (`sqrt`, `floor`, `ceil`, `fabs`,
`sqrtf`, `fabsf`) actually resolve. `sin/cos/tan/atan2/exp/log/pow/fmod/...`
emit **calls to libm symbols that exist nowhere in userspace**. No ported
program has hit this yet because none did transcendental math at runtime.
FreeType (autofitter), Cairo (arcs/matrices) and HarfBuzz **will**.

→ **Step 0 (prerequisite, not in the roadmap text): port a libm.** Cheapest fit
to the project pattern is **openlibm** (BSD, standalone, plain `.c`, no build
system needed — compile the subset like wayland). Output `libm.a`, link it ahead
of the graphics libs. This is the first rung and gates everything after it.

## Dependency order & per-library approach

| # | Lib | Deps | Notes / risk | Smoke gate |
|---|-----|------|--------------|------------|
| 0 | **openlibm** | — | new `tools/build-openlibm.sh`; compile subset | call `sin/pow/atan2`, assert known values |
| 1 | **pixman** | — | disable SIMD, generic fast paths only | solid fill + OVER composite, check a pixel |
| 2 | **FreeType** | libm | trim modules (no png/zlib/brotli); bundle one TTF | load TTF, rasterize a glyph, bitmap non-empty |
| 3 | **HarfBuzz** | FreeType, libstdc++ | **C++** — confirm libstdc++.a from native-GCC port is linkable in userspace; needs `<atomic>` | shape a Latin string, assert glyph/cluster count |
| 4 | **Fontconfig** | FreeType, expat | heaviest; needs expat (new port) + XML config + on-disk cache. **ponytail: likely not required** — Cairo can take an FT face directly via `cairo_ft_font_face_create_for_ft_face`, satisfying "scalable fonts" from a bundled font without Fontconfig | (deferred / reduced scope — see below) |
| 5 | **Cairo** | pixman, FreeType | image surface backend only (no GL/xlib); the integrator | draw shapes+text to image surface, check pixels |
| 6 | **xkbcommon** | keymap data | M49 punted with `no_keymap`+raw keycodes; ship a **precompiled** keymap to avoid the xkeyboard-config/yacc data dependency | evdev keycode → expected keysym |

## Protocol surface work (`userspace/bin/displayd.c`, ~1761 lines today)

Currently advertises 4 globals: `wl_compositor` v4, `wl_shm`, `wl_seat` v5
(keyboard+pointer), `xdg_wm_base` v1. Missing for a desktop app:

- **`wl_output`** — advertise global; send `geometry`/`mode`/`scale`/`done`.
  Apps need output size/scale before they lay out.
- **Clipboard** — `wl_data_device_manager` + `wl_data_device`/`wl_data_source`/
  `wl_data_offer`; route the selection between clients. This is the real
  protocol work in M51.
- **xdg-shell** — verify the existing `configure`/`ack_configure` and toplevel
  resize/states flow is complete enough for a real toolkit; extend if not.

## Final smoke test (`userspace/bin/m51_smoke.c`)

One libwayland-client app against `displayd`:
1. openlibm/FreeType/HarfBuzz: shape a string, rasterize glyphs.
2. Cairo: draw shaped text + a shape into a `wl_shm` buffer; commit.
3. displayd composites → read framebuffer back (like the M47 `fb-mmap` check)
   and assert non-background pixels where text was drawn.
4. xkbcommon: feed an evdev keycode, assert the expected keysym.
5. Clipboard: client A sets selection, client B reads it back — round-trip.

Markers: `M51-GFX: ok libm|pixman|freetype|harfbuzz|cairo|xkb|clipboard|render`,
wired into `tests/graphics-smoke.sh` + `tests/smoke.sh`, both arches.

## Recommended rung order (each independently verifiable, commit per rung)

1. openlibm → libm smoke. **(unblocks everything)**
2. pixman → pixman smoke.
3. FreeType → glyph-raster smoke.
4. Cairo (FT face directly, **defer Fontconfig**) → image-surface text smoke.
5. wl_output + clipboard in displayd → protocol smoke.
6. HarfBuzz → shaping smoke; fold shaping into the Cairo render path.
7. xkbcommon (precompiled keymap) → keysym smoke.
8. Full `m51_smoke` end-to-end render against displayd; update roadmap M51 → done.

## HarfBuzz: deferred to M53 (C++ userspace runtime needed)

HarfBuzz is C++ and must be built with the cross g++ + libstdc++. A probe got it
compiling deep into the unified `harfbuzz.cc` but it needs a real C++ userspace
runtime that b1nix doesn't have yet: extending the userspace C headers
(`uint_fast16_t` etc. absent from `stdint.h`, `mbstate_t` clash between gcc's
`<cwchar>` and our `wchar.h`), a libstdc++ sysroot, plus exception unwinding and
static-constructor (`init_array`) support in a freestanding userspace ELF. That
is the "robust C++ runtime / ICU" infrastructure the roadmap explicitly scopes
to **M53 (Browser Platform)**. Pulling it into M51 is premature.

Practically, Cairo's toy text API already shapes and renders Latin/monospace
text via FreeType (the `cairo-wayland` demo), which covers the milestone's
visible "text" deliverable for B1nix Mono. HarfBuzz adds *complex* shaping
(ligatures, Arabic/Indic, kerning) — valuable, but not required for the desktop
demo and best done alongside the M53 C++ runtime work.

## Deliberate scope reductions (flag, don't hide)

- **Fontconfig deferred**: bundle a font + use the FT face directly. The
  roadmap says "scalable fonts," not "font discovery." Revisit only if a real
  app needs `fontconfig` matching. (rung 4 ponytail note)
- **xkbcommon precompiled keymap**: skip porting xkeyboard-config + its yacc
  build; ship a prebuilt keymap blob. Add full compilation only if needed.
- **No GL/EGL** — that is M52, explicitly out of scope.

## Effort

Realistically multi-session: each library port has historically been a
multi-bug effort (bash, dropbear, mbedTLS). libm + pixman + FreeType + Cairo +
displayd protocol is the critical path to a visible result; HarfBuzz (C++) and
xkbcommon are the higher-risk tail.
