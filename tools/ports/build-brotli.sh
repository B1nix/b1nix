#!/bin/sh
# Build the Brotli decoder (libbrotlidec + libbrotlicommon) as static libs for
# the b1nix userspace ABI, via CMake cross-compile. Decoder only — that is all
# curl needs to handle "Content-Encoding: br" responses. Prints the install dir.
#
# M54 (port feature enablement): curl --with-brotli. Brotli is pure C.
# Build logic lives in tools/ports/drivers/cmake.sh; this is the manifest.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

CMAKE_NAME=brotli
VER="${BROTLI_VERSION:-1.1.0}"
CMAKE_SRCNAME="brotli-${VER}"
CMAKE_SENTINEL="c/include/brotli/decode.h"
CMAKE_TARBALL="brotli-${VER}.tar.gz"
CMAKE_URL="https://github.com/google/brotli/archive/refs/tags/v${VER}.tar.gz"
CMAKE_ARGS="-DBUILD_SHARED_LIBS=OFF -DBROTLI_DISABLE_TESTS=ON"
# Decoder only: brotlidec + its dependency brotlicommon (separate static
# archives — curl links both).
CMAKE_TARGETS="brotlicommon brotlidec"

port_install() {
  cp -R "$SRC_DIR/c/include/brotli" "$INSTALL_DIR/include/"
  cmake_copy_archive libbrotlidec.a
  cmake_copy_archive libbrotlicommon.a
}

. "$ROOT_DIR/tools/ports/drivers/cmake.sh"
