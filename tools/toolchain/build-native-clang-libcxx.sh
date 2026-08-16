#!/bin/sh
# Build the b1nix-native Clang/LLVM toolchain GCC-FREE: libLLVM.so + clang + lld
# compiled with the LLVM clang frontend against the b1nix *shared* libc++ runtime
# (libc++.so.1 + libc++abi.so.1 + compiler-rt), instead of cross-g++/libstdc++.
#
# This is the libc++ counterpart of build-native-clang-dynamic.sh. It builds into
# SEPARATE dirs (b1nix-libcxx-build / b1nix-libcxx-install) so the working
# libstdc++ toolchain is untouched until this one is verified and swapped in.
#
# Prereqs (from build-native-clang.sh's earlier stages):
#   build/native-clang/llvm-project-<ver>.src   LLVM source (b1nix triple patch)
#   build/native-clang/host-tblgen/bin/*tblgen  host TableGen tools
#   a built userspace libc.so.1 + the shared libc++.so.1/libc++abi.so.1 in the
#   cross sysroot (tools/toolchain/build-libcxx-shared.sh)
set -e

ARCH="${ARCH:-x86_64}"
TRIPLET="${B1NIX_TRIPLET:-x86_64-b1nix}"
LLVM_VER="${LLVM_VER:-22.1.8}"
JOBS="${JOBS:-6}"
SELF="$(readlink -f "$0" 2>/dev/null || readlink "$0" 2>/dev/null || echo "$0")"
SCRIPT_DIR="$(cd "$(dirname "$SELF")" && pwd)"
ROOT=""
cur="$SCRIPT_DIR"
while [ "$cur" != "/" ]; do
  if [ -f "$cur/Makefile" ] && [ -d "$cur/kernel" ]; then
    ROOT="$cur"
    break
  fi
  cur="$(dirname "$cur")"
done
[ -n "$ROOT" ] || ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

NC="$ROOT/build/native-clang"
SRC="$NC/llvm-project-${LLVM_VER}.src/llvm"
DEST="$NC/b1nix-libcxx/usr"
TBL="$NC/host-tblgen/bin"
BUILD="$NC/b1nix-libcxx-build"
INSTALL="$NC/b1nix-libcxx-install"
TC="$NC/b1nix-libcxx-toolchain.cmake"

[ -d "$SRC" ] || { echo "missing LLVM source $SRC — run tools/build-native-clang.sh first" >&2; exit 1; }
[ -x "$TBL/llvm-tblgen" ] || { echo "missing host tblgen — run tools/build-native-clang.sh first" >&2; exit 1; }

# Refresh the musl libc sysroot.
B1NIX_ARCH="$ARCH" sh "$ROOT/tools/ports/build-musl.sh"

# Generate the clang + shared-libc++ CMake toolchain file.
sh "$ROOT/tools/toolchain/gen-libcxx-native-cmake.sh" "$TC" >/dev/null
echo "[libcxx-clang] toolchain file: $TC"

# Clean configure so the patched HandleLLVMOptions B1nix branch sets LLVM_ON_UNIX.
if [ "${RECONFIGURE:-1}" = "1" ] || [ ! -f "$BUILD/build.ninja" ]; then
  echo "[libcxx-clang] clean configure ($BUILD)"
  rm -rf "$BUILD"
  cmake -G Ninja -S "$SRC" -B "$BUILD" \
      -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_INSTALL_PREFIX="$DEST" \
      -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_TARGETS_TO_BUILD="X86" \
      -DLLVM_DEFAULT_TARGET_TRIPLE="$TRIPLET" -DLLVM_HOST_TRIPLE="$TRIPLET" \
      -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_THREADS=OFF -DLLVM_ENABLE_ZLIB=OFF \
      -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBXML2=OFF \
      -DLLVM_ENABLE_CURL=OFF -DLLVM_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_TESTS=OFF \
      -DLLVM_BUILD_LLVM_DYLIB=ON -DLLVM_LINK_LLVM_DYLIB=ON \
      -DCLANG_LINK_CLANG_DYLIB=OFF \
      -DCLANG_DEFAULT_LINKER=lld -DCLANG_DEFAULT_CXX_STDLIB=libc++ \
      -DCLANG_DEFAULT_RTLIB=compiler-rt -DCLANG_DEFAULT_UNWINDLIB=libunwind \
      -DLLVM_TABLEGEN="$TBL/llvm-tblgen" -DCLANG_TABLEGEN="$TBL/clang-tblgen" \
      -DLLVM_NATIVE_TOOL_DIR="$TBL"
fi

if [ "${CONFIGURE_ONLY:-0}" = "1" ]; then echo "[libcxx-clang] configure-only, stopping."; exit 0; fi

# Optional single-target smoke of the toolchain before the full multi-hour build.
if [ -n "${BUILD_TARGET:-}" ]; then
  echo "[libcxx-clang] building target: $BUILD_TARGET (-j$JOBS)"
  ninja -C "$BUILD" -j"$JOBS" $BUILD_TARGET
  exit 0
fi

echo "[libcxx-clang] building libLLVM.so + clang + lld (-j$JOBS)"
ninja -C "$BUILD" -j"$JOBS" LLVM llvm-config clang clang-resource-headers lld

echo "[libcxx-clang] installing to staging $INSTALL"
rm -rf "$INSTALL"
DESTDIR="$INSTALL" ninja -C "$BUILD" \
    install-llvm-libraries install-llvm-headers install-llvm-config \
    install-clang install-clang-resource-headers install-lld

echo ""
echo "[libcxx-clang] done."
echo "  libLLVM.so : $DEST/lib/libLLVM.so"
echo "Verify GCC-free: readelf -d $DEST/lib/libLLVM.so | grep NEEDED  (should be libc++.so.1, NOT libstdc++)"
