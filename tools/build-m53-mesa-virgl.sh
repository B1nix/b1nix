#!/bin/sh
# Build the M53 variant-B render test: userspace/bin/m53_mesa_virgl.c drives
# Mesa's gallium virgl driver (host-GPU OpenGL) through the b1nix winsys. Links
# the ported Mesa static libs (which now include libvirgl.a + libvirglb1nix.a)
# the same way the M52 OSMesa demo does. Arg 1 = output ELF. B1NIX_ARCH selects
# the toolchain/arch.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:?usage: build-m53-mesa-virgl.sh <output-elf>}"
. "$ROOT_DIR/tools/toolchain-env.sh"

if [ "$B1NIX_ARCH" = "x86" ]; then LDEMU="elf_i386"; else LDEMU="elf_x86_64"; fi
CROSS="$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/cross"
GCC="$CROSS/bin/$B1NIX_TRIPLET-gcc"
GXX="$CROSS/bin/$B1NIX_TRIPLET-g++"
LD="$(command -v ld.lld 2>/dev/null || echo /opt/homebrew/bin/ld.lld)"
STRIP="$(command -v llvm-strip 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-strip)"

MESA="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/build-mesa.sh")"
MESA_SRC="$ROOT_DIR/build/ports-src/mesa-${MESA_VERSION:-24.0.9}"
LIBM="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/build-openlibm.sh")/lib/libm.a"
LIBSTDCXX="$("$GXX" -print-file-name=libstdc++.a)"
LIBSUPCXX="$("$GXX" -print-file-name=libsupc++.a)"
LIBGCC="$("$GXX" -print-libgcc-file-name)"

UB="$ROOT_DIR/userspace/build/$B1NIX_ARCH"
make B1NIX_ARCH="$B1NIX_ARCH" -C "$ROOT_DIR/userspace" -s \
  "build/$B1NIX_ARCH/libb1nix.a" "build/$B1NIX_ARCH/crt/crt0.o" 1>&2

OBJ="$(dirname "$OUT")/m53_mesa_virgl.o"
mkdir -p "$(dirname "$OUT")"
GCC_INC="$("$GCC" -print-file-name=include)"
"$GCC" -O2 -ffunction-sections -fdata-sections -Db1nix \
  -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 \
  -nostdinc -isystem "$ROOT_DIR/userspace/include" -isystem "$GCC_INC" \
  -I "$MESA_SRC/include" -I "$MESA_SRC/src" -I "$MESA_SRC/src/gallium/include" \
  -I "$MESA_SRC/src/gallium/auxiliary" -I "$MESA_SRC/src/mapi" \
  -I "$MESA_SRC/src/mesa" -I "$MESA_SRC/src/gallium/winsys/virgl/b1nix" \
  -c "$ROOT_DIR/userspace/bin/m53_mesa_virgl.c" -o "$OBJ"

# shellcheck disable=SC2046
"$LD" -m "$LDEMU" -T "$ROOT_DIR/userspace/linker-cxx.ld" --gc-sections \
  -z norelro --allow-multiple-definition -o "$OUT" \
  "$UB/crt/crt0.o" "$OBJ" \
  --start-group $(ls "$MESA"/lib/*.a) "$LIBSTDCXX" "$LIBSUPCXX" "$LIBGCC" "$LIBM" \
  --whole-archive "$UB/libb1nix.a" --no-whole-archive --end-group

"$STRIP" "$OUT"
