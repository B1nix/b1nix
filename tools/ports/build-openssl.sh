#!/bin/sh
# Build static OpenSSL 1.1.1w for the b1nix userspace ABI.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
OPENSSL_VERSION="${OPENSSL_VERSION:-1.1.1w}"
OPENSSL_TARBALL="openssl-${OPENSSL_VERSION}.tar.gz"
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/OpenSSL_1_1_1w/${OPENSSL_TARBALL}"
CCACHE=""
command -v ccache >/dev/null 2>&1 && [ "${B1NIX_NO_CCACHE:-0}" != "1" ] && CCACHE="ccache "
WRAP="${CCACHE}$ROOT_DIR/tools/toolchain/bin/b1nix-musl-autotools-cc"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"
RANLIB_BIN="${RANLIB:-$(command -v llvm-ranlib 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ranlib)}"
# Per-architecture build identity + per-triplet source/build dirs.
. "$ROOT_DIR/tools/toolchain/env.sh"
SRC_PARENT="$ROOT_DIR/build/src/openssl"
SRC_DIR="$SRC_PARENT/$B1NIX_TRIPLET/openssl-${OPENSSL_VERSION}"
BUILD_DIR="$ROOT_DIR/build/$B1NIX_ARCH/ports/openssl"
INSTALL_DIR="$BUILD_DIR/install"

mkdir -p "$SRC_PARENT/$B1NIX_TRIPLET" "$BUILD_DIR"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/${OPENSSL_TARBALL}"
  if [ ! -f "$tmp" ]; then
    if command -v curl >/dev/null 2>&1; then
      curl -L "$OPENSSL_URL" -o "$tmp"
    elif command -v wget >/dev/null 2>&1; then
      wget -O "$tmp" "$OPENSSL_URL"
    else
      echo "tools/ports/build-openssl.sh: need host curl or wget to fetch $OPENSSL_URL" >&2
      exit 1
    fi
  fi
  tar -xzf "$tmp" -C "$SRC_PARENT/$B1NIX_TRIPLET"
fi

# Time discipline/touching to avoid autotools/make rebuild dependencies
find "$SRC_DIR" -exec touch {} +

(
  cd "$SRC_DIR"
  # Configure OpenSSL for generic cc target, static only, no asm, no threads, no async.
  ./Configure cc \
    --prefix="$INSTALL_DIR" \
    --openssldir="$INSTALL_DIR/ssl" \
    no-shared \
    no-threads \
    no-async \
    no-asm \
    no-engine \
    no-hw \
    no-dso \
    no-tests \
    no-ui-console \
    no-afalgeng
)

# Build OpenSSL static libraries directly.
make -C "$SRC_DIR" clean >/dev/null 2>&1 || true
make -C "$SRC_DIR" build_generated
make -C "$SRC_DIR" -j"${JOBS:-4}" CC="$WRAP" AR="$AR_BIN" RANLIB="$RANLIB_BIN" libcrypto.a libssl.a 1>&2

# Perform manual installation of headers and libraries.
mkdir -p "$INSTALL_DIR/lib" "$INSTALL_DIR/include/openssl"
cp "$SRC_DIR"/libcrypto.a "$SRC_DIR"/libssl.a "$INSTALL_DIR/lib/"
"$RANLIB_BIN" "$INSTALL_DIR/lib/libcrypto.a"
"$RANLIB_BIN" "$INSTALL_DIR/lib/libssl.a"

cp "$SRC_DIR"/include/openssl/*.h "$INSTALL_DIR/include/openssl/"
[ -d "$SRC_DIR/include/openssl" ] && cp -R "$SRC_DIR"/include/openssl/*.h "$INSTALL_DIR/include/openssl/" || true

echo "$INSTALL_DIR"
