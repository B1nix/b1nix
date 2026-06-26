#!/bin/sh
# Build HarfBuzz (HB_TINY, no FreeType/glib/icu) as a static libharfbuzz.a for
# the b1nix userspace ABI. Unified build via src/harfbuzz.cc, compiled with the
# cross g++. HarfBuzz is designed to build -fno-exceptions/-rtti (no C++ runtime
# link), so we keep that. What changed: the toolchain's C++ headers are now
# correct (tools/toolchain/enable-cxx-toolchain.sh stages the b1nix libc into the sysroot
# and fixes mbstate_t), so the old per-build header hacks are gone. Prints dir.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
HB_VERSION="${HB_VERSION:-8.3.0}"
TARBALL="harfbuzz-${HB_VERSION}.tar.xz"
URL="https://github.com/harfbuzz/harfbuzz/releases/download/${HB_VERSION}/${TARBALL}"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

. "$ROOT_DIR/tools/toolchain/env.sh"

SRC_PARENT="$ROOT_DIR/build/ports-src"
SRC_DIR="$SRC_PARENT/harfbuzz-${HB_VERSION}"
BUILD_DIR="$ROOT_DIR/build/harfbuzz-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
INSTALL_DIR="$BUILD_DIR/install"
GXX="$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/cross/bin/$B1NIX_TRIPLET-g++"
# Route the cross g++ through ccache when present (byte-identical object).
CCACHE="$(command -v ccache 2>/dev/null || true)"

mkdir -p "$SRC_PARENT" "$OBJ_DIR" "$INSTALL_DIR/include/harfbuzz" \
  "$INSTALL_DIR/lib"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$TARBALL"
  [ -f "$tmp" ] || curl -L "$URL" -o "$tmp" 1>&2
  tar -xJf "$tmp" -C "$SRC_PARENT" 1>&2
fi

# The b1nix libc headers and libstdc++'s mbstate_t config live in the toolchain
# sysroot now (idempotent; see the script). No per-build header hacks needed.
"$ROOT_DIR/tools/toolchain/enable-cxx-toolchain.sh" "$B1NIX_TRIPLET" >/dev/null 2>&1 || true

$CCACHE "$GXX" -c -O2 -DHB_TINY -DHB_NO_MT \
  -fno-exceptions -fno-rtti -fno-threadsafe-statics -std=c++14 \
  -I"$SRC_DIR/src" "$SRC_DIR/src/harfbuzz.cc" \
  -o "$OBJ_DIR/harfbuzz.o"

"$AR_BIN" rcs "$INSTALL_DIR/lib/libharfbuzz.a" "$OBJ_DIR/harfbuzz.o"
cp "$SRC_DIR/src/hb.h" "$SRC_DIR/src"/hb-*.h "$INSTALL_DIR/include/harfbuzz/" \
  2>/dev/null || true

echo "$INSTALL_DIR"
