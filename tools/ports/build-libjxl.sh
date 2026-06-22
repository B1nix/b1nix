#!/bin/sh
# Build libjxl (JPEG-XL decoder) as static libs for the b1nix userspace ABI,
# via CMake cross-compile with the b1nix cross g++/libstdc++ toolchain. Decode
# only; every optional component (tools, tests, benchmark, examples, jpegli,
# transcode-jpeg, plugins, fuzzers, devtools, docs, sjpeg) is disabled. highway
# and brotli are built from third_party/. Prints the install dir.
#
# M53 (NetSurf browser platform) — JPEG-XL image decoder. JXL is rarely served
# on the web (Chrome dropped it), so this is for format completeness.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
VER="${LIBJXL_VERSION:-0.11.1}"

. "$ROOT_DIR/tools/toolchain/env.sh"

# Pinned submodule commits (libjxl's GitHub source tarball excludes submodules;
# deps.sh fetches these — we do the equivalent so the build is self-contained).
HWY_VER="${HIGHWAY_VERSION:-1.2.0}"
BROTLI_VER="${BROTLI_VERSION:-1.1.0}"
SKCMS_COMMIT="42030a771244ba67f86b1c1c76a6493f873c5f91"

SRC_PARENT="$ROOT_DIR/build/ports-src"
SRC_DIR="$SRC_PARENT/libjxl-${VER}"
mkdir -p "$SRC_PARENT"

fetch() { # $1 url  $2 outfile
  [ -f "$2" ] && return 0
  curl -fL "$1" -o "$2" 1>&2 || wget -O "$2" "$1" 1>&2
}

if [ ! -f "$SRC_DIR/lib/jxl/decode.cc" ]; then
  fetch "https://github.com/libjxl/libjxl/archive/refs/tags/v${VER}.tar.gz" "$SRC_PARENT/libjxl-${VER}.tar.gz"
  tar -xzf "$SRC_PARENT/libjxl-${VER}.tar.gz" -C "$SRC_PARENT" 1>&2
fi
# Place the submodule deps under third_party/ (the tarball ships empty dirs).
if [ ! -f "$SRC_DIR/third_party/highway/hwy/base.h" ]; then
  fetch "https://github.com/google/highway/archive/refs/tags/${HWY_VER}.tar.gz" "$SRC_PARENT/highway-${HWY_VER}.tar.gz"
  tar -xzf "$SRC_PARENT/highway-${HWY_VER}.tar.gz" -C "$SRC_PARENT" 1>&2
  rm -rf "$SRC_DIR/third_party/highway"; cp -a "$SRC_PARENT/highway-${HWY_VER}" "$SRC_DIR/third_party/highway"
fi
if [ ! -f "$SRC_DIR/third_party/brotli/c/include/brotli/decode.h" ]; then
  fetch "https://github.com/google/brotli/archive/refs/tags/v${BROTLI_VER}.tar.gz" "$SRC_PARENT/brotli-${BROTLI_VER}.tar.gz"
  tar -xzf "$SRC_PARENT/brotli-${BROTLI_VER}.tar.gz" -C "$SRC_PARENT" 1>&2
  rm -rf "$SRC_DIR/third_party/brotli"; cp -a "$SRC_PARENT/brotli-${BROTLI_VER}" "$SRC_DIR/third_party/brotli"
fi
if [ ! -f "$SRC_DIR/third_party/skcms/skcms.h" ]; then
  fetch "https://skia.googlesource.com/skcms/+archive/${SKCMS_COMMIT}.tar.gz" "$SRC_PARENT/skcms-${SKCMS_COMMIT}.tar.gz"
  mkdir -p "$SRC_DIR/third_party/skcms"
  tar -xzf "$SRC_PARENT/skcms-${SKCMS_COMMIT}.tar.gz" -C "$SRC_DIR/third_party/skcms" 1>&2
fi
# highway's nanobenchmark.cc uses std::mt19937/std::shuffle, which this
# libstdc++ #if's out; it's a benchmark helper unused by the decoder, so drop
# the random shuffles (idempotent).
NB="$SRC_DIR/third_party/highway/hwy/nanobenchmark.cc"
if [ -f "$NB" ] && grep -q 'std::mt19937 rng;' "$NB"; then
  perl -0pi -e 's{\s*std::mt19937 rng;\n\s*std::shuffle\([^;]*\);}{ /* b1nix: no std::mt19937 in this libstdc++; skip benchmark shuffle */}g' "$NB"
fi

# Stage the b1nix libc into the cross toolchain so libstdc++ headers resolve.
"$ROOT_DIR/tools/toolchain/enable-cxx-toolchain.sh" "$B1NIX_TRIPLET" 1>&2 || true

CROSS="$(dirname "$(command -v "$B1NIX_TRIPLET-gcc" 2>/dev/null || echo "$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/cross/bin/$B1NIX_TRIPLET-gcc")")"
GCC="$CROSS/$B1NIX_TRIPLET-gcc"
GXX="$CROSS/$B1NIX_TRIPLET-g++"
SYSROOT="$("$GCC" -print-sysroot 2>/dev/null)"
[ -n "$SYSROOT" ] || SYSROOT="$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/cross/$B1NIX_TRIPLET"

BUILD_DIR="$ROOT_DIR/build/libjxl-b1nix/$B1NIX_TRIPLET"
INSTALL_DIR="$BUILD_DIR/install"
mkdir -p "$BUILD_DIR/cmake"

# CMake toolchain file for b1nix. b1nix is unix-like (pthreads in libb1nix), so
# present as Linux for POSIX/threads detection, but force try_compile to build a
# static library (we cannot run b1nix test binaries on the host).
TC="$BUILD_DIR/b1nix-toolchain.cmake"
cat > "$TC" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "$GCC")
set(CMAKE_CXX_COMPILER "$GXX")
set(CMAKE_SYSROOT "$SYSROOT")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH "$SYSROOT")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(THREADS_PREFER_PTHREAD_FLAG OFF)
# pthread lives in libb1nix (no -pthread spec on this cross gcc).
set(CMAKE_THREAD_LIBS_INIT "")
set(CMAKE_HAVE_THREADS_LIBRARY ON)
set(CMAKE_USE_PTHREADS_INIT ON)
EOF

cd "$BUILD_DIR/cmake"
cmake "$SRC_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$TC" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
  -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_TESTING=OFF \
  -DJPEGXL_STATIC=ON \
  -DJPEGXL_ENABLE_TOOLS=OFF \
  -DJPEGXL_ENABLE_BENCHMARK=OFF \
  -DJPEGXL_ENABLE_EXAMPLES=OFF \
  -DJPEGXL_ENABLE_MANPAGES=OFF \
  -DJPEGXL_ENABLE_DOXYGEN=OFF \
  -DJPEGXL_ENABLE_JPEGLI=OFF \
  -DJPEGXL_ENABLE_TRANSCODE_JPEG=OFF \
  -DJPEGXL_ENABLE_JNI=OFF \
  -DJPEGXL_ENABLE_SJPEG=OFF \
  -DJPEGXL_ENABLE_OPENEXR=OFF \
  -DJPEGXL_ENABLE_PLUGINS=OFF \
  -DJPEGXL_ENABLE_FUZZERS=OFF \
  -DJPEGXL_ENABLE_DEVTOOLS=OFF \
  -DJPEGXL_ENABLE_SKCMS=ON \
  -DJPEGXL_BUNDLE_LIBPNG=OFF \
  -DJPEGXL_FORCE_SYSTEM_BROTLI=OFF \
  -DJPEGXL_FORCE_SYSTEM_HWY=OFF \
  -DHWY_ENABLE_CONTRIB=OFF \
  -DHWY_ENABLE_EXAMPLES=OFF \
  -DHWY_ENABLE_TESTS=OFF \
  -DHWY_ENABLE_INSTALL=OFF \
  -DBROTLI_DISABLE_TESTS=ON \
  1>&2

# Build only the decode libraries (and their deps: hwy, brotlidec/common). Do
# NOT build "all" — the brotli CLI exe needs log2 from a libm we don't link here.
cmake --build . --target jxl_dec jxl_cms -j4 1>&2

mkdir -p "$INSTALL_DIR/include/jxl" "$INSTALL_DIR/lib"
# Public headers ship in the source tree; a couple are configured into the build.
cp "$SRC_DIR/lib/include/jxl/"*.h "$INSTALL_DIR/include/jxl/" 2>/dev/null || true
cp "$BUILD_DIR/cmake/lib/include/jxl/"*.h "$INSTALL_DIR/include/jxl/" 2>/dev/null || true
# Decode lib + its static deps (color management, SIMD, brotli decompress).
for la in libjxl_dec.a libjxl_cms.a libhwy.a libbrotlidec.a libbrotlicommon.a; do
  f=$(find "$BUILD_DIR/cmake" -name "$la" 2>/dev/null | head -1)
  [ -n "$f" ] && cp "$f" "$INSTALL_DIR/lib/"
done

echo "$INSTALL_DIR"
