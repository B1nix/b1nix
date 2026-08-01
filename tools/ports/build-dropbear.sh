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
AUTOTOOLS_CONFIGURE="--enable-zlib --disable-pam --disable-syslog --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx --disable-loginfunc --disable-pututline --disable-pututxline --disable-harden"

port_pre_configure() {
  # Stage zlib for compression
  ZLIB_PREFIX="$("$ROOT_DIR/tools/ports/build-zlib.sh" 2>/dev/null | tail -n 1)"
  if [ -z "$ZLIB_PREFIX" ] || [ ! -f "$ZLIB_PREFIX/lib/libz.a" ]; then
    echo "tools/ports/build-dropbear.sh: zlib build failed" >&2
    exit 1
  fi
  export AUTOTOOLS_CONFIGURE="$AUTOTOOLS_CONFIGURE CPPFLAGS=-I$ZLIB_PREFIX/include LDFLAGS=-L$ZLIB_PREFIX/lib LIBS=-lz"
  # Write b1nix local options
  cat > "$SRC_DIR/localoptions.h" <<'EOF'
/* b1nix Dropbear build options (overrides default_options.h). */
#define DROPBEAR_SMALL_CODE 1
#define DO_HOST_LOOKUP 1
#define DROPBEAR_SYSLOG 0
#define DEBUG_TRACE 0
/* No utmp/wtmp/lastlog/PAM on b1nix. */
#define DROPBEAR_PASSWORD_AUTH 1
#define DROPBEAR_SVR_PASSWORD_AUTH 1
#define DROPBEAR_SVR_PUBKEY_AUTH 1
EOF
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
    # Makefile's own `LIBS+=@LIBS@` (which configure filled with -lz), so
    # keep -lz alongside the compiler-rt archive rather than dropping it.
    BUILTINS_LIB="$ROOT_DIR/build/${B1NIX_ARCH:-x86_64}/toolchain/llvm-runtimes-build/install/lib/libcompiler_rt.a"
    make -C "$BUILD_DIR" -j"${JOBS:-4}" \
      CC="$AUTOTOOLS_CC" AR="$AR_BIN" RANLIB="$RANLIB_BIN" \
      LIBS="-lz $BUILTINS_LIB" \
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
