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

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
NSFB_VERSION="${NSFB_VERSION:-0.2.2}"
TARBALL="libnsfb-${NSFB_VERSION}-src.tar.gz"
URL="https://download.netsurf-browser.org/libs/releases/${TARBALL}"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

. "$ROOT_DIR/tools/toolchain-env.sh"

SRC_PARENT="$ROOT_DIR/build/netsurf-src"
SRC_DIR="$SRC_PARENT/libnsfb-${NSFB_VERSION}"
BUILD_DIR="$ROOT_DIR/build/libnsfb-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
INSTALL_DIR="$BUILD_DIR/install"

mkdir -p "$SRC_PARENT" "$OBJ_DIR" "$INSTALL_DIR/include" "$INSTALL_DIR/lib"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$TARBALL"
  [ -f "$tmp" ] || curl -L "$URL" -o "$tmp" 1>&2
  tar -xzf "$tmp" -C "$SRC_PARENT" 1>&2
fi

# Drop in the b1nix surfaces: /dev/fb0 (on-screen) and displayd (windowed client
# of the b1nix Wayland-shaped compositor, with input).
cp "$ROOT_DIR/tools/netsurf-assets/libnsfb-b1keymap.h" "$SRC_DIR/src/surface/libnsfb-b1keymap.h"
cp "$ROOT_DIR/tools/netsurf-assets/libnsfb-b1nixfb.c" "$SRC_DIR/src/surface/b1nixfb.c"
cp "$ROOT_DIR/tools/netsurf-assets/libnsfb-displayd.c" "$SRC_DIR/src/surface/displayd.c"

if [ "$B1NIX_ARCH" = "x86" ]; then
  TARGET="i686-unknown-elf"
else
  TARGET="x86_64-unknown-elf"
fi

CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fno-strict-aliasing -Db1nix -I$SRC_DIR/include -I$SRC_DIR/src
  -include stdbool.h -Wno-implicit-function-declaration"

# Compiled translation units only — common.c / 32bpp-common.c / 1bpp.c /
# 24bpp.c are templates #included by the per-bpp plotters (see plot/Makefile).
SOURCES="
  src/libnsfb.c src/cursor.c src/palette.c src/dump.c
  src/plot/api.c src/plot/generic.c src/plot/util.c
  src/plot/8bpp.c src/plot/16bpp.c
  src/plot/32bpp-xbgr8888.c src/plot/32bpp-xrgb8888.c
  src/surface/surface.c src/surface/ram.c src/surface/b1nixfb.c
  src/surface/displayd.c
"

OBJS=""
for rel in $SOURCES; do
  obj="$OBJ_DIR/$(echo "$rel" | tr '/' '_').o"
  # shellcheck disable=SC2086
  clang $CFLAGS -c "$SRC_DIR/$rel" -o "$obj"
  OBJS="$OBJS $obj"
done

# shellcheck disable=SC2086
"$AR_BIN" rcs "$INSTALL_DIR/lib/libnsfb.a" $OBJS
cp "$SRC_DIR"/include/*.h "$INSTALL_DIR/include/"

echo "$INSTALL_DIR"
