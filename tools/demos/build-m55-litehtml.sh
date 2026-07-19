#!/bin/sh
# Build the M55 litehtml demo ELF: compile userspace/bin/m55_litehtml.cpp
# against the litehtml/gumbo headers and link it with the ported litehtml +
# gumbo static libs, the LLVM C++ runtime (libc++/libc++abi + compiler-rt), libm
# and libb1nix. This is the M55 acceptance binary (real C++ HTML/CSS engine
# running on b1nix). Arg 1 = output path. B1NIX_ARCH selects the toolchain/arch.
#
# M90: the default C++ runtime is GCC-free LLVM libc++. Set B1NIX_CXX_STDLIB=libstdc++
# to opt into the legacy GCC libstdc++ path.

set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?usage: build-m55-litehtml.sh <output-elf>}"
B1NIX_CXX_STDLIB="${B1NIX_CXX_STDLIB:-libc++}"; export B1NIX_CXX_STDLIB
. "$ROOT_DIR/tools/toolchain/env.sh"
resolve_cxx_cross

if [ "$B1NIX_ARCH" = "x86" ]; then LDEMU="elf_i386"; else LDEMU="elf_x86_64"; fi
LD="$(command -v ld.lld 2>/dev/null || echo /opt/homebrew/bin/ld.lld)"
STRIP="$(command -v llvm-strip 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-strip)"

LITEHTML="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/ports/build-litehtml.sh")"
LIBM="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/ports/build-openlibm.sh")/lib/libm.a"

UB="$ROOT_DIR/userspace/build/$B1NIX_ARCH"
MUSL_USR="$ROOT_DIR/build/musl-b1nix/$B1NIX_TRIPLET/install/usr"
# Dynamic by default: link the shared libstdc++.so.6 + shared libc.so.1 +
# shared libgcc_s.so (the cxx_smoke model). litehtml/gumbo stay statically folded
# (they are non-PIC) but resolve the C++ runtime from the .so. Cross-DSO C++
# exceptions work because every loaded module's .eh_frame is registered with
# libgcc (kernel section-header lookup + __b1nix_run_dso_init); crt0 registers the
# executable's frames via the real __register_frame (-u forces the import).
# B1NIX_LINK=static restores the historical fully-folded whole-archive link.
SHARED_LIBC="$UB/libc.so.1"
STDCXX_SO=""; SHARED_LIBGCC=""; CXXLIB_SO=""
CXX_COMPAT=""
# linker-cxx.ld (GCC/libgcc model) vs linker-libcxx.ld (libc++/libunwind: maps the
# program headers + keeps .eh_frame_hdr so libunwind unwinds via dl_iterate_phdr).
LINKER_LD="$ROOT_DIR/userspace/linker-cxx.ld"
[ "${B1NIX_CXX_STDLIB:-}" = "libc++" ] && LINKER_LD="$ROOT_DIR/userspace/linker-libcxx.ld"
if [ "${B1NIX_LINK:-dynamic}" = "static" ] || [ ! -f "$SHARED_LIBC" ]; then
  DYN_CRT0="$UB/crt/crt0.o"; DYN_FLAGS=""; DYN_LIBC="--whole-archive $UB/libb1nix.a --no-whole-archive"
else
  DYN_CRT0="$UB/crt/crt0-dynamic.o"
  DYN_FLAGS="-z norelro --hash-style=sysv --eh-frame-hdr --dynamic-linker /lib/ld-b1nix.so"
  DYN_LIBC="$SHARED_LIBC"
  if [ "${B1NIX_CXX_STDLIB:-}" = "libc++" ]; then
    # Shared LLVM libc++ runtime: libc++.so.1 -> libc++abi.so.1 (folds libunwind).
    # No -lgcc_s / __register_frame (that is the libgcc model); the static
    # libunwind is dropped too (LLVM_UNW_CROSS) so the unwinder is single-instance
    # in libc++abi.so.1. compiler-rt builtins stay static.
    _cxxsodir="$TOOLCHAIN_BUILD_HOME/cross/$B1NIX_TRIPLET/lib"
    if [ -f "$_cxxsodir/libc++.so.1" ] && [ -f "$_cxxsodir/libc++abi.so.1" ]; then
      CXXLIB_SO="$_cxxsodir/libc++.so.1 $_cxxsodir/libc++abi.so.1"
      STDLIB_CROSS_A=""; STDLIB_ABI_CROSS_A=""
      LLVM_UNW_CROSS=""
    fi
  else
    _scxx="$(dirname "$STDLIB_CROSS_A")/libstdc++.so.6"
    if [ -f "$_scxx" ]; then
      STDCXX_SO="$_scxx"
      SHARED_LIBGCC="-u __register_frame -L$(dirname "$_scxx") -lgcc_s"
      STDLIB_CROSS_A=""; STDLIB_ABI_CROSS_A=""  # provided by the shared object
    fi
  fi
fi
if [ "${LINK:-musl}" = musl ]; then
  make B1NIX_ARCH="$B1NIX_ARCH" -C "$ROOT_DIR/userspace" -s build/$B1NIX_ARCH/libcxx_compat.o 1>&2
  CXX_COMPAT="$UB/libcxx_compat.o"
  DYN_CRT0="$MUSL_USR/lib/Scrt1.o"
  DYN_FLAGS="-pie -z norelro --hash-style=sysv --eh-frame-hdr --allow-shlib-undefined --dynamic-linker /lib/ld-musl-x86_64.so.1"
  DYN_LIBC="$MUSL_USR/lib/crti.o -L$MUSL_USR/lib -l:libc.so $MUSL_USR/lib/crtn.o"
  CXXLIB_SO="$MUSL_USR/lib/libc++.so.1 $MUSL_USR/lib/libc++abi.so.1"
  STDLIB_CROSS_A=""
  STDLIB_ABI_CROSS_A=""
  LLVM_UNW_CROSS=""
  CXXFLAGS_CROSS="--target=$B1NIX_TRIPLET --sysroot=$TOOLCHAIN_BUILD_HOME/sysroot -nostdinc++ -isystem $TOOLCHAIN_BUILD_HOME/llvm-runtimes-build-musl/libcxx-install/include/c++/v1 -isystem $MUSL_USR/include -O2 -ffunction-sections -fdata-sections -fPIC -fPIE -Db1nix -D_GNU_SOURCE"
else
  make B1NIX_ARCH="$B1NIX_ARCH" -C "$ROOT_DIR/userspace" -s \
    "build/$B1NIX_ARCH/libb1nix.a" "$([ "${B1NIX_LINK:-dynamic}" = static ] && echo build/$B1NIX_ARCH/crt/crt0.o || echo build/$B1NIX_ARCH/crt/crt0-dynamic.o)" \
    "$([ "${B1NIX_LINK:-dynamic}" = static ] || echo build/$B1NIX_ARCH/libc.so.1)" 1>&2
fi

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
# $STDLIB_CROSS_A / $STDLIB_ABI_CROSS_A are empty in dynamic mode (the shared
# libstdc++.so.6 provides them) — left unquoted so they expand to nothing.
# $SHARED_LIBGCC (-lgcc_s) goes before the static-libgcc group so the shared
# unwinder wins; $STDCXX_SO (the shared libstdc++) goes after, before libc.
"$LD" -m "$LDEMU" -T "$LINKER_LD" --gc-sections \
  --allow-multiple-definition $DYN_FLAGS -o "$OUT" \
  "$DYN_CRT0" "$OBJ" "$CXX_COMPAT" $SHARED_LIBGCC \
  --start-group "$LITEHTML/lib/liblitehtml.a" "$LITEHTML/lib/libgumbo.a" \
  $STDLIB_CROSS_A $STDLIB_ABI_CROSS_A $CRT_LIBS "$LIBM" --end-group \
  $STDCXX_SO $CXXLIB_SO $DYN_LIBC

"$STRIP" "$OUT"

if [ "${LINK:-musl}" = musl ]; then
  python3 -c "import sys; f=sys.argv[1]; d=open(f,'rb').read(); d=d.replace(b'\\x00ld-musl-x86_64.so.1\\x00',b'\\x00libc.so\\x00'+b'\\x00'*12); open(f,'wb').write(d); h=open(f,'r+b'); h.seek(7); h.write(bytes([3])); h.close()" "$OUT"
fi
