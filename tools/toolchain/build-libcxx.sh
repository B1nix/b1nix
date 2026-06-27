#!/usr/bin/env bash
# tools/toolchain/build-libcxx.sh
#
# Cross-compile LLVM libc++ + libc++abi for the b1nix target.
# These replace GCC's libstdc++ / libsupc++ as the C++ standard library.
#
# Prerequisites:
#   - Cross GCC toolchain (tools/toolchain/build-toolchain.sh)
#   - b1nix userspace libc headers (userspace/Makefile)
#   - LLVM runtimes: compiler-rt + libunwind (tools/toolchain/build-llvm-runtimes.sh)
#
# Output (installed into the cross sysroot):
#   lib/libc++.a           — C++ standard library (static)
#   lib/libc++abi.a        — C++ ABI support (exceptions, RTTI, atexit)
#   include/c++/v1/        — libc++ headers
#
# Usage:
#   tools/toolchain/build-libcxx.sh            # build for default arch
#   B1NIX_ARCH=x86 tools/toolchain/build-libcxx.sh  # build for i686
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$PROJECT_DIR/tools/toolchain/env.sh"

LLVM_VER="${LLVM_VER:-18.1.8}"
BUILD_HOME="$TOOLCHAIN_BUILD_HOME/llvm-runtimes-build"
SRC_DIR="$BUILD_HOME/llvm-project-${LLVM_VER}.src"
INSTALL_DIR="$BUILD_HOME/libcxx-install"
CROSS="$TOOLCHAIN_BUILD_HOME/cross"
SYSROOT="$TOOLCHAIN_BUILD_HOME/sysroot"
GCC_INC="$CROSS/lib/gcc/$B1NIX_TRIPLET"
GCC_VER_DIR=$(ls "$GCC_INC" 2>/dev/null | head -1 || true)

OS="$(uname -s)"
if [ "$OS" = "Darwin" ]; then NPROC=$(sysctl -n hw.ncpu); else NPROC=$(nproc); fi

# Verify prerequisites
[ -d "$SRC_DIR" ] || { echo "LLVM sources not found at $SRC_DIR — run build-llvm-runtimes.sh first" >&2; exit 1; }
[ -f "$BUILD_HOME/install/lib/libcompiler_rt.a" ] || { echo "compiler-rt not found — run build-llvm-runtimes.sh first" >&2; exit 1; }
[ -f "$BUILD_HOME/install/lib/libunwind.a" ] || { echo "libunwind not found — run build-llvm-runtimes.sh first" >&2; exit 1; }
[ -f "$SYSROOT/include/pthread.h" ] || { echo "b1nix sysroot headers not staged — run build-toolchain.sh first" >&2; exit 1; }

mkdir -p "$INSTALL_DIR/lib" "$INSTALL_DIR/include"

CLANG_BIN="$(command -v clang 2>/dev/null || echo clang)"
CLANGXX_BIN="$(command -v clang++ 2>/dev/null || echo clang++)"
AR_BIN="$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)"

# b1nix-specific defines needed by libc++/libc++abi:
#   _POSIX_THREADS=1    — we have pthread
#   _POSIX_C_SOURCE=200809L — POSIX.1-2008 APIs
#   __B1NIX__=1         — identify b1nix target
B1NIX_DEFINES="-D__B1NIX__=1 -D_POSIX_THREADS=1 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE=1"

# Common CMake flags for cross-compiling to b1nix
COMMON_CMAKE_ARGS=(
    -G "Unix Makefiles"
    -DCMAKE_SYSTEM_NAME=Linux
    -DCMAKE_SYSTEM_PROCESSOR="$B1NIX_GCC_ARCH"
    -DCMAKE_C_COMPILER="$CLANG_BIN"
    -DCMAKE_CXX_COMPILER="$CLANGXX_BIN"
    -DCMAKE_AR="$AR_BIN"
    -DCMAKE_C_COMPILER_TARGET="$B1NIX_TRIPLET"
    -DCMAKE_CXX_COMPILER_TARGET="$B1NIX_TRIPLET"
    -DCMAKE_SYSROOT="$SYSROOT"
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_C_FLAGS="$B1NIX_DEFINES"
    -DCMAKE_CXX_FLAGS="$B1NIX_DEFINES"
    -DLLVM_ENABLE_ASSERTIONS=OFF
    -DLLVM_ENABLE_THREADS=ON
    -DLLVM_ENABLE_PIC=OFF
    -DCMAKE_POSITION_INDEPENDENT_CODE=OFF
)

# ── 1. Build libc++abi (must be built first — libc++ depends on it) ────────────
CXXABI_BUILD="$BUILD_HOME/build-libcxxabi"
CXXABI_INSTALL="$BUILD_HOME/libcxxabi-install"
if [ ! -f "$CXXABI_INSTALL/lib/libc++abi.a" ]; then
    echo "Building libc++abi..."
    rm -rf "$CXXABI_BUILD"
    mkdir -p "$CXXABI_BUILD"
    cd "$CXXABI_BUILD"
    cmake "${COMMON_CMAKE_ARGS[@]}" \
        "$SRC_DIR/libcxxabi" \
        -DCMAKE_INSTALL_PREFIX="$CXXABI_INSTALL" \
        -DLIBCXXABI_ENABLE_SHARED=OFF \
        -DLIBCXXABI_ENABLE_STATIC=ON \
        -DLIBCXXABI_ENABLE_EXCEPTIONS=ON \
        -DLIBCXXABI_ENABLE_RTTI=ON \
        -DLIBCXXABI_ENABLE_THREADS=ON \
        -DLIBCXXABI_USE_LLVM_UNWINDER=ON \
        -DLIBCXXABI_USE_COMPILER_RT=ON \
        -DLIBCXXABI_INCLUDE_TESTS=OFF \
        -DLIBCXXABI_INCLUDE_DOCS=OFF \
        -DLLVM_PATH="$SRC_DIR/llvm" \
        -DLIBUNWIND_PATH="$SRC_DIR/libunwind" \
        -DCOMPILER_RT_PATH="$SRC_DIR/compiler-rt" \
        -DLIBCXXABI_INSTALL_HEADERS=ON \
        -DLIBCXXABI_INSTALL_HEADER_PREFIX="" \
        2>&1 | tail -10
    make -j"$NPROC" cxxabi_static 2>&1 | tail -10
    mkdir -p "$CXXABI_INSTALL/lib"
    find . -name "libc++abi.a" -exec cp {} "$CXXABI_INSTALL/lib/libc++abi.a" \;
    # Install headers
    if [ -d "$SRC_DIR/libcxxabi/include" ]; then
        mkdir -p "$CXXABI_INSTALL/include"
        cp -R "$SRC_DIR/libcxxabi/include/"* "$CXXABI_INSTALL/include/" 2>/dev/null || true
    fi
    if [ -f "$CXXABI_INSTALL/lib/libc++abi.a" ]; then
        echo "  libc++abi: $(ls -la "$CXXABI_INSTALL/lib/libc++abi.a")"
    else
        echo "  WARNING: libc++abi build may have failed — check $CXXABI_BUILD" >&2
    fi
fi

cd "$PROJECT_DIR"

# ── 2. Build libc++ (the C++ standard library) ────────────────────────────────
LIBCXX_BUILD="$BUILD_HOME/build-libcxx"
if [ ! -f "$INSTALL_DIR/lib/libc++.a" ]; then
    echo "Building libc++..."
    rm -rf "$LIBCXX_BUILD"
    mkdir -p "$LIBCXX_BUILD"
    cd "$LIBCXX_BUILD"
    cmake "${COMMON_CMAKE_ARGS[@]}" \
        "$SRC_DIR/libcxx" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
        -DLIBCXX_ENABLE_SHARED=OFF \
        -DLIBCXX_ENABLE_STATIC=ON \
        -DLIBCXX_ENABLE_EXPERIMENTAL_LIBRARY=OFF \
        -DLIBCXX_ENABLE_FILESYSTEM=ON \
        -DLIBCXX_ENABLE_EXCEPTIONS=ON \
        -DLIBCXX_ENABLE_RTTI=ON \
        -DLIBCXX_ENABLE_THREADS=ON \
        -DLIBCXX_ENABLE_MONOTONIC_CLOCK=ON \
        -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON \
        -DLIBCXX_CXX_ABI_LIBRARY_PATH="$CXXABI_INSTALL/lib" \
        -DLIBCXX_CXX_ABI_INCLUDE_PATHS="$SRC_DIR/libcxxabi/include;$CXXABI_INSTALL/include" \
        -DLIBCXX_CXX_ABI_USE_LLVM_UNWINDER=ON \
        -DLIBCXX_CXX_ABI_USE_COMPILER_RT=ON \
        -DLIBCXX_INCLUDE_TESTS=OFF \
        -DLIBCXX_INCLUDE_DOCS=OFF \
        -DLLVM_PATH="$SRC_DIR/llvm" \
        -DLIBUNWIND_PATH="$SRC_DIR/libunwind" \
        -DCOMPILER_RT_PATH="$SRC_DIR/compiler-rt" \
        2>&1 | tail -10
    make -j"$NPROC" cxx_static 2>&1 | tail -10
    mkdir -p "$INSTALL_DIR/lib"
    find . -name "libc++.a" -exec cp {} "$INSTALL_DIR/lib/libc++.a" \;
    if [ -f "$INSTALL_DIR/lib/libc++.a" ]; then
        echo "  libc++: $(ls -la "$INSTALL_DIR/lib/libc++.a")"
    else
        echo "  WARNING: libc++ build may have failed — check $LIBCXX_BUILD" >&2
    fi
fi

cd "$PROJECT_DIR"

# ── 3. Install libc++ headers ─────────────────────────────────────────────────
echo "Installing libc++ headers..."
LIBCXX_HDR_DIR="$INSTALL_DIR/include/c++/v1"
mkdir -p "$LIBCXX_HDR_DIR"
# Copy libc++ headers
if [ -d "$SRC_DIR/libcxx/include" ]; then
    cp -R "$SRC_DIR/libcxx/include/"* "$LIBCXX_HDR_DIR/" 2>/dev/null || true
fi
# Copy libc++abi headers (needed for exception support)
if [ -d "$CXXABI_INSTALL/include" ]; then
    cp -R "$CXXABI_INSTALL/include/"* "$LIBCXX_HDR_DIR/" 2>/dev/null || true
fi
echo "  headers: $(find "$LIBCXX_HDR_DIR" -name "*.h" | wc -l) files"

# ── 4. Install into cross sysroot ─────────────────────────────────────────────
echo ""
echo "Installing libc++ into cross sysroot..."

# Libraries → sysroot
for f in "$INSTALL_DIR/lib/libc++.a" "$CXXABI_INSTALL/lib/libc++abi.a"; do
    [ -f "$f" ] || continue
    name="$(basename "$f")"
    cp -f "$f" "$SYSROOT/usr/lib/$name" 2>/dev/null || true
    cp -f "$f" "$CROSS/$B1NIX_TRIPLET/lib/$name" 2>/dev/null || true
    [ -n "$GCC_VER_DIR" ] && cp -f "$f" "$CROSS/lib/gcc/$B1NIX_TRIPLET/$GCC_VER_DIR/$name" 2>/dev/null || true
    echo "  lib: $name"
done

# Headers → sysroot
LIBCXX_SYSROOT_HDR="$CROSS/$B1NIX_TRIPLET/include/c++/v1"
mkdir -p "$LIBCXX_SYSROOT_HDR"
cp -R "$LIBCXX_HDR_DIR/"* "$LIBCXX_SYSROOT_HDR/" 2>/dev/null || true
echo "  headers → $LIBCXX_SYSROOT_HDR"

echo ""
echo "libc++ build complete!"
echo "  libc++:    $INSTALL_DIR/lib/libc++.a"
echo "  libc++abi: $CXXABI_INSTALL/lib/libc++abi.a"
echo "  headers:   $LIBCXX_HDR_DIR"
echo ""
echo "To use instead of libstdc++:"
echo "  Pass B1NIX_CXX_STDLIB=libc++ to b1nix-c++ or set it in the build"
echo "  Example: B1NIX_CXX_STDLIB=libc++ b1nix-c++ myapp.cpp -o myapp"
