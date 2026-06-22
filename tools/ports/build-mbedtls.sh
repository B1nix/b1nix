#!/bin/sh
# Build upstream mbedTLS static libraries for the b1nix userspace ABI.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
MBEDTLS_VERSION="${MBEDTLS_VERSION:-3.6.0}"
MBEDTLS_TARBALL="mbedtls-${MBEDTLS_VERSION}.tar.gz"
MBEDTLS_URL="https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v${MBEDTLS_VERSION}.tar.gz"
WRAP="$ROOT_DIR/tools/b1nix-autotools-cc"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"
RANLIB_BIN="${RANLIB:-$(command -v llvm-ranlib 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ranlib)}"
# Per-architecture build identity + per-triplet source/build dirs.
. "$ROOT_DIR/tools/toolchain-env.sh"
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

CFG="$SRC_DIR/include/mbedtls/mbedtls_config.h"
# Enable time/date support: b1nix libc provides time() (SYS_TIME) and gmtime_r,
# and the wall clock is now settable (settimeofday / SYS_SETTIMEOFDAY), so
# mbedTLS can validate certificate notBefore/notAfter windows. Re-enable the
# config if a prior build had undef'd it (idempotent).
if grep -q '^/\* #undef MBEDTLS_HAVE_TIME \*/$' "$CFG"; then
  sed -i.bak 's@^/\* #undef MBEDTLS_HAVE_TIME \*/$@#define MBEDTLS_HAVE_TIME@' "$CFG"
fi
if grep -q '^/\* #undef MBEDTLS_HAVE_TIME_DATE \*/$' "$CFG"; then
  sed -i.bak 's@^/\* #undef MBEDTLS_HAVE_TIME_DATE \*/$@#define MBEDTLS_HAVE_TIME_DATE@' "$CFG"
fi
# MBEDTLS_TIMING_C (M54): the timing layer (DTLS retransmit timers,
# mbedtls_timing_get_timer) only needs gettimeofday in mbedTLS 3.x, which b1nix
# libc provides. Keep it enabled (re-enable idempotently if a prior build
# undef'd it). The portability #error gate in timing.c is taught about b1nix
# below.
if grep -q '^/\* #undef MBEDTLS_TIMING_C \*/$' "$CFG"; then
  sed -i.bak 's@^/\* #undef MBEDTLS_TIMING_C \*/$@#define MBEDTLS_TIMING_C@' "$CFG"
fi
if grep -q '^#define MBEDTLS_NET_C$' "$CFG"; then
  sed -i.bak 's/^#define MBEDTLS_NET_C$/\/\* #undef MBEDTLS_NET_C \*\//' "$CFG"
fi
# Recover from prior local edits where PSA core may have been commented out.
if grep -q '^/\* #undef MBEDTLS_PSA_CRYPTO_C \*/$' "$CFG"; then
  sed -i.bak 's@^/\* #undef MBEDTLS_PSA_CRYPTO_C \*/$@#define MBEDTLS_PSA_CRYPTO_C@' "$CFG"
fi
rm -f "$CFG.bak"

# MBEDTLS_HAVE_TIME pulls in mbedtls_ms_time(), whose implementation is gated on
# _POSIX_VERSION >= 199309L. b1nix does not advertise that macro, yet the body
# only needs clock_gettime(CLOCK_MONOTONIC) and time() — both of which b1nix
# libc provides — so the gate, not the code, is the problem. Teach the gate to
# accept b1nix (the -Db1nix macro is always passed by the cross wrapper).
PLATFORM_UTIL="$SRC_DIR/library/platform_util.c"
if ! grep -q 'defined(b1nix)' "$PLATFORM_UTIL"; then
  sed -i.bak 's@^#if (defined(_POSIX_VERSION) && _POSIX_VERSION >= 199309L) || defined(__HAIKU__)$@#if (defined(_POSIX_VERSION) \&\& _POSIX_VERSION >= 199309L) || defined(__HAIKU__) || defined(b1nix)@' "$PLATFORM_UTIL"
  rm -f "$PLATFORM_UTIL.bak"
fi

# timing.c has a hard #error unless one of the known Unix/Windows macros is
# defined; b1nix is Unix-like (the non-Windows path uses only gettimeofday).
# Teach the gate about b1nix so MBEDTLS_TIMING_C compiles.
TIMING_C="$SRC_DIR/library/timing.c"
if ! grep -q 'defined(b1nix)' "$TIMING_C"; then
  sed -i.bak 's@^    !defined(__APPLE__) && !defined(_WIN32) && !defined(__QNXNTO__) && \\$@    !defined(__APPLE__) \&\& !defined(_WIN32) \&\& !defined(__QNXNTO__) \&\& !defined(b1nix) \&\& \\@' "$TIMING_C"
  rm -f "$TIMING_C.bak"
fi

# On b1nix, /dev/urandom is not guaranteed, but getrandom(2) is.
# Inject a tiny platform hook so mbedTLS entropy poll uses getrandom directly.
ENTROPY_POLL="$SRC_DIR/library/entropy_poll.c"
if ! grep -q "B1NIX_GETRANDOM_SHIM" "$ENTROPY_POLL"; then
  perl -0pi -e 's@#include <stdio\.h>\n@#include <stdio.h>\n\n#if defined(b1nix)\n#include <sys/random.h>\n#include <errno.h>\n#define HAVE_GETRANDOM\n#define B1NIX_GETRANDOM_SHIM\nstatic int getrandom_wrapper(void *buf, size_t buflen, unsigned int flags)\n{\n    return (int) getrandom(buf, buflen, flags);\n}\n#endif\n@' "$ENTROPY_POLL"
fi

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
