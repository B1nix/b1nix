#!/bin/sh
# Build libnsutils as a static libnsutils.a for the b1nix userspace ABI.
# Freestanding curated compile (no autotools). libnsutils is a small NetSurf
# helper library (base64, monotonic time, pread/pwrite). The default
# clock_gettime(CLOCK_MONOTONIC) path is used (b1nix libc provides it). Prints
# the install dir.
#
# M53 (NetSurf browser platform) — browser lib chain helper (step 6).
# Build logic lives in tools/ports/drivers/nslib.sh; this is the manifest.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

NSLIB_NAME=libnsutils
NSLIB_VERSION="${NSU_VERSION:-0.1.0}"
NSLIB_ARCHIVE=libnsutils.a
NSLIB_SOURCES="src/base64.c src/time.c src/unistd.c"
NSLIB_CFLAGS="-Wno-implicit-function-declaration"
NSLIB_HEADERS="tree:include/nsutils"

# NetSurf 3.11 expects nsutils/assert.h (compile-time assert helper) which this
# libnsutils release predates. Provide the standard shim.
port_post_install() {
  if [ ! -f "$INSTALL_DIR/include/nsutils/assert.h" ]; then
    cat >"$INSTALL_DIR/include/nsutils/assert.h" <<'EOF'
/* nsutils/assert.h shim for b1nix (libnsutils release predates this header). */
#ifndef NSUTILS_ASSERT_H_
#define NSUTILS_ASSERT_H_
#include <assert.h>
#ifndef ns_static_assert
#define ns_static_assert(e) _Static_assert((e), #e)
#endif
#endif
EOF
  fi
}

. "$ROOT_DIR/tools/ports/drivers/nslib.sh"
