#!/bin/sh
# Build TinyGL (software OpenGL 1.1 subset, C-Chads fork) plus the b1nix EGL
# shim as static libTinyGL.a + libEGL.a for the userspace ABI. Freestanding
# compile of the upstream src/ subset; no CMake/Make from the port. TinyGL
# renders into an in-memory ZBuffer (ZB_MODE_RGBA, 0x00RRGGBB) which the EGL
# shim presents to displayd via b1gui. Needs libm (openlibm). Prints install dir.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
TGL_BRANCH="${TGL_BRANCH:-main}"
URL="https://github.com/C-Chads/tinygl/archive/refs/heads/${TGL_BRANCH}.tar.gz"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

. "$ROOT_DIR/tools/toolchain-env.sh"

SRC_PARENT="$ROOT_DIR/build/ports-src"
SRC_DIR="$SRC_PARENT/tinygl"
BUILD_DIR="$ROOT_DIR/build/tinygl-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
INSTALL_DIR="$BUILD_DIR/install"
LIBM_DIR="$($ROOT_DIR/tools/ports/build-openlibm.sh)"

mkdir -p "$SRC_PARENT" "$OBJ_DIR" \
  "$INSTALL_DIR/include/GL" "$INSTALL_DIR/lib"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/tinygl.tar.gz"
  [ -f "$tmp" ] || curl -L "$URL" -o "$tmp" 1>&2
  tar -xzf "$tmp" -C "$SRC_PARENT" 1>&2
  mv "$SRC_PARENT/tinygl-${TGL_BRANCH}" "$SRC_DIR"
fi

# Single-threaded smoke: disable the multithreaded raster paths (OpenMP pragmas)
# so the port has no thread runtime dependency. Idempotent.
sed -i.bak -E 's/(#define TGL_FEATURE_MULTITHREADED_[A-Z_]+)[[:space:]]+1/\1 0/' \
  "$SRC_DIR/include/zfeatures.h"

if [ "$B1NIX_ARCH" = "x86" ]; then
  TARGET="i686-unknown-elf"
else
  TARGET="x86_64-unknown-elf"
fi

CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fno-strict-aliasing -Db1nix
  -I$SRC_DIR/include -I$SRC_DIR/src -I$LIBM_DIR/include
  -Wno-implicit-function-declaration -Wno-incompatible-pointer-types -Wno-int-conversion -Wno-implicit-int -Wno-incompatible-function-pointer-types"

OBJS=""
for c in "$SRC_DIR"/src/*.c; do
  obj="$OBJ_DIR/$(basename "$c" .c).o"
  clang $CFLAGS -c "$c" -o "$obj"
  OBJS="$OBJS $obj"
done

# shellcheck disable=SC2086
"$AR_BIN" rcs "$INSTALL_DIR/lib/libTinyGL.a" $OBJS

# b1nix EGL shim (userspace-owned glue) -> libEGL.a. Its public header lives in
# userspace/include/EGL/egl.h (already on the -isystem path), like b1nix/gui.h.
clang $CFLAGS -c "$ROOT_DIR/userspace/libegl/b1egl.c" -o "$OBJ_DIR/b1egl.o"
"$AR_BIN" rcs "$INSTALL_DIR/lib/libEGL.a" "$OBJ_DIR/b1egl.o"

cp "$SRC_DIR/include/GL/gl.h" "$INSTALL_DIR/include/GL/"
cp "$SRC_DIR/include/zbuffer.h" "$SRC_DIR/include/zfeatures.h" \
  "$INSTALL_DIR/include/"

echo "$INSTALL_DIR"
