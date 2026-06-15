#!/bin/sh
# Build zlib as a static libz.a for the b1nix userspace ABI. Freestanding
# compile of the portable C sources (no autotools/configure), like the other
# ports. The gzip file-I/O wrappers (gz*.c) are omitted: they pull in unistd
# file descriptors that the in-memory deflate/inflate API consumers (libpng,
# the NetSurf image libs) never use. Prints the install dir.
#
# First M53 (NetSurf browser platform) image-codec dependency.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ZLIB_VERSION="${ZLIB_VERSION:-1.3.1}"
TARBALL="zlib-${ZLIB_VERSION}.tar.gz"
URL="https://github.com/madler/zlib/archive/refs/tags/v${ZLIB_VERSION}.tar.gz"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

. "$ROOT_DIR/tools/toolchain-env.sh"

SRC_PARENT="$ROOT_DIR/build/zlib-src"
SRC_DIR="$SRC_PARENT/zlib-${ZLIB_VERSION}"
BUILD_DIR="$ROOT_DIR/build/zlib-b1nix/$B1NIX_TRIPLET"
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

# zlib's zconf.h ships ready to use. Core sources only — skip the gz* file I/O
# wrappers and their unistd dependency. HAVE_HIDDEN keeps internal symbols off
# the export surface; -DZ_HAVE_STDARG_H matches the b1nix libc stdarg.
CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fno-strict-aliasing -Db1nix -DHAVE_HIDDEN -I$SRC_DIR
  -Wno-implicit-function-declaration"

# Core in-memory codec plus the gz* file wrappers: NetSurf's utils/hashtable.c
# reads gzipped data via gzopen/gzgets/gzclose, so the gz layer is needed for
# the browser port. (Earlier codec-only consumers never referenced these, but
# including them is harmless — they only pull in open/read/write/lseek/close.)
CORE="adler32 compress crc32 deflate infback inffast inflate inftrees trees uncompr zutil \
gzlib gzread gzwrite gzclose"
OBJS=""
for base in $CORE; do
  obj="$OBJ_DIR/$base.o"
  # shellcheck disable=SC2086
  clang $CFLAGS -c "$SRC_DIR/$base.c" -o "$obj"
  OBJS="$OBJS $obj"
done

# shellcheck disable=SC2086
"$AR_BIN" rcs "$INSTALL_DIR/lib/libz.a" $OBJS
cp "$SRC_DIR/zlib.h" "$SRC_DIR/zconf.h" "$INSTALL_DIR/include/"

echo "$INSTALL_DIR"
