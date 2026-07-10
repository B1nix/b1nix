#!/bin/sh
# Build static libpsl for the b1nix userspace ABI.
# Uses the autotools driver (tools/ports/drivers/autotools.sh).
#
# libpsl is built with --disable-runtime (no IDNA library dependency) so the
# only external dependencies are the b1nix libc itself. The Public Suffix List
# data is compiled in as a DAFSA (--enable-builtin) using the upstream Python
# script; wget uses the built-in list at runtime.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

AUTOTOOLS_NAME=libpsl
AUTOTOOLS_VERSION="${LIBPSL_VERSION:-0.21.5}"
AUTOTOOLS_URL="https://distfiles.gentoo.org/distfiles/d1/libpsl-${AUTOTOOLS_VERSION}.tar.gz"
AUTOTOOLS_CONFIGURE="--disable-shared --enable-static --disable-runtime --enable-builtin --disable-nls --disable-man --without-psl-distfile"
PATCHES="libpsl/b1nix-config-sub.sh"

# Disable autotools maintainer-mode rebuild rules (absent on host).
AUTOGEN_OFF="ACLOCAL=: AUTOCONF=: AUTOMAKE=: AUTOHEADER=: MAKEINFO=: am__maybe_remake_makefiles="

port_pre_configure() {
  PYTHON_BIN="${PYTHON:-$(command -v python3 2>/dev/null || command -v python 2>/dev/null || true)}"
  if [ -z "$PYTHON_BIN" ]; then
    echo "build-libpsl.sh: need host python3 (or set PYTHON=/path/to/python)" >&2
    exit 1
  fi
  # Recreate build dir to avoid stale state
  rm -rf "$BUILD_DIR"
  mkdir -p "$BUILD_DIR"
}

port_build() {
  # Generate the builtin PSL DAFSA header directly with Python (avoids going
  # through make which can trigger automake reconstruction).
  PSL_DAT="$SRC_DIR/list/public_suffix_list.dat"
  DAFSA_H="$BUILD_DIR/src/suffixes_dafsa.h"
  PYTHON_BIN="${PYTHON_BIN:-python3}"
  "$PYTHON_BIN" "$SRC_DIR/src/psl-make-dafsa" --output-format=cxx+ "$PSL_DAT" "$DAFSA_H" 1>&2
  # Build only the library (src/ subdirectory — that's where libpsl.la lives).
  # shellcheck disable=SC2086
  make -C "$BUILD_DIR/src" -j"${JOBS:-4}" $AUTOGEN_OFF libpsl.la 1>&2
}

port_install() {
  # shellcheck disable=SC2086
  make -C "$BUILD_DIR/src" $AUTOGEN_OFF install-libLTLIBRARIES 1>&2
  # shellcheck disable=SC2086
  make -C "$BUILD_DIR/include" $AUTOGEN_OFF install 1>&2
}

. "$ROOT_DIR/tools/ports/drivers/autotools.sh"
