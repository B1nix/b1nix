#!/bin/sh
# Build utf8proc as a static libutf8proc.a for the b1nix userspace ABI.
# Freestanding curated compile (no autotools). utf8proc is a single-file Unicode
# library; NetSurf uses it (utils/idna.c) for IDNA Unicode normalisation /
# property lookup. It ships inside the netsurf-all bundle. The header installs
# under libutf8proc/ (NetSurf includes <libutf8proc/utf8proc.h>). Prints the
# install dir.
#
# M53 (NetSurf browser platform) — Unicode / IDNA support.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

. "$ROOT_DIR/tools/toolchain/env.sh"

SRC_PARENT="$ROOT_DIR/build/netsurf-src"
SRC_DIR="$(ls -d "$SRC_PARENT"/netsurf-all-*/libutf8proc 2>/dev/null | head -1)"
if [ -z "$SRC_DIR" ] || [ ! -f "$SRC_DIR/src/utf8proc.c" ]; then
  echo "build-libutf8proc.sh: libutf8proc source not found (run build-netsurf-fb.sh first)" >&2
  exit 1
fi

BUILD_DIR="$ROOT_DIR/build/libutf8proc-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
INSTALL_DIR="$BUILD_DIR/install"
mkdir -p "$OBJ_DIR" "$INSTALL_DIR/include/libutf8proc" "$INSTALL_DIR/lib"

if [ "$B1NIX_ARCH" = "x86" ]; then TARGET="i686-unknown-elf"; else TARGET="x86_64-unknown-elf"; fi

# UTF8PROC_STATIC avoids dllexport attributes; utf8proc.c #includes utf8proc_data.c.
CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fno-strict-aliasing -Db1nix -DUTF8PROC_STATIC
  -I$SRC_DIR/src -I$SRC_DIR/include/libutf8proc
  -Wno-implicit-function-declaration"

# shellcheck disable=SC2086
clang $CFLAGS -c "$SRC_DIR/src/utf8proc.c" -o "$OBJ_DIR/utf8proc.o"
"$AR_BIN" rcs "$INSTALL_DIR/lib/libutf8proc.a" "$OBJ_DIR/utf8proc.o"
cp "$SRC_DIR/include/libutf8proc/utf8proc.h" "$INSTALL_DIR/include/libutf8proc/"

echo "$INSTALL_DIR"
