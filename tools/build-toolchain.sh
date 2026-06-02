#!/usr/bin/env bash
# tools/build-toolchain.sh - Builds x86_64-b1nix cross GCC & Binutils
# Works on: Arch Linux / WSL and macOS (with Homebrew)
set -euo pipefail

TARGET="x86_64-b1nix"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

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

# ── Space-free build directory ────────────────────────────────────────────────
# GNU make's $(CURDIR) and libtool resolve ALL symlinks/junctions to the real
# path. On WSL, the Windows-side filesystem (/mnt/c/...) can have spaces in
# usernames (e.g. "Dmytro Manko"), which breaks libtool's unquoted paths.
#
# Solution: build entirely in the Linux-side filesystem ($HOME/b1nix-toolchain).
# Linux paths never have spaces here. After building, symlink build/cross →
# $HOME/b1nix-toolchain/cross so the rest of the build system finds the tools.
#
if echo "$PROJECT_DIR" | grep -q ' '; then
    # Path has spaces → build on the Linux-side filesystem
    BUILD_HOME="$HOME/b1nix-toolchain"
    echo "Note: project path has spaces; building toolchain in $BUILD_HOME"
else
    BUILD_HOME="$PROJECT_DIR/build/toolchain_build"
fi

WORK_DIR="$BUILD_HOME/src"
PREFIX="$BUILD_HOME/cross"
# Binutils/GCC configure scripts break if sysroot contains spaces.
# Use a space-free symlink in the BUILD_HOME directory.
SYSROOT="$BUILD_HOME/sysroot"

mkdir -p "$WORK_DIR" "$PREFIX"

# Keep build/cross pointing at the real prefix so Makefiles find the tools
CROSS_LINK="$PROJECT_DIR/build/cross"
if [ "$PREFIX" != "$CROSS_LINK" ]; then
    mkdir -p "$(dirname "$CROSS_LINK")"
    ln -sfn "$PREFIX" "$CROSS_LINK" 2>/dev/null || true
fi

# Sysroot symlink
mkdir -p "$PROJECT_DIR/build/x86_64/rootfs"
ln -sfn "$PROJECT_DIR/build/x86_64/rootfs" "$SYSROOT" 2>/dev/null || true

cd "$WORK_DIR"

# ── 1. Download sources ───────────────────────────────────────────────────────
if [ ! -f "binutils-${BINUTILS_VER}.tar.xz" ]; then
    echo "Downloading Binutils ${BINUTILS_VER}..."
    curl -LO "$BINUTILS_URL"
fi

if [ ! -f "gcc-${GCC_VER}.tar.xz" ]; then
    echo "Downloading GCC ${GCC_VER}..."
    curl -LO "$GCC_URL"
fi

# ── 2. Extract & Patch Binutils ───────────────────────────────────────────────
if [ ! -d "binutils-${BINUTILS_VER}" ]; then
    echo "Extracting Binutils..."
    tar -xf "binutils-${BINUTILS_VER}.tar.xz"
    echo "Patching Binutils..."
    cd "binutils-${BINUTILS_VER}"
    patch -p1 < "$PROJECT_DIR/tools/patches/binutils.patch"
    SED_INPLACE 's/| fiwix\*/| fiwix* | b1nix*/g' config.sub
    cd ..
fi

# ── 3. Extract & Patch GCC ───────────────────────────────────────────────────
if [ ! -d "gcc-${GCC_VER}" ]; then
    echo "Extracting GCC..."
    tar -xf "gcc-${GCC_VER}.tar.xz"
    echo "Patching GCC (b1nix target)..."
    python3 "$PROJECT_DIR/tools/patch-gcc.py" "$WORK_DIR/gcc-${GCC_VER}"
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
if [ ! -f "$PREFIX/bin/${TARGET}-ld" ]; then
    echo "Building Cross-Binutils..."
    rm -rf build-binutils
    mkdir -p build-binutils
    cd build-binutils
    ../binutils-${BINUTILS_VER}/configure \
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

# ── 5. Build cross-GCC ───────────────────────────────────────────────────────
if [ ! -f "$PREFIX/bin/${TARGET}-gcc" ]; then
    echo "Building Cross-GCC (bootstrap)..."
    rm -rf build-gcc
    mkdir -p build-gcc
    cd build-gcc
    ../gcc-${GCC_VER}/configure \
        --target="$TARGET" \
        --prefix="$PREFIX" \
        --with-sysroot="$SYSROOT" \
        --disable-nls \
        --enable-languages=c,c++ \
        --without-headers \
        --disable-shared \
        --disable-multilib \
        --disable-threads \
        --disable-libgomp \
        --disable-libmudflap \
        --disable-libssp \
        --disable-libquadmath \
        --with-newlib \
        --with-system-zlib \
        ${GMP_FLAG:+"$GMP_FLAG"} \
        ${MPFR_FLAG:+"$MPFR_FLAG"} \
        ${MPC_FLAG:+"$MPC_FLAG"} \
        MAKEINFO=true
    make -j"$NPROC" all-gcc all-target-libgcc MAKEINFO=true
    make install-gcc install-target-libgcc MAKEINFO=true
    cd ..
fi

# ── 6. Build target C++ standard library ──────────────────────────────────────
# The native GCC port (tools/build-native-toolchain.sh) cross-compiles GCC's own
# C++ sources (libcpp, gcc/*.cc) for x86_64-b1nix. Those include <new>, <vector>,
# etc. and link against libstdc++. Without a target libstdc++ the native build
# fails at libcpp with: "fatal error: new: No such file or directory".
if [ ! -f "$PREFIX/x86_64-b1nix/lib/libstdc++.a" ]; then
    echo "Building target libstdc++-v3 for ${TARGET}..."
    cd build-gcc
    make -j"$NPROC" all-target-libstdc++-v3 MAKEINFO=true
    make install-target-libstdc++-v3 MAKEINFO=true
    cd ..
fi

echo ""
echo "Toolchain build completed successfully!"
echo "Cross compiler: $PREFIX/bin/${TARGET}-gcc"
