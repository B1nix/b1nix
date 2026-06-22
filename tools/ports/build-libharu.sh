#!/bin/sh
# Build libharu (libhpdf) as a static lib for the b1nix userspace ABI, via CMake
# cross-compile with the b1nix cross gcc. libharu is NetSurf's PDF-export backend
# (NETSURF_USE_HARU_PDF: desktop/save_pdf.c + font_haru.c, links -lhpdf -lpng).
# Pure C; needs zlib + libpng (already staged into the netsurf sysroot). Prints
# the install dir.
#
# M53/M54 (NetSurf) — PDF export.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
VER="${LIBHARU_VERSION:-2.4.4}"

. "$ROOT_DIR/tools/toolchain/env.sh"

SRC_PARENT="$ROOT_DIR/build/ports-src"
SRC_DIR="$SRC_PARENT/libharu-${VER}"
mkdir -p "$SRC_PARENT"
if [ ! -f "$SRC_DIR/CMakeLists.txt" ]; then
  TARBALL="$SRC_PARENT/libharu-${VER}.tar.gz"
  URL="https://github.com/libharu/libharu/archive/refs/tags/v${VER}.tar.gz"
  [ -f "$TARBALL" ] || { curl -fL "$URL" -o "$TARBALL" 1>&2 || wget -O "$TARBALL" "$URL" 1>&2; }
  tar -xzf "$TARBALL" -C "$SRC_PARENT" 1>&2
fi

CROSS="$(dirname "$(command -v "$B1NIX_TRIPLET-gcc" 2>/dev/null || echo "$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/cross/bin/$B1NIX_TRIPLET-gcc")")"
GCC="$CROSS/$B1NIX_TRIPLET-gcc"
SYSROOT="$("$GCC" -print-sysroot 2>/dev/null)"
[ -n "$SYSROOT" ] || SYSROOT="$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/cross/$B1NIX_TRIPLET"

# zlib + libpng (+ their headers) live in the netsurf sysroot from build-netsurf-fb.sh.
NSROOT="$ROOT_DIR/build/netsurf-sysroot/$B1NIX_TRIPLET"

BUILD_DIR="$ROOT_DIR/build/libharu-b1nix/$B1NIX_TRIPLET"
INSTALL_DIR="$BUILD_DIR/install"
mkdir -p "$BUILD_DIR/cmake"

TC="$BUILD_DIR/b1nix-toolchain.cmake"
cat > "$TC" <<EOF
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR ${B1NIX_GCC_ARCH})
set(CMAKE_C_COMPILER "$GCC")
set(CMAKE_SYSROOT "$SYSROOT")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_FLAGS "-I$NSROOT/include")
set(CMAKE_FIND_ROOT_PATH "$SYSROOT;$NSROOT")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF

cd "$BUILD_DIR/cmake"
cmake "$SRC_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$TC" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
  -DBUILD_SHARED_LIBS=OFF \
  -DLIBHPDF_SHARED=OFF \
  -DLIBHPDF_STATIC=ON \
  -DLIBHPDF_EXAMPLES=OFF \
  -DBUILD_TESTING=OFF \
  -DZLIB_LIBRARY="$NSROOT/lib/libz.a" \
  -DZLIB_INCLUDE_DIR="$NSROOT/include" \
  -DPNG_LIBRARY="$NSROOT/lib/libpng16.a" \
  -DPNG_PNG_INCLUDE_DIR="$NSROOT/include" \
  1>&2

cmake --build . --target hpdf -j4 1>&2 || cmake --build . -j4 1>&2

mkdir -p "$INSTALL_DIR/include" "$INSTALL_DIR/lib"
# Headers: libharu installs hpdf*.h; copy from src/include and the configured one.
cp "$SRC_DIR/include/"*.h "$INSTALL_DIR/include/" 2>/dev/null || true
find "$BUILD_DIR/cmake" -name 'hpdf_config.h' -exec cp {} "$INSTALL_DIR/include/" \; 2>/dev/null || true
find "$BUILD_DIR/cmake" -name 'libhpdf*.a' -exec cp {} "$INSTALL_DIR/lib/libhpdf.a" \; 2>/dev/null || true

echo "$INSTALL_DIR"
