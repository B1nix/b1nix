#!/bin/sh
# tools/patches/mbedtls/b1nix-config.sh — applied by build-mbedtls.sh via the
# port driver's PATCHES= mechanism (port_apply_patches runs it as `<this> <srcdir>`).
#
# Idempotent b1nix adaptation of the mbedTLS 3.6.x source tree:
#   * mbedtls_config.h:  re-enable HAVE_TIME / HAVE_TIME_DATE / TIMING_C /
#     PSA_CRYPTO_C and disable NET_C (b1nix has no BSD sockets layer mbedTLS NET
#     expects); recovers a tree a prior build may have edited.
#   * platform_util.c / timing.c:  teach the POSIX/Unix #if gates about b1nix
#     (the bodies only use clock_gettime/gettimeofday/time, all in b1nix libc).
#   * entropy_poll.c:  inject a getrandom(2)-backed entropy shim (b1nix has no
#     guaranteed /dev/urandom).
# Each edit is guarded so re-running is a no-op.
set -eu
SRC_DIR="${1:?usage: b1nix-config.sh <mbedtls-src-dir>}"

CFG="$SRC_DIR/include/mbedtls/mbedtls_config.h"
if grep -q '^/\* #undef MBEDTLS_HAVE_TIME \*/$' "$CFG"; then
  sed -i.bak 's@^/\* #undef MBEDTLS_HAVE_TIME \*/$@#define MBEDTLS_HAVE_TIME@' "$CFG"
fi
if grep -q '^/\* #undef MBEDTLS_HAVE_TIME_DATE \*/$' "$CFG"; then
  sed -i.bak 's@^/\* #undef MBEDTLS_HAVE_TIME_DATE \*/$@#define MBEDTLS_HAVE_TIME_DATE@' "$CFG"
fi
if grep -q '^/\* #undef MBEDTLS_TIMING_C \*/$' "$CFG"; then
  sed -i.bak 's@^/\* #undef MBEDTLS_TIMING_C \*/$@#define MBEDTLS_TIMING_C@' "$CFG"
fi
if grep -q '^#define MBEDTLS_NET_C$' "$CFG"; then
  sed -i.bak 's/^#define MBEDTLS_NET_C$/\/\* #undef MBEDTLS_NET_C \*\//' "$CFG"
fi
if grep -q '^/\* #undef MBEDTLS_PSA_CRYPTO_C \*/$' "$CFG"; then
  sed -i.bak 's@^/\* #undef MBEDTLS_PSA_CRYPTO_C \*/$@#define MBEDTLS_PSA_CRYPTO_C@' "$CFG"
fi
rm -f "$CFG.bak"

# mbedtls_ms_time() POSIX gate.
PLATFORM_UTIL="$SRC_DIR/library/platform_util.c"
if ! grep -q 'defined(b1nix)' "$PLATFORM_UTIL"; then
  sed -i.bak 's@^#if (defined(_POSIX_VERSION) && _POSIX_VERSION >= 199309L) || defined(__HAIKU__)$@#if (defined(_POSIX_VERSION) \&\& _POSIX_VERSION >= 199309L) || defined(__HAIKU__) || defined(b1nix)@' "$PLATFORM_UTIL"
  rm -f "$PLATFORM_UTIL.bak"
fi

# timing.c hard #error gate.
TIMING_C="$SRC_DIR/library/timing.c"
if ! grep -q 'defined(b1nix)' "$TIMING_C"; then
  sed -i.bak 's@^    !defined(__APPLE__) && !defined(_WIN32) && !defined(__QNXNTO__) && \\$@    !defined(__APPLE__) \&\& !defined(_WIN32) \&\& !defined(__QNXNTO__) \&\& !defined(b1nix) \&\& \\@' "$TIMING_C"
  rm -f "$TIMING_C.bak"
fi

# getrandom(2)-backed entropy shim.
ENTROPY_POLL="$SRC_DIR/library/entropy_poll.c"
if ! grep -q "B1NIX_GETRANDOM_SHIM" "$ENTROPY_POLL"; then
  perl -0pi -e 's@#include <stdio\.h>\n@#include <stdio.h>\n\n#if defined(b1nix)\n#include <sys/random.h>\n#include <errno.h>\n#define HAVE_GETRANDOM\n#define B1NIX_GETRANDOM_SHIM\nstatic int getrandom_wrapper(void *buf, size_t buflen, unsigned int flags)\n{\n    return (int) getrandom(buf, buflen, flags);\n}\n#endif\n@' "$ENTROPY_POLL"
fi
