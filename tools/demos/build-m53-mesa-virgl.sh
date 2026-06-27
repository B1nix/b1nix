#!/bin/sh
# Build the M53 variant-B render test: userspace/bin/m53_mesa_virgl.c drives
# Mesa's gallium virgl driver (host-GPU OpenGL) through the b1nix winsys. Links
# the ported Mesa static libs (which now include libvirgl.a + libvirglb1nix.a)
# the same way the M52 OSMesa demo does. Arg 1 = output ELF. B1NIX_ARCH selects
# the toolchain/arch.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?usage: build-m53-mesa-virgl.sh <output-elf>}"
. "$ROOT_DIR/tools/toolchain/env.sh"
resolve_cxx_cross

if [ "$B1NIX_ARCH" = "x86" ]; then LDEMU="elf_i386"; else LDEMU="elf_x86_64"; fi
CROSS="$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/cross"
LD="$(command -v ld.lld 2>/dev/null || echo /opt/homebrew/bin/ld.lld)"
STRIP="$(command -v llvm-strip 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-strip)"

MESA="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/ports/build-mesa.sh")"
MESA_SRC="$ROOT_DIR/build/ports-src/mesa-${MESA_VERSION:-24.0.9}"
LIBM="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/ports/build-openlibm.sh")/lib/libm.a"

UB="$ROOT_DIR/userspace/build/$B1NIX_ARCH"
make B1NIX_ARCH="$B1NIX_ARCH" -C "$ROOT_DIR/userspace" -s \
  "build/$B1NIX_ARCH/libb1nix.a" "build/$B1NIX_ARCH/crt/crt0.o" 1>&2

OBJ="$(dirname "$OUT")/m53_mesa_virgl.o"
mkdir -p "$(dirname "$OUT")"
CC_CROSS="${B1NIX_CC:-$(command -v /opt/homebrew/opt/llvm/bin/clang 2>/dev/null || command -v clang 2>/dev/null || echo "$CROSS/bin/$B1NIX_TRIPLET-gcc")}"
CC_RES="$("$CC_CROSS" -print-resource-dir 2>/dev/null || true)"
# shellcheck disable=SC2086
"$CC_CROSS" --target="$B1NIX_TRIPLET" -O2 -ffunction-sections -fdata-sections -Db1nix \
  -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 \
  -DHAVE_FUNC_ATTRIBUTE_PACKED=1 \
  -nostdinc ${CC_RES:+-isystem "$CC_RES/include"} -isystem "$ROOT_DIR/userspace/include" \
  -I "$MESA_SRC/include" -I "$MESA_SRC/src" -I "$MESA_SRC/src/gallium/include" \
  -I "$MESA_SRC/src/gallium/auxiliary" -I "$MESA_SRC/src/mapi" \
  -I "$MESA_SRC/src/mesa" -I "$MESA_SRC/src/gallium/winsys/virgl/b1nix" \
  -c "$ROOT_DIR/userspace/bin/m53_mesa_virgl.c" -o "$OBJ"

# Dynamic by default: libc from the shared libc.so.1 via /lib/ld-b1nix.so (d8
# model); B1NIX_LINK=static restores the whole-archive static link.
if [ "${B1NIX_LINK:-dynamic}" = "static" ]; then
  DYN_CRT0="$UB/crt/crt0.o"; DYN_FLAGS=""; DYN_LIBC="--whole-archive $UB/libb1nix.a --no-whole-archive"
else
  DYN_CRT0="$UB/crt/crt0-dynamic.o"; DYN_FLAGS="--dynamic-linker /lib/ld-b1nix.so --hash-style=sysv"; DYN_LIBC="$UB/libc.so.1"
fi
# Use LLVM runtimes when available, else fall back to libgcc
if [ -n "${LLVM_CRT_CROSS:-}" ]; then
  CRT_LIBS="$LLVM_CRT_CROSS $LLVM_UNW_CROSS"
else
  CRT_LIBS="$LIBGCC_CROSS"
fi

# shellcheck disable=SC2046,SC2086
"$LD" -m "$LDEMU" -T "$ROOT_DIR/userspace/linker-cxx.ld" --gc-sections \
  -z norelro --allow-multiple-definition $DYN_FLAGS -o "$OUT" \
  "$DYN_CRT0" "$OBJ" \
  --start-group $(ls "$MESA"/lib/*.a) "$STDLIB_CROSS_A" "$STDLIB_ABI_CROSS_A" $CRT_LIBS "$LIBM" --end-group \
  $DYN_LIBC

"$STRIP" "$OUT"
