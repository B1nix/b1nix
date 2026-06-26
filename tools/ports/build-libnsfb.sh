#!/bin/sh
# Build libnsfb as a static libnsfb.a for the b1nix userspace ABI. Freestanding
# curated compile (no autotools). libnsfb is NetSurf's framebuffer surface +
# plotter library; the framebuffer frontend draws through it. We build all the
# software plotters plus the RAM surface (an in-memory framebuffer) and the
# surface registry; the host-dependent surfaces (sdl/x/vnc/wld) are omitted.
# Surfaces self-register via __attribute__((constructor)); b1nix crt0 walks
# .init_array, so the RAM surface is available at runtime. Prints the install
# dir.
#
# M53 (NetSurf browser platform) — framebuffer frontend foundation (step 7).
# Build logic lives in tools/ports/drivers/nslib.sh; this is the manifest.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

NSLIB_NAME=libnsfb
NSLIB_VERSION="${NSFB_VERSION:-0.2.2}"
NSLIB_ARCHIVE=libnsfb.a
NSLIB_CFLAGS="-include stdbool.h -Wno-implicit-function-declaration"
NSLIB_HEADERS="glob:include"
# Compiled translation units only — common.c / 32bpp-common.c / 1bpp.c /
# 24bpp.c are templates #included by the per-bpp plotters (see plot/Makefile).
NSLIB_SOURCES="
  src/libnsfb.c src/cursor.c src/palette.c src/dump.c
  src/plot/api.c src/plot/generic.c src/plot/util.c
  src/plot/8bpp.c src/plot/16bpp.c
  src/plot/32bpp-xbgr8888.c src/plot/32bpp-xrgb8888.c
  src/surface/surface.c src/surface/ram.c src/surface/b1nixfb.c
  src/surface/displayd.c
"

# Drop in the b1nix surfaces: /dev/fb0 (on-screen) and displayd (windowed client
# of the b1nix Wayland-shaped compositor, with input).
port_pre_build() {
  cp "$ROOT_DIR/tools/netsurf-assets/libnsfb-b1keymap.h" "$SRC_DIR/src/surface/libnsfb-b1keymap.h"
  cp "$ROOT_DIR/tools/netsurf-assets/libnsfb-b1nixfb.c" "$SRC_DIR/src/surface/b1nixfb.c"
  cp "$ROOT_DIR/tools/netsurf-assets/libnsfb-displayd.c" "$SRC_DIR/src/surface/displayd.c"
}

. "$ROOT_DIR/tools/ports/drivers/nslib.sh"
