#!/usr/bin/env bash
# tools/build-toolchain.sh - Automates building x86_64-b1nix GCC & Binutils
set -euo pipefail

TARGET="x86_64-b1nix"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORK_DIR="$PROJECT_DIR/build/toolchain_build"
PREFIX="$PROJECT_DIR/build/cross"

BINUTILS_VER="2.41"
GCC_VER="13.2.0"

BINUTILS_URL="https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.xz"
GCC_URL="https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.xz"

mkdir -p "$WORK_DIR" "$PREFIX"

cd "$WORK_DIR"

# 1. Download source tars
if [ ! -f "binutils-${BINUTILS_VER}.tar.xz" ]; then
    echo "Downloading Binutils..."
    curl -LO "$BINUTILS_URL"
fi

if [ ! -f "gcc-${GCC_VER}.tar.xz" ]; then
    echo "Downloading GCC..."
    curl -LO "$GCC_URL"
fi

# 2. Extract and Patch Binutils
if [ ! -d "binutils-${BINUTILS_VER}" ]; then
    echo "Extracting Binutils..."
    tar -xf "binutils-${BINUTILS_VER}.tar.xz"
    echo "Patching Binutils..."
    cd "binutils-${BINUTILS_VER}"
    patch -p1 < "$SCRIPT_DIR/patches/binutils.patch"
    
    # Patch config.sub
    sed -i.bak 's/| fiwix\*/| fiwix* | b1nix*/g' config.sub
    cd ..
fi

# 3. Extract and Patch GCC
if [ ! -d "gcc-${GCC_VER}" ]; then
    echo "Extracting GCC..."
    tar -xf "gcc-${GCC_VER}.tar.xz"
    echo "Patching GCC..."
    python3 "$SCRIPT_DIR/patch-gcc.py"
    
    # Patch config.sub
    cd "gcc-${GCC_VER}"
    sed -i.bak 's/| fiwix\*/| fiwix* | b1nix*/g' config.sub
    # Also patch libgcc/config.sub if it exists
    if [ -f "libgcc/config.sub" ]; then
        sed -i.bak 's/| fiwix\*/| fiwix* | b1nix*/g' libgcc/config.sub
    fi
    cd ..
fi

# 4. Build cross-binutils
if [ ! -f "$PREFIX/bin/${TARGET}-ld" ]; then
    echo "Building Cross-Binutils..."
    mkdir -p build-binutils
    cd build-binutils
    ../binutils-${BINUTILS_VER}/configure \
        --target="$TARGET" \
        --prefix="$PREFIX" \
        --with-sysroot="$PROJECT_DIR/build/x86/rootfs" \
        --disable-nls \
        --disable-werror \
        --with-system-zlib \
        MAKEINFO=true
    make -j2 MAKEINFO=true
    make install MAKEINFO=true
    cd ..
fi

# 5. Build cross-gcc
if [ ! -f "$PREFIX/bin/${TARGET}-gcc" ]; then
    echo "Building Cross-GCC (bootstrap)..."
    mkdir -p build-gcc
    cd build-gcc
    ../gcc-${GCC_VER}/configure \
        --target="$TARGET" \
        --prefix="$PREFIX" \
        --with-sysroot="$PROJECT_DIR/build/x86/rootfs" \
        --disable-nls \
        --enable-languages=c \
        --without-headers \
        --disable-shared \
        --disable-multilib \
        --disable-threads \
        --disable-libgomp \
        --disable-libmudflap \
        --disable-libssp \
        --disable-libquadmath \
        --disable-libstdcxx \
        --with-newlib \
        --with-system-zlib \
        --with-gmp=/opt/homebrew \
        --with-mpfr=/opt/homebrew \
        --with-mpc=/opt/homebrew \
        MAKEINFO=true
    make -j2 all-gcc all-target-libgcc MAKEINFO=true
    make install-gcc install-target-libgcc MAKEINFO=true
    cd ..
fi

echo "Toolchain build completed successfully!"
echo "Cross compiler is available at: $PREFIX/bin/${TARGET}-gcc"
