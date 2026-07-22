#!/bin/sh
# Build libpng as a static libpng16.a for the b1nix userspace ABI. Freestanding
# compile of the portable core (no autotools/configure, no SIMD), against the
# ported zlib. Uses libpng's shipped scripts/pnglibconf.h.prebuilt as the feature
# config. Prints the install dir.
#
# M53 (NetSurf browser platform) image-codec dependency. Depends on build-zlib.sh.
# Build logic lives in tools/ports/drivers/cport.sh; this is the manifest.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

CPORT_NAME=libpng
CPORT_ARCHIVE=libpng16.a
CPORT_SRC_PARENT="$ROOT_DIR/build/src/libpng"
VER="${PNG_VERSION:-1.6.43}"
CPORT_SRCNAME="libpng-${VER}"
CPORT_TARBALL="libpng-${VER}.tar.gz"
CPORT_URL="https://github.com/pnggroup/libpng/archive/refs/tags/v${VER}.tar.gz"
# Portable core only — the per-arch SIMD dirs (intel/, arm/, ...) stay out.
CPORT_SOURCES="png.c pngerror.c pngget.c pngmem.c pngpread.c pngread.c pngrio.c \
pngrtran.c pngrutil.c pngset.c pngtrans.c pngwio.c pngwrite.c pngwtran.c pngwutil.c"
CPORT_HEADERS="flat:png.h flat:pngconf.h gen:pnglibconf.h"

# Userspace can use (soft) float, which libpng's gamma/colour code needs.
# PNG_NO_HARDWARE_OPTIMIZATIONS keeps the portable C filter paths (no SSE/NEON).
PORT_DEPS="zlib"

port_pre_build() {
  port_resolve_deps
  # Feature config: libpng ships a ready-made one for exactly this purpose.
  cp "$SRC_DIR/scripts/pnglibconf.h.prebuilt" "$GEN_DIR/pnglibconf.h"
  CPORT_CFLAGS="-DPNG_NO_HARDWARE_OPTIMIZATIONS -I$GEN_DIR -I$SRC_DIR \
-I$ZLIB_DIR/include -Wno-implicit-function-declaration"
}

. "$ROOT_DIR/tools/ports/drivers/cport.sh"
