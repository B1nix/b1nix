#!/usr/bin/env bash
# tools/build-native-clang.sh
#
# Build a Clang/LLVM toolchain for b1nix with default target x86_64-unknown-b1nix.
#
# Strategy: build LLVM as a native Linux binary with LLVM_DEFAULT_TARGET_TRIPLE
# set to x86_64-unknown-b1nix. This produces a fast, reliable cross-compiler
# that defaults to targeting b1nix. The binary itself is a Linux ELF.
#
# For self-hosting (a clang that runs ON b1nix), a second pass cross-compiles
# this Linux clang into a b1nix ELF using the cross GCC + b1nix sysroot.
#
# Output: build/native-clang/installed/bin/{clang,clang++,lld,llvm-ar,...}
#
# Usage:
#   tools/build-native-clang.sh
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export PROJECT_DIR="$ROOT"
. "$ROOT/tools/toolchain/env.sh" 2>/dev/null || true
B1NIX_TRIPLET="${B1NIX_TRIPLET:-x86_64-b1nix}"
CROSS="$ROOT/build/toolchain_build/$B1NIX_TRIPLET/cross"
SYSROOT="$ROOT/build/toolchain_build/$B1NIX_TRIPLET/sysroot"
HOST_TBLGEN="$ROOT/build/native-clang/host-tblgen"
CLANG_BUILD="$ROOT/build/native-clang/full-build"
NATIVE_DEST="$ROOT/build/native-clang/installed"
JOBS="${NATIVE_CLANG_JOBS:-$(nproc 2>/dev/null || echo 4)}"

LLVM_VER="${LLVM_VER:-22.1.8}"
LLVM_URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VER}/llvm-project-${LLVM_VER}.src.tar.xz"

# ── 1. Download LLVM sources ──────────────────────────────────────────────────
LLVM_SRC="$ROOT/build/native-clang/llvm-project-${LLVM_VER}.src"
if [ ! -d "$LLVM_SRC/clang" ]; then
    echo "[native-clang] Downloading LLVM ${LLVM_VER}..."
    LLVM_DL="$ROOT/build/native-clang/llvm-${LLVM_VER}.src.tar.xz"
    mkdir -p "$(dirname "$LLVM_DL")"
    [ -f "$LLVM_DL" ] || curl -fL --retry 3 "$LLVM_URL" -o "$LLVM_DL"
    echo "[native-clang] Extracting LLVM sources..."
    tar -xJf "$LLVM_DL" -C "$ROOT/build/native-clang/" 2>/dev/null
fi

# ── 2. Build host TableGen (needed for cross-compiling LLVM .td -> .inc) ──────
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

# ── 3. Build LLVM + Clang + LLD (native Linux, defaults to b1nix target) ─────
if [ ! -x "$CLANG_BUILD/bin/clang" ]; then
    echo "[native-clang] Building LLVM + Clang + LLD (default target: x86_64-unknown-b1nix)..."
    rm -rf "$CLANG_BUILD"
    mkdir -p "$CLANG_BUILD"
    cd "$CLANG_BUILD"
    cmake -G Ninja -S "$LLVM_SRC/llvm" -B "$CLANG_BUILD" \
        -DCMAKE_BUILD_TYPE=Release \
        -DLLVM_ENABLE_PROJECTS="clang;lld" \
        -DLLVM_TARGETS_TO_BUILD="X86" \
        -DLLVM_ENABLE_ASSERTIONS=OFF \
        -DLLVM_ENABLE_LIBCXX=ON \
        -DLLVM_LINK_LLVM_DYLIB=ON \
        -DLLVM_BUILD_LLVM_DYLIB=ON \
        -DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-unknown-b1nix \
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
echo "This is a Linux ELF cross-compiler that defaults to x86_64-unknown-b1nix."
echo "For self-hosting (b1nix-native ELF), run: tools/build-native-clang.sh --b1nix-elf"
