#!/bin/sh
# Build litehtml (C++ HTML/CSS layout engine) + its bundled gumbo HTML parser as
# static libs for the b1nix userspace ABI, via CMake cross-compile with the
# b1nix cross g++ / libstdc++. M55 "validate the C++ runtime with a real modern
# engine" — litehtml exercises libstdc++ exceptions, RTTI, std::string/vector/
# map/shared_ptr end to end. Prints the install dir.
#
# Build logic lives in tools/ports/drivers/cmake.sh; this is the manifest.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

CMAKE_NAME=litehtml
VER="${LITEHTML_VERSION:-master}"
CMAKE_SRCNAME="litehtml-${VER}"
CMAKE_SENTINEL="include/litehtml/document.h"
CMAKE_TARBALL="litehtml-${VER}.tar.gz"
CMAKE_URL="https://github.com/litehtml/litehtml/archive/refs/heads/${VER}.tar.gz"
CMAKE_NEED_CXX=1
CMAKE_ARGS="-DCMAKE_CXX_STANDARD=17 -DBUILD_SHARED_LIBS=OFF -DLITEHTML_BUILD_TESTING=OFF -DEXTERNAL_GUMBO=OFF"
CMAKE_TARGETS="litehtml gumbo"

port_install() {
  # litehtml public headers + master.css are under include/.
  cp -R "$SRC_DIR/include/." "$INSTALL_DIR/include/"
  cmake_copy_archive liblitehtml.a
  cmake_copy_archive libgumbo.a
}

. "$ROOT_DIR/tools/ports/drivers/cmake.sh"
