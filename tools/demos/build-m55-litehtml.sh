#!/bin/sh
# Build the M55 litehtml demo ELF: compile userspace/bin/m55_litehtml.cpp
# against the litehtml/gumbo headers and link it with the ported litehtml +
# gumbo static libs, the cross GCC's libstdc++/libsupc++/libgcc, libm and
# libb1nix. This is the M55 acceptance binary (real C++ HTML/CSS engine running
# on b1nix). Arg 1 = output path. B1NIX_ARCH selects the toolchain/arch.

set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?usage: build-m55-litehtml.sh <output-elf>}"
. "$ROOT_DIR/tools/toolchain/env.sh"
resolve_cxx_cross

if [ "$B1NIX_ARCH" = "x86" ]; then LDEMU="elf_i386"; else LDEMU="elf_x86_64"; fi
LD="$(command -v ld.lld 2>/dev/null || echo /opt/homebrew/bin/ld.lld)"
STRIP="$(command -v llvm-strip 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-strip)"

LITEHTML="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/ports/build-litehtml.sh")"
LIBM="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/ports/build-openlibm.sh")/lib/libm.a"

UB="$ROOT_DIR/userspace/build/$B1NIX_ARCH"
make B1NIX_ARCH="$B1NIX_ARCH" -C "$ROOT_DIR/userspace" -s \
  "build/$B1NIX_ARCH/libb1nix.a" "build/$B1NIX_ARCH/crt/crt0.o" 1>&2

OBJ="$(dirname "$OUT")/m55_litehtml.o"
mkdir -p "$(dirname "$OUT")"
# shellcheck disable=SC2086
"$CXX_CROSS" $CXXFLAGS_CROSS -std=c++17 \
  -I "$LITEHTML/include" \
  -c "$ROOT_DIR/userspace/bin/m55_litehtml.cpp" -o "$OBJ"

# Use LLVM runtimes when available, else fall back to libgcc
if [ -n "${LLVM_CRT_CROSS:-}" ]; then
  CRT_LIBS="$LLVM_CRT_CROSS $LLVM_UNW_CROSS"
else
  CRT_LIBS="$LIBGCC_CROSS"
fi

# litehtml + gumbo + the C++ runtime + libm all go in one --start-group so the
# cyclic static-archive references (litehtml<->gumbo<->libstdc++<->libm) resolve.
"$LD" -m "$LDEMU" -T "$ROOT_DIR/userspace/linker-cxx.ld" --gc-sections \
  --allow-multiple-definition -o "$OUT" \
  "$UB/crt/crt0.o" "$OBJ" \
  --start-group "$LITEHTML/lib/liblitehtml.a" "$LITEHTML/lib/libgumbo.a" \
  "$STDLIB_CROSS_A" "$STDLIB_ABI_CROSS_A" $CRT_LIBS "$LIBM" \
  --whole-archive "$UB/libb1nix.a" --no-whole-archive --end-group

"$STRIP" "$OUT"
