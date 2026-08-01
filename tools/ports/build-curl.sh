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

# libidn2 (IDNA/Punycode, needs libunistring), libpsl (Public Suffix List,
# cookie domain-matching security), brotli (Content-Encoding: br) — all
# already ported (tools/ports/build-{libidn2,libunistring,libpsl,brotli}.sh)
# but previously left unwired here from before those ports existed; wire
# them in now the same way mbedTLS is above.
UNISTRING_PREFIX="$("$ROOT_DIR/tools/ports/build-libunistring.sh" | tail -n 1)"
IDN2_PREFIX="$("$ROOT_DIR/tools/ports/build-libidn2.sh" | tail -n 1)"
PSL_PREFIX="$("$ROOT_DIR/tools/ports/build-libpsl.sh" | tail -n 1)"
BROTLI_PREFIX="$("$ROOT_DIR/tools/ports/build-brotli.sh" | tail -n 1)"
for pair in "$IDN2_PREFIX:libidn2.a" "$PSL_PREFIX:libpsl.a" "$BROTLI_PREFIX:libbrotlidec.a"; do
  p="${pair%:*}"; f="${pair#*:}"
  [ -f "$p/lib/$f" ] || { echo "build-curl.sh: $f not found under $p" >&2; exit 1; }
done
export CPPFLAGS="-I$IDN2_PREFIX/include -I$PSL_PREFIX/include -I$BROTLI_PREFIX/include ${CPPFLAGS:-}"
export LDFLAGS="-L$IDN2_PREFIX/lib -L$UNISTRING_PREFIX/lib -L$PSL_PREFIX/lib -L$BROTLI_PREFIX/lib ${LDFLAGS:-}"
export LIBS="-lidn2 -lunistring -lpsl -lbrotlidec -lbrotlicommon ${LIBS:-}"

# --enable-threaded-resolver: previously forced off in favor of curl's
# blocking single-threaded resolver, from before b1nix had a complete
# pthread port. Threads are real now (M29 posix-threads); re-enable.
AUTOTOOLS_CONFIGURE="--disable-shared --enable-static --with-mbedtls=$MBEDTLS_PREFIX --without-zlib --with-brotli --without-zstd --without-nghttp2 --without-nghttp3 --without-ngtcp2 --disable-ldap --disable-ldaps --disable-ftp --enable-file --disable-gopher --disable-imap --disable-mqtt --disable-pop3 --disable-rtsp --disable-smb --disable-smtp --disable-telnet --disable-tftp --disable-dict --disable-manual --disable-docs --disable-libcurl-option --enable-threaded-resolver --enable-ipv6 --enable-unix-sockets --enable-cookies --enable-alt-svc --enable-hsts --enable-websockets --enable-headers-api --enable-dateparse --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt --with-libpsl --with-libidn2"

. "$ROOT_DIR/tools/ports/drivers/autotools.sh"
