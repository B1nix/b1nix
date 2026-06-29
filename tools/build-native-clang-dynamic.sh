#!/bin/sh
# Build the b1nix-native Clang/LLVM toolchain in DYNAMIC form: a shared
# libLLVM.so plus a clang that links it at runtime through the M69 .so loader,
# instead of the ~90 MB statically-linked monolith from build-native-clang.sh.
#
# Why: the static clang is a ~90 MB non-swappable load-staging transient — the
# very thing that kept the M26 self-host RAM floor ~128 MB above its theoretical
# bound. With a dynamic clang, the 72 MB of LLVM lives in libLLVM.so (file-backed,
# demand-paged, reclaimable via the M52/M72 page-cache path), so only the LLVM
# pages a compile actually touches stay resident. The clang executable itself
# drops to ~44 MB (clang frontend stays statically folded; CLANG_LINK_CLANG_DYLIB
# is OFF because the clang-cpp dylib target is not generated in this config).
#
# Prereqs (produced by build-native-clang.sh's earlier stages):
#   build/native-clang/llvm-project-<ver>.src   LLVM source
#   build/native-clang/host-tblgen/bin/*tblgen  host TableGen tools
#   build/native-clang/b1nix-toolchain.cmake    cross toolchain file
#   a built userspace libc.so.1 (make -C userspace build/<arch>/libc.so.1)
#
# Three cross-build walls this script encodes the fixes for (see commit history):
#   1. CLEAN configure so the patched HandleLLVMOptions.cmake B1nix branch sets
#      LLVM_ON_UNIX=1 (an in-place `cmake -D` of a stale dir drops it and LLVM's
#      own headers then fail to compile).
#   2. shared libc so the .so does not fold the non-PIC static libm.a — handled
#      by tools/toolchain/stage-shared-libc.sh (musl-style libm.so -> libc.so.1).
#   3. a fresh cross-sysroot libc.so.1 (same staging script) so newer exports
#      like mallinfo2 resolve.
set -e

ARCH="${ARCH:-x86_64}"
TRIPLET="${B1NIX_TRIPLET:-x86_64-b1nix}"
LLVM_VER="${LLVM_VER:-22.1.8}"
JOBS="${JOBS:-6}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

NC="$ROOT/build/native-clang"
SRC="$NC/llvm-project-${LLVM_VER}.src/llvm"
TC="$NC/b1nix-toolchain.cmake"
DEST="$NC/b1nix/usr"
TBL="$NC/host-tblgen/bin"
DYN="$NC/b1nix-dyn-build"
DYN_INSTALL="$NC/b1nix-dyn-install"

[ -d "$SRC" ] || { echo "missing LLVM source $SRC — run tools/build-native-clang.sh first" >&2; exit 1; }
[ -x "$TBL/llvm-tblgen" ] || { echo "missing host tblgen — run tools/build-native-clang.sh first" >&2; exit 1; }

# Refresh the cross sysroot's shared libc + libm.so alias (walls 2 & 3).
ARCH="$ARCH" sh "$ROOT/tools/toolchain/stage-shared-libc.sh"

# Wall 1: a CLEAN configure (fresh dir) so LLVM_ON_UNIX is set for B1nix.
echo "[dyn-clang] clean configure ($DYN)"
rm -rf "$DYN"
cmake -G Ninja -S "$SRC" -B "$DYN" \
    -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_INSTALL_PREFIX="$DEST" \
    -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_TARGETS_TO_BUILD="X86" \
    -DLLVM_DEFAULT_TARGET_TRIPLE="$TRIPLET" -DLLVM_HOST_TRIPLE="$TRIPLET" \
    -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_THREADS=OFF -DLLVM_ENABLE_ZLIB=OFF \
    -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_ENABLE_CURL=OFF -DLLVM_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_TESTS=OFF \
    -DLLVM_BUILD_LLVM_DYLIB=ON -DLLVM_LINK_LLVM_DYLIB=ON \
    -DCLANG_LINK_CLANG_DYLIB=OFF \
    -DCLANG_DEFAULT_LINKER=ld -DCLANG_DEFAULT_CXX_STDLIB=libstdc++ -DCLANG_DEFAULT_RTLIB=libgcc \
    -DLLVM_TABLEGEN="$TBL/llvm-tblgen" -DCLANG_TABLEGEN="$TBL/clang-tblgen" \
    -DLLVM_NATIVE_TOOL_DIR="$TBL"

echo "[dyn-clang] building libLLVM.so + clang (-j$JOBS)"
ninja -C "$DYN" -j"$JOBS" LLVM llvm-config clang clang-resource-headers lld

echo "[dyn-clang] installing to staging $DYN_INSTALL"
rm -rf "$DYN_INSTALL"
DESTDIR="$DYN_INSTALL" ninja -C "$DYN" \
    install-llvm-libraries install-llvm-headers install-llvm-config \
    install-clang install-clang-resource-headers install-lld

# Give libLLVM.so a proper SONAME (libLLVM-22.so) so dependents record a
# resolvable name, not the build-tree path. LLVM's dylib link does not set it in
# this config, and clang ends up with NEEDED 'lib/libLLVM.so' (a relative path).
# patchelf fixes both post-link without a rebuild.
if command -v patchelf >/dev/null 2>&1; then
    patchelf --set-soname libLLVM-22.so "$DEST/lib/libLLVM.so" 2>/dev/null || true
    DYN_CLANG="$(find "$DYN_INSTALL" -name clang-22 -type f | head -1)"
    [ -n "$DYN_CLANG" ] && patchelf --replace-needed lib/libLLVM.so libLLVM-22.so "$DYN_CLANG" 2>/dev/null || true
    echo "[dyn-clang] set libLLVM.so SONAME=libLLVM-22.so + fixed clang NEEDED"
else
    echo "[dyn-clang] WARNING: patchelf not found — libLLVM.so has no SONAME, clang NEEDED stays 'lib/libLLVM.so'" >&2
fi

echo ""
echo "[dyn-clang] done."
echo "  libLLVM.so : $DYN/lib/libLLVM.so"
echo "  clang      : $(find "$DYN_INSTALL" -name clang-22 -type f | head -1)"
echo "Package libLLVM.so as /lib/libLLVM.so in the b1nix rootfs and ensure the"
echo "clang NEEDED entry resolves before running it on b1nix (b1nix.clangrun)."
