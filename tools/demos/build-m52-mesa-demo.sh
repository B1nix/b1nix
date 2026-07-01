#!/bin/sh
# Build an M52 Mesa demo ELF against the
# Mesa OSMesa headers and link it with the ported Mesa static libraries, the
# b1gui client, libstdc++/libsupc++/libgcc, libm and libb1nix. --gc-sections +
# strip keep the (still large) binary as small as Mesa allows. Arg 1 = output
# path. Arg 1 is the source stem, arg 2 is the output path.

set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
DEMO="${1:?usage: build-m52-mesa-demo.sh <source-stem> <output-elf>}"
OUT="${2:?usage: build-m52-mesa-demo.sh <source-stem> <output-elf>}"
. "$ROOT_DIR/tools/toolchain/env.sh"
resolve_cxx_cross

if [ "$B1NIX_ARCH" = "x86" ]; then LDEMU="elf_i386"; else LDEMU="elf_x86_64"; fi
CROSS="$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/cross"
LD="$(command -v ld.lld 2>/dev/null || echo /opt/homebrew/bin/ld.lld)"
STRIP="$(command -v llvm-strip 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-strip)"

MESA="$(B1NIX_ARCH="$B1NIX_ARCH" MESA_LLVMPIPE="${MESA_LLVMPIPE:-0}" "$ROOT_DIR/tools/ports/build-mesa.sh")"
LIBM="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/ports/build-openlibm.sh")/lib/libm.a"

# M75: with llvmpipe, the Mesa archives carry undefined LLVM C-API symbols that
# resolve from the shared libLLVM-22 at link/runtime (static Mesa + dynamic LLVM).
LLVM_LINK=""
if [ "${MESA_LLVMPIPE:-0}" = "1" ]; then
  LLVM_LINK="-L$ROOT_DIR/build/native-clang/b1nix/usr/lib -lLLVM-22"
fi

UB="$ROOT_DIR/userspace/build/$B1NIX_ARCH"
make B1NIX_ARCH="$B1NIX_ARCH" -C "$ROOT_DIR/userspace" -s \
  "build/$B1NIX_ARCH/libb1nix.a" "build/$B1NIX_ARCH/libb1gui.a" \
  "build/$B1NIX_ARCH/crt/crt0.o" 1>&2

OBJ="$(dirname "$OUT")/$DEMO.o"
mkdir -p "$(dirname "$OUT")"
CC_CROSS="${B1NIX_CC:-$(command -v /opt/homebrew/opt/llvm/bin/clang 2>/dev/null || command -v clang 2>/dev/null || echo "$CROSS/bin/$B1NIX_TRIPLET-gcc")}"
CC_RES="$("$CC_CROSS" -print-resource-dir 2>/dev/null || true)"
# shellcheck disable=SC2086
"$CC_CROSS" --target="$B1NIX_TRIPLET" -O2 -ffunction-sections -fdata-sections -Db1nix \
  -nostdinc ${CC_RES:+-isystem "$CC_RES/include"} -isystem "$ROOT_DIR/userspace/include" -I "$MESA/include" \
  -c "$ROOT_DIR/userspace/bin/$DEMO.c" -o "$OBJ"

# Dynamic by default: libc from the shared libc.so.1 via /lib/ld-b1nix.so (d8
# model); B1NIX_LINK=static restores the whole-archive static link.
if [ "${B1NIX_LINK:-dynamic}" = "static" ]; then
  DYN_CRT0="$UB/crt/crt0.o"; DYN_FLAGS=""; DYN_LIBC="--whole-archive $UB/libb1nix.a --no-whole-archive"
else
  DYN_CRT0="$UB/crt/crt0-dynamic.o"; DYN_FLAGS="--dynamic-linker /lib/ld-b1nix.so -z norelro --hash-style=sysv"; DYN_LIBC="$UB/libc.so.1"
fi
# Linker script: linker-cxx.ld (GCC/libgcc) vs linker-libcxx.ld (libc++/libunwind:
# maps the program headers + keeps .eh_frame_hdr so libunwind unwinds via
# dl_iterate_phdr — required even for the statically-folded libc++ in this exe).
LINKER_LD="$ROOT_DIR/userspace/linker-cxx.ld"
if [ "${B1NIX_CXX_STDLIB:-}" = "libc++" ]; then
  LINKER_LD="$ROOT_DIR/userspace/linker-libcxx.ld"
  # libunwind finds FDEs via PT_GNU_EH_FRAME (.eh_frame_hdr); emit it.
  DYN_FLAGS="$DYN_FLAGS --eh-frame-hdr"
fi
# Use LLVM runtimes when available, else fall back to libgcc. With libc++ the
# unwinder is already inside the folded libc++abi.a (STDLIB_ABI_CROSS_A), so do NOT
# add the standalone libunwind.a too (duplicate _Unwind_*).
if [ -n "${LLVM_CRT_CROSS:-}" ]; then
  if [ "${B1NIX_CXX_STDLIB:-}" = "libc++" ]; then
    CRT_LIBS="$LLVM_CRT_CROSS"
  else
    CRT_LIBS="$LLVM_CRT_CROSS $LLVM_UNW_CROSS"
  fi
else
  CRT_LIBS="$LIBGCC_CROSS"
fi

# shellcheck disable=SC2046,SC2086
"$LD" -m "$LDEMU" -T "$LINKER_LD" --gc-sections \
  --allow-multiple-definition $DYN_FLAGS -o "$OUT" \
  "$DYN_CRT0" "$OBJ" "$MESA/lib/osmesa_target.o" \
  --start-group $(ls "$MESA"/lib/*.a) "$STDLIB_CROSS_A" "$STDLIB_ABI_CROSS_A" \
  $CRT_LIBS "$LIBM" \
  --whole-archive "$UB/libb1gui.a" --no-whole-archive --end-group $LLVM_LINK $DYN_LIBC

"$STRIP" "$OUT"
