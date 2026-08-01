#!/bin/sh
# Build upstream curl for the b1nix userspace ABI.
# Uses the autotools driver (tools/ports/drivers/autotools.sh).
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

AUTOTOOLS_NAME=curl
AUTOTOOLS_VERSION="${CURL_VERSION:-8.20.0}"
AUTOTOOLS_URL="https://curl.se/download/curl-${AUTOTOOLS_VERSION}.tar.gz"

# TLS backend: build libcurl against the b1nix mbedTLS port so NetSurf can fetch
# https:// URLs (M53 web access). Without this libcurl has no TLS backend and
# every HTTPS fetch fails silently (has-content=0) — build-netsurf-fb.sh already
# stages the mbedTLS archives into NetSurf's libcurl.pc and expects the curl port
# to carry a real TLS backend. build-mbedtls.sh is idempotent and prints its
# install prefix on the last stdout line.
MBEDTLS_PREFIX="$("$ROOT_DIR/tools/ports/build-mbedtls.sh" | tail -n 1)"
if [ ! -f "$MBEDTLS_PREFIX/lib/libmbedtls.a" ]; then
  echo "build-curl.sh: mbedTLS build did not produce libmbedtls.a at $MBEDTLS_PREFIX" >&2
  exit 1
fi
# curl's configure auto-detects the mbedTLS lib set; export the include/lib paths
# and the archive list so its link probes (and the final link) resolve.
# (build-mbedtls.sh folds compiler-rt's __udivti3 etc. straight into
# libmbedcrypto.a, so nothing extra is needed here for that.)
export CPPFLAGS="-I$MBEDTLS_PREFIX/include ${CPPFLAGS:-}"
export LDFLAGS="-L$MBEDTLS_PREFIX/lib ${LDFLAGS:-}"
export LIBS="-lmbedtls -lmbedx509 -lmbedcrypto ${LIBS:-}"

AUTOTOOLS_CONFIGURE="--disable-shared --enable-static --with-mbedtls=$MBEDTLS_PREFIX --without-zlib --without-brotli --without-zstd --without-nghttp2 --without-nghttp3 --without-ngtcp2 --disable-ldap --disable-ldaps --disable-ftp --enable-file --disable-gopher --disable-imap --disable-mqtt --disable-pop3 --disable-rtsp --disable-smb --disable-smtp --disable-telnet --disable-tftp --disable-dict --disable-manual --disable-docs --disable-libcurl-option --disable-threaded-resolver --enable-ipv6 --enable-unix-sockets --enable-cookies --enable-alt-svc --enable-hsts --enable-websockets --enable-headers-api --enable-dateparse --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt --without-libpsl --without-libidn2 --without-libunistring"

. "$ROOT_DIR/tools/ports/drivers/autotools.sh"
