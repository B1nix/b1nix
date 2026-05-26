#!/usr/bin/env bash
# tools/build-native-toolchain.sh - Builds native GCC & Binutils for b1nix
# Works on: Arch Linux / WSL and macOS (with Homebrew)
# Requires: cross-toolchain already built (run build-toolchain.sh first)
set -euo pipefail

TARGET="x86_64-b1nix"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

BINUTILS_VER="2.41"
GCC_VER="13.2.0"

# ── Platform detection ────────────────────────────────────────────────────────
OS="$(uname -s)"
if [ "$OS" = "Darwin" ]; then
    NPROC=$(sysctl -n hw.ncpu)
    SED_INPLACE() { sed -i '' "$@"; }
else
    NPROC=$(nproc)
    SED_INPLACE() { sed -i "$@"; }
fi

# ── Space-free build directory ────────────────────────────────────────────────
if echo "$PROJECT_DIR" | grep -q ' '; then
    # Path has spaces → build on the Linux-side filesystem
    BUILD_HOME="$HOME/b1nix-toolchain"
    echo "Note: project path has spaces; building native toolchain in $BUILD_HOME"
else
    BUILD_HOME="$PROJECT_DIR/build/toolchain_build"
fi

SRC_DIR="$BUILD_HOME/src"
WORK_DIR="$BUILD_HOME/native_build"
CROSS_PREFIX="$BUILD_HOME/cross"
NATIVE_DEST="$BUILD_HOME/native_root"
SYSROOT="$BUILD_HOME/sysroot"

# Sanity check: cross-compiler must exist first
if [ ! -f "$CROSS_PREFIX/bin/${TARGET}-gcc" ]; then
    echo "Error: cross-compiler not found at $CROSS_PREFIX/bin/${TARGET}-gcc"
    echo "Run tools/build-toolchain.sh first."
    exit 1
fi

export PATH="$CROSS_PREFIX/bin:$PATH"
export ac_cv_c_bigendian=no

CC_VAL="${TARGET}-gcc"
CXX_VAL="${TARGET}-g++"
AR_VAL="${TARGET}-ar"
RANLIB_VAL="${TARGET}-ranlib"
CFLAGS_VAL="-isystem $SYSROOT/include -Wl,-Ttext-segment=0x2000000"
CPPFLAGS_VAL="-isystem $SYSROOT/include"
LDFLAGS_VAL="-Wl,-Ttext-segment=0x2000000"
LIBS_VAL="-lb1nix -lgcc"

mkdir -p "$WORK_DIR" "$NATIVE_DEST"

# ── 1. Download GCC prerequisites if needed ───────────────────────────────────
if [ -d "$SRC_DIR/gcc-${GCC_VER}" ] && [ ! -d "$SRC_DIR/gcc-${GCC_VER}/gmp" ]; then
    echo "Downloading GCC prerequisites (GMP, MPFR, MPC)..."
    (cd "$SRC_DIR/gcc-${GCC_VER}" && ./contrib/download_prerequisites)
fi

# Always ensure dynamically downloaded prerequisites accept b1nix
if [ -d "$SRC_DIR/gcc-${GCC_VER}" ]; then
    echo "Ensuring GCC prerequisites are patched for b1nix..."
    # GMP
    if [ -f "$SRC_DIR/gcc-${GCC_VER}/gmp-6.2.1/configfsf.sub" ]; then
        SED_INPLACE 's/| redox\* | bme\*/| redox* | b1nix* | bme*/g' "$SRC_DIR/gcc-${GCC_VER}/gmp-6.2.1/configfsf.sub"
    fi
    # MPFR
    if [ -f "$SRC_DIR/gcc-${GCC_VER}/mpfr-4.1.0/config.sub" ]; then
        SED_INPLACE 's/| -redox\* | -bme\*/| -redox* | -b1nix* | -bme*/g' "$SRC_DIR/gcc-${GCC_VER}/mpfr-4.1.0/config.sub"
    fi
    # MPC
    if [ -f "$SRC_DIR/gcc-${GCC_VER}/mpc-1.2.1/build-aux/config.sub" ]; then
        SED_INPLACE 's/| redox\* | bme\*/| redox* | b1nix* | bme*/g' "$SRC_DIR/gcc-${GCC_VER}/mpc-1.2.1/build-aux/config.sub"
    fi
    # ISL
    if [ -f "$SRC_DIR/gcc-${GCC_VER}/isl-0.24/config.sub" ]; then
        SED_INPLACE 's/| -redox\* | -bme\*/| -redox* | -b1nix* | -bme*/g' "$SRC_DIR/gcc-${GCC_VER}/isl-0.24/config.sub"
    fi
fi

# ── 2. Build native binutils ──────────────────────────────────────────────────
if [ ! -f "$NATIVE_DEST/bin/ld" ]; then
    echo "Building Native Binutils (host=$TARGET)..."
    rm -rf "$WORK_DIR/build-native-binutils"
    mkdir -p "$WORK_DIR/build-native-binutils"
    cd "$WORK_DIR/build-native-binutils"
    "$SRC_DIR/binutils-${BINUTILS_VER}/configure" \
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
    make -j"$NPROC" MAKEINFO=true
    make install DESTDIR="$NATIVE_DEST" MAKEINFO=true
fi

# ── 3. Build native GCC ───────────────────────────────────────────────────────
if [ ! -f "$NATIVE_DEST/bin/gcc" ]; then
    echo "Building Native GCC (host=$TARGET)..."
    rm -rf "$WORK_DIR/build-native-gcc"
    mkdir -p "$WORK_DIR/build-native-gcc"
    cd "$WORK_DIR/build-native-gcc"
    export CXXFLAGS="-O2 -DCODY_NETWORKING=0"
    export CXXFLAGS_FOR_BUILD="-O2"
    export CFLAGS_FOR_BUILD="-O2"
    "$SRC_DIR/gcc-${GCC_VER}/configure" \
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
    make -j"$NPROC" all-gcc MAKEINFO=true
    make install-gcc DESTDIR="$NATIVE_DEST" MAKEINFO=true
    unset CXXFLAGS CXXFLAGS_FOR_BUILD CFLAGS_FOR_BUILD
fi

echo ""
echo "Native toolchain build completed successfully!"
echo "Output: $NATIVE_DEST"
echo "Copy into rootfs image to use gcc inside b1nix."
