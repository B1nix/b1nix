#!/bin/sh
# M64 Phase 2 compiler shim: drop the handful of flags that Chromium's //build
# config emits for its *bundled* clang fork but that a stock clang rejects, then
# exec the real compiler. Used as the cc/cxx for the V8 clang build.
#
#   v8-clang-filter.sh <real-clang> <args...>
#
# Keeps the arg list space-safe (rotates $@: pop each original arg, re-push the
# kept ones). Add to the denylist case below if a later file hits another
# bundled-clang-only flag.
real="$1"; shift
n=$#
i=0
while [ "$i" -lt "$n" ]; do
  a="$1"; shift; i=$((i + 1))
  case "$a" in
    -fdiagnostics-show-inlining-chain) ;;
    -fno-lifetime-dse) ;;
    -fsanitize-ignore-for-ubsan-feature=*) ;;
    *) set -- "$@" "$a" ;;
  esac
done
# The Chromium build's b1nix config emits GCC-only warning flags
# (-Wno-maybe-uninitialized, -Werror=changes-meaning, -Wno-class-memaccess, ...)
# that stock clang rejects under -Werror=unknown-warning-option. Injecting
# -Wno-unknown-warning-option makes clang silently ignore any flag it doesn't
# know, so we don't have to enumerate the whole GCC-only set.
exec "$real" -Wno-unknown-warning-option "$@"
