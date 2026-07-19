#!/bin/sh
# Build the M59 EGL smoke ELF: compile userspace/bin/m59_smoke.c together with
# the OSMesa-backed EGL implementation (userspace/libegl/b1egl_mesa.c) against
# the Mesa OSMesa headers, and link it with the ported Mesa static libraries,
# the b1gui client, the LLVM C++ runtime (libc++/libc++abi + compiler-rt), libm
# and libb1nix. This is the same link shape as the M52 OSMesa demo (Mesa is a C++
# codebase), but the GL is now reached through the standard egl* API instead of
# the OSMesa* API directly. Arg 1 = output path. B1NIX_ARCH selects the arch.
#
# M90: default C++ runtime is GCC-free LLVM libc++ (matches Mesa, now libc++).
# Set B1NIX_CXX_STDLIB=libstdc++ to opt into the legacy GCC path.

set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?usage: build-m59-egl.sh <output-elf>}"
B1NIX_CXX_STDLIB="${B1NIX_CXX_STDLIB:-libc++}"; export B1NIX_CXX_STDLIB
. "$ROOT_DIR/tools/toolchain/env.sh"
resolve_cxx_cross

if [ "$B1NIX_ARCH" = "x86" ]; then LDEMU="elf_i386"; else LDEMU="elf_x86_64"; fi
CROSS="$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/cross"
LD="$(command -v ld.lld 2>/dev/null || echo /opt/homebrew/bin/ld.lld)"
STRIP="$(command -v llvm-strip 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-strip)"

MESA="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/ports/build-mesa.sh")"
LIBM="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/ports/build-openlibm.sh")/lib/libm.a"

UB="$ROOT_DIR/userspace/build/$B1NIX_ARCH"
MUSL_USR="$ROOT_DIR/build/musl-b1nix/$B1NIX_TRIPLET/install/usr"
if [ -f "$MUSL_USR/lib/libc.so" ]; then
  make B1NIX_ARCH="$B1NIX_ARCH" LINK=musl -C "$ROOT_DIR/userspace" -s \
    "build/$B1NIX_ARCH/libb1gui.a" 1>&2
else
  make B1NIX_ARCH="$B1NIX_ARCH" -C "$ROOT_DIR/userspace" -s \
    "build/$B1NIX_ARCH/libb1nix.a" "build/$B1NIX_ARCH/libb1gui.a" \
    "build/$B1NIX_ARCH/crt/crt0.o" 1>&2
fi

OUTDIR="$(dirname "$OUT")"
mkdir -p "$OUTDIR"
SMOKE_OBJ="$OUTDIR/m59_smoke.o"
EGL_OBJ="$OUTDIR/b1egl_mesa.o"

CC_CROSS="${B1NIX_CC:-$(command -v /opt/homebrew/opt/llvm/bin/clang 2>/dev/null || command -v clang 2>/dev/null || echo "$CROSS/bin/$B1NIX_TRIPLET-gcc")}"
CC_RES="$("$CC_CROSS" -print-resource-dir 2>/dev/null || true)"

if [ -f "$MUSL_USR/lib/libc.so" ]; then
  CFLAGS="-O2 -fPIC -fPIE -ffunction-sections -fdata-sections -Db1nix \
    --target=x86_64-unknown-elf -ffreestanding -fno-builtin -nostdinc \
    ${CC_RES:+-isystem $CC_RES/include} -isystem $MUSL_USR/include \
    -isystem $ROOT_DIR/userspace/include -I $MESA/include"
else
  CFLAGS="-O2 -ffunction-sections -fdata-sections -Db1nix \
    --target=$B1NIX_TRIPLET -nostdinc ${CC_RES:+-isystem $CC_RES/include} -isystem $ROOT_DIR/userspace/include -I $MESA/include"
fi

# shellcheck disable=SC2086
"$CC_CROSS" $CFLAGS -c "$ROOT_DIR/userspace/bin/m59_smoke.c" -o "$SMOKE_OBJ"
# shellcheck disable=SC2086
"$CC_CROSS" $CFLAGS -c "$ROOT_DIR/userspace/libegl/b1egl_mesa.c" -o "$EGL_OBJ"

MESA_LIB="$ROOT_DIR/build/mesa-b1nix/$B1NIX_TRIPLET/install/lib"

# shellcheck disable=SC2046
if [ -f "$MUSL_USR/lib/libc.so" ]; then
  MUSL_LIB="$MUSL_USR/lib"
  make B1NIX_ARCH="$B1NIX_ARCH" LINK=musl -C "$ROOT_DIR/userspace" -s \
    "build/$B1NIX_ARCH/libcxx_compat.o" 1>&2
  # Use lld's default PIE layout: it maps the ELF and program-header tables
  # in the first PT_LOAD, which is required by musl's AT_PHDR startup path.
  "$LD" -m "$LDEMU" -pie \
    -z norelro -z now --hash-style=sysv --allow-shlib-undefined \
    --dynamic-linker /lib/ld-musl-x86_64.so.1 -o "$OUT" \
    "$MUSL_LIB/Scrt1.o" "$MUSL_LIB/crti.o" "$SMOKE_OBJ" "$EGL_OBJ" \
    --start-group "$UB/libb1gui.a" "$LIBM" \
    --end-group -L "$MUSL_LIB" -l:libc.so -L "$MESA_LIB" -lOSMesa \
    "$MUSL_LIB/crtn.o"
  "$STRIP" "$OUT"
  exit 0
fi

# Dynamic by default: links libc from the shared libc.so.1 via /lib/ld-b1nix.so
# (the d8 model). B1NIX_LINK=static restores the whole-archive static link.
if [ "${B1NIX_LINK:-dynamic}" = "static" ]; then
  DYN_CRT0="$UB/crt/crt0.o"; DYN_FLAGS=""; DYN_LIBC='--whole-archive '"$UB"'/libb1nix.a --no-whole-archive'
else
  DYN_CRT0="$UB/crt/crt0-dynamic.o"; DYN_FLAGS="--dynamic-linker /lib/ld-b1nix.so -z norelro --hash-style=sysv"; DYN_LIBC="$UB/libc.so.1"
fi
# Linker script: linker-cxx.ld (GCC/libgcc) vs linker-libcxx.ld (libc++/libunwind:
# maps the program headers + keeps .eh_frame_hdr for dl_iterate_phdr unwinding).
LINKER_LD="$ROOT_DIR/userspace/linker-cxx.ld"
if [ "${B1NIX_CXX_STDLIB:-}" = "libc++" ]; then
  LINKER_LD="$ROOT_DIR/userspace/linker-libcxx.ld"
  DYN_FLAGS="$DYN_FLAGS --eh-frame-hdr"
fi
# Use LLVM runtimes when available, else fall back to libgcc. With libc++ the
# unwinder is already folded into libc++abi.a (STDLIB_ABI_CROSS_A), so do NOT add
# the standalone libunwind.a too (duplicate _Unwind_*).
if [ -n "${LLVM_CRT_CROSS:-}" ]; then
  if [ "${B1NIX_CXX_STDLIB:-}" = "libc++" ]; then
    CRT_LIBS="$LLVM_CRT_CROSS"
  else
    CRT_LIBS="$LLVM_CRT_CROSS $LLVM_UNW_CROSS"
  fi
else
  CRT_LIBS="$LIBGCC_CROSS"
fi

# Link Mesa as shared library (-lOSMesa).
# shellcheck disable=SC2086
"$LD" -m "$LDEMU" -T "$LINKER_LD" --gc-sections --allow-shlib-undefined \
  --allow-multiple-definition $DYN_FLAGS -o "$OUT" \
  "$DYN_CRT0" "$SMOKE_OBJ" "$EGL_OBJ" \
  -L "$MESA_LIB" \
  --start-group \
  "$STDLIB_CROSS_A" "$STDLIB_ABI_CROSS_A" $CRT_LIBS "$LIBM" \
  --whole-archive "$UB/libb1gui.a" --no-whole-archive \
  --end-group -lOSMesa $DYN_LIBC

"$STRIP" "$OUT"
