#!/bin/sh
# Build libpng as a static libpng16.a for the b1nix userspace ABI. Freestanding
# compile of the portable core (no autotools/configure, no SIMD), against the
# ported zlib. Uses libpng's shipped scripts/pnglibconf.h.prebuilt as the
# feature config. Prints the install dir.
#
# M53 (NetSurf browser platform) image-codec dependency. Depends on build-zlib.sh.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
PNG_VERSION="${PNG_VERSION:-1.6.43}"
TARBALL="libpng-${PNG_VERSION}.tar.gz"
URL="https://github.com/pnggroup/libpng/archive/refs/tags/v${PNG_VERSION}.tar.gz"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

. "$ROOT_DIR/tools/toolchain-env.sh"

ZLIB_DIR="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/ports/build-zlib.sh")"

SRC_PARENT="$ROOT_DIR/build/libpng-src"
SRC_DIR="$SRC_PARENT/libpng-${PNG_VERSION}"
BUILD_DIR="$ROOT_DIR/build/libpng-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
GEN_DIR="$BUILD_DIR/gen"
INSTALL_DIR="$BUILD_DIR/install"

mkdir -p "$SRC_PARENT" "$OBJ_DIR" "$GEN_DIR" "$INSTALL_DIR/include" "$INSTALL_DIR/lib"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$TARBALL"
  [ -f "$tmp" ] || curl -L "$URL" -o "$tmp" 1>&2
  tar -xzf "$tmp" -C "$SRC_PARENT" 1>&2
fi

# Feature config: libpng ships a ready-made one for exactly this purpose.
cp "$SRC_DIR/scripts/pnglibconf.h.prebuilt" "$GEN_DIR/pnglibconf.h"

if [ "$B1NIX_ARCH" = "x86" ]; then
  TARGET="i686-unknown-elf"
else
  TARGET="x86_64-unknown-elf"
fi

# Userspace can use (soft) float, which libpng's gamma/colour code needs.
# PNG_NO_HARDWARE_OPTIMIZATIONS keeps the portable C filter paths (no SSE/NEON).
CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fno-strict-aliasing -Db1nix -DPNG_NO_HARDWARE_OPTIMIZATIONS
  -I$GEN_DIR -I$SRC_DIR -I$ZLIB_DIR/include -Wno-implicit-function-declaration"

# Portable core only — the per-arch SIMD dirs (intel/, arm/, ...) stay out.
CORE="png pngerror pngget pngmem pngpread pngread pngrio pngrtran pngrutil \
pngset pngtrans pngwio pngwrite pngwtran pngwutil"
OBJS=""
for base in $CORE; do
  obj="$OBJ_DIR/$base.o"
  # shellcheck disable=SC2086
  clang $CFLAGS -c "$SRC_DIR/$base.c" -o "$obj"
  OBJS="$OBJS $obj"
done

# shellcheck disable=SC2086
"$AR_BIN" rcs "$INSTALL_DIR/lib/libpng16.a" $OBJS
cp "$SRC_DIR/png.h" "$SRC_DIR/pngconf.h" "$GEN_DIR/pnglibconf.h" "$INSTALL_DIR/include/"

echo "$INSTALL_DIR"
