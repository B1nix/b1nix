#!/bin/sh
# Build libwapcaplet as a static liblwc.a for the b1nix userspace ABI.
# Freestanding compile of the single portable C source (no autotools), like the
# other ports. libwapcaplet is NetSurf's string-internment / hash-table library
# and the smallest, lowest dependency in the browser lib chain. Prints the
# install dir.
#
# M53 (NetSurf browser platform) — first NetSurf-own library dependency.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
LWC_VERSION="${LWC_VERSION:-0.4.3}"
TARBALL="libwapcaplet-${LWC_VERSION}-src.tar.gz"
URL="https://download.netsurf-browser.org/libs/releases/${TARBALL}"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

. "$ROOT_DIR/tools/toolchain/env.sh"

SRC_PARENT="$ROOT_DIR/build/netsurf-src"
SRC_DIR="$SRC_PARENT/libwapcaplet-${LWC_VERSION}"
BUILD_DIR="$ROOT_DIR/build/libwapcaplet-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
INSTALL_DIR="$BUILD_DIR/install"

mkdir -p "$SRC_PARENT" "$OBJ_DIR" "$INSTALL_DIR/include" "$INSTALL_DIR/lib"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$TARBALL"
  [ -f "$tmp" ] || curl -L "$URL" -o "$tmp" 1>&2
  tar -xzf "$tmp" -C "$SRC_PARENT" 1>&2
fi

if [ "$B1NIX_ARCH" = "x86" ]; then
  TARGET="i686-unknown-elf"
else
  TARGET="x86_64-unknown-elf"
fi

# Freestanding compile against the b1nix userspace libc headers. libwapcaplet is
# pure portable C (malloc/free/memcpy/strncmp/strlen) with no SIMD or arch code.
CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fno-strict-aliasing -Db1nix -I$SRC_DIR/include -I$SRC_DIR/src
  -Wno-implicit-function-declaration"

# shellcheck disable=SC2086
clang $CFLAGS -c "$SRC_DIR/src/libwapcaplet.c" -o "$OBJ_DIR/libwapcaplet.o"

# shellcheck disable=SC2086
"$AR_BIN" rcs "$INSTALL_DIR/lib/liblwc.a" "$OBJ_DIR/libwapcaplet.o"
mkdir -p "$INSTALL_DIR/include/libwapcaplet"
cp "$SRC_DIR/include/libwapcaplet/libwapcaplet.h" "$INSTALL_DIR/include/libwapcaplet/"

echo "$INSTALL_DIR"
