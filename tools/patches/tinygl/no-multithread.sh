#!/bin/sh
# tools/patches/tinygl/no-multithread.sh — applied by build-tinygl.sh via the
# port driver PATCHES= mechanism (runs as `<this> <srcdir>`).
#
# Single-threaded smoke: disable the multithreaded raster paths (OpenMP pragmas)
# so the port has no thread runtime dependency. Idempotent.
set -eu
SRC_DIR="${1:?usage: no-multithread.sh <tinygl-src-dir>}"
if sed --version >/dev/null 2>&1; then sed_inplace() { sed -i -E "$@"; }
else sed_inplace() { sed -i '' -E "$@"; }; fi

sed_inplace 's/(#define TGL_FEATURE_MULTITHREADED_[A-Z_]+)[[:space:]]+1/\1 0/' \
  "$SRC_DIR/include/zfeatures.h"
