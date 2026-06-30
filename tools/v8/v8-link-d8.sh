#!/bin/sh
# Link d8 as a b1nix ELF from the gn-compiled objects.
#
# gn's own link step uses the plain g++ driver with an empty sysroot (no crt0,
# no libc) and produces a glibc-shaped binary — wrong for b1nix. This relinks
# the exact object set gn computed (out/b1nix/d8.rsp) with the b1nix C++ recipe
# from tools/toolchain/bin/b1nix-c++: crt0.o + linker-cxx.ld + the libstdc++/libsupc++/libgcc
# group + openlibm + whole-archived libb1nix.a, wrapped so inter-archive
# circular refs resolve.
#
# Run after: ninja -C out/b1nix d8  (which compiles everything; its link step
# fails on crt0 — that's expected, the .o/.rsp are what we use).
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
TC="$ROOT_DIR/build/toolchain_build/x86_64-b1nix/cross/bin"
# Out dir defaults to the jitless build; pass "b1nix-jit" (or any out/<name>) to
# relink the JIT d8:  sh tools/v8/v8-link-d8.sh b1nix-jit
OUT_NAME="${1:-b1nix}"
OUT="$ROOT_DIR/build/toolchain_build/v8-skeleton/v8/out/$OUT_NAME"
GXX="$TC/x86_64-b1nix-g++"
CC_CLANG="${B1NIX_CLANG:-$(command -v /opt/homebrew/opt/llvm/bin/clang 2>/dev/null || command -v clang 2>/dev/null || echo "$TC/x86_64-b1nix-gcc")}"
LD="$TC/x86_64-b1nix-ld"
RANLIB="$TC/x86_64-b1nix-ranlib"

CRT0="$ROOT_DIR/userspace/build/x86_64/crt/crt0.o"
LINKER="$ROOT_DIR/userspace/linker-cxx.ld"
LIBB1="$ROOT_DIR/userspace/build/x86_64/libb1nix.a"
LIBM="$ROOT_DIR/build/openlibm-b1nix/x86_64-b1nix/install/lib/libm.a"
# B1NIX_LINK=dynamic links d8 against the shared libc.so.1 via the in-kernel
# loader (interpreter /lib/ld-b1nix.so) instead of whole-archiving libb1nix.a —
# matches the default-ISO port flip. Default stays static for back-compat.
DYNCRT0="$ROOT_DIR/userspace/build/x86_64/crt/crt0-dynamic.o"
SHARED_LIBC="$ROOT_DIR/userspace/build/x86_64/libc.so.1"
B1NIX_LINK="${B1NIX_LINK:-dynamic}"
LIBGCC="$("$GXX" -print-libgcc-file-name)"
# C++ runtime selection. SHARED_LIBGCC/STDCXX_SO are appended to the link below
# and are empty for the static paths, so those links are unchanged:
#  - clang+libc++ build (use_custom_libcxx): link the in-tree libc++.a/libc++abi.a
#    statically (V8 vendors libc++; there is no shared libc++.so).
#  - GCC libstdc++, dynamic link, shared .so present (DEFAULT): resolve libstdc++
#    from the shared libstdc++.so.6 + the single shared unwinder libgcc_s.so (the
#    cxx_smoke/litehtml model). Static libgcc.a stays in the group for the
#    compiler builtins (__multi3 etc.) that libgcc_s.so does not export; -lgcc_s
#    placed first makes the shared _Unwind_* win. Cross-DSO frames are registered
#    by the kernel loader + __b1nix_run_dso_init. Set B1NIX_CXX=static to fold.
#  - otherwise: fold GCC libstdc++.a/libsupc++.a statically.
LIBCXX_A="$OUT/obj/buildtools/third_party/libc++/libc++.a"
LIBCXXABI_A="$OUT/obj/buildtools/third_party/libc++abi/libc++abi.a"
SHARED_LIBGCC=""; STDCXX_SO=""
if [ -f "$LIBCXX_A" ]; then
  CXXRT="$LIBCXX_A $LIBCXXABI_A"
  echo "  C++ runtime: libc++ (built in-tree, static)"
else
  _scxxdir="$(dirname "$("$GXX" -print-file-name=libstdc++.a)")"
  if [ "$B1NIX_LINK" = "dynamic" ] && [ "${B1NIX_CXX:-shared}" != "static" ] && [ -f "$_scxxdir/libstdc++.so.6" ]; then
    CXXRT=""
    STDCXX_SO="$_scxxdir/libstdc++.so.6"
    SHARED_LIBGCC="-u __register_frame -L$_scxxdir -lgcc_s"
    echo "  C++ runtime: GCC libstdc++.so.6 (shared) + libgcc_s.so (shared unwinder)"
  else
    CXXRT="$("$GXX" -print-file-name=libstdc++.a) $("$GXX" -print-file-name=libsupc++.a)"
    echo "  C++ runtime: GCC libstdc++ (static)"
  fi
fi

# powl: d8 needs the long-double pow (openlibm lacks it). Compile the thin
# wrapper HERE rather than shipping it in libb1nix — libb1nix is whole-archived
# into the freestanding userspace binaries, which don't link libm, so it must
# stay self-contained. Here it resolves `pow` from libm, which d8 links.
MATHL_O="$OUT/mathl-b1nix.o"
"$CC_CLANG" --target=x86_64-unknown-elf -x c -ffreestanding -fno-builtin -fno-stack-protector \
  -nostdinc -isystem "$ROOT_DIR/userspace/include" -c "$ROOT_DIR/userspace/libc/mathl.c" -o "$MATHL_O"

[ -f "$OUT/d8.rsp" ] || { echo "missing $OUT/d8.rsp — run 'ninja -C out/b1nix d8' first"; exit 1; }
[ -f "$CRT0" ] || { echo "missing crt0.o — build userspace (B1NIX_ARCH=x86_64)"; exit 1; }

# gn uses thin archives created with `ar -D` and no symbol index; ld needs one.
echo "ranlib'ing archives..."
find "$OUT" -name '*.a' -exec "$RANLIB" {} \; 2>/dev/null || true

echo "linking d8..."
cd "$OUT"

# Rust-enabled builds (e.g. v8_enable_temporal_support -> the temporal_capi
# crate): GN's link rule appends the rust rlib set via a separate `rlibs =`
# binding in obj/d8.ninja, plus the b1nix rust std in prebuilt_rustc_sysroot
# (via ldflags) — NEITHER is in d8.rsp. Without them the V8 C++ that calls
# temporal_capi leaves ~558 `temporal_rs_*` C symbols undefined. Pull both into
# the link group. Empty for a non-rust build, so that path is unchanged.
RUST_RLIBS=""
[ -f obj/d8.ninja ] && RUST_RLIBS="$(grep -E '^[[:space:]]*rlibs =' obj/d8.ninja | head -1 | sed 's/^[[:space:]]*rlibs = //')"
RUST_STD=""
[ -d prebuilt_rustc_sysroot/lib/rustlib/x86_64-unknown-b1nix/lib ] && \
  RUST_STD="$(ls prebuilt_rustc_sysroot/lib/rustlib/x86_64-unknown-b1nix/lib/*.rlib 2>/dev/null | tr '\n' ' ')"
[ -n "$RUST_RLIBS" ] && echo "  + rust: $(printf '%s' "$RUST_RLIBS" | wc -w) app rlibs + $(printf '%s' "$RUST_STD" | wc -w) std rlibs"

# One big --start-group so the C++<->rust<->rust-std<->libc<->libstdc++ circular
# references all resolve (rust rlibs need rust std; rust std needs libb1nix libc).
if [ "$B1NIX_LINK" = "dynamic" ]; then
  echo "  link model: DYNAMIC (shared libc.so.1 via /lib/ld-b1nix.so)"
  # libc is now a shared dependency outside the group; the C++/rust/rust-std
  # archives still resolve their libc references as dynamic imports against it.
  # $SHARED_LIBGCC (-lgcc_s) precedes the group so the shared unwinder wins;
  # $STDCXX_SO (libstdc++.so.6) follows it, before libc. Both empty in static-C++.
  "$LD" -m elf_x86_64 -T "$LINKER" --gc-sections --allow-multiple-definition \
    --dynamic-linker /lib/ld-b1nix.so -z norelro --hash-style=sysv --eh-frame-hdr \
    -o d8.b1nix \
    "$DYNCRT0" $SHARED_LIBGCC \
    --start-group @d8.rsp $RUST_RLIBS $RUST_STD \
    "$MATHL_O" $CXXRT "$LIBGCC" "$LIBM" --end-group \
    $STDCXX_SO "$SHARED_LIBC"
else
  "$LD" -m elf_x86_64 -T "$LINKER" --gc-sections --allow-multiple-definition \
    -o d8.b1nix \
    "$CRT0" \
    --start-group @d8.rsp $RUST_RLIBS $RUST_STD \
    "$MATHL_O" $CXXRT "$LIBGCC" "$LIBM" \
    --whole-archive "$LIBB1" --no-whole-archive --end-group
fi

echo "=== linked: $OUT/d8.b1nix ==="
"$TC/x86_64-b1nix-readelf" -h d8.b1nix | grep -E 'Class|Machine|Type|Entry'
ls -la d8.b1nix
