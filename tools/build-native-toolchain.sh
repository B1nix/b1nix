#!/usr/bin/env bash
# tools/build-native-toolchain.sh - Builds native x86_64-b1nix GCC & Binutils
set -euo pipefail

TARGET="x86_64-b1nix"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORK_DIR="$PROJECT_DIR/build/toolchain_build"
CROSS_PREFIX="$PROJECT_DIR/build/cross"
NATIVE_DEST="$PROJECT_DIR/build/native_root"
SYSROOT="$PROJECT_DIR/build/x86/rootfs"

export PATH="$CROSS_PREFIX/bin:$PATH"

# Define host build variables
CC_VAL="x86_64-b1nix-gcc"
CXX_VAL="x86_64-b1nix-gcc"
AR_VAL="x86_64-b1nix-ar"
RANLIB_VAL="x86_64-b1nix-ranlib"
CFLAGS_VAL="-isystem $SYSROOT/include -Wl,-Ttext-segment=0x2000000"
CPPFLAGS_VAL="-isystem $SYSROOT/include"
LDFLAGS_VAL="-Wl,-Ttext-segment=0x2000000"
LIBS_VAL="-lb1nix -lgcc"

BINUTILS_VER="2.41"
GCC_VER="13.2.0"

mkdir -p "$WORK_DIR" "$NATIVE_DEST"

cd "$WORK_DIR"

# 1. Download GCC prerequisites if not present
if [ -d "gcc-${GCC_VER}" ] && [ ! -d "gcc-${GCC_VER}/gmp" ]; then
    echo "Downloading GCC prerequisites (GMP, MPFR, MPC)..."
    cd "gcc-${GCC_VER}"
    ./contrib/download_prerequisites
    cd ..
fi

# 2. Build native binutils
if [ ! -f "$NATIVE_DEST/bin/ld" ]; then
    echo "Building Native Binutils..."
    rm -rf build-native-binutils
    mkdir -p build-native-binutils
    cd build-native-binutils
    ../binutils-${BINUTILS_VER}/configure \
        CC="$CC_VAL" \
        CXX="$CXX_VAL" \
        AR="$AR_VAL" \
        RANLIB="$RANLIB_VAL" \
        CFLAGS="$CFLAGS_VAL" \
        CPPFLAGS="$CPPFLAGS_VAL" \
        LDFLAGS="$LDFLAGS_VAL" \
        LIBS="$LIBS_VAL" \
        --host="$TARGET" \
        --target="$TARGET" \
        --prefix="" \
        --with-sysroot="/" \
        --disable-nls \
        --disable-werror \
        MAKEINFO=true
    
    make -j2 MAKEINFO=true
    make install DESTDIR="$NATIVE_DEST" MAKEINFO=true
    cd ..
fi

# 3. Build native gcc
if [ ! -f "$NATIVE_DEST/bin/gcc" ]; then
    echo "Building Native GCC..."
    rm -rf build-native-gcc
    mkdir -p build-native-gcc
    cd build-native-gcc
    ../gcc-${GCC_VER}/configure \
        CC="$CC_VAL" \
        CXX="$CXX_VAL" \
        AR="$AR_VAL" \
        RANLIB="$RANLIB_VAL" \
        CFLAGS="$CFLAGS_VAL" \
        CPPFLAGS="$CPPFLAGS_VAL" \
        LDFLAGS="$LDFLAGS_VAL" \
        LIBS="$LIBS_VAL" \
        --host="$TARGET" \
        --target="$TARGET" \
        --prefix="" \
        --with-sysroot="/" \
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
        MAKEINFO=true
    
    make -j2 all-gcc MAKEINFO=true
    make install-gcc DESTDIR="$NATIVE_DEST" MAKEINFO=true
    cd ..
fi

echo "Native toolchain build completed successfully!"
