#!/bin/sh
# Build upstream mbedTLS static libraries for the b1nix userspace ABI.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MBEDTLS_VERSION="${MBEDTLS_VERSION:-3.6.0}"
MBEDTLS_TARBALL="mbedtls-${MBEDTLS_VERSION}.tar.gz"
MBEDTLS_URL="https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v${MBEDTLS_VERSION}.tar.gz"
SRC_DIR="$ROOT_DIR/build/mbedtls-src/mbedtls-${MBEDTLS_VERSION}"
BUILD_DIR="$ROOT_DIR/build/mbedtls-b1nix"
INSTALL_DIR="$BUILD_DIR/install"
WRAP="$ROOT_DIR/tools/b1nix-autotools-cc"
AR_BIN="${AR:-/opt/homebrew/opt/llvm/bin/llvm-ar}"
RANLIB_BIN="${RANLIB:-/opt/homebrew/opt/llvm/bin/llvm-ranlib}"

mkdir -p "$ROOT_DIR/build/mbedtls-src" "$BUILD_DIR"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$ROOT_DIR/build/mbedtls-src/${MBEDTLS_TARBALL}"
  if [ ! -f "$tmp" ]; then
    if command -v curl >/dev/null 2>&1; then
      curl -L "$MBEDTLS_URL" -o "$tmp"
    elif command -v wget >/dev/null 2>&1; then
      wget -O "$tmp" "$MBEDTLS_URL"
    else
      echo "tools/build-mbedtls.sh: need host curl or wget to fetch $MBEDTLS_URL" >&2
      exit 1
    fi
  fi
  tar -xzf "$tmp" -C "$ROOT_DIR/build/mbedtls-src"
fi

CFG="$SRC_DIR/include/mbedtls/mbedtls_config.h"
if grep -q '^#define MBEDTLS_HAVE_TIME$' "$CFG"; then
  sed -i.bak 's/^#define MBEDTLS_HAVE_TIME$/\/\* #undef MBEDTLS_HAVE_TIME \*\//' "$CFG"
fi
if grep -q '^#define MBEDTLS_HAVE_TIME_DATE$' "$CFG"; then
  sed -i.bak 's/^#define MBEDTLS_HAVE_TIME_DATE$/\/\* #undef MBEDTLS_HAVE_TIME_DATE \*\//' "$CFG"
fi
if grep -q '^#define MBEDTLS_TIMING_C$' "$CFG"; then
  sed -i.bak 's/^#define MBEDTLS_TIMING_C$/\/\* #undef MBEDTLS_TIMING_C \*\//' "$CFG"
fi
if grep -q '^#define MBEDTLS_NET_C$' "$CFG"; then
  sed -i.bak 's/^#define MBEDTLS_NET_C$/\/\* #undef MBEDTLS_NET_C \*\//' "$CFG"
fi
# Recover from prior local edits where PSA core may have been commented out.
if grep -q '^/\* #undef MBEDTLS_PSA_CRYPTO_C \*/$' "$CFG"; then
  sed -i.bak 's@^/\* #undef MBEDTLS_PSA_CRYPTO_C \*/$@#define MBEDTLS_PSA_CRYPTO_C@' "$CFG"
fi
rm -f "$CFG.bak"

# On b1nix, /dev/urandom is not guaranteed, but getrandom(2) is.
# Inject a tiny platform hook so mbedTLS entropy poll uses getrandom directly.
ENTROPY_POLL="$SRC_DIR/library/entropy_poll.c"
if ! grep -q "B1NIX_GETRANDOM_SHIM" "$ENTROPY_POLL"; then
  perl -0pi -e 's@#include <stdio\.h>\n@#include <stdio.h>\n\n#if defined(b1nix)\n#include <sys/random.h>\n#include <errno.h>\n#define HAVE_GETRANDOM\n#define B1NIX_GETRANDOM_SHIM\nstatic int getrandom_wrapper(void *buf, size_t buflen, unsigned int flags)\n{\n    return (int) getrandom(buf, buflen, flags);\n}\n#endif\n@' "$ENTROPY_POLL"
fi

make -C "$ROOT_DIR/userspace" -s build/libb1nix.a build/crt/crt0.o

# mbedTLS PSA wrapper generation depends on python jsonschema.
# Keep this hermetic by provisioning a local venv under build/.
PY_VENV="$ROOT_DIR/build/py-mbedtls-tools"
if [ ! -x "$PY_VENV/bin/python3" ]; then
  python3 -m venv "$PY_VENV"
fi
"$PY_VENV/bin/python3" -m pip -q install --upgrade pip >/dev/null 2>&1 || true
"$PY_VENV/bin/python3" -m pip -q install jsonschema jinja2 >/dev/null 2>&1

make -C "$SRC_DIR/library" clean >/dev/null 2>&1 || true
PATH="$PY_VENV/bin:$PATH" make -C "$SRC_DIR/library" -j"${JOBS:-4}" \
  CC="$WRAP" AR="$AR_BIN" RANLIB="$RANLIB_BIN" \
  CFLAGS="-O2 -fno-builtin -D__unix__"

mkdir -p "$INSTALL_DIR/lib" "$INSTALL_DIR/include"
cp "$SRC_DIR"/library/libmbed*.a "$INSTALL_DIR/lib/"
cp -R "$SRC_DIR/include/mbedtls" "$INSTALL_DIR/include/"
cp -R "$SRC_DIR/include/psa" "$INSTALL_DIR/include/"

echo "$INSTALL_DIR"
