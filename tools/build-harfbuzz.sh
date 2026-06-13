#!/bin/sh
# Build HarfBuzz (HB_TINY, no FreeType/glib/icu) as a static libharfbuzz.a for
# the b1nix userspace ABI. Unified build via src/harfbuzz.cc, compiled with the
# cross g++. -fno-exceptions/-rtti/-threadsafe-statics keep it off the C++
# runtime entirely (no libstdc++ link needed). Prints the install dir.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HB_VERSION="${HB_VERSION:-8.3.0}"
TARBALL="harfbuzz-${HB_VERSION}.tar.xz"
URL="https://github.com/harfbuzz/harfbuzz/releases/download/${HB_VERSION}/${TARBALL}"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

. "$ROOT_DIR/tools/toolchain-env.sh"

SRC_PARENT="$ROOT_DIR/build/ports-src"
SRC_DIR="$SRC_PARENT/harfbuzz-${HB_VERSION}"
BUILD_DIR="$ROOT_DIR/build/harfbuzz-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
INSTALL_DIR="$BUILD_DIR/install"
GXX="$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/cross/bin/$B1NIX_TRIPLET-g++"

mkdir -p "$SRC_PARENT" "$OBJ_DIR" "$INSTALL_DIR/include/harfbuzz" \
  "$INSTALL_DIR/lib"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$TARBALL"
  [ -f "$tmp" ] || curl -L "$URL" -o "$tmp" 1>&2
  tar -xJf "$tmp" -C "$SRC_PARENT" 1>&2
fi

UINC="$ROOT_DIR/userspace/include"
# The cross g++ sysroot ($B1NIX_ROOTFS/include) holds a copy of the userspace
# headers that gcc's <stdint.h>/<cmath> reach via include_next. Keep it in sync
# so newly added types (uint_fast*, hypotf) are visible to the C++ build.
if [ -d "$B1NIX_ROOTFS/include" ]; then
  cp -f "$UINC/stdint.h" "$UINC/math.h" "$B1NIX_ROOTFS/include/" 2>/dev/null || true
fi
# -idirafter so libstdc++'s headers win for <cmath> etc. and include_next finds
# our C headers. _GLIBCXX_HAVE_MBSTATE_T stops <cwchar> redefining mbstate_t.
"$GXX" -c -O2 -DHB_TINY -DHB_NO_MT -D_GLIBCXX_HAVE_MBSTATE_T \
  -fno-exceptions -fno-rtti -fno-threadsafe-statics -std=c++14 \
  -idirafter "$UINC" -I"$SRC_DIR/src" "$SRC_DIR/src/harfbuzz.cc" \
  -o "$OBJ_DIR/harfbuzz.o"

"$AR_BIN" rcs "$INSTALL_DIR/lib/libharfbuzz.a" "$OBJ_DIR/harfbuzz.o"
cp "$SRC_DIR/src/hb.h" "$SRC_DIR/src"/hb-*.h "$INSTALL_DIR/include/harfbuzz/" \
  2>/dev/null || true

echo "$INSTALL_DIR"
