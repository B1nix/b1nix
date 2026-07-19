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
OBJ="$(dirname "$OUT")/m53_mesa_virgl.o"
mkdir -p "$(dirname "$OUT")"
CC_CROSS="${B1NIX_CC:-$(command -v /opt/homebrew/opt/llvm/bin/clang 2>/dev/null || command -v clang 2>/dev/null || echo "$CROSS/bin/$B1NIX_TRIPLET-gcc")}"
CC_RES="$("$CC_CROSS" -print-resource-dir 2>/dev/null || true)"
# shellcheck disable=SC2086
"$CC_CROSS" --target="$B1NIX_TRIPLET" -O2 -fPIC -ffunction-sections -fdata-sections -Db1nix \
  -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 \
  -DHAVE_FUNC_ATTRIBUTE_PACKED=1 -DHAVE_SECURE_GETENV=1 \
  -nostdinc ${CC_RES:+-isystem "$CC_RES/include"} -isystem "$ROOT_DIR/build/musl-b1nix/$B1NIX_TRIPLET/install/usr/include" -idirafter "$ROOT_DIR/userspace/include" \
  -I "$MESA_SRC/include" -I "$MESA_SRC/src" -I "$MESA_SRC/src/gallium/include" \
  -I "$MESA_SRC/src/gallium/auxiliary" -I "$MESA_SRC/src/mapi" \
  -I "$MESA_SRC/src/mesa" -I "$MESA_SRC/src/gallium/winsys/virgl/b1nix" \
  -c "$ROOT_DIR/userspace/bin/m53_mesa_virgl.c" -o "$OBJ"

# Link the executable against the real musl dynamic loader and libc.
MUSL_LIB="$ROOT_DIR/build/musl-b1nix/$B1NIX_TRIPLET/install/usr/lib"
BUILTINS_LIB="$(clang -print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a"
DYN_CRT0="$MUSL_LIB/Scrt1.o"
DYN_FLAGS="-pie -z norelro --hash-style=sysv --dynamic-linker /lib/ld-musl-x86_64.so.1 -L$MUSL_LIB"
DYN_LIBC="$MUSL_LIB/crti.o -lc $MUSL_LIB/crtn.o $BUILTINS_LIB"
# Linker script: linker-libcxx.ld for libc++ (maps phdrs + keeps .eh_frame_hdr
# for libunwind), else the GCC/libgcc linker-cxx.ld.
LINKER_ARGS=""
# Use LLVM runtimes when available, else fall back to libgcc. With libc++ the
# unwinder is already inside libc++abi.a, so don't add standalone libunwind.a too.
if [ -n "${LLVM_CRT_CROSS:-}" ]; then
  if [ "${B1NIX_CXX_STDLIB:-}" = "libc++" ]; then
    CRT_LIBS="$LLVM_CRT_CROSS"
  else
    CRT_LIBS="$LLVM_CRT_CROSS $LLVM_UNW_CROSS"
  fi
else
  CRT_LIBS="$LIBGCC_CROSS"
fi

# M53 virgl uses internal Mesa symbols (util_make_*_shader) not in libOSMesa.so,
# so link statically against the .a archives.
MESA_BUILD="$ROOT_DIR/build/mesa-b1nix/$B1NIX_TRIPLET/meson"
MESA_A_LIST=""
for a in $(find "$MESA_BUILD" -name "*.a" ! -name "libglsl_standalone.a" ! -name "libgtest.a" 2>/dev/null | sort); do
  MESA_A_LIST="$MESA_A_LIST $a"
done
OSMESA_TARGET="$MESA_BUILD/src/gallium/targets/osmesa/libOSMesa.so.8.0.0.p/target.c.o"
ZLIB_A="$ROOT_DIR/build/zlib-b1nix/$B1NIX_TRIPLET/install/lib/libz.a"
[ ! -f "$ZLIB_A" ] && ZLIB_A="$ROOT_DIR/build/mesa-b1nix/$B1NIX_TRIPLET/install/lib/libz.a"
# shellcheck disable=SC2086
"$LD" -m "$LDEMU" $LINKER_ARGS --gc-sections \
  -z norelro --allow-multiple-definition $DYN_FLAGS -o "$OUT" \
  "$DYN_CRT0" "$OBJ" \
  --start-group $MESA_A_LIST \
  ${OSMESA_TARGET:+"$OSMESA_TARGET"} \
  ${ZLIB_A:+"$ZLIB_A"} \
  "$STDLIB_CROSS_A" "$STDLIB_ABI_CROSS_A" $CRT_LIBS "$LIBM" \
  --end-group \
  $DYN_LIBC

"$STRIP" "$OUT"
