#!/bin/sh
# Build upstream mbedTLS static libraries for the b1nix userspace ABI.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
MBEDTLS_VERSION="${MBEDTLS_VERSION:-3.6.0}"
MBEDTLS_TARBALL="mbedtls-${MBEDTLS_VERSION}.tar.gz"
MBEDTLS_URL="https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v${MBEDTLS_VERSION}.tar.gz"
WRAP="$ROOT_DIR/tools/toolchain/bin/b1nix-autotools-cc"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"
RANLIB_BIN="${RANLIB:-$(command -v llvm-ranlib 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ranlib)}"
# Per-architecture build identity + per-triplet source/build dirs.
. "$ROOT_DIR/tools/toolchain/env.sh"
SRC_PARENT="$ROOT_DIR/build/mbedtls-src"
SRC_DIR="$SRC_PARENT/$B1NIX_TRIPLET/mbedtls-${MBEDTLS_VERSION}"
BUILD_DIR="$ROOT_DIR/build/mbedtls-b1nix/$B1NIX_TRIPLET"
INSTALL_DIR="$BUILD_DIR/install"

mkdir -p "$SRC_PARENT/$B1NIX_TRIPLET" "$BUILD_DIR"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/${MBEDTLS_TARBALL}"
  if [ ! -f "$tmp" ]; then
    if command -v curl >/dev/null 2>&1; then
      curl -L "$MBEDTLS_URL" -o "$tmp"
    elif command -v wget >/dev/null 2>&1; then
      wget -O "$tmp" "$MBEDTLS_URL"
    else
      echo "tools/ports/build-mbedtls.sh: need host curl or wget to fetch $MBEDTLS_URL" >&2
      exit 1
    fi
  fi
  tar -xzf "$tmp" -C "$SRC_PARENT/$B1NIX_TRIPLET"
fi

# b1nix adaptation of the mbedTLS source tree (config toggles for time/timing/
# PSA, the platform_util.c / timing.c #if gates, and the getrandom entropy
# shim). Extracted to a separate, idempotent patch file (was 7 inline edits).
sh "$ROOT_DIR/tools/patches/mbedtls/b1nix-config.sh" "$SRC_DIR"

make -C "$ROOT_DIR/userspace" -s "build/$B1NIX_ARCH/libb1nix.a" "build/$B1NIX_ARCH/crt/crt0.o" 1>&2

# mbedTLS PSA wrapper generation depends on python jsonschema.
# Keep this hermetic by provisioning a local venv under build/.
PY_VENV="$ROOT_DIR/build/py-mbedtls-tools"
if [ ! -x "$PY_VENV/bin/python3" ]; then
  python3 -m venv "$PY_VENV"
fi
"$PY_VENV/bin/python3" -m pip -q install --upgrade pip >/dev/null 2>&1 || true
"$PY_VENV/bin/python3" -m pip -q install jsonschema jinja2 >/dev/null 2>&1

make -C "$SRC_DIR/library" clean >/dev/null 2>&1 || true
# Build noise must go to stderr so stdout carries only the install dir path
# (build-curl.sh captures this script's stdout to derive -I/-L flags).
PATH="$PY_VENV/bin:$PATH" make -C "$SRC_DIR/library" -j"${JOBS:-4}" \
  CC="$WRAP" AR="$AR_BIN" RANLIB="$RANLIB_BIN" \
  CFLAGS="-O2 -fno-builtin -D__unix__" 1>&2

mkdir -p "$INSTALL_DIR/lib" "$INSTALL_DIR/include"
cp "$SRC_DIR"/library/libmbed*.a "$INSTALL_DIR/lib/"
cp -R "$SRC_DIR/include/mbedtls" "$INSTALL_DIR/include/"
cp -R "$SRC_DIR/include/psa" "$INSTALL_DIR/include/"

echo "$INSTALL_DIR"
