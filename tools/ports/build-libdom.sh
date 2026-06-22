#!/bin/sh
# Build libdom as a static libdom.a for the b1nix userspace ABI. Freestanding
# curated compile of the portable C sources (no autotools), like the other
# ports. libdom is NetSurf's DOM; the smoke builds a document from HTML via the
# bundled libhubbub binding. We compile the core DOM (src/**) plus the hubbub
# binding (bindings/hubbub/parser.c); the libxml2/expat bindings are not built
# (HTML rendering only needs the hubbub path). Prints the install dir.
#
# M53 (NetSurf browser platform) — browser lib chain step 5. Depends on
# build-libhubbub.sh, build-libwapcaplet.sh, build-libparserutils.sh.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
DOM_VERSION="${DOM_VERSION:-0.4.2}"
TARBALL="libdom-${DOM_VERSION}-src.tar.gz"
URL="https://download.netsurf-browser.org/libs/releases/${TARBALL}"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

. "$ROOT_DIR/tools/toolchain-env.sh"

HUBBUB_DIR="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/ports/build-libhubbub.sh")"
LWC_DIR="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/ports/build-libwapcaplet.sh")"
PU_DIR="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/ports/build-libparserutils.sh")"

SRC_PARENT="$ROOT_DIR/build/netsurf-src"
SRC_DIR="$SRC_PARENT/libdom-${DOM_VERSION}"
BUILD_DIR="$ROOT_DIR/build/libdom-b1nix/$B1NIX_TRIPLET"
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

# -Iinclude for <dom/...>; -Ibindings/hubbub for the binding's local headers;
# the dep install include dirs for <hubbub/...>, <parserutils/...>,
# <libwapcaplet/...>.
CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fno-strict-aliasing -Db1nix
  -I$SRC_DIR/include -I$SRC_DIR/src -I$SRC_DIR/bindings/hubbub
  -I$HUBBUB_DIR/include -I$PU_DIR/include -I$LWC_DIR/include
  -Wno-implicit-function-declaration"

OBJS=""
for src in $(find "$SRC_DIR/src" -name '*.c' | sort) "$SRC_DIR/bindings/hubbub/parser.c"; do
  obj="$OBJ_DIR/$(echo "${src#$SRC_DIR/}" | tr '/' '_').o"
  # shellcheck disable=SC2086
  clang $CFLAGS -c "$src" -o "$obj"
  OBJS="$OBJS $obj"
done

# shellcheck disable=SC2086
"$AR_BIN" rcs "$INSTALL_DIR/lib/libdom.a" $OBJS
cp -R "$SRC_DIR/include/dom" "$INSTALL_DIR/include/"
mkdir -p "$INSTALL_DIR/include/dom/bindings/hubbub"
cp "$SRC_DIR/bindings/hubbub/errors.h" "$SRC_DIR/bindings/hubbub/parser.h" \
   "$INSTALL_DIR/include/dom/bindings/hubbub/"

echo "$INSTALL_DIR"
