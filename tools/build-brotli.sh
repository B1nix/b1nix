#!/bin/sh
# Build the Brotli decoder (libbrotlidec + libbrotlicommon) as static libs for
# the b1nix userspace ABI, via CMake cross-compile. Decoder only — that is all
# curl needs to handle "Content-Encoding: br" responses. Prints the install dir.
#
# M54 (port feature enablement): curl --with-brotli. Brotli is pure C; the same
# cross toolchain already builds it inside libjxl's third_party tree.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
VER="${BROTLI_VERSION:-1.1.0}"

. "$ROOT_DIR/tools/toolchain-env.sh"

SRC_PARENT="$ROOT_DIR/build/ports-src"
SRC_DIR="$SRC_PARENT/brotli-${VER}"
mkdir -p "$SRC_PARENT"

fetch() { # $1 url  $2 outfile
  [ -f "$2" ] && return 0
  curl -fL "$1" -o "$2" 1>&2 || wget -O "$2" "$1" 1>&2
}

if [ ! -f "$SRC_DIR/c/include/brotli/decode.h" ]; then
  fetch "https://github.com/google/brotli/archive/refs/tags/v${VER}.tar.gz" "$SRC_PARENT/brotli-${VER}.tar.gz"
  tar -xzf "$SRC_PARENT/brotli-${VER}.tar.gz" -C "$SRC_PARENT" 1>&2
fi

# Stage the b1nix libc into the cross toolchain sysroot so C headers resolve.
"$ROOT_DIR/tools/enable-cxx-toolchain.sh" "$B1NIX_TRIPLET" 1>&2 || true

CROSS="$(dirname "$(command -v "$B1NIX_TRIPLET-gcc" 2>/dev/null || echo "$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/cross/bin/$B1NIX_TRIPLET-gcc")")"
GCC="$CROSS/$B1NIX_TRIPLET-gcc"
SYSROOT="$("$GCC" -print-sysroot 2>/dev/null)"
[ -n "$SYSROOT" ] || SYSROOT="$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/cross/$B1NIX_TRIPLET"

BUILD_DIR="$ROOT_DIR/build/brotli-b1nix/$B1NIX_TRIPLET"
INSTALL_DIR="$BUILD_DIR/install"
mkdir -p "$BUILD_DIR/cmake" "$INSTALL_DIR/lib" "$INSTALL_DIR/include"

# Same b1nix CMake toolchain file pattern as build-libjxl.sh: present as Linux
# for POSIX detection, force try_compile to a static library (host cannot run
# b1nix test binaries).
TC="$BUILD_DIR/b1nix-toolchain.cmake"
cat > "$TC" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "$GCC")
set(CMAKE_SYSROOT "$SYSROOT")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH "$SYSROOT")
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
  -DBROTLI_DISABLE_TESTS=ON \
  1>&2

# Decoder only: brotlidec + its dependency brotlicommon (each a separate static
# archive — curl links both).
cmake --build . --target brotlicommon brotlidec -j"${JOBS:-4}" 1>&2

cp -R "$SRC_DIR/c/include/brotli" "$INSTALL_DIR/include/"
for la in libbrotlidec.a libbrotlicommon.a; do
  f=$(find "$BUILD_DIR/cmake" -name "$la" 2>/dev/null | head -1)
  [ -n "$f" ] && cp "$f" "$INSTALL_DIR/lib/"
done

echo "$INSTALL_DIR"
