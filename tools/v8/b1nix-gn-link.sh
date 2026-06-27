#!/bin/sh
# b1nix gn LINK shim — the proper close for "gn toolchain can't link" (Option A).
#
# gn's link/solink rules (build/toolchain/gcc_toolchain.gni) drive `$ld` with the
# host clang++ defaults, which resolves against /usr/lib/libc.so.6 and fails on the
# errno TLS-vs-non-TLS mismatch. Set `ld = <this script>` in the b1nix gn toolchain
# (build/toolchain/b1nix/BUILD.gn) so gn invokes us instead: we re-issue the link
# with the b1nix cross `ld` + the proven d8 recipe (crt0 + b1nix linker scripts +
# libc++/libgcc/libm + whole-archived libb1nix, in one --start-group). This lets
# `ninja content_shell` link the .so's AND the executable as b1nix ELFs natively,
# and makes the standalone chromium-link.sh crutch unnecessary (that script is
# archived under archive/tools/v8/chromium-link.sh).
#
# Invoked by gn as the link driver, e.g.:
#   solink: <shim> -shared -Wl,-soname="lib.so" <ldflags> -o "./lib.so" @"lib.so.rsp" <rlibs>
#   link:   <shim> <ldflags> -o "./exe" -Wl,--start-group @"exe.rsp" -Wl,--end-group <solibs> <libs> -Wl,--start-group <rlibs> -Wl,--end-group
# Only -shared, -o, -Wl,-soname=, -Wl,--version-script=, @rsp, *.so (solibs) and
# *.rlib (rlibs) are meaningful; clang-driver host ldflags/-l libs are dropped.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
TC="$ROOT_DIR/build/toolchain_build/x86_64-b1nix/cross/bin"
LD="$TC/x86_64-b1nix-ld"
RANLIB="$TC/x86_64-b1nix-ranlib"
GXX="$TC/x86_64-b1nix-g++"
CC_CLANG="${B1NIX_CLANG:-$(command -v /opt/homebrew/opt/llvm/bin/clang 2>/dev/null || command -v clang 2>/dev/null || echo "$TC/x86_64-b1nix-gcc")}"

CRT0="$ROOT_DIR/userspace/build/x86_64/crt/crt0.o"
LINKER_EXE="$ROOT_DIR/userspace/linker-cxx.ld"
LINKER_SO="$ROOT_DIR/userspace/linker_shared_cxx.ld"
LIBB1="$ROOT_DIR/userspace/build/x86_64/libb1nix.a"
LIBC_SO="$ROOT_DIR/userspace/build/x86_64/libc.so.1"
LIBM="$ROOT_DIR/build/openlibm-b1nix/x86_64-b1nix/install/lib/libm.a"
LIBGCC="$("$GXX" -print-libgcc-file-name)"

# --- parse the gn-issued link line ------------------------------------------
mode=exe; out=; rsp=; soname=; verscript=; rlibs=; solibs=
while [ $# -gt 0 ]; do
  case "$1" in
    -shared)                  mode=so ;;
    -o)                       shift; out="$1" ;;
    -Wl,-soname=*)            soname="${1#-Wl,-soname=}" ;;
    -Wl,--version-script=*)   verscript="${1#-Wl,--version-script=}" ;;
    @*)                       rsp="${1#@}" ;;
    *.rlib)                   rlibs="$rlibs $1" ;;
    *.so|*.so.[0-9]*)         solibs="$solibs $1" ;;
    *)                        : ;;   # drop host ldflags / -l libs / group markers
  esac
  shift
done
soname="$(printf '%s' "$soname" | tr -d '"')"   # gn quotes the soname

[ -n "$out" ] || { echo "b1nix-gn-link: no -o output" >&2; exit 1; }
[ -n "$rsp" ] || { echo "b1nix-gn-link: no @rspfile" >&2; exit 1; }

# gn emits thin archives (ar -D) with no symbol index; ld needs one. ranlib the
# whole tree once per build (gated by a stamp so the per-edge invocations don't
# each walk ~2k archives).
STAMP=".b1nix_ranlib.stamp"
if [ ! -f "$STAMP" ] || [ -n "$(find . -name '*.a' -newer "$STAMP" -print -quit 2>/dev/null)" ]; then
  find . -name '*.a' -exec "$RANLIB" {} \; 2>/dev/null || true
  : > "$STAMP"
fi

# powl/long-double math the freestanding libb1nix can't carry (whole-archived into
# binaries that don't link libm); compile the thin wrapper once for the executable.
MATHL_O=".b1nix_mathl.o"
[ -f "$MATHL_O" ] || "$CC_CLANG" --target=x86_64-unknown-elf -x c -ffreestanding -fno-builtin \
  -fno-stack-protector -nostdinc -isystem "$ROOT_DIR/userspace/include" \
  -c "$ROOT_DIR/userspace/libc/mathl.c" -o "$MATHL_O"

# Strip clang-driver-only bits from the gn rsp so raw ld can consume it:
#  - drop -Wl, prefixes (the rsp's --whole-archive/--no-whole-archive survive raw)
#  - drop EVERY host -l<lib> (glib/gcc_s/rt/atomic/...): b1nix provides its own
#    runtime via the explicit lib group below, so none of the host -l are wanted.
clean="$out.b1nix.rsp"
sed -e 's/-Wl,//g' \
    -e 's/\(^\| \)-l[A-Za-z0-9._+-]*/ /g' "$rsp" > "$clean"

if [ "$mode" = so ]; then
  # Shared objects need PIC inputs; the static libb1nix/libgcc/libm are NOT PIC
  # (built for static exes → R_X86_64_32). These .so's are dlopen'd into
  # content_shell, which already has all of libb1nix statically, so leave libc/
  # libgcc symbols undefined here (--allow-shlib-undefined) and link the PIC shared
  # libc.so.1 for the dynamic-import side. Only the .so's own PIC objects + rlibs.
  set -- -m elf_x86_64 -shared -z norelro --hash-style=sysv \
         --allow-multiple-definition --allow-shlib-undefined
  [ -n "$soname" ]    && set -- "$@" -soname "$soname"
  [ -n "$verscript" ] && set -- "$@" --version-script="$verscript"
  exec "$LD" "$@" -T "$LINKER_SO" -o "$out" \
    --start-group @"$clean" $rlibs $solibs --end-group "$LIBC_SO"
else
  exec "$LD" -m elf_x86_64 -T "$LINKER_EXE" --gc-sections --allow-multiple-definition \
    -o "$out" "$CRT0" \
    --start-group @"$clean" $rlibs $solibs "$MATHL_O" "$LIBGCC" "$LIBM" \
    --whole-archive "$LIBB1" --no-whole-archive --end-group
fi
