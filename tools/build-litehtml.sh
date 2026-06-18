#!/bin/sh
# Build litehtml (C++ HTML/CSS layout engine) + its bundled gumbo HTML parser as
# static libs for the b1nix userspace ABI, via CMake cross-compile with the
# b1nix cross g++ / libstdc++. This is the M55 "validate the C++ runtime with a
# real modern engine" milestone — litehtml exercises libstdc++ exceptions, RTTI,
# std::string/vector/map/shared_ptr end to end. Prints the install dir.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
VER="${LITEHTML_VERSION:-master}"

. "$ROOT_DIR/tools/toolchain-env.sh"

SRC_PARENT="$ROOT_DIR/build/ports-src"
SRC_DIR="$SRC_PARENT/litehtml-${VER}"
mkdir -p "$SRC_PARENT"

fetch() { # $1 url  $2 outfile
  [ -f "$2" ] && return 0
  curl -fL "$1" -o "$2" 1>&2 || wget -O "$2" "$1" 1>&2
}

if [ ! -f "$SRC_DIR/include/litehtml/document.h" ]; then
  fetch "https://github.com/litehtml/litehtml/archive/refs/heads/${VER}.tar.gz" "$SRC_PARENT/litehtml-${VER}.tar.gz"
  tar -xzf "$SRC_PARENT/litehtml-${VER}.tar.gz" -C "$SRC_PARENT" 1>&2
fi

# Stage the b1nix libc into the cross toolchain so libstdc++ headers resolve.
"$ROOT_DIR/tools/enable-cxx-toolchain.sh" "$B1NIX_TRIPLET" 1>&2 || true

CROSS="$(dirname "$(command -v "$B1NIX_TRIPLET-gcc" 2>/dev/null || echo "$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/cross/bin/$B1NIX_TRIPLET-gcc")")"
GCC="$CROSS/$B1NIX_TRIPLET-gcc"
GXX="$CROSS/$B1NIX_TRIPLET-g++"
SYSROOT="$("$GCC" -print-sysroot 2>/dev/null)"
[ -n "$SYSROOT" ] || SYSROOT="$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/cross/$B1NIX_TRIPLET"

BUILD_DIR="$ROOT_DIR/build/litehtml-b1nix/$B1NIX_TRIPLET"
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
set(CMAKE_CXX_COMPILER "$GXX")
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
  -DCMAKE_CXX_STANDARD=17 \
  -DBUILD_SHARED_LIBS=OFF \
  -DLITEHTML_BUILD_TESTING=OFF \
  -DEXTERNAL_GUMBO=OFF \
  1>&2

cmake --build . --target litehtml gumbo -j"${JOBS:-4}" 1>&2

# Headers: litehtml public headers + the master.css are under include/.
cp -R "$SRC_DIR/include/." "$INSTALL_DIR/include/"
for la in liblitehtml.a libgumbo.a; do
  f=$(find "$BUILD_DIR/cmake" -name "$la" 2>/dev/null | head -1)
  [ -n "$f" ] && cp "$f" "$INSTALL_DIR/lib/"
done

echo "$INSTALL_DIR"
