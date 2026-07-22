#!/usr/bin/env bash
# tools/toolchain/build-toolchain.sh - Sets up the pure LLVM/Clang cross-toolchain for b1nix
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Sourcing env.sh defines target triplet and build directories
. "$PROJECT_DIR/tools/toolchain/env.sh"

echo "=== Setting up pure LLVM/Clang cross-toolchain for $B1NIX_TRIPLET ==="

# 1. Initialize directories
BUILD_HOME="$TOOLCHAIN_BUILD_HOME"
PREFIX="$BUILD_HOME/cross"
SYSROOT="$BUILD_HOME/sysroot"

mkdir -p "$BUILD_HOME" "$PREFIX" "$PREFIX/bin" "$PREFIX/$B1NIX_TRIPLET/bin" "$PREFIX/$B1NIX_TRIPLET/lib"

# Sysroot symlink -> the per-arch userspace rootfs (build/<arch>/rootfs)
mkdir -p "$B1NIX_ROOTFS/lib" "$B1NIX_ROOTFS/usr/lib"
ln -sfn "$B1NIX_ROOTFS" "$SYSROOT" 2>/dev/null || true

# 2. Build musl libc headers/libs and stage into sysroot
echo "Building musl libc and staging headers/libs..."
B1NIX_ARCH="$B1NIX_ARCH" sh "$PROJECT_DIR/tools/ports/build-musl.sh" 1>&2
MUSL_USR="$PROJECT_DIR/build/$B1NIX_ARCH/ports/musl/install"
if [ -d "$MUSL_USR" ]; then
    mkdir -p "$SYSROOT/usr/include" "$SYSROOT/usr/lib" "$SYSROOT/include" "$SYSROOT/lib"
    cp -Rf "$MUSL_USR/include/"* "$SYSROOT/include/" 2>/dev/null || true
    cp -Rf "$MUSL_USR/lib/"* "$SYSROOT/lib/" 2>/dev/null || true
fi

# 3. Create the LLVM/Clang compiler wrappers and tool symlinks in PREFIX/bin
echo "Creating compiler shims and tool symlinks..."

# Compile wrappers: SYMLINK the host-side b1nix-autotools-cc / b1nix-c++ wrappers
# (not cp) so edits to the wrappers always take effect without re-running this
# script — copying left a stale x86_64-b1nix-gcc that missed wrapper fixes. The
# wrappers locate the project root by walking up from their own path (Makefile +
# kernel/), which resolves correctly through the symlink.
ln -sf "$PROJECT_DIR/tools/toolchain/bin/b1nix-autotools-cc" "$PREFIX/bin/${B1NIX_TRIPLET}-cc"
ln -sf "$PROJECT_DIR/tools/toolchain/bin/b1nix-c++" "$PREFIX/bin/${B1NIX_TRIPLET}-c++"
ln -sf "$PROJECT_DIR/tools/toolchain/bin/b1nix-autotools-cc" "$PREFIX/bin/${B1NIX_TRIPLET}-clang"
ln -sf "$PROJECT_DIR/tools/toolchain/bin/b1nix-c++" "$PREFIX/bin/${B1NIX_TRIPLET}-clang++"

# Direct symlinks for standard LLVM toolchain utilities
HOST_LLVM_AR="$(command -v llvm-ar 2>/dev/null || echo ar)"
HOST_LLVM_RANLIB="$(command -v llvm-ranlib 2>/dev/null || echo ranlib)"
HOST_LLVM_STRIP="$(command -v llvm-strip 2>/dev/null || echo strip)"
HOST_LLVM_NM="$(command -v llvm-nm 2>/dev/null || echo nm)"
HOST_LD_LLD="$(command -v ld.lld 2>/dev/null || echo ld)"
HOST_CLANG="$(command -v clang 2>/dev/null || echo clang)"

HOST_LLVM_READELF="$(command -v llvm-readelf 2>/dev/null || command -v readelf 2>/dev/null || echo readelf)"

ln -sf "$HOST_LLVM_AR" "$PREFIX/bin/${B1NIX_TRIPLET}-ar"
ln -sf "$HOST_LLVM_RANLIB" "$PREFIX/bin/${B1NIX_TRIPLET}-ranlib"
ln -sf "$HOST_LLVM_STRIP" "$PREFIX/bin/${B1NIX_TRIPLET}-strip"
ln -sf "$HOST_LLVM_NM" "$PREFIX/bin/${B1NIX_TRIPLET}-nm"
ln -sf "$HOST_LD_LLD" "$PREFIX/bin/${B1NIX_TRIPLET}-ld"
ln -sf "$HOST_LD_LLD" "$PREFIX/bin/${B1NIX_TRIPLET}-ld.lld"
ln -sf "$HOST_CLANG" "$PREFIX/bin/${B1NIX_TRIPLET}-as"
ln -sf "$HOST_LLVM_READELF" "$PREFIX/bin/${B1NIX_TRIPLET}-readelf"

# Now setup the inner bin dir (for meson/autotools cross file resolution)
(
    cd "$PREFIX/$B1NIX_TRIPLET/bin"
    ln -sf "../../bin/${B1NIX_TRIPLET}-cc" cc
    ln -sf "../../bin/${B1NIX_TRIPLET}-c++" c++
    ln -sf "../../bin/${B1NIX_TRIPLET}-clang" clang
    ln -sf "../../bin/${B1NIX_TRIPLET}-clang++" clang++
    ln -sf "../../bin/${B1NIX_TRIPLET}-ar" ar
    ln -sf "../../bin/${B1NIX_TRIPLET}-ranlib" ranlib
    ln -sf "../../bin/${B1NIX_TRIPLET}-strip" strip
    ln -sf "../../bin/${B1NIX_TRIPLET}-ld" ld
    ln -sf "../../bin/${B1NIX_TRIPLET}-ld.lld" ld.lld
    ln -sf "../../bin/${B1NIX_TRIPLET}-nm" nm
    ln -sf "../../bin/${B1NIX_TRIPLET}-as" as
    ln -sf "../../bin/${B1NIX_TRIPLET}-readelf" readelf
)

# 4. Generate empty stub archives for libpthread.a, librt.a, libdl.a
echo "Generating empty stub archives..."
for stubdir in "$SYSROOT/usr/lib" "$SYSROOT/lib" "$PREFIX/$B1NIX_TRIPLET/lib"; do
    mkdir -p "$stubdir"
    for stub in libpthread librt libdl; do
        if [ ! -f "$stubdir/$stub.a" ]; then
            "$HOST_LLVM_AR" rcs "$stubdir/$stub.a" 2>/dev/null || true
        fi
    done
done

# 5. Build the compiler-rt runtimes (builtins + libunwind)
echo "Building compiler-rt and libunwind..."
sh "$PROJECT_DIR/tools/toolchain/build-llvm-runtimes.sh"

# 6. Build the C++ runtimes (libc++ and libc++abi)
echo "Building libc++ and libc++abi..."
sh "$PROJECT_DIR/tools/toolchain/build-libcxx.sh"

# 7. Build the shared C++ runtimes
echo "Building shared C++ runtime (libc++.so.1 + libc++abi.so.1)..."
sh "$PROJECT_DIR/tools/toolchain/build-libcxx-shared.sh"

echo "=== Cross-toolchain setup complete ==="
