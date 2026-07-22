#!/bin/sh
# Build HarfBuzz (HB_TINY, no FreeType/glib/icu) as a static libharfbuzz.a for
# the b1nix userspace ABI. Uses the cport driver (tools/ports/drivers/cport.sh).
# Unified build via src/harfbuzz.cc, compiled with cross clang++/libc++.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

CPORT_NAME=harfbuzz
CPORT_ARCHIVE=libharfbuzz.a
CPORT_SRCNAME="harfbuzz-${HB_VERSION:-8.3.0}"
CPORT_URL="https://github.com/harfbuzz/harfbuzz/releases/download/${HB_VERSION:-8.3.0}/harfbuzz-${HB_VERSION:-8.3.0}.tar.xz"
CPORT_TARBALL="harfbuzz-${HB_VERSION:-8.3.0}.tar.xz"
CPORT_HEADERS="flat:src/hb.h tree:src"

port_pre_build() {
  # HarfBuzz needs C++ compilation — use the LLVM libc++ cross configuration.
  . "$ROOT_DIR/tools/toolchain/env.sh"
  resolve_cxx_cross
  CCACHE="$(command -v ccache 2>/dev/null || true)"
}

port_build() {
  _ccache=""
  [ -n "${CCACHE:-}" ] && [ "${B1NIX_NO_CCACHE:-0}" != "1" ] && _ccache="$CCACHE "
  # shellcheck disable=SC2086
  ${_ccache}"$CXX_CROSS" $CXXFLAGS_CROSS -fPIC -DHB_TINY -DHB_NO_MT \
    -fno-exceptions -fno-rtti -fno-threadsafe-statics -std=c++14 \
    -I"$SRC_DIR/src" -c "$SRC_DIR/src/harfbuzz.cc" \
    -o "$OBJ_DIR/harfbuzz.o"
  OBJS="$OBJ_DIR/harfbuzz.o"
}

port_install_headers() {
  cp "$SRC_DIR/src/hb.h" "$SRC_DIR/src"/hb-*.h "$INSTALL_DIR/include/" 2>/dev/null || true
}

. "$ROOT_DIR/tools/ports/drivers/cport.sh"
