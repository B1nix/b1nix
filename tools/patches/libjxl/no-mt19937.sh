#!/bin/sh
# tools/patches/libjxl/no-mt19937.sh — applied by build-libjxl.sh via the port
# driver PATCHES= mechanism (runs as `<this> <srcdir>`).
#
# highway's nanobenchmark.cc uses std::mt19937/std::shuffle, which this
# libstdc++ #if's out; it is a benchmark helper unused by the decoder, so drop
# the random shuffles. Idempotent.
set -eu
SRC_DIR="${1:?usage: no-mt19937.sh <libjxl-src-dir>}"
NB="$SRC_DIR/third_party/highway/hwy/nanobenchmark.cc"
if [ -f "$NB" ] && grep -q 'std::mt19937 rng;' "$NB"; then
  perl -0pi -e 's{\s*std::mt19937 rng;\n\s*std::shuffle\([^;]*\);}{ /* b1nix: no std::mt19937 in this libstdc++; skip benchmark shuffle */}g' "$NB"
fi
