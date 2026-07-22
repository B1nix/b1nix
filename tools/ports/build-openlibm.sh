#!/bin/sh
# Build openlibm (BSD libm) as a static libm.a for the b1nix userspace ABI.
# Userspace uses hardware SSE doubles, so this is an ordinary freestanding
# compile of openlibm's portable C sources — no soft-float, no autotools. Prints
# the install dir.
#
# Build logic lives in tools/ports/drivers/cport.sh; this is the manifest.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

CPORT_NAME=openlibm
CPORT_ARCHIVE=libm.a
CPORT_SRC_PARENT="$ROOT_DIR/build/src/openlibm"
VER="${OPENLIBM_VERSION:-0.8.1}"
CPORT_SRCNAME="openlibm-${VER}"
CPORT_TARBALL="openlibm-${VER}.tar.gz"
CPORT_URL="https://github.com/JuliaMath/openlibm/archive/refs/tags/v${VER}.tar.gz"
# b1nix libc already defines ldexp/frexp; make openlibm's collide weakly.
PATCHES="openlibm/b1nix-weak-aliases.sh"

port_pre_build() {
  if [ "$B1NIX_ARCH" = "x86" ]; then OLM_ARCH="i387"; else OLM_ARCH="amd64"; fi
  CPORT_CFLAGS="-fPIC -Wno-implicit-function-declaration -D__BSD_VISIBLE=1 \
-I$SRC_DIR -I$SRC_DIR/src -I$SRC_DIR/include -I$SRC_DIR/$OLM_ARCH -I$SRC_DIR/bsdsrc"
}

# Portable C sources. The per-arch dir (amd64/i387) is x87/SSE fenv + .S that
# assumes a hosted ABI; b1nix doesn't expose fenv, so skip it and rely on the
# generic rounding the C routines do themselves.
port_build() {
  compile_dir() {
    for src in "$SRC_DIR/$1"/*.c; do
      [ -e "$src" ] || continue
      base=$(basename "$src" .c)
      # Skip long-double (*l) routines: they pull ld80/ld128 arch headers.
      case "$base" in s_ceil|s_creal|s_isnormal) ;; *l) continue;; esac
      # Skip Bessel (j0/j1/jn/y0/y1) and gamma routines: need signgam/j0 globals.
      case "$base" in
        *gamma*|e_j0|e_j1|e_jn|e_y0|e_y1|w_j0|w_j1|w_jn|w_y0|w_y1) continue;;
      esac
      # Skip complex routines (need <complex.h>, b1nix lacks it) — EXCEPT k_exp/
      # k_expf (define the real __ldexp_exp/__ldexp_expf cosh/sinh need) and the
      # trivial creal/cimag/conj. Detect by include so real routines are kept.
      case "$base" in
        k_exp|k_expf|s_creal|s_cimag|s_conj) ;;
        *) if grep -q "complex.h" "$src"; then continue; fi;;
      esac
      cport_cc "$src"
    done
  }
  compile_dir src
  compile_dir bsdsrc
}

port_install_headers() {
  cp "$SRC_DIR/include/"*.h "$INSTALL_DIR/include/" 2>/dev/null || true
}

. "$ROOT_DIR/tools/ports/drivers/cport.sh"
