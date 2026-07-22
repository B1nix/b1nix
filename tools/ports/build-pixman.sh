#!/bin/sh
# Build pixman (generic C, no SIMD) as a static libpixman-1.a for the b1nix
# userspace ABI. Freestanding compile of the portable subset — meson/autotools
# are bypassed. Prints the install dir.
#
# Build logic lives in tools/ports/drivers/cport.sh; this is the manifest.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

CPORT_NAME=pixman
CPORT_ARCHIVE=libpixman-1.a
CPORT_SRC_PARENT="$ROOT_DIR/build/src/pixman"
VER="${PIXMAN_VERSION:-0.42.2}"
CPORT_SRCNAME="pixman-pixman-${VER}"
CPORT_TARBALL="pixman-${VER}.tar.gz"
CPORT_URL="https://gitlab.freedesktop.org/pixman/pixman/-/archive/pixman-${VER}/pixman-pixman-${VER}.tar.gz"

PORT_DEPS="openlibm"

port_pre_build() {
  P="$SRC_DIR/pixman"
  port_resolve_deps
  MAJOR=$(echo "$VER" | cut -d. -f1); MINOR=$(echo "$VER" | cut -d. -f2); MICRO=$(echo "$VER" | cut -d. -f3)
  sed -e "s/@PIXMAN_VERSION_MAJOR@/$MAJOR/g" \
      -e "s/@PIXMAN_VERSION_MINOR@/$MINOR/g" \
      -e "s/@PIXMAN_VERSION_MICRO@/$MICRO/g" \
      "$P/pixman-version.h.in" > "$GEN_DIR/pixman-version.h"
  # Minimal config.h. `PACKAGE` is the sentinel pixman-private.h checks for.
  cat > "$GEN_DIR/config.h" <<EOF
#define PACKAGE "pixman"
#define PACKAGE_VERSION "$VER"
#define PACKAGE_BUGREPORT ""
EOF
  CPORT_CFLAGS="-I$OPENLIBM_DIR/include -DHAVE_CONFIG_H -I$GEN_DIR -I$P \
-Wno-implicit-function-declaration -DPIXMAN_NO_TLS"
}

# Generic subset: everything except the SIMD implementations. The per-arch
# dispatch stubs stay (they no-op without the USE_* SIMD macros).
port_build() {
  for src in "$P"/*.c; do
    base=$(basename "$src" .c)
    case "$base" in
      pixman-mmx|pixman-sse2|pixman-ssse3|pixman-arm-neon|pixman-arm-simd|\
pixman-vmx|pixman-mips-dspr2) continue;;
      pixman-region) continue;;   # template #included by region16/region32
    esac
    cport_cc "$src"
  done
}

port_install_headers() {
  mkdir -p "$INSTALL_DIR/include/pixman-1"
  cp "$P/pixman.h" "$GEN_DIR/pixman-version.h" "$INSTALL_DIR/include/pixman-1/"
}

. "$ROOT_DIR/tools/ports/drivers/cport.sh"
