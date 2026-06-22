#!/bin/sh
# Make the cross GCC's libstdc++ usable against the b1nix libc.
#
# The cross toolchain ships a prebuilt libstdc++, but it was configured against
# an empty sysroot, so:
#   1. the b1nix libc headers are not in the toolchain's target include dir, and
#   2. libstdc++'s config recorded "no mbstate_t in libc" (_GLIBCXX_HAVE_MBSTATE_T
#      undefined) — which now collides with b1nix's wchar.h that *does* define it.
#
# This script repairs an EXISTING toolchain (no rebuild) so hosted C++ (std::*,
# exceptions) compiles and links into a b1nix ELF. It is idempotent and runs per
# triplet. build-toolchain.sh can also call it after the toolchain is built.
#
# Link recipe for a b1nix C++ binary (see tools/toolchain/bin/b1nix-c++):
#   ld crt0.o objs --start-group libstdc++.a libsupc++.a libgcc.a \
#                  --whole-archive libb1nix.a --no-whole-archive --end-group

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

enable_one() {
  triplet="$1"
  cross="$ROOT_DIR/build/toolchain_build/$triplet/cross"
  gxx="$cross/bin/$triplet-g++"
  [ -x "$gxx" ] || { echo "enable-cxx: $triplet: no cross g++, skipping" >&2; return 0; }

  # copy_if_changed: only copy when content differs, so unchanged headers keep
  # their mtime. This script runs on every Mesa/port build (build-mesa.sh calls
  # it); an unconditional cp would bump header mtimes each time and force ninja
  # to recompile the whole port. ponytail: cmp -s is cheap vs a full rebuild.
  copy_if_changed() {
    cmp -s "$1" "$2" 2>/dev/null && return 0
    mkdir -p "$(dirname "$2")"
    cp "$1" "$2"
  }

  # 1. Stage b1nix libc headers into the toolchain's target include dir so
  #    libstdc++ headers (#include_next <stdlib.h>, <wchar.h>, ...) resolve.
  tgtinc="$cross/$triplet/include"
  mkdir -p "$tgtinc"
  ( cd "$ROOT_DIR/userspace/include" && find . -type f ) | while read -r rel; do
    copy_if_changed "$ROOT_DIR/userspace/include/$rel" "$tgtinc/$rel"
  done

  # 1b. GCC's fixincludes baked stale copies of a few b1nix headers into
  #     include-fixed (assert/stddef/stdio/stdlib/wchar). clang searches
  #     include-fixed before the target include dir, so it sees the stale ones
  #     and misses source fixes (e.g. the C++ wchar_t guard). Refresh every
  #     fixed header that has a b1nix source counterpart; leave GCC's own files.
  for fixed in "$cross"/lib/gcc/"$triplet"/*/include-fixed; do
    [ -d "$fixed" ] || continue
    for f in "$fixed"/*.h; do
      [ -e "$f" ] || continue
      src="$ROOT_DIR/userspace/include/$(basename "$f")"
      [ -f "$src" ] && copy_if_changed "$src" "$f"
    done
  done

  # 2. Tell libstdc++ the libc now provides mbstate_t (its stale config said
  #    otherwise, causing a conflicting typedef against b1nix's wchar.h).
  cfg=$(ls "$cross"/*/include/c++/*/"$triplet"/bits/c++config.h 2>/dev/null | head -1 || true)
  if [ -n "$cfg" ]; then
    if grep -q '/\* #undef _GLIBCXX_HAVE_MBSTATE_T \*/' "$cfg"; then
      sed -i.bak 's|/\* #undef _GLIBCXX_HAVE_MBSTATE_T \*/|#define _GLIBCXX_HAVE_MBSTATE_T 1|' "$cfg"
    fi
    # 3. Enable libstdc++ threading (std::mutex / call_once). The toolchain was
    #    built --disable-threads (gthr-default == gthr-single), so select the
    #    posix gthr model and turn on _GLIBCXX_HAS_GTHREADS. b1nix's pthread is
    #    complete enough for gthr-posix's header-only path. build-toolchain.sh
    #    sets --enable-threads=posix so a fresh full build does this natively.
    bits=$(dirname "$cfg")
    if [ -f "$bits/gthr-posix.h" ] && ! grep -q '_GLIBCXX_GCC_GTHR_POSIX_H' "$bits/gthr-default.h" 2>/dev/null; then
      cp "$bits/gthr-posix.h" "$bits/gthr-default.h"
    fi
    if grep -q '/\* #undef _GLIBCXX_HAS_GTHREADS \*/' "$cfg"; then
      sed -i.bak 's|/\* #undef _GLIBCXX_HAS_GTHREADS \*/|#define _GLIBCXX_HAS_GTHREADS 1|' "$cfg"
    fi
    # 4. The toolchain was configured against an empty sysroot, so libstdc++
    #    recorded "libc has no C99 math/stdint/fenv" and #if's std::log2,
    #    std::isfinite, std::mt19937 (needs <stdint> fast types), etc. out of
    #    <cmath>/<cstdint>. b1nix's libc/libm (openlibm) now provide them, so
    #    enable the TR1 feature macros (b1nix's math.h is C99-complete, and its
    #    classification macros are C++-guarded so std::isfinite resolves). Needed
    #    by C++ ports like libjxl; harmless for others (a declared-but-unused fn
    #    only fails if it is actually linked).
    for m in _GLIBCXX_USE_C99_MATH_TR1 _GLIBCXX_USE_C99_STDINT_TR1 \
             _GLIBCXX_USE_C99_FENV_TR1 _GLIBCXX_USE_C99 \
             _GLIBCXX11_USE_C99_MATH _GLIBCXX98_USE_C99_MATH \
             _GLIBCXX11_USE_C99_STDLIB _GLIBCXX98_USE_C99_STDLIB; do
      if grep -q "/\* #undef $m \*/" "$cfg"; then
        sed -i.bak "s|/\* #undef $m \*/|#define $m 1|" "$cfg"
      fi
    done
  fi
  rm -f "$cross"/*/include/c++/*/"$triplet"/bits/c++config.h.bak 2>/dev/null || true
  echo "enable-cxx: $triplet ready" >&2
}

# Unquoted on purpose: the default must word-split into two triplets.
# shellcheck disable=SC2086
for t in ${*:-x86_64-b1nix i686-b1nix}; do
  enable_one "$t"
done
