#!/usr/bin/env bash
# tools/build-native-clang.sh
#
# Build a Clang/LLVM toolchain for b1nix with default target x86_64-b1nix.
#
# Strategy:
#   - default: build a Linux-hosted cross Clang that defaults to b1nix.
#   - --b1nix-elf: cross-build LLVM/Clang itself with host=x86_64-b1nix, using
#     host TableGen tools only for .td generation. This is the self-host binary
#     that can be installed into the b1nix rootfs.
#
# Output:
#   build/native-clang/installed/        Linux-hosted cross compiler
#   build/native-clang/b1nix/usr/        b1nix-native compiler (--b1nix-elf)
#
# Usage:
#   tools/build-native-clang.sh
#   tools/build-native-clang.sh --b1nix-elf
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export PROJECT_DIR="$ROOT"
. "$ROOT/tools/toolchain/env.sh" 2>/dev/null || true
B1NIX_TRIPLET="${B1NIX_TRIPLET:-x86_64-b1nix}"
CROSS="${TOOLCHAIN_BUILD_HOME:-$ROOT/build/toolchain_build/$B1NIX_TRIPLET}/cross"
SYSROOT="${B1NIX_ROOTFS:-$ROOT/build/x86_64/rootfs}"
HOST_TBLGEN="$ROOT/build/native-clang/host-tblgen"
CLANG_BUILD="$ROOT/build/native-clang/full-build"
NATIVE_DEST="$ROOT/build/native-clang/installed"
B1NIX_BUILD="$ROOT/build/native-clang/b1nix-host-build"
B1NIX_DEST="$ROOT/build/native-clang/b1nix"
JOBS="${NATIVE_CLANG_JOBS:-$(nproc 2>/dev/null || echo 4)}"

LLVM_VER="${LLVM_VER:-22.1.8}"
LLVM_URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VER}/llvm-project-${LLVM_VER}.src.tar.xz"

# ── 1. Download LLVM sources ──────────────────────────────────────────────────
LLVM_SRC="$ROOT/build/native-clang/llvm-project-${LLVM_VER}.src"
ensure_sources() {
    if [ ! -d "$LLVM_SRC/clang" ]; then
        echo "[native-clang] Downloading LLVM ${LLVM_VER}..."
        LLVM_DL="$ROOT/build/native-clang/llvm-${LLVM_VER}.src.tar.xz"
        mkdir -p "$(dirname "$LLVM_DL")"
        [ -f "$LLVM_DL" ] || curl -fL --retry 3 "$LLVM_URL" -o "$LLVM_DL"
        echo "[native-clang] Extracting LLVM sources..."
        tar -xJf "$LLVM_DL" -C "$ROOT/build/native-clang/" 2>/dev/null
    fi
    if ! grep -q "B1nix" "$LLVM_SRC/llvm/include/llvm/TargetParser/Triple.h"; then
        echo "[native-clang] Applying b1nix triple patch..."
        (cd "$LLVM_SRC" && patch -p1 < "$ROOT/tools/patches/llvm/b1nix-triple.patch")
    fi
}

# ── 2. Build host TableGen (needed for cross-compiling LLVM .td -> .inc) ──────
ensure_host_tblgen() {
if [ ! -x "$HOST_TBLGEN/bin/clang-tblgen" ] || [ ! -x "$HOST_TBLGEN/bin/llvm-tblgen" ]; then
    echo "[native-clang] Building host TableGen tools..."
    rm -rf "$HOST_TBLGEN"
    cmake -G Ninja -S "$LLVM_SRC/llvm" -B "$HOST_TBLGEN" \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_BUILD_TYPE=Release \
        -DLLVM_ENABLE_PROJECTS="clang" \
        -DLLVM_TARGETS_TO_BUILD="X86" \
        -DLLVM_ENABLE_ASSERTIONS=OFF >/dev/null
    ninja -C "$HOST_TBLGEN" -j"$JOBS" clang-tblgen llvm-tblgen
else
    echo "[native-clang] Host TableGen already built"
fi
}

build_b1nix_native() {
    ensure_sources
    ensure_host_tblgen

    CC_CROSS="$CROSS/bin/$B1NIX_TRIPLET-gcc"
    CXX_CROSS="$CROSS/bin/$B1NIX_TRIPLET-g++"
    if [ ! -x "$CC_CROSS" ] || [ ! -x "$CXX_CROSS" ]; then
        echo "[native-clang] Missing b1nix C/C++ cross compiler:" >&2
        echo "  $CC_CROSS" >&2
        echo "  $CXX_CROSS" >&2
        echo "Build/enable the x86_64-b1nix GCC C++ toolchain first." >&2
        exit 1
    fi
    if [ ! -d "$SYSROOT/include" ] || [ ! -f "$SYSROOT/lib/libb1nix.a" ]; then
        echo "[native-clang] Missing b1nix sysroot at $SYSROOT" >&2
        echo "Run: make ARCH=x86_64 userspace-install" >&2
        exit 1
    fi

    TOOLCHAIN_FILE="$ROOT/build/native-clang/b1nix-toolchain.cmake"
    mkdir -p "$(dirname "$TOOLCHAIN_FILE")" "$B1NIX_BUILD"
    cat > "$TOOLCHAIN_FILE" <<EOF
set(CMAKE_SYSTEM_NAME B1nix)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "$CC_CROSS")
set(CMAKE_CXX_COMPILER "$CXX_CROSS")
set(CMAKE_SYSROOT "$SYSROOT")
set(CMAKE_FIND_ROOT_PATH "$SYSROOT" "$CROSS/$B1NIX_TRIPLET" "$CROSS")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_FLAGS_INIT "-O2 -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_INIT "-O2 -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--gc-sections -Wl,-Ttext-segment=0x2000000")
EOF

    echo "[native-clang] Cross-building b1nix-native LLVM + Clang..."
    cmake -G Ninja -S "$LLVM_SRC/llvm" -B "$B1NIX_BUILD" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
        -DCMAKE_BUILD_TYPE=MinSizeRel \
        -DCMAKE_INSTALL_PREFIX="$B1NIX_DEST/usr" \
        -DLLVM_ENABLE_PROJECTS="clang;lld" \
        -DLLVM_TARGETS_TO_BUILD="X86" \
        -DLLVM_DEFAULT_TARGET_TRIPLE="$B1NIX_TRIPLET" \
        -DLLVM_HOST_TRIPLE="$B1NIX_TRIPLET" \
        -DLLVM_ENABLE_ASSERTIONS=OFF \
        -DLLVM_ENABLE_THREADS=OFF \
        -DLLVM_ENABLE_ZLIB=OFF \
        -DLLVM_ENABLE_ZSTD=OFF \
        -DLLVM_ENABLE_TERMINFO=OFF \
        -DLLVM_ENABLE_LIBXML2=OFF \
        -DLLVM_ENABLE_CURL=OFF \
        -DLLVM_BUILD_LLVM_DYLIB=OFF \
        -DLLVM_LINK_LLVM_DYLIB=OFF \
        -DLLVM_INCLUDE_TESTS=OFF \
        -DCLANG_INCLUDE_TESTS=OFF \
        -DCLANG_DEFAULT_LINKER="ld" \
        -DCLANG_DEFAULT_CXX_STDLIB="libstdc++" \
        -DCLANG_DEFAULT_RTLIB="libgcc" \
        -DLLVM_TABLEGEN="$HOST_TBLGEN/bin/llvm-tblgen" \
        -DCLANG_TABLEGEN="$HOST_TBLGEN/bin/clang-tblgen" \
        -DLLVM_NATIVE_TOOL_DIR="$HOST_TBLGEN/bin"

    ninja -C "$B1NIX_BUILD" -j"$JOBS" clang clang-resource-headers lld
    DESTDIR= ninja -C "$B1NIX_BUILD" install-clang install-clang-resource-headers install-lld

    echo ""
    echo "b1nix-native Clang installed: $B1NIX_DEST/usr/bin/"
    file "$B1NIX_DEST/usr/bin/clang" 2>/dev/null || true
}

if [ "${1:-}" = "--b1nix-elf" ]; then
    shift
    build_b1nix_native "$@"
    exit 0
fi

ensure_sources
ensure_host_tblgen

# ── 3. Build LLVM + Clang + LLD (native Linux, defaults to b1nix target) ─────
if [ ! -x "$CLANG_BUILD/bin/clang" ]; then
    echo "[native-clang] Building LLVM + Clang + LLD (default target: x86_64-b1nix)..."
    rm -rf "$CLANG_BUILD"
    mkdir -p "$CLANG_BUILD"
    cd "$CLANG_BUILD"
    cmake -G Ninja -S "$LLVM_SRC/llvm" -B "$CLANG_BUILD" \
        -DCMAKE_BUILD_TYPE=Release \
        -DLLVM_ENABLE_PROJECTS="clang;lld" \
        -DLLVM_TARGETS_TO_BUILD="X86" \
        -DLLVM_ENABLE_ASSERTIONS=OFF \
        -DLLVM_ENABLE_ZSTD=OFF \
        -DLLVM_ENABLE_LIBCXX=ON \
        -DLLVM_LINK_LLVM_DYLIB=ON \
        -DLLVM_BUILD_LLVM_DYLIB=ON \
        -DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-b1nix \
        -DCLANG_DEFAULT_LINKER="lld" \
        -DCLANG_DEFAULT_CXX_STDLIB="libc++" \
        -DCLANG_DEFAULT_RTLIB="compiler-rt" \
        -DCLANG_INCLUDE_TESTS=OFF \
        -DLLVM_INCLUDE_TESTS=OFF \
        -DLLVM_TABLEGEN="$HOST_TBLGEN/bin/llvm-tblgen" \
        -DCLANG_TABLEGEN="$HOST_TBLGEN/bin/clang-tblgen" \
        -DLLVM_NATIVE_TOOL_DIR="$HOST_TBLGEN/bin" \
        >/dev/null
    ninja -j"$JOBS" clang lld llvm-ar llvm-nm llvm-objcopy llvm-strip llvm-readelf
    cd "$ROOT"
fi

# ── 4. Install ────────────────────────────────────────────────────────────────
echo "[native-clang] Installing..."
mkdir -p "$NATIVE_DEST/bin" "$NATIVE_DEST/lib/clang/22/include"

for tool in clang-22 lld llvm-ar llvm-nm llvm-objcopy llvm-readobj; do
    cp "$CLANG_BUILD/bin/$tool" "$NATIVE_DEST/bin/" 2>/dev/null || true
done
cd "$NATIVE_DEST/bin"
ln -sf clang-22 clang
ln -sf clang-22 clang++
ln -sf llvm-readobj llvm-readelf
ln -sf llvm-objcopy llvm-strip
cd "$ROOT"

# Resource headers (needed for -nostdinc)
if [ -d "$CLANG_BUILD/lib/clang/22/include" ]; then
    cp -R "$CLANG_BUILD/lib/clang/22/include/"* "$NATIVE_DEST/lib/clang/22/include/" 2>/dev/null || true
fi

echo ""
echo "Native Clang toolchain installed: $NATIVE_DEST/bin/"
$NATIVE_DEST/bin/clang --version | head -2
echo ""
echo "This is a Linux ELF cross-compiler that defaults to x86_64-b1nix."
echo "For self-hosting (b1nix-native ELF), run: tools/build-native-clang.sh --b1nix-elf"
