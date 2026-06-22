#!/bin/sh
# Build libnspsl as a static libnspsl.a for the b1nix userspace ABI.
# Freestanding curated compile (no autotools). libnspsl is NetSurf's public
# suffix list lookup; the browser uses it to scope cookies/same-origin to a
# registrable domain. The perfect-hash table (psl.inc) ships pre-generated in
# the source, so no perl/gperf step is needed. Prints the install dir.
#
# M53 (NetSurf browser platform) — public suffix list.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

. "$ROOT_DIR/tools/toolchain-env.sh"

# libnspsl ships inside the netsurf-all bundle that build-netsurf-fb.sh extracts.
SRC_PARENT="$ROOT_DIR/build/netsurf-src"
SRC_DIR="$(ls -d "$SRC_PARENT"/netsurf-all-*/libnspsl 2>/dev/null | head -1)"
if [ -z "$SRC_DIR" ] || [ ! -f "$SRC_DIR/src/nspsl.c" ]; then
  echo "build-libnspsl.sh: libnspsl source not found (run build-netsurf-fb.sh first)" >&2
  exit 1
fi

BUILD_DIR="$ROOT_DIR/build/libnspsl-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
INSTALL_DIR="$BUILD_DIR/install"
mkdir -p "$OBJ_DIR" "$INSTALL_DIR/include" "$INSTALL_DIR/lib"

if [ "$B1NIX_ARCH" = "x86" ]; then TARGET="i686-unknown-elf"; else TARGET="x86_64-unknown-elf"; fi

CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fno-strict-aliasing -Db1nix -I$SRC_DIR/include -I$SRC_DIR/src"

# shellcheck disable=SC2086
clang $CFLAGS -c "$SRC_DIR/src/nspsl.c" -o "$OBJ_DIR/nspsl.o"
"$AR_BIN" rcs "$INSTALL_DIR/lib/libnspsl.a" "$OBJ_DIR/nspsl.o"
cp "$SRC_DIR/include/nspsl.h" "$INSTALL_DIR/include/"

echo "$INSTALL_DIR"
