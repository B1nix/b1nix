#!/bin/sh
# Build an M52 Mesa demo ELF against the
# Mesa OSMesa headers and link it with the ported Mesa static libraries, the
# b1gui client, the LLVM C++ runtime (libc++/libc++abi + compiler-rt), libm and
# libb1nix. --gc-sections + strip keep the (still large) binary as small as Mesa
# allows. Arg 1 is the source stem, arg 2 is the output path.
#
# M90: default C++ runtime is GCC-free LLVM libc++ (matches Mesa, now libc++).
# Set B1NIX_CXX_STDLIB=libstdc++ to opt into the legacy GCC path.

set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
DEMO="${1:?usage: build-m52-mesa-demo.sh <source-stem> <output-elf>}"
OUT="${2:?usage: build-m52-mesa-demo.sh <source-stem> <output-elf>}"
B1NIX_CXX_STDLIB="${B1NIX_CXX_STDLIB:-libc++}"; export B1NIX_CXX_STDLIB
. "$ROOT_DIR/tools/toolchain/env.sh"
resolve_cxx_cross

if [ "$B1NIX_ARCH" = "x86" ]; then LDEMU="elf_i386"; else LDEMU="elf_x86_64"; fi
CROSS="$TOOLCHAIN_BUILD_HOME/cross"
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
make B1NIX_ARCH="$B1NIX_ARCH" LINK=musl -C "$ROOT_DIR/userspace" -s \
  "build/$B1NIX_ARCH/libb1gui.a" 1>&2

OBJ="$(dirname "$OUT")/$DEMO.o"
mkdir -p "$(dirname "$OUT")"
CC_CROSS="${B1NIX_CC:-$(command -v clang 2>/dev/null || echo "$CROSS/bin/$B1NIX_TRIPLET-cc")}"
CC_RES="$("$CC_CROSS" -print-resource-dir 2>/dev/null || true)"
# shellcheck disable=SC2086
"$CC_CROSS" --target="$B1NIX_TRIPLET" -O2 -fPIC -ffunction-sections -fdata-sections -Db1nix \
  -nostdinc ${CC_RES:+-isystem "$CC_RES/include"} -isystem "$ROOT_DIR/build/$B1NIX_ARCH/ports/musl/install/include" -idirafter "$ROOT_DIR/userspace/include" -I "$MESA/include" \
  -c "$ROOT_DIR/userspace/bin/$DEMO.c" -o "$OBJ"

# Link the executable against the real musl dynamic loader and libc.
MUSL_LIB="$ROOT_DIR/build/$B1NIX_ARCH/ports/musl/install/lib"
BUILTINS_LIB="$(clang -print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a"
DYN_CRT0="$MUSL_LIB/Scrt1.o"
DYN_FLAGS="-pie -z norelro --hash-style=sysv --dynamic-linker /lib/ld-musl-x86_64.so.1 -L$MUSL_LIB"
DYN_LIBC="$MUSL_LIB/crti.o -lc $MUSL_LIB/crtn.o $BUILTINS_LIB"
# Linker script: linker-cxx.ld (GCC/libgcc) vs linker-libcxx.ld (libc++/libunwind:
# maps the program headers + keeps .eh_frame_hdr so libunwind unwinds via
# dl_iterate_phdr — required even for the statically-folded libc++ in this exe).
LINKER_ARGS=""
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

# Link Mesa as shared library (-lOSMesa) — the kernel mounts rootfs.img at
# /mnt/root in test mode so the dynamic linker finds libOSMesa.so.8 at runtime.
MESA_LIB="$ROOT_DIR/build/$B1NIX_ARCH/ports/mesa/install/lib"
# shellcheck disable=SC2046,SC2086
# libOSMesa.so carries a few undefined symbols (e.g. `errno` referenced by Mesa's
# flex-generated GLSL lexer) that b1nix's dynamic linker resolves/ignores at
# runtime — mirroring the -Wl,--unresolved-symbols=ignore-all the Mesa shared
# libs are built with. ld.lld defaults to --no-allow-shlib-undefined when linking
# an executable, which would reject those; allow them so the demo links as it did
# before (the same libOSMesa.so already passes the M52 runtime tests).
"$LD" -m "$LDEMU" $LINKER_ARGS --gc-sections --allow-shlib-undefined \
  --allow-multiple-definition $DYN_FLAGS -o "$OUT" \
  "$DYN_CRT0" "$OBJ" \
  -L "$MESA_LIB" \
  --start-group \
  "$STDLIB_CROSS_A" "$STDLIB_ABI_CROSS_A" \
  $CRT_LIBS "$LIBM" \
  --whole-archive "$UB/libb1gui.a" --no-whole-archive \
  --end-group -lOSMesa $LLVM_LINK $DYN_LIBC

"$STRIP" "$OUT"
