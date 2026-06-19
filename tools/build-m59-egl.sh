#!/bin/sh
# Build the M59 EGL smoke ELF: compile userspace/bin/m59_smoke.c together with
# the OSMesa-backed EGL implementation (userspace/libegl/b1egl_mesa.c) against
# the Mesa OSMesa headers, and link it with the ported Mesa static libraries,
# the b1gui client, libstdc++/libsupc++/libgcc, libm and libb1nix. This is the
# same link shape as the M52 OSMesa demo (Mesa is a C++ codebase), but the GL is
# now reached through the standard egl* API instead of the OSMesa* API directly.
# Arg 1 = output path. B1NIX_ARCH selects the toolchain/arch.

set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:?usage: build-m59-egl.sh <output-elf>}"
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

OUTDIR="$(dirname "$OUT")"
mkdir -p "$OUTDIR"
SMOKE_OBJ="$OUTDIR/m59_smoke.o"
EGL_OBJ="$OUTDIR/b1egl_mesa.o"

CFLAGS="-O2 -ffunction-sections -fdata-sections -Db1nix \
  -nostdinc -isystem $ROOT_DIR/userspace/include -I $MESA/include"

# shellcheck disable=SC2086
"$GCC" $CFLAGS -c "$ROOT_DIR/userspace/bin/m59_smoke.c" -o "$SMOKE_OBJ"
# shellcheck disable=SC2086
"$GCC" $CFLAGS -c "$ROOT_DIR/userspace/libegl/b1egl_mesa.c" -o "$EGL_OBJ"

# shellcheck disable=SC2046
"$LD" -m "$LDEMU" -T "$ROOT_DIR/userspace/linker-cxx.ld" --gc-sections \
  --allow-multiple-definition -o "$OUT" \
  "$UB/crt/crt0.o" "$SMOKE_OBJ" "$EGL_OBJ" "$MESA/lib/osmesa_target.o" \
  --start-group $(ls "$MESA"/lib/*.a) "$LIBSTDCXX" "$LIBSUPCXX" "$LIBGCC" "$LIBM" \
  --whole-archive "$UB/libb1gui.a" "$UB/libb1nix.a" --no-whole-archive --end-group

"$STRIP" "$OUT"
