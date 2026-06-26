#!/bin/sh
# tools/patches/freetype/no-sse2.sh — applied by build-freetype.sh via the port
# driver PATCHES= mechanism (runs as `<this> <srcdir>`).
#
# The smooth rasterizer's SSE2 fast path needs <emmintrin.h>, absent in the
# freestanding sysroot. Force the generic (correct, unaccelerated) path.
# Idempotent (no-op once FT_SSE2 is already 0).
set -eu
SRC_DIR="${1:?usage: no-sse2.sh <freetype-src-dir>}"
if sed --version >/dev/null 2>&1; then sed_inplace() { sed -i "$@"; }
else sed_inplace() { sed -i '' "$@"; }; fi

sed_inplace 's/^#  define FT_SSE2 1/#  define FT_SSE2 0/' \
  "$SRC_DIR/src/smooth/ftgrays.c"
