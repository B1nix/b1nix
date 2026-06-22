#!/bin/sh
# Build libnslog as a static libnslog.a for the b1nix userspace ABI.
# Freestanding curated compile (no autotools). libnslog is NetSurf's logging
# library; its category-filter expression language is a flex/bison grammar, so
# the lexer/parser are generated at build time exactly as the upstream Makefile
# does (bison 3.x api.prefix={filter_}). Prints the install dir.
#
# M53 (NetSurf browser platform) — browser lib chain helper (step 6).

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
NSLOG_VERSION="${NSLOG_VERSION:-0.1.3}"
TARBALL="libnslog-${NSLOG_VERSION}-src.tar.gz"
URL="https://download.netsurf-browser.org/libs/releases/${TARBALL}"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"
FLEX_BIN="${FLEX:-$(command -v flex)}"
BISON_BIN="${BISON:-$(command -v /opt/homebrew/opt/bison/bin/bison 2>/dev/null || command -v bison)}"

. "$ROOT_DIR/tools/toolchain/env.sh"

SRC_PARENT="$ROOT_DIR/build/netsurf-src"
SRC_DIR="$SRC_PARENT/libnslog-${NSLOG_VERSION}"
BUILD_DIR="$ROOT_DIR/build/libnslog-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
GEN_DIR="$BUILD_DIR/gen"
INSTALL_DIR="$BUILD_DIR/install"

mkdir -p "$SRC_PARENT" "$OBJ_DIR" "$GEN_DIR" "$INSTALL_DIR/include" "$INSTALL_DIR/lib"

BISON_MAJOR="$("$BISON_BIN" --version | sed -n '1s/.* \([0-9][0-9]*\)\..*/\1/p')"
if [ -z "$BISON_MAJOR" ] || [ "$BISON_MAJOR" -lt 3 ]; then
  echo "build-libnslog.sh: bison 3.x is required for filter-parser.y (found: $("$BISON_BIN" --version | head -1))" >&2
  exit 1
fi

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$TARBALL"
  [ -f "$tmp" ] || curl -L "$URL" -o "$tmp" 1>&2
  tar -xzf "$tmp" -C "$SRC_PARENT" 1>&2
fi

# Generate the filter parser (bison) and lexer (flex) into GEN_DIR, then wrap
# the lexer .inc in a .c (mirrors the upstream src/Makefile rules).
"$BISON_BIN" -d -t --define=api.prefix={filter_} \
  --output="$GEN_DIR/filter-parser.c" --defines="$GEN_DIR/filter-parser.h" \
  "$SRC_DIR/src/filter-parser.y" 1>&2
"$FLEX_BIN" --outfile="$GEN_DIR/filter-lexer.inc" \
  --header-file="$GEN_DIR/filter-lexer.h" "$SRC_DIR/src/filter-lexer.l" 1>&2
{
  echo '#ifndef __clang_analyzer__'
  echo '#include "filter-lexer.inc"'
  echo '#endif'
} >"$GEN_DIR/filter-lexer.c"

if [ "$B1NIX_ARCH" = "x86" ]; then
  TARGET="i686-unknown-elf"
else
  TARGET="x86_64-unknown-elf"
fi

# -IGEN for the generated parser/lexer headers, -Isrc for nslog_internal.h.
CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fno-strict-aliasing -Db1nix -I$SRC_DIR/include -I$SRC_DIR/src -I$GEN_DIR
  -Wno-implicit-function-declaration -Wno-unused-function"

OBJS=""
for src in "$SRC_DIR/src/core.c" "$SRC_DIR/src/filter.c" \
           "$GEN_DIR/filter-parser.c" "$GEN_DIR/filter-lexer.c"; do
  obj="$OBJ_DIR/$(basename "$src").o"
  # shellcheck disable=SC2086
  clang $CFLAGS -c "$src" -o "$obj"
  OBJS="$OBJS $obj"
done

# shellcheck disable=SC2086
"$AR_BIN" rcs "$INSTALL_DIR/lib/libnslog.a" $OBJS
cp -R "$SRC_DIR/include/nslog" "$INSTALL_DIR/include/"

echo "$INSTALL_DIR"
