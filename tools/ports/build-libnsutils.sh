#!/bin/sh
# Build libnsutils as a static libnsutils.a for the b1nix userspace ABI.
# Freestanding curated compile (no autotools). libnsutils is a small NetSurf
# helper library (base64, monotonic time, pread/pwrite). The default
# clock_gettime(CLOCK_MONOTONIC) path is used (b1nix libc provides it). Prints
# the install dir.
#
# M53 (NetSurf browser platform) — browser lib chain helper (step 6).

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
NSU_VERSION="${NSU_VERSION:-0.1.0}"
TARBALL="libnsutils-${NSU_VERSION}-src.tar.gz"
URL="https://download.netsurf-browser.org/libs/releases/${TARBALL}"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

. "$ROOT_DIR/tools/toolchain-env.sh"

SRC_PARENT="$ROOT_DIR/build/netsurf-src"
SRC_DIR="$SRC_PARENT/libnsutils-${NSU_VERSION}"
BUILD_DIR="$ROOT_DIR/build/libnsutils-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
INSTALL_DIR="$BUILD_DIR/install"

mkdir -p "$SRC_PARENT" "$OBJ_DIR" "$INSTALL_DIR/include" "$INSTALL_DIR/lib"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$TARBALL"
  [ -f "$tmp" ] || curl -L "$URL" -o "$tmp" 1>&2
  tar -xzf "$tmp" -C "$SRC_PARENT" 1>&2
fi

if [ "$B1NIX_ARCH" = "x86" ]; then
  TARGET="i686-unknown-elf"
else
  TARGET="x86_64-unknown-elf"
fi

CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fno-strict-aliasing -Db1nix -I$SRC_DIR/include -I$SRC_DIR/src
  -Wno-implicit-function-declaration"

OBJS=""
for rel in src/base64.c src/time.c src/unistd.c; do
  obj="$OBJ_DIR/$(echo "$rel" | tr '/' '_').o"
  # shellcheck disable=SC2086
  clang $CFLAGS -c "$SRC_DIR/$rel" -o "$obj"
  OBJS="$OBJS $obj"
done

# shellcheck disable=SC2086
"$AR_BIN" rcs "$INSTALL_DIR/lib/libnsutils.a" $OBJS
cp -R "$SRC_DIR/include/nsutils" "$INSTALL_DIR/include/"

# NetSurf 3.11 expects nsutils/assert.h (compile-time assert helper) which this
# libnsutils release predates. Provide the standard shim.
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

echo "$INSTALL_DIR"
