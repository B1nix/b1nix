#!/bin/sh
# Build Cairo (image surface + FreeType font backend only) as a static
# libcairo.a for the b1nix userspace ABI. Uses the cport driver.
# Needs pixman + FreeType + libm.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

CPORT_NAME=cairo
CPORT_ARCHIVE=libcairo.a
CPORT_SRCNAME="cairo-${CAIRO_VERSION:-1.16.0}"
CPORT_URL="https://www.cairographics.org/releases/cairo-${CAIRO_VERSION:-1.16.0}.tar.xz"
CPORT_TARBALL="cairo-${CAIRO_VERSION:-1.16.0}.tar.xz"
CPORT_HEADERS="flat:cairo.h flat:cairo-ft.h flat:cairo-deprecated.h flat:cairo-version.h gen:cairo-features.h"
PORT_DEPS="openlibm pixman freetype"

port_pre_build() {
  S="$SRC_DIR/src"
  port_resolve_deps

  mkdir -p "$INSTALL_DIR/include/cairo"

  if [ "$B1NIX_ARCH" = "x86" ]; then PTR=4; LONG=4; else PTR=8; LONG=8; fi

  cat > "$GEN_DIR/cairo-features.h" <<'EOF'
#ifndef CAIRO_FEATURES_H
#define CAIRO_FEATURES_H
#define CAIRO_HAS_IMAGE_SURFACE 1
#define CAIRO_HAS_FT_FONT 1
#define CAIRO_HAS_USER_FONT 1
#endif
EOF

  cat > "$GEN_DIR/config.h" <<EOF
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_UINT64_T 1
#define HAVE_CC_ATOMIC_PRIMITIVES 1
#define SIZEOF_VOID_P $PTR
#define SIZEOF_INT 4
#define SIZEOF_LONG $LONG
#define SIZEOF_LONG_LONG 8
#define CAIRO_NO_MUTEX 1
EOF

  CPORT_CFLAGS="-DHAVE_CONFIG_H -I$GEN_DIR -I$S -I$OPENLIBM_DIR/include \
    -I$PIXMAN_DIR/include/pixman-1 -I$FREETYPE_DIR/include \
    -Wno-implicit-function-declaration -Wno-incompatible-pointer-types \
    -Wno-int-conversion -Wno-implicit-int -Wno-incompatible-function-pointer-types"
}

port_build() {
  # cairo.c (the public context API) has no dash, so the dash regex misses it.
  SOURCES="cairo.c $(sed -n '137,245p' "$SRC_DIR/src/Makefile.sources" \
    | grep -oE 'cairo-[a-z0-9-]+\.c') cairo-ft-font.c"
  for c in $SOURCES; do
    cport_cc "$SRC_DIR/src/$c"
  done
}

port_install_headers() {
  cp "$SRC_DIR/src/cairo.h" "$SRC_DIR/src/cairo-ft.h" "$SRC_DIR/src/cairo-deprecated.h" \
     "$GEN_DIR/cairo-features.h" "$SRC_DIR/cairo-version.h" "$INSTALL_DIR/include/cairo/"
}

. "$ROOT_DIR/tools/ports/drivers/cport.sh"
