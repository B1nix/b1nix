#!/bin/sh
# Build librosprite as a static librosprite.a for the b1nix userspace ABI.
# Freestanding curated compile (no autotools). librosprite decodes RISC-OS
# sprite images; NetSurf registers it as the image/x-riscos-sprite content
# handler. Pure C, stdlib only. Prints the install dir.
#
# M53 (NetSurf browser platform) — RISC-OS sprite image decoder.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

. "$ROOT_DIR/tools/toolchain-env.sh"

SRC_PARENT="$ROOT_DIR/build/netsurf-src"
SRC_DIR="$(ls -d "$SRC_PARENT"/netsurf-all-*/librosprite 2>/dev/null | head -1)"
if [ -z "$SRC_DIR" ] || [ ! -f "$SRC_DIR/src/librosprite.c" ]; then
  echo "build-librosprite.sh: librosprite source not found (run build-netsurf-fb.sh first)" >&2
  exit 1
fi

BUILD_DIR="$ROOT_DIR/build/librosprite-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
INSTALL_DIR="$BUILD_DIR/install"
mkdir -p "$OBJ_DIR" "$INSTALL_DIR/include" "$INSTALL_DIR/lib"

if [ "$B1NIX_ARCH" = "x86" ]; then TARGET="i686-unknown-elf"; else TARGET="x86_64-unknown-elf"; fi

CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fno-strict-aliasing -Db1nix -I$SRC_DIR/include -I$SRC_DIR/src
  -Wno-implicit-function-declaration"

# shellcheck disable=SC2086
clang $CFLAGS -c "$SRC_DIR/src/librosprite.c" -o "$OBJ_DIR/librosprite.o"
"$AR_BIN" rcs "$INSTALL_DIR/lib/librosprite.a" "$OBJ_DIR/librosprite.o"
cp "$SRC_DIR/include/librosprite.h" "$INSTALL_DIR/include/"

echo "$INSTALL_DIR"
