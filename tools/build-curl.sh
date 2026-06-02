#!/bin/sh
# Build upstream curl for the b1nix userspace ABI.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CURL_VERSION="${CURL_VERSION:-8.20.0}"
CURL_TARBALL="curl-${CURL_VERSION}.tar.gz"
CURL_URL="https://curl.se/download/${CURL_TARBALL}"
SRC_DIR="$ROOT_DIR/build/curl-src/curl-${CURL_VERSION}"
BUILD_DIR="$ROOT_DIR/build/curl-b1nix"
WRAP="$ROOT_DIR/tools/b1nix-autotools-cc"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"
RANLIB_BIN="${RANLIB:-$(command -v llvm-ranlib 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ranlib)}"
B1NIX_ARCH="${B1NIX_ARCH:-x86_64}"
if [ "$B1NIX_ARCH" = "x86" ]; then
  HOST_TRIPLET="i686-b1nix"
else
  HOST_TRIPLET="x86_64-b1nix"
fi

SSL_FLAGS="--without-ssl"
TLS_CPPFLAGS=""
TLS_LDFLAGS=""
TLS_LIBS=""
if [ "$B1NIX_TLS" = "mbedtls" ]; then
  # Capture the full output so a build failure is not masked by the pipe;
  # the install prefix is the final stdout line (build noise goes to stderr).
  if ! MBEDTLS_OUT="$("$ROOT_DIR/tools/build-mbedtls.sh")"; then
    echo "tools/build-curl.sh: mbedTLS build failed" >&2
    exit 1
  fi
  MBEDTLS_PREFIX="$(printf '%s\n' "$MBEDTLS_OUT" | tail -n 1)"
  SSL_FLAGS="--with-mbedtls=$MBEDTLS_PREFIX"
  TLS_CPPFLAGS="-I$MBEDTLS_PREFIX/include"
  TLS_LDFLAGS="-L$MBEDTLS_PREFIX/lib"
  TLS_LIBS="-lmbedx509 -lmbedcrypto"
fi

mkdir -p "$ROOT_DIR/build/curl-src" "$BUILD_DIR"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$ROOT_DIR/build/curl-src/${CURL_TARBALL}"
  if [ ! -f "$tmp" ]; then
    if command -v curl >/dev/null 2>&1; then
      curl -L "$CURL_URL" -o "$tmp"
    elif command -v wget >/dev/null 2>&1; then
      wget -O "$tmp" "$CURL_URL"
    else
      echo "tools/build-curl.sh: need host curl or wget to fetch $CURL_URL" >&2
      exit 1
    fi
  fi
  tar -xzf "$tmp" -C "$ROOT_DIR/build/curl-src"
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

make -C "$ROOT_DIR/userspace" -s build/libb1nix.a build/crt/crt0.o

(
  cd "$BUILD_DIR"
  "$SRC_DIR/configure" \
    --host="$HOST_TRIPLET" \
    --disable-shared --enable-static \
    "$SSL_FLAGS" --without-zlib --without-brotli --without-zstd \
    --without-libpsl --without-libidn2 --without-nghttp2 --without-nghttp3 \
    --without-ngtcp2 \
    --disable-ldap --disable-ldaps --disable-ftp --disable-file \
    --disable-gopher --disable-imap --disable-mqtt --disable-pop3 \
    --disable-rtsp --disable-smb --disable-smtp --disable-telnet \
    --disable-tftp --disable-dict --disable-manual --disable-docs \
    --disable-threaded-resolver --enable-ipv6 --disable-unix-sockets \
    --disable-cookies --disable-alt-svc --disable-hsts \
    --disable-websockets --disable-headers-api --disable-mime \
    --disable-dateparse \
    --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt \
    CC="$WRAP" AR="$AR_BIN" RANLIB="$RANLIB_BIN" \
    CPPFLAGS="$TLS_CPPFLAGS" LDFLAGS="$TLS_LDFLAGS" LIBS="$TLS_LIBS"
)

make -C "$BUILD_DIR/lib" -j"${JOBS:-4}" libcurl.la
make -C "$BUILD_DIR/src" -j"${JOBS:-4}" curl
