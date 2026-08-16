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

# 2. Fetch musl libc headers/libs from Alpine packages and stage into sysroot
echo "Fetching musl libc (Alpine packages) and staging headers/libs..."
MUSL_USR="$(B1NIX_ARCH="$B1NIX_ARCH" "$PROJECT_DIR/tools/packages/pkg-prefix.sh" musl | tail -1)"
if [ -d "$MUSL_USR" ]; then
    mkdir -p "$SYSROOT/usr/include" "$SYSROOT/usr/lib" "$SYSROOT/include" "$SYSROOT/lib"
    cp -Rf "$MUSL_USR/include/"* "$SYSROOT/include/" 2>/dev/null || true
    cp -Rf "$MUSL_USR/lib/"* "$SYSROOT/lib/" 2>/dev/null || true
fi

# The kernel UAPI headers b1nix publishes (linux/, asm/, asm-generic/) plus the
# b1nix-only extras and <sys/cdefs.h>. musl owns every libc header — copying the
# native b1nix libc headers over it would swap the O_* constants out from under
# the ABI — so only the trees musl does NOT provide are layered in, and never
# over an existing file (-n). This is the linux-headers package a distribution
# ships: without it, a port that includes <linux/...> only builds if its own
# build script bolts on a private copy of the header, which is a patch by
# another name.
if [ -d "$PROJECT_DIR/userspace/include" ]; then
    for _dir in "$SYSROOT/include" "$SYSROOT/usr/include"; do
        mkdir -p "$_dir/sys"
        for _tree in linux asm asm-generic b1nix; do
            [ -d "$PROJECT_DIR/userspace/include/$_tree" ] &&
                cp -Rn "$PROJECT_DIR/userspace/include/$_tree" "$_dir/" 2>/dev/null
        done
        [ -f "$PROJECT_DIR/userspace/include/sys/cdefs.h" ] &&
            cp -n "$PROJECT_DIR/userspace/include/sys/cdefs.h" "$_dir/sys/" 2>/dev/null
    done
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
