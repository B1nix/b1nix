#!/bin/sh
# Build openlibm (BSD libm) as a static libm.a for the b1nix userspace ABI.
# Userspace uses hardware SSE doubles, so this is an ordinary freestanding
# compile of openlibm's portable C sources — no soft-float, no autotools.
# Prints the install dir on stdout.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
OPENLIBM_VERSION="${OPENLIBM_VERSION:-0.8.1}"
TARBALL="openlibm-${OPENLIBM_VERSION}.tar.gz"
URL="https://github.com/JuliaMath/openlibm/archive/refs/tags/v${OPENLIBM_VERSION}.tar.gz"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

. "$ROOT_DIR/tools/toolchain/env.sh"

# Portable in-place sed: GNU sed wants `sed -i EXPR`, BSD/macOS sed wants
# `sed -i '' EXPR`. Passing BSD's empty-suffix arg to GNU sed makes it treat the
# expression as a filename ("can't read s/.../...").
if sed --version >/dev/null 2>&1; then
  sed_inplace() { sed -i "$@"; }
else
  sed_inplace() { sed -i '' "$@"; }
fi

SRC_PARENT="$ROOT_DIR/build/openlibm-src"
SRC_DIR="$SRC_PARENT/openlibm-${OPENLIBM_VERSION}"
BUILD_DIR="$ROOT_DIR/build/openlibm-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
INSTALL_DIR="$BUILD_DIR/install"

mkdir -p "$SRC_PARENT" "$OBJ_DIR" "$INSTALL_DIR/include" "$INSTALL_DIR/lib"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$TARBALL"
  [ -f "$tmp" ] || curl -L "$URL" -o "$tmp" 1>&2
  tar -xzf "$tmp" -C "$SRC_PARENT" 1>&2
fi

# Patch idempotently (each sed is a no-op once applied) and OUTSIDE the
# extraction guard, so a source tree left over from a previously failed run
# still gets patched. b1nix's libc already defines ldexp/frexp (used by binaries
# that don't link libm). openlibm exports ldexp as a *strong* alias of scalbn,
# which collides. Make it weak so the libc definition wins when both archives
# are linked.
sed_inplace 's/openlibm_strong_reference(scalbn, ldexp)/openlibm_weak_reference(scalbn, ldexp)/' \
  "$SRC_DIR/src/s_scalbn.c"
# Same for frexp (a real definition, not an alias): make it weak so the
# libc frexp wins when both archives are linked.
sed_inplace 's/^frexp(double x, int \*eptr)/__attribute__((__weak__)) frexp(double x, int *eptr)/' \
  "$SRC_DIR/src/s_frexp.c"

if [ "$B1NIX_ARCH" = "x86" ]; then
  TARGET="i686-unknown-elf"
  OLM_ARCH="i387"
else
  TARGET="x86_64-unknown-elf"
  OLM_ARCH="amd64"
fi

CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fPIC -fno-strict-aliasing -Db1nix -Wno-implicit-function-declaration
  -D__BSD_VISIBLE=1
  -I$SRC_DIR -I$SRC_DIR/src -I$SRC_DIR/include -I$SRC_DIR/$OLM_ARCH
  -I$SRC_DIR/bsdsrc"

# Portable C sources. The per-arch dir (amd64/i387) is x87/SSE fenv + .S that
# assumes a hosted ABI; b1nix doesn't expose fenv, so skip it and rely on the
# generic rounding the C routines do themselves.
OBJS=""
compile_dir() {
  for src in "$SRC_DIR/$1"/*.c; do
    [ -e "$src" ] || continue
    base=$(basename "$src" .c)
    # Skip long-double (*l) routines: they pull ld80/ld128 arch headers
    # (invtrig.h, fpmath) and b1nix graphics libs only use double/float.
    case "$base" in s_ceil) ;; *l) continue;; esac
    # Skip Bessel (j0/j1/jn/y0/y1) and gamma routines: they need signgam/j0
    # globals b1nix's math.h omits, and no graphics lib uses them.
    case "$base" in
      *gamma*|e_j0|e_j1|e_jn|e_y0|e_y1|w_j0|w_j1|w_jn|w_y0|w_y1) continue;;
    esac
    # Skip complex-number routines: they need <complex.h> which b1nix lacks,
    # and no graphics lib uses complex math. Detect by the include, so real
    # routines (cos/ceil/cbrt/copysign) are not caught by name. EXCEPT k_exp/
    # k_expf: they define __ldexp_exp/__ldexp_expf (the real scaled exp that
    # cosh/sinh/__ldexp need) alongside the complex __ldexp_cexp — and they only
    # pull openlibm_complex.h, which works with GCC's built-in _Complex.
    case "$base" in
      k_exp|k_expf) ;;
      *) if grep -q "complex.h" "$src"; then continue; fi;;
    esac
    obj="$OBJ_DIR/$1_$base.o"
    clang $CFLAGS -c "$src" -o "$obj"
    OBJS="$OBJS $obj"
  done
}

compile_dir src
compile_dir bsdsrc

# shellcheck disable=SC2086
"$AR_BIN" rcs "$INSTALL_DIR/lib/libm.a" $OBJS
cp "$SRC_DIR/include/openlibm_math.h" "$SRC_DIR/include"/*.h "$INSTALL_DIR/include/" 2>/dev/null || true

echo "$INSTALL_DIR"
