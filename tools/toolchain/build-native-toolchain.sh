#!/usr/bin/env bash
# tools/toolchain/build-native-toolchain.sh - Builds native GCC & Binutils for b1nix
# Works on: Arch Linux / WSL and macOS (with Homebrew)
# Requires: cross-toolchain already built (run build-toolchain.sh first)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Per-architecture build identity (B1NIX_ARCH -> triplet, shared sources,
# per-triplet build/output paths).
. "$PROJECT_DIR/tools/toolchain/env.sh"
TARGET="$B1NIX_TRIPLET"

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

ccache_prefix() {
    if [ "${B1NIX_NO_CCACHE:-0}" != "1" ] && command -v ccache >/dev/null 2>&1; then
        printf 'ccache '
    fi
}

# ── Shared source tree + per-triplet build/output directories ────────────────
BUILD_HOME="$TOOLCHAIN_BUILD_HOME"

SRC_DIR="$TOOLCHAIN_SRC_DIR"
WORK_DIR="$BUILD_HOME/native_build"
CROSS_PREFIX="$BUILD_HOME/cross"
NATIVE_DEST="$BUILD_HOME/native_root"
SYSROOT="$BUILD_HOME/sysroot"

# Sanity check: cross-compiler must exist first
if [ ! -f "$CROSS_PREFIX/bin/${TARGET}-gcc" ]; then
    echo "Error: cross-compiler not found at $CROSS_PREFIX/bin/${TARGET}-gcc"
    echo "Run tools/toolchain/build-toolchain.sh first."
    exit 1
fi

export PATH="$CROSS_PREFIX/bin:$PATH"
export ac_cv_c_bigendian=no
# b1nix exposes some libc helpers as static inline functions in public headers.
# Autoconf's cross AC_CHECK_FUNC probes do not include those headers, so they
# can misdetect the helpers as missing and libiberty then builds replacement
# objects that collide with the inline definitions.
export ac_cv_func_clock=yes
export ac_cv_func_tmpnam=yes
export ac_cv_header_fcntl_h=yes
export ac_cv_header_sys_resource_h=yes

CC_VAL="$(ccache_prefix)${TARGET}-gcc"
CXX_VAL="$(ccache_prefix)${TARGET}-g++"
AR_VAL="${TARGET}-ar"
RANLIB_VAL="${TARGET}-ranlib"
CFLAGS_VAL="--sysroot=$SYSROOT -isystem $SYSROOT/include -Wl,-Ttext-segment=0x2000000 -Wl,--allow-multiple-definition"
CXXFLAGS_VAL="--sysroot=$SYSROOT -idirafter $SYSROOT/include -Wl,-Ttext-segment=0x2000000 -Wl,--allow-multiple-definition -O2 -DCODY_NETWORKING=0"
CPPFLAGS_VAL="--sysroot=$SYSROOT -isystem $SYSROOT/include"
LDFLAGS_VAL="--sysroot=$SYSROOT -Wl,-Ttext-segment=0x2000000 -Wl,--allow-multiple-definition"
LIBS_VAL="-lgcc -lb1nix"
HOST_LIBS_VAL="$LIBS_VAL"

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
        --with-zstd=no \
        MAKEINFO=true
    make -j"$NPROC" MAKEINFO=true
    make install DESTDIR="$NATIVE_DEST" MAKEINFO=true
fi

# ── 3. Build native GCC ───────────────────────────────────────────────────────
if [ ! -f "$NATIVE_DEST/bin/gcc" ]; then
    echo "Building Native GCC (host=$TARGET)..."
    # Do NOT export CXXFLAGS/CFLAGS (the target --sysroot flags): GCC's build-side
    # modules (build-libcpp etc., compiled by the HOST g++) would inherit them
    # from the environment and redirect their include search into the b1nix
    # sysroot, where host headers like bits/wordsize.h don't exist. The host-side
    # (the gcc binary built FOR b1nix) gets these flags via the explicit configure
    # CFLAGS=/CXXFLAGS= args below; the build side uses *_FOR_BUILD.
    export CXXFLAGS_FOR_BUILD="-O2"
    export CFLAGS_FOR_BUILD="-O2"
    if [ ! -f "$WORK_DIR/build-native-gcc/Makefile" ]; then
        rm -rf "$WORK_DIR/build-native-gcc"
        mkdir -p "$WORK_DIR/build-native-gcc"
        cd "$WORK_DIR/build-native-gcc"
        "$SRC_DIR/gcc-${GCC_VER}/configure" \
            CC="$CC_VAL" \
            CXX="$CXX_VAL" \
            AR="$AR_VAL" \
            RANLIB="$RANLIB_VAL" \
            CFLAGS="$CFLAGS_VAL" \
            CXXFLAGS="$CXXFLAGS_VAL" \
            CPPFLAGS="$CPPFLAGS_VAL" \
            LDFLAGS="$LDFLAGS_VAL" \
            LIBS="$LIBS_VAL" \
            HOST_LIBS="$HOST_LIBS_VAL" \
            CC_FOR_BUILD="$PROJECT_DIR/tools/toolchain/bin/b1nix-buildcc" \
            CXX_FOR_BUILD="$PROJECT_DIR/tools/toolchain/bin/b1nix-buildcxx" \
            CFLAGS_FOR_BUILD="-O2" \
            CXXFLAGS_FOR_BUILD="-O2" \
            CPPFLAGS_FOR_BUILD="" \
            BUILD_CPPFLAGS="" \
            GCC_FOR_TARGET="$CC_VAL" \
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
    else
        cd "$WORK_DIR/build-native-gcc"
    fi
    if [ -f "$WORK_DIR/build-native-gcc/gcc/Makefile" ]; then
        SED_INPLACE "s|^HOST_LIBS =.*|HOST_LIBS = $HOST_LIBS_VAL|" "$WORK_DIR/build-native-gcc/gcc/Makefile"
        if ! grep -q '^override HOST_LIBS' "$WORK_DIR/build-native-gcc/gcc/Makefile"; then
            printf '\noverride HOST_LIBS += %s\n' "$HOST_LIBS_VAL" >> "$WORK_DIR/build-native-gcc/gcc/Makefile"
        fi
    fi
    make -j"$NPROC" all-gcc MAKEINFO=true HOST_LIBS="$HOST_LIBS_VAL"
    make install-gcc DESTDIR="$NATIVE_DEST" MAKEINFO=true
    unset CXXFLAGS CXXFLAGS_FOR_BUILD CFLAGS_FOR_BUILD
fi

echo ""
echo "Native toolchain build completed successfully!"
echo "Output: $NATIVE_DEST"
echo "Copy into rootfs image to use gcc inside b1nix."
