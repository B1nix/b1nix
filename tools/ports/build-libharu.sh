#!/bin/sh
# Build libharu (libhpdf) as a static lib for the b1nix userspace ABI, via CMake
# cross-compile with the b1nix cross gcc. libharu is NetSurf's PDF-export backend
# (NETSURF_USE_HARU_PDF: desktop/save_pdf.c + font_haru.c, links -lhpdf -lpng).
# Pure C; needs zlib + libpng (already staged into the netsurf sysroot). Prints
# the install dir.
#
# M53/M54 (NetSurf) — PDF export.
# Build logic lives in tools/ports/drivers/cmake.sh; this is the manifest.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

CMAKE_NAME=libharu
VER="${LIBHARU_VERSION:-2.4.4}"
CMAKE_SRCNAME="libharu-${VER}"
CMAKE_SENTINEL="CMakeLists.txt"
CMAKE_TARBALL="libharu-${VER}.tar.gz"
CMAKE_URL="https://github.com/libharu/libharu/archive/refs/tags/v${VER}.tar.gz"
# Bare libc target (libharu predates b1nix's CXX sysroot staging and does not
# need it); present as Generic, do not stage the C++ sysroot.
CMAKE_SYSTEM_NAME=Generic
CMAKE_STAGE_CXX=0
CMAKE_TARGETS=hpdf

# zlib + libpng (+ headers) live in the netsurf sysroot from build-netsurf-fb.sh.
port_pre_configure() {
  NSROOT="$ROOT_DIR/build/$B1NIX_ARCH/ports/netsurf-fb/sysroot"
  CMAKE_FIND_ROOT_EXTRA="$NSROOT"
  CMAKE_TC_EXTRA="set(CMAKE_C_FLAGS \"-I$NSROOT/include\")"
  CMAKE_ARGS="-DLIBHPDF_SHARED=OFF -DLIBHPDF_STATIC=ON -DLIBHPDF_EXAMPLES=OFF \
-DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF \
-DZLIB_LIBRARY=$NSROOT/lib/libz.a -DZLIB_INCLUDE_DIR=$NSROOT/include \
-DPNG_LIBRARY=$NSROOT/lib/libpng16.a -DPNG_PNG_INCLUDE_DIR=$NSROOT/include"
}

port_install() {
  cp "$SRC_DIR/include/"*.h "$INSTALL_DIR/include/" 2>/dev/null || true
  find "$BUILD_DIR/cmake" -name 'hpdf_config.h' -exec cp {} "$INSTALL_DIR/include/" \; 2>/dev/null || true
  _f="$(find "$BUILD_DIR/cmake" -name 'libhpdf*.a' 2>/dev/null | head -1)"
  [ -n "$_f" ] && cp "$_f" "$INSTALL_DIR/lib/libhpdf.a"
}

. "$ROOT_DIR/tools/ports/drivers/cmake.sh"
