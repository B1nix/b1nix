#!/bin/sh
# tools/patches/openlibm/b1nix-weak-aliases.sh — applied by build-openlibm.sh via
# the port driver PATCHES= mechanism (runs as `<this> <srcdir>`).
#
# b1nix's libc already defines ldexp/frexp (used by binaries that don't link
# libm). openlibm exports ldexp as a *strong* alias of scalbn and defines frexp
# outright, which collide when both archives link. Make both weak so the libc
# definition wins. Idempotent (each sed is a no-op once applied).
set -eu
SRC_DIR="${1:?usage: b1nix-weak-aliases.sh <openlibm-src-dir>}"

# GNU vs BSD sed in-place portability.
if sed --version >/dev/null 2>&1; then sed_inplace() { sed -i "$@"; }
else sed_inplace() { sed -i '' "$@"; }; fi

sed_inplace 's/openlibm_strong_reference(scalbn, ldexp)/openlibm_weak_reference(scalbn, ldexp)/' \
  "$SRC_DIR/src/s_scalbn.c"
sed_inplace 's/^frexp(double x, int \*eptr)/__attribute__((__weak__)) frexp(double x, int *eptr)/' \
  "$SRC_DIR/src/s_frexp.c"
