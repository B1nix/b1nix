#!/bin/sh
# Build libnsbmp as a static libnsbmp.a for the b1nix userspace ABI.
# Freestanding curated compile (no autotools). libnsbmp is NetSurf's standalone
# BMP/ICO decoder; the browser registers it as an image content handler. Prints
# the install dir.
#
# M53 (NetSurf browser platform) — browser image decoder (step 6).

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
NSBMP_VERSION="${NSBMP_VERSION:-0.1.7}"
TARBALL="libnsbmp-${NSBMP_VERSION}-src.tar.gz"
URL="https://download.netsurf-browser.org/libs/releases/${TARBALL}"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

. "$ROOT_DIR/tools/toolchain/env.sh"

SRC_PARENT="$ROOT_DIR/build/netsurf-src"
SRC_DIR="$SRC_PARENT/libnsbmp-${NSBMP_VERSION}"
BUILD_DIR="$ROOT_DIR/build/libnsbmp-b1nix/$B1NIX_TRIPLET"
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

CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fno-strict-aliasing -Db1nix -I$SRC_DIR/include -I$SRC_DIR/src
  -Wno-implicit-function-declaration"

clang $CFLAGS -c "$SRC_DIR/src/libnsbmp.c" -o "$OBJ_DIR/libnsbmp.o"

"$AR_BIN" rcs "$INSTALL_DIR/lib/libnsbmp.a" "$OBJ_DIR/libnsbmp.o"
cp "$SRC_DIR/include/libnsbmp.h" "$INSTALL_DIR/include/"

echo "$INSTALL_DIR"
