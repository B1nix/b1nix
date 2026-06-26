#!/bin/sh
# Build libnslog as a static libnslog.a for the b1nix userspace ABI.
# Freestanding curated compile (no autotools). libnslog is NetSurf's logging
# library; its category-filter expression language is a flex/bison grammar, so
# the lexer/parser are generated at build time exactly as the upstream Makefile
# does (bison 3.x api.prefix={filter_}). Prints the install dir.
#
# M53 (NetSurf browser platform) — browser lib chain helper (step 6).
# Build logic lives in tools/ports/drivers/nslib.sh; this is the manifest.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

NSLIB_NAME=libnslog
NSLIB_VERSION="${NSLOG_VERSION:-0.1.3}"
NSLIB_ARCHIVE=libnslog.a
NSLIB_SOURCES="src/core.c src/filter.c"
NSLIB_CFLAGS="-Wno-implicit-function-declaration -Wno-unused-function"
NSLIB_HEADERS="tree:include/nslog"

# Generate the filter parser (bison) and lexer (flex) into GEN_DIR, then wrap
# the lexer .inc in a .c (mirrors the upstream src/Makefile rules), and add the
# generated TUs + their include dir to the build.
port_pre_build() {
  FLEX_BIN="${FLEX:-$(command -v flex)}"
  BISON_BIN="${BISON:-$(command -v /opt/homebrew/opt/bison/bin/bison 2>/dev/null || command -v bison)}"
  BISON_MAJOR="$("$BISON_BIN" --version | sed -n '1s/.* \([0-9][0-9]*\)\..*/\1/p')"
  if [ -z "$BISON_MAJOR" ] || [ "$BISON_MAJOR" -lt 3 ]; then
    echo "build-libnslog.sh: bison 3.x is required for filter-parser.y (found: $("$BISON_BIN" --version | head -1))" >&2
    exit 1
  fi
  GEN_DIR="$BUILD_DIR/gen"
  mkdir -p "$GEN_DIR"
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
  NSLIB_CFLAGS="$NSLIB_CFLAGS -I$GEN_DIR"
  NSLIB_EXTRA_SOURCES="$GEN_DIR/filter-parser.c $GEN_DIR/filter-lexer.c"
}

. "$ROOT_DIR/tools/ports/drivers/nslib.sh"
