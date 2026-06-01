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
AR_BIN="${AR:-/opt/homebrew/opt/llvm/bin/llvm-ar}"
RANLIB_BIN="${RANLIB:-/opt/homebrew/opt/llvm/bin/llvm-ranlib}"

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

if ! grep -q 'b1nix\*' "$SRC_DIR/config.sub"; then
  tmp_config_sub="$SRC_DIR/config.sub.tmp"
  sed 's/| fiwix\\*/| fiwix* | b1nix*/' "$SRC_DIR/config.sub" > "$tmp_config_sub"
  mv "$tmp_config_sub" "$SRC_DIR/config.sub"
fi

make -C "$ROOT_DIR/userspace" -s build/libb1nix.a build/crt/crt0.o

(
  cd "$BUILD_DIR"
  "$SRC_DIR/configure" \
    --host=x86_64-b1nix \
    --disable-shared --enable-static \
    --without-ssl --without-zlib --without-brotli --without-zstd \
    --without-libpsl --without-libidn2 --without-nghttp2 --without-nghttp3 \
    --without-ngtcp2 \
    --disable-ldap --disable-ldaps --disable-ftp --disable-file \
    --disable-gopher --disable-imap --disable-mqtt --disable-pop3 \
    --disable-rtsp --disable-smb --disable-smtp --disable-telnet \
    --disable-tftp --disable-dict --disable-manual --disable-docs \
    --disable-threaded-resolver --disable-ipv6 --disable-unix-sockets \
    --disable-cookies --disable-alt-svc --disable-hsts \
    --disable-websockets --disable-headers-api --disable-mime \
    --disable-dateparse \
    CC="$WRAP" AR="$AR_BIN" RANLIB="$RANLIB_BIN"
)

make -C "$BUILD_DIR/lib" -j"${JOBS:-4}" libcurl.la
make -C "$BUILD_DIR/src" -j"${JOBS:-4}" curl
