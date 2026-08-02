#!/bin/sh
# Build Dropbear SSH for the b1nix userspace ABI.
# Uses the autotools driver (tools/ports/drivers/autotools.sh).
#
# Two phases, selected by the first argument:
#   crypto  — build only bundled libtomcrypt/libtommath archives
#   all     — (default) build full static dropbearmulti binary
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

PHASE="${1:-all}"

AUTOTOOLS_NAME=dropbear
AUTOTOOLS_VERSION="${DROPBEAR_VERSION:-2022.83}"
AUTOTOOLS_URL="https://matt.ucc.asn.au/dropbear/releases/dropbear-${AUTOTOOLS_VERSION}.tar.bz2"
AUTOTOOLS_TARBALL="dropbear-${AUTOTOOLS_VERSION}.tar.bz2"
# M104: PAM support via OpenPAM (tools/ports/build-openpam.sh). Dropbear's own
# --enable-pam just does an AC_CHECK_LIB(pam, pam_authenticate) link probe and
# an AC_CHECK_HEADERS(security/pam_appl.h) compile probe — both are link/compile
# -only (no execution), so they cross-compile fine once CPPFLAGS/LDFLAGS point
# at OpenPAM's install dir via --with-pam. DROPBEAR_SVR_PASSWORD_AUTH and
# DROPBEAR_SVR_PAM_AUTH are mutually exclusive per dropbear's own
# default_options.h comment ("You can't enable both PASSWORD and PAM") — PAM
# fully replaces the direct-crypt(3) path, it doesn't sit next to it.
AUTOTOOLS_CONFIGURE="--enable-zlib --enable-pam --disable-syslog --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx --disable-loginfunc --disable-pututline --disable-pututxline --disable-harden"

port_pre_configure() {
  # Stage zlib for compression
  ZLIB_PREFIX="$("$ROOT_DIR/tools/ports/build-zlib.sh" 2>/dev/null | tail -n 1)"
  if [ -z "$ZLIB_PREFIX" ] || [ ! -f "$ZLIB_PREFIX/lib/libz.a" ]; then
    echo "tools/ports/build-dropbear.sh: zlib build failed" >&2
    exit 1
  fi
  # Stage OpenPAM (libpam.so.2 + headers) — dropbear's --enable-pam probe and
  # its final link both need -lpam / <security/pam_appl.h> to resolve.
  OPENPAM_PREFIX="$("$ROOT_DIR/tools/ports/build-openpam.sh" 2>/dev/null | tail -n 1)"
  if [ -z "$OPENPAM_PREFIX" ] || [ ! -f "$OPENPAM_PREFIX/lib/libpam.so.2" ]; then
    echo "tools/ports/build-dropbear.sh: openpam build failed" >&2
    exit 1
  fi
  # Two -I/-L pairs (zlib + OpenPAM) can't be embedded as unquoted words in
  # AUTOTOOLS_CONFIGURE — port_configure below word-splits that string on
  # spaces, which would break "CPPFLAGS=-Ia -Ib" into two separate (invalid)
  # configure arguments. Export them as their own variables instead and have
  # port_configure pass CPPFLAGS=/LDFLAGS=/LIBS= as single quoted argv words.
  # Build options as plain -D, so nothing is written into the upstream source
  # tree. dropbear runs default_options.h through default_options_guard.h,
  # which wraps every default in #ifndef — so a -D on the command line wins,
  # exactly like localoptions.h would, without editing anything.
  #
  # Only these two are actually needed: DROPBEAR_SVR_PAM_AUTH defaults to 0 and
  # DROPBEAR_SVR_PASSWORD_AUTH to 1, and sysoptions.h #errors if both are on.
  # (The rest of what used to be written here either matched the upstream
  # default already — SMALL_CODE, DEBUG_TRACE, SVR_PUBKEY_AUTH — or named
  # macros dropbear 2022.83 does not have at all: DROPBEAR_PASSWORD_AUTH and
  # DROPBEAR_SYSLOG appear nowhere in its sources; syslog is switched off by
  # the --disable-syslog we already pass to configure.)
  DROPBEAR_OPTS="-DDROPBEAR_SVR_PASSWORD_AUTH=0 -DDROPBEAR_SVR_PAM_AUTH=1"
  export DROPBEAR_CPPFLAGS="-I$ZLIB_PREFIX/include -I$OPENPAM_PREFIX/include $DROPBEAR_OPTS"
  export DROPBEAR_LDFLAGS="-L$ZLIB_PREFIX/lib -L$OPENPAM_PREFIX/lib"
  export DROPBEAR_LIBS="-lz -lpam"
}

port_configure() {
  # Only configure once - config.h is stable
  if [ ! -f "$SRC_DIR/config.h" ]; then
    (
      cd "$BUILD_DIR"
      BUILD_TRIPLET="$("$SRC_DIR/config.guess" 2>/dev/null || echo "$(uname -m)-pc-linux-gnu")"
      export cross_compiling=yes
      "$SRC_DIR/configure" \
        --host="$B1NIX_TRIPLET" \
        --build="$BUILD_TRIPLET" \
        $AUTOTOOLS_CONFIGURE \
        CC="$AUTOTOOLS_CC" AR="$AR_BIN" RANLIB="$RANLIB_BIN" \
        "CPPFLAGS=${DROPBEAR_CPPFLAGS:-}" "LDFLAGS=${DROPBEAR_LDFLAGS:-}" "LIBS=${DROPBEAR_LIBS:-}" \
        1>&2
    )
  fi
}

port_build() {
  if [ "$PHASE" = "crypto" ]; then
    make -C "$BUILD_DIR" -j"${JOBS:-4}" \
      CC="$AUTOTOOLS_CC" AR="$AR_BIN" RANLIB="$RANLIB_BIN" \
      libtomcrypt/libtomcrypt.a libtommath/libtommath.a 1>&2
  else
    # curve25519/libtomcrypt bignum code needs compiler-rt's 128-bit division
    # helper (__udivti3) — without it the final dropbearmulti link fails (see
    # userspace/Makefile's m32_nettool/m53_httpsd rules and build-curl.sh for
    # the same requirement).
    # A `make LIBS=...` command-line override replaces (not appends to) the
    # Makefile's own `LIBS+=@LIBS@` (which configure filled with -lz -lpam),
    # so keep both alongside the compiler-rt archive rather than dropping
    # them — -lpam is M104's OpenPAM link (svr-authpam.c).
    BUILTINS_LIB="$ROOT_DIR/build/${B1NIX_ARCH:-x86_64}/toolchain/llvm-runtimes-build/install/lib/libcompiler_rt.a"
    OPENPAM_PREFIX="$("$ROOT_DIR/tools/ports/build-openpam.sh" 2>/dev/null | tail -n 1)"
    make -C "$BUILD_DIR" -j"${JOBS:-4}" \
      CC="$AUTOTOOLS_CC" AR="$AR_BIN" RANLIB="$RANLIB_BIN" \
      LIBS="-L$OPENPAM_PREFIX/lib -lz -lpam $BUILTINS_LIB" \
      PROGRAMS="dropbear dbclient dropbearkey dropbearconvert" \
      MULTI=1 dropbearmulti 1>&2
  fi
}

port_install() {
  if [ "$PHASE" = "crypto" ]; then
    mkdir -p "$INSTALL_DIR/lib"
    cp "$SRC_DIR/libtomcrypt/libtomcrypt.a" "$INSTALL_DIR/lib/"
    cp "$SRC_DIR/libtommath/libtommath.a" "$INSTALL_DIR/lib/"
  fi
}

# The caller needs the out-of-tree build directory (not the install prefix),
# so suppress the generic driver's default path and print the correct one below.
AUTOTOOLS_ECHO=""

. "$ROOT_DIR/tools/ports/drivers/autotools.sh"

# For crypto phase, print src dir instead of install dir
if [ "$PHASE" = "crypto" ]; then
  echo "$SRC_DIR"
else
  # The generic driver configures/builds out-of-tree; expose the actual build
  # directory so callers can consume dropbearmulti without guessing its path.
  echo "$BUILD_DIR"
fi
