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
# Link recipe for a b1nix C++ binary (see tools/b1nix-c++):
#   ld crt0.o objs --start-group libstdc++.a libsupc++.a libgcc.a \
#                  --whole-archive libb1nix.a --no-whole-archive --end-group

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

enable_one() {
  triplet="$1"
  cross="$ROOT_DIR/build/toolchain_build/$triplet/cross"
  gxx="$cross/bin/$triplet-g++"
  [ -x "$gxx" ] || { echo "enable-cxx: $triplet: no cross g++, skipping" >&2; return 0; }

  # 1. Stage b1nix libc headers into the toolchain's target include dir so
  #    libstdc++ headers (#include_next <stdlib.h>, <wchar.h>, ...) resolve.
  tgtinc="$cross/$triplet/include"
  mkdir -p "$tgtinc"
  cp -R "$ROOT_DIR/userspace/include/." "$tgtinc/"

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
  fi
  rm -f "$cross"/*/include/c++/*/"$triplet"/bits/c++config.h.bak 2>/dev/null || true
  echo "enable-cxx: $triplet ready" >&2
}

# Unquoted on purpose: the default must word-split into two triplets.
# shellcheck disable=SC2086
for t in ${*:-x86_64-b1nix i686-b1nix}; do
  enable_one "$t"
done
