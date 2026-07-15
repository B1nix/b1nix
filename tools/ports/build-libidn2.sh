#!/bin/sh
# Build static libidn2 for the b1nix userspace ABI.
# Uses the autotools driver (tools/ports/drivers/autotools.sh).
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

AUTOTOOLS_NAME=libidn2
AUTOTOOLS_VERSION="${LIBIDN2_VERSION:-2.3.7}"
AUTOTOOLS_URL="https://ftp.gnu.org/gnu/libidn/libidn2-${AUTOTOOLS_VERSION}.tar.gz"
AUTOTOOLS_CONFIGURE="--disable-shared --enable-static --disable-nls --disable-doc --disable-rpath --with-included-libunistring=no"
AUTOTOOLS_SENTINEL="configure"
PATCHES="libidn2/b1nix-config.sh"

port_pre_configure() {
  UNISTR_PREFIX="$ROOT_DIR/build/libunistring-b1nix/$B1NIX_TRIPLET/install"
  if [ ! -f "$UNISTR_PREFIX/lib/libunistring.a" ]; then
    "$ROOT_DIR/tools/ports/build-libunistring.sh" >/dev/null
  fi
  # Recreate build dir contents to avoid stale state from interrupted builds
  rm -rf "$BUILD_DIR"/*
  # Extend configure flags with libunistring paths and cache overrides
  # shellcheck disable=SC2016
  AUTOTOOLS_CONFIGURE="$AUTOTOOLS_CONFIGURE \
    --with-libunistring-prefix=$UNISTR_PREFIX \
    CPPFLAGS=-I$UNISTR_PREFIX/include \
    LDFLAGS=-L$UNISTR_PREFIX/lib \
    LIBS=-lunistring \
    ac_cv_func_strchrnul=yes \
    ac_cv_have_decl_strchrnul=yes \
    gl_cv_onwards_func_strchrnul=yes \
    ac_cv_func_strverscmp=yes \
    ac_cv_func_rawmemchr=yes \
    ac_cv_func_getline=yes \
    ac_cv_func_getdelim=yes \
    ac_cv_func_getdtablesize=yes \
    ac_cv_func_basename=yes \
    ac_cv_func_strerror=yes"
}

port_build() {
  # Single-job: this cross tree is sensitive to jobserver propagation.
  # Prevent building the idn2 CLI tool (links gnulib's strchrnul which
  # conflicts with libb1nix.a's whole-archive'd copy).
  make -C "$BUILD_DIR" -j1 1>&2
}

port_install() {
  make -C "$BUILD_DIR/lib" install-libLTLIBRARIES 1>&2
  make -C "$BUILD_DIR/lib" install-includeHEADERS 1>&2
}

port_post_install() {
  # Strip gnulib replacement objects from libidn2.a that duplicate symbols
  # already provided by libb1nix.a (when linked --whole-archive).
  LIBA="$INSTALL_DIR/lib/libidn2.a"
  CONFLICTS="rawmemchr.o strerror.o strerror-override.o"
  for obj in $CONFLICTS; do
    "$AR_BIN" d "$LIBA" "libgnu_la-$obj" 2>/dev/null || true
    "$AR_BIN" d "$LIBA" "libunistring_la-$obj" 2>/dev/null || true
  done
  "$AR_BIN" d "$LIBA" "libgnu_la-basename-lgpl.o" 2>/dev/null || true

  # Self-heal rpl_strverscmp if undefined
  NM_BIN="${NM:-$(command -v llvm-nm 2>/dev/null || echo llvm-nm)}"
  if "$NM_BIN" "$LIBA" 2>/dev/null | grep -q 'U rpl_strverscmp' &&
     ! "$NM_BIN" "$LIBA" 2>/dev/null | grep -qiE ' [tw] rpl_strverscmp'; then
    AUTOTOOLS_CC="${AUTOTOOLS_CC:-$ROOT_DIR/tools/toolchain/bin/b1nix-autotools-cc}"
    "$AUTOTOOLS_CC" -O2 -c "$ROOT_DIR/tools/patches/libidn2/rpl_strverscmp.c" \
      -o "$BUILD_DIR/.rpl_strverscmp.o"
    "$AR_BIN" r "$LIBA" "$BUILD_DIR/.rpl_strverscmp.o"
  fi
}

. "$ROOT_DIR/tools/ports/drivers/autotools.sh"
