#!/bin/sh
# Build Cairo (image surface + FreeType font backend only) as a static
# libcairo.a for the b1nix userspace ABI. Freestanding compile of the curated
# subset; meson/autotools bypassed. Needs pixman + FreeType + libm. Prints the
# install dir.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
CAIRO_VERSION="${CAIRO_VERSION:-1.16.0}"
TARBALL="cairo-${CAIRO_VERSION}.tar.xz"
URL="https://www.cairographics.org/releases/${TARBALL}"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

. "$ROOT_DIR/tools/toolchain-env.sh"

SRC_PARENT="$ROOT_DIR/build/ports-src"
SRC_DIR="$SRC_PARENT/cairo-${CAIRO_VERSION}"
S="$SRC_DIR/src"
BUILD_DIR="$ROOT_DIR/build/cairo-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
GEN_DIR="$BUILD_DIR/gen"
INSTALL_DIR="$BUILD_DIR/install"
LIBM_DIR="$($ROOT_DIR/tools/ports/build-openlibm.sh)"
PIXMAN_DIR="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/ports/build-pixman.sh")"
FT_DIR="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/ports/build-freetype.sh")"

mkdir -p "$SRC_PARENT" "$OBJ_DIR" "$GEN_DIR" "$INSTALL_DIR/include/cairo" \
  "$INSTALL_DIR/lib"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$TARBALL"
  [ -f "$tmp" ] || curl -L "$URL" -o "$tmp" 1>&2
  tar -xJf "$tmp" -C "$SRC_PARENT" 1>&2
fi

if [ "$B1NIX_ARCH" = "x86" ]; then
  TARGET="i686-unknown-elf"; PTR=4; LONG=4
else
  TARGET="x86_64-unknown-elf"; PTR=8; LONG=8
fi

# Enabled backends: image surface + FreeType font + user font. No PNG, no
# fontconfig, no PS/PDF/SVG/Xlib, no pthread (single-threaded smoke).
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

CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fno-strict-aliasing -Db1nix -DHAVE_CONFIG_H
  -I$GEN_DIR -I$S -I$LIBM_DIR/include
  -I$PIXMAN_DIR/include/pixman-1 -I$FT_DIR/include
  -Wno-implicit-function-declaration -Wno-incompatible-pointer-types
  -Wno-int-conversion -Wno-implicit-int -Wno-incompatible-function-pointer-types"

# cairo.c (the public context API) has no dash, so the dash regex misses it.
SOURCES="cairo.c $(sed -n '137,245p' "$SRC_DIR/src/Makefile.sources" \
  | grep -oE 'cairo-[a-z0-9-]+\.c') cairo-ft-font.c"

OBJS=""
for c in $SOURCES; do
  obj="$OBJ_DIR/$(basename "$c" .c).o"
  clang $CFLAGS -c "$S/$c" -o "$obj"
  OBJS="$OBJS $obj"
done

# shellcheck disable=SC2086
"$AR_BIN" rcs "$INSTALL_DIR/lib/libcairo.a" $OBJS
cp "$S/cairo.h" "$S/cairo-ft.h" "$S/cairo-deprecated.h" "$GEN_DIR/cairo-features.h" \
  "$SRC_DIR/cairo-version.h" "$INSTALL_DIR/include/cairo/"

echo "$INSTALL_DIR"
