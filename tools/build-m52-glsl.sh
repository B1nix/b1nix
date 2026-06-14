#!/bin/sh
# Build the M52 GLSL demo ELF: compile userspace/bin/m52_glsl.c against the Mesa
# OSMesa/GL headers and link it with the ported Mesa static libraries, the b1gui
# client, libstdc++/libsupc++/libgcc, libm and libb1nix. Mirrors
# tools/build-m52-osmesa.sh — the same Mesa build serves both demos.
# Arg 1 = output path. B1NIX_ARCH selects the toolchain/arch.

set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:?usage: build-m52-glsl.sh <output-elf>}"
. "$ROOT_DIR/tools/toolchain-env.sh"

if [ "$B1NIX_ARCH" = "x86" ]; then LDEMU="elf_i386"; else LDEMU="elf_x86_64"; fi
CROSS="$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/cross"
GCC="$CROSS/bin/$B1NIX_TRIPLET-gcc"
GXX="$CROSS/bin/$B1NIX_TRIPLET-g++"
LD="$(command -v ld.lld 2>/dev/null || echo /opt/homebrew/bin/ld.lld)"
STRIP="$(command -v llvm-strip 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-strip)"

MESA="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/build-mesa.sh")"
LIBM="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/build-openlibm.sh")/lib/libm.a"
LIBSTDCXX="$("$GXX" -print-file-name=libstdc++.a)"
LIBSUPCXX="$("$GXX" -print-file-name=libsupc++.a)"
LIBGCC="$("$GXX" -print-libgcc-file-name)"

UB="$ROOT_DIR/userspace/build/$B1NIX_ARCH"
make B1NIX_ARCH="$B1NIX_ARCH" -C "$ROOT_DIR/userspace" -s \
  "build/$B1NIX_ARCH/libb1nix.a" "build/$B1NIX_ARCH/libb1gui.a" \
  "build/$B1NIX_ARCH/crt/crt0.o" 1>&2

OBJ="$(dirname "$OUT")/m52_glsl.o"
mkdir -p "$(dirname "$OUT")"
"$GCC" -O2 -ffunction-sections -fdata-sections -Db1nix \
  -nostdinc -isystem "$ROOT_DIR/userspace/include" -I "$MESA/include" \
  -c "$ROOT_DIR/userspace/bin/m52_glsl.c" -o "$OBJ"

# shellcheck disable=SC2046
"$LD" -m "$LDEMU" -T "$ROOT_DIR/userspace/linker-cxx.ld" --gc-sections \
  --allow-multiple-definition -o "$OUT" \
  "$UB/crt/crt0.o" "$OBJ" "$MESA/lib/osmesa_target.o" \
  --start-group $(ls "$MESA"/lib/*.a) "$LIBSTDCXX" "$LIBSUPCXX" "$LIBGCC" "$LIBM" \
  --whole-archive "$UB/libb1gui.a" "$UB/libb1nix.a" --no-whole-archive --end-group

"$STRIP" "$OUT"
