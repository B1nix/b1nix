#!/bin/sh
# Build upstream curl for the b1nix userspace ABI.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
CURL_VERSION="${CURL_VERSION:-8.20.0}"
CURL_TARBALL="curl-${CURL_VERSION}.tar.gz"
CURL_URL="https://curl.se/download/${CURL_TARBALL}"
WRAP="$ROOT_DIR/tools/toolchain/bin/b1nix-autotools-cc"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"
RANLIB_BIN="${RANLIB:-$(command -v llvm-ranlib 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ranlib)}"
# Per-architecture build identity (B1NIX_ARCH -> triplet).
. "$ROOT_DIR/tools/toolchain/env.sh"
HOST_TRIPLET="$B1NIX_TRIPLET"
# Pin the arch the wrapper compiles for to the triplet we're building, and
# export it so every sub-make / libtool invocation that runs b1nix-autotools-cc
# sees it (otherwise a leaked CC=clang or a scrubbed env makes the wrapper fall
# back to its x86_64 default — curl then compiles x86_64 and fails to link the
# i686 mbedTLS archives with "skipping incompatible").
case "$HOST_TRIPLET" in
  i686*) B1NIX_ARCH=x86 ;;
  *)     B1NIX_ARCH=x86_64 ;;
esac
export B1NIX_ARCH
# Per-triplet source tree + build dir so x86 and x86_64 never share objects.
SRC_PARENT="$ROOT_DIR/build/curl-src"
SRC_DIR="$SRC_PARENT/$HOST_TRIPLET/curl-${CURL_VERSION}"
BUILD_DIR="$ROOT_DIR/build/curl-b1nix/$HOST_TRIPLET"

SSL_FLAGS="--without-ssl"
TLS_CPPFLAGS=""
TLS_LDFLAGS=""
TLS_LIBS=""
if [ "$B1NIX_TLS" = "mbedtls" ]; then
  # Capture the full output so a build failure is not masked by the pipe;
  # the install prefix is the final stdout line (build noise goes to stderr).
  if ! MBEDTLS_OUT="$("$ROOT_DIR/tools/ports/build-mbedtls.sh")"; then
    echo "tools/ports/build-curl.sh: mbedTLS build failed" >&2
    exit 1
  fi
  MBEDTLS_PREFIX="$(printf '%s\n' "$MBEDTLS_OUT" | tail -n 1)"
  SSL_FLAGS="--with-mbedtls=$MBEDTLS_PREFIX"
  TLS_CPPFLAGS="-I$MBEDTLS_PREFIX/include"
  TLS_LDFLAGS="-L$MBEDTLS_PREFIX/lib"
  TLS_LIBS="-lmbedx509 -lmbedcrypto"
fi

# zlib: enable curl's Content-Encoding (gzip) support against the ported zlib so
# clients like NetSurf can request compressed transfers (no half-measures).
ZLIB_FLAG="--without-zlib"
if ZLIB_OUT="$("$ROOT_DIR/tools/ports/build-zlib.sh")"; then
  ZLIB_PREFIX="$(printf '%s\n' "$ZLIB_OUT" | tail -n 1)"
  ZLIB_FLAG="--with-zlib=$ZLIB_PREFIX"
  TLS_CPPFLAGS="$TLS_CPPFLAGS -I$ZLIB_PREFIX/include"
  TLS_LDFLAGS="$TLS_LDFLAGS -L$ZLIB_PREFIX/lib"
  TLS_LIBS="$TLS_LIBS -lz"
fi

# Brotli decoder: enable curl's "Content-Encoding: br" decompression (common on
# the modern web; NetSurf fetches through curl). Decoder-only static libs.
BROTLI_PREFIX="$ROOT_DIR/build/brotli-b1nix/$HOST_TRIPLET/install"
BROTLI_FLAG="--without-brotli"
if [ ! -f "$BROTLI_PREFIX/lib/libbrotlidec.a" ] || [ ! -f "$BROTLI_PREFIX/lib/libbrotlicommon.a" ]; then
  "$ROOT_DIR/tools/ports/build-brotli.sh" >/dev/null || true
fi
if [ -f "$BROTLI_PREFIX/lib/libbrotlidec.a" ]; then
  BROTLI_FLAG="--with-brotli=$BROTLI_PREFIX"
  TLS_CPPFLAGS="$TLS_CPPFLAGS -I$BROTLI_PREFIX/include"
  TLS_LDFLAGS="$TLS_LDFLAGS -L$BROTLI_PREFIX/lib"
  TLS_LIBS="$TLS_LIBS -lbrotlidec -lbrotlicommon"
fi

LIBUNISTRING_PREFIX="$ROOT_DIR/build/libunistring-b1nix/$HOST_TRIPLET/install"
if [ ! -f "$LIBUNISTRING_PREFIX/lib/libunistring.a" ]; then
  "$ROOT_DIR/tools/ports/build-libunistring.sh" >/dev/null
fi

LIBIDN2_PREFIX="$ROOT_DIR/build/libidn2-b1nix/$HOST_TRIPLET/install"
if [ ! -f "$LIBIDN2_PREFIX/lib/libidn2.a" ]; then
  "$ROOT_DIR/tools/ports/build-libidn2.sh" >/dev/null
fi

LIBPSL_PREFIX="$ROOT_DIR/build/libpsl-b1nix/$HOST_TRIPLET/install"
if [ ! -f "$LIBPSL_PREFIX/lib/libpsl.a" ]; then
  "$ROOT_DIR/tools/ports/build-libpsl.sh" >/dev/null
fi

TLS_CPPFLAGS="$TLS_CPPFLAGS -I$LIBPSL_PREFIX/include -I$LIBIDN2_PREFIX/include -I$LIBUNISTRING_PREFIX/include"
TLS_LDFLAGS="$TLS_LDFLAGS -L$LIBPSL_PREFIX/lib -L$LIBIDN2_PREFIX/lib -L$LIBUNISTRING_PREFIX/lib"
TLS_LIBS="$TLS_LIBS -lpsl -lidn2 -lunistring"

mkdir -p "$SRC_PARENT/$HOST_TRIPLET" "$BUILD_DIR"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/${CURL_TARBALL}"
  if [ ! -f "$tmp" ]; then
    if command -v curl >/dev/null 2>&1; then
      curl -L "$CURL_URL" -o "$tmp"
    elif command -v wget >/dev/null 2>&1; then
      wget -O "$tmp" "$CURL_URL"
    else
      echo "tools/ports/build-curl.sh: need host curl or wget to fetch $CURL_URL" >&2
      exit 1
    fi
  fi
  tar -xzf "$tmp" -C "$SRC_PARENT/$HOST_TRIPLET"
fi

# Time discipline/NTP can move clocks and confuse autotools dependency checks.
# Keep generated files newer than *.am/*.ac so make does not require autoreconf.
find "$SRC_DIR" -exec touch {} +
find "$SRC_DIR" -name 'Makefile.in' -exec touch {} +
[ -f "$SRC_DIR/configure" ] && touch "$SRC_DIR/configure"
[ -f "$SRC_DIR/aclocal.m4" ] && touch "$SRC_DIR/aclocal.m4"

if ! grep -q 'b1nix\*' "$SRC_DIR/config.sub"; then
  tmp_config_sub="$SRC_DIR/config.sub.tmp"
  sed 's/| fiwix\\*/| fiwix* | b1nix*/' "$SRC_DIR/config.sub" > "$tmp_config_sub"
  mv "$tmp_config_sub" "$SRC_DIR/config.sub"
fi

make -C "$ROOT_DIR/userspace" -s "build/$B1NIX_ARCH/libb1nix.a" "build/$B1NIX_ARCH/crt/crt0.o"

(
  cd "$BUILD_DIR"
  # Pass an explicit --build distinct from --host so autoconf KNOWS this is a
  # cross build and never tries to *run* a freshly compiled i686-b1nix test
  # binary. Without it, on an x86_64 Linux host with 32-bit binfmt the
  # "checking whether we are cross compiling" conftest is executed and hangs
  # forever (the b1nix binary has no Linux ABI). cross_compiling=yes belt-and-
  # suspenders for any sub-configure that still slips through.
  BUILD_TRIPLET="$("$SRC_DIR/config.guess" 2>/dev/null || echo "$(uname -m)-pc-linux-gnu")"
  export cross_compiling=yes
  "$SRC_DIR/configure" \
    --host="$HOST_TRIPLET" \
    --build="$BUILD_TRIPLET" \
    --disable-shared --enable-static \
    "$SSL_FLAGS" "$ZLIB_FLAG" "$BROTLI_FLAG" --without-zstd \
    --with-libpsl="$LIBPSL_PREFIX" --with-libidn2="$LIBIDN2_PREFIX" --without-nghttp2 --without-nghttp3 \
    --without-ngtcp2 \
    --disable-ldap --disable-ldaps --disable-ftp --enable-file \
    --disable-gopher --disable-imap --disable-mqtt --disable-pop3 \
    --disable-rtsp --disable-smb --disable-smtp --disable-telnet \
    --disable-tftp --disable-dict --disable-manual --disable-docs \
    --enable-threaded-resolver --enable-ipv6 --enable-unix-sockets \
    --enable-cookies --enable-alt-svc --enable-hsts \
    --enable-websockets --enable-headers-api \
    --enable-dateparse \
    --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt \
    CC="$WRAP" AR="$AR_BIN" RANLIB="$RANLIB_BIN" \
    CPPFLAGS="$TLS_CPPFLAGS" LDFLAGS="$TLS_LDFLAGS" LIBS="$TLS_LIBS"
)

make -C "$BUILD_DIR/lib" -j"${JOBS:-4}" libcurl.la
make -C "$BUILD_DIR/src" -j"${JOBS:-4}" curl
