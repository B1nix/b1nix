#!/bin/sh
# Assemble the canonical DYNAMIC b1nix-native clang/lld toolchain into a clean
# install prefix, so it can be the standard native toolchain (install-native-
# toolchain, the self-host module, b1nix-cc, ...) instead of the ~94 MB static
# clang. The dynamic clang (44 MB) + dynamic lld (5.5 MB) keep LLVM out of the
# binary: the 78 MB libLLVM-22.so is loaded once and demand-paged by the M69
# loader (reclaimable), which is exactly what lowers the M26 self-host RAM floor.
#
# Inputs (produced by tools/build-native-clang-dynamic.sh):
#   build/native-clang/b1nix-dyn-build/bin/{clang-22,lld}   the dynamic binaries
#   build/native-clang/b1nix-dyn-build/lib/clang/<v>/include clang resource hdrs
#   build/native-clang/b1nix/usr/lib/libLLVM.so             real 78 MB .so (soname
#                                                           libLLVM-22.so)
# Output (the canonical dynamic prefix, mirroring the static b1nix/usr layout):
#   build/native-clang/b1nix-dyn/usr/bin/{clang-22,clang,clang++,lld,ld.lld}
#   build/native-clang/b1nix-dyn/usr/lib/libLLVM-22.so
#   build/native-clang/b1nix-dyn/usr/lib/clang/<v>/include/...
#
# Prints the prefix path on stdout (like the port scripts) so callers can `$( )` it.
# Idempotent. patchelf rewrites each binary's relative NEEDED 'lib/libLLVM.so'
# to the resolvable soname 'libLLVM-22.so'.
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
NC="$ROOT/build/native-clang"
# Source build dir + libLLVM.so are overridable so the GCC-free libc++ toolchain
# (tools/build-native-clang-libcxx.sh -> b1nix-libcxx-build) can be staged as the
# canonical toolchain. Defaults = the historical libstdc++ dynamic build.
DYN="${NATIVE_CLANG_DYN_BUILD:-$NC/b1nix-dyn-build}"
SO="${NATIVE_CLANG_SO:-$NC/b1nix/usr/lib/libLLVM.so}"
PREFIX="${NATIVE_CLANG_PREFIX:-$NC/b1nix-dyn/usr}"
# The NEEDED name clang/lld record for the dylib (libc++ build: 'libLLVM.so';
# the older libstdc++ build: 'lib/libLLVM.so').
LIBLLVM_NEEDED="${NATIVE_CLANG_LIBLLVM_NEEDED:-lib/libLLVM.so}"

CLANG="$DYN/bin/clang-22"
LLD="$DYN/bin/lld"
RESDIR="$(ls -d "$DYN"/lib/clang/* 2>/dev/null | head -1)"

for f in "$CLANG" "$LLD" "$SO"; do
    [ -f "$f" ] || { echo "stage-dynamic-clang: missing $f — run tools/build-native-clang-dynamic.sh first" >&2; exit 1; }
done
[ -d "$RESDIR/include" ] || { echo "stage-dynamic-clang: missing clang resource headers under $DYN/lib/clang" >&2; exit 1; }

rm -rf "$PREFIX"
mkdir -p "$PREFIX/bin" "$PREFIX/lib"

cp "$CLANG" "$PREFIX/bin/clang-22"
cp "$LLD"   "$PREFIX/bin/lld"
( cd "$PREFIX/bin" && ln -sf clang-22 clang && ln -sf clang-22 clang++ && ln -sf lld ld.lld )

cp "$SO" "$PREFIX/lib/libLLVM-22.so"
RESV="$(basename "$RESDIR")"
mkdir -p "$PREFIX/lib/clang/$RESV"
cp -R "$RESDIR/include" "$PREFIX/lib/clang/$RESV/"

# Make the dynamic binaries resolve the .so by soname rather than the build-tree
# relative path lld/clang record by default in this LLVM config.
if command -v patchelf >/dev/null 2>&1; then
    patchelf --set-soname libLLVM-22.so "$PREFIX/lib/libLLVM-22.so" 2>/dev/null || true
    patchelf --replace-needed "$LIBLLVM_NEEDED" libLLVM-22.so "$PREFIX/bin/clang-22" 2>/dev/null || true
    patchelf --replace-needed "$LIBLLVM_NEEDED" libLLVM-22.so "$PREFIX/bin/lld" 2>/dev/null || true
else
    echo "stage-dynamic-clang: WARNING patchelf not found — clang/lld NEEDED stays 'lib/libLLVM.so'" >&2
fi

echo "$PREFIX"
