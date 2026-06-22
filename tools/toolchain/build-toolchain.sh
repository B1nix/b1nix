#!/usr/bin/env bash
# tools/toolchain/build-toolchain.sh - Builds the cross GCC & Binutils for the b1nix target
# selected by B1NIX_ARCH (x86 -> i686-b1nix, x86_64 -> x86_64-b1nix; default x86_64)
# Works on: Arch Linux / WSL and macOS (with Homebrew)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Per-architecture build identity: resolves B1NIX_ARCH -> B1NIX_TRIPLET,
# shared TOOLCHAIN_SRC_DIR, and per-triplet TOOLCHAIN_BUILD_HOME / B1NIX_ROOTFS
# (see tools/toolchain/env.sh).
. "$PROJECT_DIR/tools/toolchain/env.sh"
TARGET="$B1NIX_TRIPLET"

BINUTILS_VER="2.41"
GCC_VER="13.2.0"

BINUTILS_URL="https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.xz"
GCC_URL="https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.xz"

# ── Platform detection ────────────────────────────────────────────────────────
OS="$(uname -s)"
if [ "$OS" = "Darwin" ]; then
    NPROC=$(sysctl -n hw.ncpu)
    BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"
    GMP_FLAG="--with-gmp=$BREW_PREFIX"
    MPFR_FLAG="--with-mpfr=$BREW_PREFIX"
    MPC_FLAG="--with-mpc=$BREW_PREFIX"
    SED_INPLACE() { sed -i '' "$@"; }
else
    NPROC=$(nproc)
    GMP_FLAG=""
    MPFR_FLAG=""
    MPC_FLAG=""
    SED_INPLACE() { sed -i "$@"; }
fi

# ── Shared source tree + per-triplet build/output directories ────────────────
# TOOLCHAIN_SRC_DIR holds one patched binutils/gcc source tree shared by every
# target. TOOLCHAIN_BUILD_HOME remains per triplet, because configure output,
# object files, sysroot symlink, and installed cross compilers are target-
# specific (GCC is --disable-multilib).
BUILD_HOME="$TOOLCHAIN_BUILD_HOME"
echo "Building $TARGET cross toolchain in $BUILD_HOME"

SRC_DIR="$TOOLCHAIN_SRC_DIR"
WORK_DIR="$BUILD_HOME/build"
PREFIX="$BUILD_HOME/cross"
# Binutils/GCC configure scripts break if sysroot contains spaces; the
# per-triplet sysroot is a symlink to the matching build/<arch>/rootfs.
SYSROOT="$BUILD_HOME/sysroot"

mkdir -p "$SRC_DIR" "$WORK_DIR" "$PREFIX"

# Sysroot symlink → the per-arch userspace rootfs (build/<arch>/rootfs).
mkdir -p "$B1NIX_ROOTFS"
ln -sfn "$B1NIX_ROOTFS" "$SYSROOT" 2>/dev/null || true

cd "$SRC_DIR"

# ── 1. Download sources (shared cache — fetched once, reused by every triplet) ─
mkdir -p "$TOOLCHAIN_DIST_DIR"
if [ ! -f "$TOOLCHAIN_DIST_DIR/binutils-${BINUTILS_VER}.tar.xz" ]; then
    echo "Downloading Binutils ${BINUTILS_VER}..."
    curl -L -o "$TOOLCHAIN_DIST_DIR/binutils-${BINUTILS_VER}.tar.xz" "$BINUTILS_URL"
fi

if [ ! -f "$TOOLCHAIN_DIST_DIR/gcc-${GCC_VER}.tar.xz" ]; then
    echo "Downloading GCC ${GCC_VER}..."
    curl -L -o "$TOOLCHAIN_DIST_DIR/gcc-${GCC_VER}.tar.xz" "$GCC_URL"
fi

# ── 2. Extract & Patch Binutils (shared source tree) ─────────────────────────
if [ ! -d "binutils-${BINUTILS_VER}" ]; then
    echo "Extracting Binutils..."
    tar -xf "$TOOLCHAIN_DIST_DIR/binutils-${BINUTILS_VER}.tar.xz"
    echo "Patching Binutils..."
    cd "binutils-${BINUTILS_VER}"
    patch -p1 < "$PROJECT_DIR/tools/patches/binutils.patch"
    SED_INPLACE 's/| fiwix\*/| fiwix* | b1nix*/g' config.sub
    cd ..
fi

# ── 3. Extract & Patch GCC (shared source tree) ──────────────────────────────
if [ ! -d "gcc-${GCC_VER}" ]; then
    echo "Extracting GCC..."
    tar -xf "$TOOLCHAIN_DIST_DIR/gcc-${GCC_VER}.tar.xz"
    echo "Patching GCC (b1nix target)..."
    python3 "$PROJECT_DIR/tools/toolchain/patch-gcc.py" "$SRC_DIR/gcc-${GCC_VER}"
    cd "gcc-${GCC_VER}"
    SED_INPLACE 's/| fiwix\*/| fiwix* | b1nix*/g' config.sub
    if [ -f "libgcc/config.sub" ]; then
        SED_INPLACE 's/| fiwix\*/| fiwix* | b1nix*/g' libgcc/config.sub
    fi
    # Fix libcody char8_t issue: GCC 13 uses u8"" literals (become char8_t in
    # C++20) but libcody's S2C() expects const char[]. Strip the u8 prefix.
    echo "Patching libcody (char8_t fix for modern host compilers)..."
    for f in libcody/*.cc libcody/*.hh; do
        [ -f "$f" ] && perl -i -pe 's/\bu8"/"/g' "$f"
    done
    cd ..
fi

# ── 4. Build cross-binutils ───────────────────────────────────────────────────
cd "$WORK_DIR"
if [ ! -f "$PREFIX/bin/${TARGET}-ld" ]; then
    echo "Building Cross-Binutils..."
    rm -rf build-binutils
    mkdir -p build-binutils
    cd build-binutils
    "$SRC_DIR/binutils-${BINUTILS_VER}/configure" \
        --target="$TARGET" \
        --prefix="$PREFIX" \
        --with-sysroot="$SYSROOT" \
        --disable-nls \
        --disable-werror \
        --with-system-zlib \
        MAKEINFO=true
    make -j"$NPROC" MAKEINFO=true
    make install MAKEINFO=true
    cd ..
fi

# ── 4b. Stage the b1nix libc headers into the sysroot ───────────────────────
# cross-GCC is configured with --enable-threads=posix, so libgcc's gthr-posix
# (#include <pthread.h>) needs the b1nix headers visible in the sysroot. They
# are produced by the clang-built userspace libc (no cross-GCC dependency), so
# stage them now — without this, all-target-libgcc dies with
# "pthread.h: No such file or directory".
if [ ! -f "$B1NIX_ROOTFS/include/pthread.h" ]; then
    echo "Staging b1nix libc headers into sysroot ($B1NIX_ROOTFS)..."
    make -C "$PROJECT_DIR/userspace" B1NIX_ARCH="$B1NIX_ARCH" install-headers-libs
fi

# ── 5. Build cross-GCC ───────────────────────────────────────────────────────
if [ ! -f "$PREFIX/bin/${TARGET}-gcc" ]; then
    echo "Building Cross-GCC (bootstrap)..."
    rm -rf build-gcc
    mkdir -p build-gcc
    cd build-gcc
    "$SRC_DIR/gcc-${GCC_VER}/configure" \
        --target="$TARGET" \
        --prefix="$PREFIX" \
        --with-sysroot="$SYSROOT" \
        --disable-nls \
        --enable-languages=c,c++ \
        --without-headers \
        --disable-shared \
        --disable-multilib \
        --enable-threads=posix \
        --disable-libgomp \
        --disable-libmudflap \
        --disable-libssp \
        --disable-libquadmath \
        --with-newlib \
        --with-system-zlib \
        --without-isl \
        ${GMP_FLAG:+"$GMP_FLAG"} \
        ${MPFR_FLAG:+"$MPFR_FLAG"} \
        ${MPC_FLAG:+"$MPC_FLAG"} \
        MAKEINFO=true
    make -j"$NPROC" all-gcc all-target-libgcc MAKEINFO=true
    make install-gcc install-target-libgcc MAKEINFO=true
    cd ..
fi

# ── 6. Build target C++ standard library ──────────────────────────────────────
# The native GCC port (tools/toolchain/build-native-toolchain.sh) cross-compiles GCC's own
# C++ sources (libcpp, gcc/*.cc) for x86_64-b1nix. Those include <new>, <vector>,
# etc. and link against libstdc++. Without a target libstdc++ the native build
# fails at libcpp with: "fatal error: new: No such file or directory".
if [ ! -f "$PREFIX/$TARGET/lib/libstdc++.a" ]; then
    echo "Building target libstdc++-v3 for ${TARGET}..."
    if [ ! -f build-gcc/Makefile ]; then
        rm -rf build-gcc
        mkdir -p build-gcc
        cd build-gcc
        "$SRC_DIR/gcc-${GCC_VER}/configure" \
            --target="$TARGET" \
            --prefix="$PREFIX" \
            --with-sysroot="$SYSROOT" \
            --disable-nls \
            --enable-languages=c,c++ \
            --without-headers \
            --disable-shared \
            --disable-multilib \
            --enable-threads=posix \
            --disable-libgomp \
            --disable-libmudflap \
            --disable-libssp \
            --disable-libquadmath \
            --with-newlib \
            --with-system-zlib \
            --without-isl \
            ${GMP_FLAG:+"$GMP_FLAG"} \
            ${MPFR_FLAG:+"$MPFR_FLAG"} \
            ${MPC_FLAG:+"$MPC_FLAG"} \
            MAKEINFO=true
        cd ..
    fi
    cd build-gcc
    make -j"$NPROC" all-target-libstdc++-v3 MAKEINFO=true
    make install-target-libstdc++-v3 MAKEINFO=true
    cd ..
fi

echo ""
echo "Toolchain build completed successfully!"
echo "Cross compiler: $PREFIX/bin/${TARGET}-gcc"
