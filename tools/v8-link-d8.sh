#!/bin/sh
# Link d8 as a b1nix ELF from the gn-compiled objects.
#
# gn's own link step uses the plain g++ driver with an empty sysroot (no crt0,
# no libc) and produces a glibc-shaped binary — wrong for b1nix. This relinks
# the exact object set gn computed (out/b1nix/d8.rsp) with the b1nix C++ recipe
# from tools/b1nix-c++: crt0.o + linker-cxx.ld + the libstdc++/libsupc++/libgcc
# group + openlibm + whole-archived libb1nix.a, wrapped so inter-archive
# circular refs resolve.
#
# Run after: ninja -C out/b1nix d8  (which compiles everything; its link step
# fails on crt0 — that's expected, the .o/.rsp are what we use).
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TC="$ROOT_DIR/build/toolchain_build/x86_64-b1nix/cross/bin"
OUT="$ROOT_DIR/build/toolchain_build/v8-skeleton/v8/out/b1nix"
GXX="$TC/x86_64-b1nix-g++"
LD="$TC/x86_64-b1nix-ld"
RANLIB="$TC/x86_64-b1nix-ranlib"

CRT0="$ROOT_DIR/userspace/build/x86_64/crt/crt0.o"
LINKER="$ROOT_DIR/userspace/linker-cxx.ld"
LIBB1="$ROOT_DIR/userspace/build/x86_64/libb1nix.a"
LIBM="$ROOT_DIR/build/openlibm-b1nix/x86_64-b1nix/install/lib/libm.a"
LIBSTDCXX="$("$GXX" -print-file-name=libstdc++.a)"
LIBSUPCXX="$("$GXX" -print-file-name=libsupc++.a)"
LIBGCC="$("$GXX" -print-libgcc-file-name)"

[ -f "$OUT/d8.rsp" ] || { echo "missing $OUT/d8.rsp — run 'ninja -C out/b1nix d8' first"; exit 1; }
[ -f "$CRT0" ] || { echo "missing crt0.o — build userspace (B1NIX_ARCH=x86_64)"; exit 1; }

# gn uses thin archives created with `ar -D` and no symbol index; ld needs one.
echo "ranlib'ing archives..."
find "$OUT" -name '*.a' -exec "$RANLIB" {} \; 2>/dev/null || true

echo "linking d8..."
cd "$OUT"
"$LD" -m elf_x86_64 -T "$LINKER" --gc-sections --allow-multiple-definition \
  -o d8.b1nix \
  "$CRT0" --start-group @d8.rsp --end-group \
  --start-group "$LIBSTDCXX" "$LIBSUPCXX" "$LIBGCC" "$LIBM" \
  --whole-archive "$LIBB1" --no-whole-archive --end-group

echo "=== linked: $OUT/d8.b1nix ==="
"$TC/x86_64-b1nix-readelf" -h d8.b1nix | grep -E 'Class|Machine|Type|Entry'
ls -la d8.b1nix
