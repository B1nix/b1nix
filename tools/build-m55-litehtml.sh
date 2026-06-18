#!/bin/sh
# Build the M55 litehtml demo ELF: compile userspace/bin/m55_litehtml.cpp
# against the litehtml/gumbo headers and link it with the ported litehtml +
# gumbo static libs, the cross GCC's libstdc++/libsupc++/libgcc, libm and
# libb1nix. This is the M55 acceptance binary (real C++ HTML/CSS engine running
# on b1nix). Arg 1 = output path. B1NIX_ARCH selects the toolchain/arch.

set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:?usage: build-m55-litehtml.sh <output-elf>}"
. "$ROOT_DIR/tools/toolchain-env.sh"

if [ "$B1NIX_ARCH" = "x86" ]; then LDEMU="elf_i386"; else LDEMU="elf_x86_64"; fi
CROSS="$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/cross"
GXX="$CROSS/bin/$B1NIX_TRIPLET-g++"
LD="$(command -v ld.lld 2>/dev/null || echo /opt/homebrew/bin/ld.lld)"
STRIP="$(command -v llvm-strip 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-strip)"

LITEHTML="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/build-litehtml.sh")"
LIBM="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/build-openlibm.sh")/lib/libm.a"
LIBSTDCXX="$("$GXX" -print-file-name=libstdc++.a)"
LIBSUPCXX="$("$GXX" -print-file-name=libsupc++.a)"
LIBGCC="$("$GXX" -print-libgcc-file-name)"

UB="$ROOT_DIR/userspace/build/$B1NIX_ARCH"
make B1NIX_ARCH="$B1NIX_ARCH" -C "$ROOT_DIR/userspace" -s \
  "build/$B1NIX_ARCH/libb1nix.a" "build/$B1NIX_ARCH/crt/crt0.o" 1>&2

OBJ="$(dirname "$OUT")/m55_litehtml.o"
mkdir -p "$(dirname "$OUT")"
"$GXX" -O2 -std=c++17 -ffunction-sections -fdata-sections -Db1nix \
  -I "$LITEHTML/include" \
  -c "$ROOT_DIR/userspace/bin/m55_litehtml.cpp" -o "$OBJ"

# litehtml + gumbo + the C++ runtime + libm all go in one --start-group so the
# cyclic static-archive references (litehtml<->gumbo<->libstdc++<->libm) resolve.
"$LD" -m "$LDEMU" -T "$ROOT_DIR/userspace/linker-cxx.ld" --gc-sections \
  --allow-multiple-definition -o "$OUT" \
  "$UB/crt/crt0.o" "$OBJ" \
  --start-group "$LITEHTML/lib/liblitehtml.a" "$LITEHTML/lib/libgumbo.a" \
  "$LIBSTDCXX" "$LIBSUPCXX" "$LIBGCC" "$LIBM" \
  --whole-archive "$UB/libb1nix.a" --no-whole-archive --end-group

"$STRIP" "$OUT"
