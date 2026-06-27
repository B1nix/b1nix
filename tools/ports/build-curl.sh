#!/bin/sh
# Build upstream curl for the b1nix userspace ABI.
# Uses the autotools driver (tools/ports/drivers/autotools.sh).
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

AUTOTOOLS_NAME=curl
AUTOTOOLS_VERSION="${CURL_VERSION:-8.20.0}"
AUTOTOOLS_URL="https://curl.se/download/curl-${AUTOTOOLS_VERSION}.tar.gz"
AUTOTOOLS_CONFIGURE="--disable-shared --enable-static --without-ssl --without-zlib --without-brotli --without-zstd --without-nghttp2 --without-nghttp3 --without-ngtcp2 --disable-ldap --disable-ldaps --disable-ftp --enable-file --disable-gopher --disable-imap --disable-mqtt --disable-pop3 --disable-rtsp --disable-smb --disable-smtp --disable-telnet --disable-tftp --disable-dict --disable-manual --disable-docs --enable-threaded-resolver --enable-ipv6 --enable-unix-sockets --enable-cookies --enable-alt-svc --enable-hsts --enable-websockets --enable-headers-api --enable-dateparse --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt --without-libpsl --without-libidn2 --without-libunistring"

. "$ROOT_DIR/tools/ports/drivers/autotools.sh"
