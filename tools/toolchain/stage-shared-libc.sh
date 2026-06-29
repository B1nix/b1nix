#!/bin/sh
# Stage the b1nix shared libc into the cross toolchain sysroot so that SHARED
# libraries (libLLVM.so for M75 llvmpipe, libOSMesa.so, ...) can link against it
# dynamically instead of folding the non-PIC static libc.a/libm.a (which cannot
# go into a .so — R_X86_64_32 against .rodata).
#
# Two things the cross sysroot otherwise lacks:
#   1. An up-to-date libc.so.1. The cross sysroot's copy is staged once at
#      toolchain-build time and goes stale when userspace/libc/ rebuilds (e.g. a
#      newly added export like mallinfo2), so a shared link fails with an
#      "undefined reference" to a symbol that *is* in the current libc.
#   2. A libm.so. b1nix is musl-style — the math functions live in libc, so there
#      is no separate libm — but `-lm` (added implicitly by g++/clang) still needs
#      a libm.so to resolve against. We provide it as a linker script that pulls
#      libc.so.1, mirroring how userspace/Makefile folds openlibm into libc.so.1.
#
# Idempotent. Run after `make -C userspace build/<arch>/libc.so.1`.
set -e

ARCH="${ARCH:-x86_64}"
TRIPLET="${B1NIX_TRIPLET:-x86_64-b1nix}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

SRC_LIBC="$ROOT/userspace/build/$ARCH/libc.so.1"
[ -f "$SRC_LIBC" ] || SRC_LIBC="$ROOT/build/$ARCH/rootfs/lib/libc.so.1"
if [ ! -f "$SRC_LIBC" ]; then
    echo "stage-shared-libc: no built libc.so.1 found — run 'make -C userspace build/$ARCH/libc.so.1' first" >&2
    exit 1
fi

# Destinations that a cross link searches: the toolchain sysroot lib dir and the
# rootfs lib dir (used as --sysroot for ported builds).
DESTS="$ROOT/build/toolchain_build/$TRIPLET/cross/$TRIPLET/lib $ROOT/build/$ARCH/rootfs/lib"

for d in $DESTS; do
    [ -d "$d" ] || continue
    cp -f "$SRC_LIBC" "$d/libc.so.1"
    [ -e "$d/libc.so" ] || ln -sf libc.so.1 "$d/libc.so"
    # musl-style libm: math is in libc, so -lm resolves through libc.so.1.
    printf 'INPUT(libc.so.1)\n' > "$d/libm.so"
    echo "stage-shared-libc: refreshed libc.so.1 + libm.so in $d"
done
