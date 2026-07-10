#!/bin/sh
# Build libjxl (JPEG-XL decoder) as static libs for the b1nix userspace ABI, via
# CMake cross-compile with the b1nix cross clang++/libc++ toolchain. Decode only;
# every optional component (tools, tests, benchmark, examples, jpegli,
# transcode-jpeg, plugins, fuzzers, devtools, docs, sjpeg) is disabled. highway
# and brotli are built from third_party/. Prints the install dir.
#
# M53 (NetSurf browser platform) — JPEG-XL image decoder.
# Build logic lives in tools/ports/drivers/cmake.sh; this is the manifest.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

CMAKE_NAME=libjxl
VER="${LIBJXL_VERSION:-0.11.1}"
CMAKE_SRCNAME="libjxl-${VER}"
CMAKE_SENTINEL="lib/jxl/decode.cc"
CMAKE_NEED_CXX=1
# b1nix is unix-like (pthreads in libb1nix); the bundled libc++ detection wants
# threads, but the clang cross driver has no hosted pthread spec, so wire threads
# in by hand.
CMAKE_TC_EXTRA='set(THREADS_PREFER_PTHREAD_FLAG OFF)
set(CMAKE_THREAD_LIBS_INIT "")
set(CMAKE_HAVE_THREADS_LIBRARY ON)
set(CMAKE_USE_PTHREADS_INIT ON)'
CMAKE_ARGS="-DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DJPEGXL_STATIC=ON \
-DJPEGXL_ENABLE_TOOLS=OFF -DJPEGXL_ENABLE_BENCHMARK=OFF -DJPEGXL_ENABLE_EXAMPLES=OFF \
-DJPEGXL_ENABLE_MANPAGES=OFF -DJPEGXL_ENABLE_DOXYGEN=OFF -DJPEGXL_ENABLE_JPEGLI=OFF \
-DJPEGXL_ENABLE_TRANSCODE_JPEG=OFF -DJPEGXL_ENABLE_JNI=OFF -DJPEGXL_ENABLE_SJPEG=OFF \
-DJPEGXL_ENABLE_OPENEXR=OFF -DJPEGXL_ENABLE_PLUGINS=OFF -DJPEGXL_ENABLE_FUZZERS=OFF \
-DJPEGXL_ENABLE_DEVTOOLS=OFF -DJPEGXL_ENABLE_SKCMS=ON -DJPEGXL_BUNDLE_LIBPNG=OFF \
-DJPEGXL_FORCE_SYSTEM_BROTLI=OFF -DJPEGXL_FORCE_SYSTEM_HWY=OFF -DHWY_ENABLE_CONTRIB=OFF \
-DHWY_ENABLE_EXAMPLES=OFF -DHWY_ENABLE_TESTS=OFF -DHWY_ENABLE_INSTALL=OFF -DBROTLI_DISABLE_TESTS=ON"
# Build only the decode libraries (and their deps: hwy, brotlidec/common). Do NOT
# build "all" — the brotli CLI exe needs log2 from a libm we don't link here.
CMAKE_TARGETS="jxl_dec jxl_cms"
PATCHES="libjxl/no-mt19937.sh"

# Pinned submodule commits (libjxl's GitHub source tarball excludes submodules;
# deps.sh fetches these — do the equivalent so the build is self-contained).
port_fetch() {
  HWY_VER="${HIGHWAY_VERSION:-1.2.0}"
  BROTLI_VER="${BROTLI_VERSION:-1.1.0}"
  SKCMS_COMMIT="42030a771244ba67f86b1c1c76a6493f873c5f91"
  port_fetch_tarball "https://github.com/libjxl/libjxl/archive/refs/tags/v${VER}.tar.gz" \
    "$SRC_PARENT/libjxl-${VER}.tar.gz" "$SRC_PARENT" "$SRC_DIR/$CMAKE_SENTINEL"
  if [ ! -f "$SRC_DIR/third_party/highway/hwy/base.h" ]; then
    port_fetch_tarball "https://github.com/google/highway/archive/refs/tags/${HWY_VER}.tar.gz" \
      "$SRC_PARENT/highway-${HWY_VER}.tar.gz" "$SRC_PARENT" "$SRC_PARENT/highway-${HWY_VER}"
    rm -rf "$SRC_DIR/third_party/highway"; cp -a "$SRC_PARENT/highway-${HWY_VER}" "$SRC_DIR/third_party/highway"
  fi
  if [ ! -f "$SRC_DIR/third_party/brotli/c/include/brotli/decode.h" ]; then
    port_fetch_tarball "https://github.com/google/brotli/archive/refs/tags/v${BROTLI_VER}.tar.gz" \
      "$SRC_PARENT/brotli-${BROTLI_VER}.tar.gz" "$SRC_PARENT" "$SRC_PARENT/brotli-${BROTLI_VER}"
    rm -rf "$SRC_DIR/third_party/brotli"; cp -a "$SRC_PARENT/brotli-${BROTLI_VER}" "$SRC_DIR/third_party/brotli"
  fi
  if [ ! -f "$SRC_DIR/third_party/skcms/skcms.h" ]; then
    [ -f "$SRC_PARENT/skcms-${SKCMS_COMMIT}.tar.gz" ] || \
      { curl -fL "https://skia.googlesource.com/skcms/+archive/${SKCMS_COMMIT}.tar.gz" -o "$SRC_PARENT/skcms-${SKCMS_COMMIT}.tar.gz" 1>&2 || \
        wget -O "$SRC_PARENT/skcms-${SKCMS_COMMIT}.tar.gz" "https://skia.googlesource.com/skcms/+archive/${SKCMS_COMMIT}.tar.gz" 1>&2; }
    mkdir -p "$SRC_DIR/third_party/skcms"
    tar -xzf "$SRC_PARENT/skcms-${SKCMS_COMMIT}.tar.gz" -C "$SRC_DIR/third_party/skcms" 1>&2
  fi
}

port_install() {
  mkdir -p "$INSTALL_DIR/include/jxl"
  cp "$SRC_DIR/lib/include/jxl/"*.h "$INSTALL_DIR/include/jxl/" 2>/dev/null || true
  cp "$BUILD_DIR/cmake/lib/include/jxl/"*.h "$INSTALL_DIR/include/jxl/" 2>/dev/null || true
  for la in libjxl_dec.a libjxl_cms.a libhwy.a libbrotlidec.a libbrotlicommon.a; do
    cmake_copy_archive "$la"
  done
}

. "$ROOT_DIR/tools/ports/drivers/cmake.sh"
