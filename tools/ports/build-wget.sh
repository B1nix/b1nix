#!/bin/sh
# Build upstream GNU wget for the b1nix userspace ABI.
# Uses the autotools driver (tools/ports/drivers/autotools.sh).
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

B1NIX_TLS="${B1NIX_TLS:-none}"
if [ "$B1NIX_TLS" != "none" ]; then
  echo "build-wget.sh: TLS provider '$B1NIX_TLS' requested, but wget TLS wiring is not enabled yet; building HTTP-only wget." >&2
fi

AUTOTOOLS_NAME=wget
AUTOTOOLS_VERSION="${WGET_VERSION:-1.21.4}"
AUTOTOOLS_URL="https://ftpmirror.gnu.org/wget/wget-${AUTOTOOLS_VERSION}.tar.gz"
PATCHES="wget/b1nix-config-sub.sh wget/b1nix-gnulib.sh"

port_pre_configure() {
  HOST_TRIPLET="$B1NIX_TRIPLET"

  # Stage dependencies
  PCRE2_PREFIX="$ROOT_DIR/build/$B1NIX_ARCH/ports/pcre2/install"
  if [ ! -f "$PCRE2_PREFIX/lib/libpcre2-8.a" ]; then
    if ! "$ROOT_DIR/tools/ports/build-pcre2.sh" >/dev/null; then
      echo "build-wget.sh: PCRE2 build failed" >&2; exit 1
    fi
  fi

  OPENSSL_PREFIX="$ROOT_DIR/build/$B1NIX_ARCH/ports/openssl/install"
  if [ ! -f "$OPENSSL_PREFIX/lib/libssl.a" ]; then
    if ! "$ROOT_DIR/tools/ports/build-openssl.sh" >/dev/null; then
      echo "build-wget.sh: OpenSSL build failed" >&2; exit 1
    fi
  fi

  LIBIDN2_PREFIX="$ROOT_DIR/build/$B1NIX_ARCH/ports/libidn2/install"
  LIBUNISTRING_PREFIX="$ROOT_DIR/build/$B1NIX_ARCH/ports/libunistring/install"
  if [ ! -f "$LIBIDN2_PREFIX/lib/libidn2.a" ]; then
    if ! "$ROOT_DIR/tools/ports/build-libidn2.sh" >/dev/null; then
      echo "build-wget.sh: libidn2 build failed" >&2; exit 1
    fi
  fi

  LIBPSL_PREFIX="$ROOT_DIR/build/$B1NIX_ARCH/ports/libpsl/install"
  if [ ! -f "$LIBPSL_PREFIX/lib/libpsl.a" ]; then
    if ! "$ROOT_DIR/tools/ports/build-libpsl.sh" >/dev/null; then
      echo "build-wget.sh: libpsl build failed" >&2; exit 1
    fi
  fi

  ZLIB_PREFIX="$("$ROOT_DIR/tools/ports/build-zlib.sh" 2>/dev/null | tail -n 1)"
  if [ -z "$ZLIB_PREFIX" ] || [ ! -f "$ZLIB_PREFIX/lib/libz.a" ]; then
    echo "build-wget.sh: zlib build failed" >&2; exit 1
  fi

  # Export paths for port_configure
  export WGET_PCRE2_PREFIX="$PCRE2_PREFIX"
  export WGET_OPENSSL_PREFIX="$OPENSSL_PREFIX"
  export WGET_LIBIDN2_PREFIX="$LIBIDN2_PREFIX"
  export WGET_LIBUNISTRING_PREFIX="$LIBUNISTRING_PREFIX"
  export WGET_LIBPSL_PREFIX="$LIBPSL_PREFIX"
  export WGET_ZLIB_PREFIX="$ZLIB_PREFIX"
}

port_configure() {
  (
    cd "$BUILD_DIR"
    BUILD_TRIPLET="$("$SRC_DIR/config.guess" 2>/dev/null || echo "$(uname -m)-pc-linux-gnu")"
    export cross_compiling=yes
    "$SRC_DIR/configure" \
      --host="$B1NIX_TRIPLET" \
      --build="$BUILD_TRIPLET" \
      --prefix="$INSTALL_DIR" \
      --disable-maintainer-mode \
      --disable-shared --enable-static \
      --with-ssl=openssl \
      --with-zlib \
      --with-libpsl \
      --enable-iri \
      --disable-pcre \
      --enable-threads=posix \
      --disable-nls \
      --enable-ipv6 \
      CC="$AUTOTOOLS_CC" AR="$AR_BIN" RANLIB="$RANLIB_BIN" \
      CPPFLAGS="-I$WGET_ZLIB_PREFIX/include" \
      LDFLAGS="-L$WGET_ZLIB_PREFIX/lib" \
      gl_cv_func_getpass_good=yes \
      gl_cv_func_working_mktime=yes \
      ac_cv_func_timegm=yes \
      "PCRE2_CFLAGS=-I$WGET_PCRE2_PREFIX/include" \
      "PCRE2_LIBS=-L$WGET_PCRE2_PREFIX/lib -lpcre2-8" \
      "OPENSSL_CFLAGS=-I$WGET_OPENSSL_PREFIX/include" \
      "OPENSSL_LIBS=-L$WGET_OPENSSL_PREFIX/lib -lssl -lcrypto" \
      "LIBIDN2_CFLAGS=-I$WGET_LIBIDN2_PREFIX/include -I$WGET_LIBUNISTRING_PREFIX/include" \
      "LIBIDN2_LIBS=-L$WGET_LIBIDN2_PREFIX/lib -lidn2 -L$WGET_LIBUNISTRING_PREFIX/lib -lunistring" \
      "LIBPSL_CFLAGS=-I$WGET_LIBPSL_PREFIX/include" \
      "LIBPSL_LIBS=-L$WGET_LIBPSL_PREFIX/lib -lpsl" \
      1>&2
  )
}

port_build() {
  # wget bundles gnulib, which provides its own strndup/strpbrk/strcasecmp etc.
  # b1nix-autotools-cc whole-archives libb1nix.a (also defines these), so the
  # final link sees duplicates. Tolerate them.
  export B1NIX_LD_EXTRA="--allow-multiple-definition"

  # Neutralise autotools maintainer-mode rebuild rules for gnulib
  ACLOCAL=":" AUTOCONF=":" AUTOMAKE=":" AUTOHEADER=":" MAKEINFO=":" \
    make -C "$BUILD_DIR/lib" -j"${JOBS:-4}" 1>&2
  ACLOCAL=":" AUTOCONF=":" AUTOMAKE=":" AUTOHEADER=":" MAKEINFO=":" \
    make -C "$BUILD_DIR/src" -j"${JOBS:-4}" wget 1>&2
}

port_install() {
  mkdir -p "$INSTALL_DIR/bin"
  cp "$BUILD_DIR/src/wget" "$INSTALL_DIR/bin/"
}

. "$ROOT_DIR/tools/ports/drivers/autotools.sh"
