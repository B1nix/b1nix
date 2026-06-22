#!/bin/sh
# Build libparserutils as a static libparserutils.a for the b1nix userspace ABI.
# Freestanding curated compile of the portable C sources (no autotools), like
# the other ports. libparserutils provides NetSurf's input streams, buffers and
# its OWN bundled charset codecs (8859/ASCII/ext8/UTF-8/UTF-16) — built with
# -DWITHOUT_ICONV_FILTER so it never touches a system iconv (b1nix has none).
# Prints the install dir.
#
# M53 (NetSurf browser platform) — browser lib chain step 2. libhubbub and
# libcss/libdom all parse their input through this.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
PU_VERSION="${PU_VERSION:-0.2.5}"
TARBALL="libparserutils-${PU_VERSION}-src.tar.gz"
URL="https://download.netsurf-browser.org/libs/releases/${TARBALL}"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"
PERL_BIN="${PERL:-$(command -v perl)}"

. "$ROOT_DIR/tools/toolchain/env.sh"

SRC_PARENT="$ROOT_DIR/build/netsurf-src"
SRC_DIR="$SRC_PARENT/libparserutils-${PU_VERSION}"
BUILD_DIR="$ROOT_DIR/build/libparserutils-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
INSTALL_DIR="$BUILD_DIR/install"

mkdir -p "$SRC_PARENT" "$OBJ_DIR" "$INSTALL_DIR/include" "$INSTALL_DIR/lib"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$TARBALL"
  [ -f "$tmp" ] || curl -L "$URL" -o "$tmp" 1>&2
  tar -xzf "$tmp" -C "$SRC_PARENT" 1>&2
fi

# Charset-alias table: the upstream build generates src/charset/aliases.inc from
# build/Aliases via a perl script. Run it once (it is host perl, arch-agnostic).
if [ ! -f "$SRC_DIR/src/charset/aliases.inc" ]; then
  ( cd "$SRC_DIR" && "$PERL_BIN" build/make-aliases.pl ) 1>&2
fi

if [ "$B1NIX_ARCH" = "x86" ]; then
  TARGET="i686-unknown-elf"
else
  TARGET="x86_64-unknown-elf"
fi

# WITHOUT_ICONV_FILTER → use the bundled codecs, no <iconv.h>. -Isrc for the
# internal "charset/..."/"utils/..." headers; quoted aliases.inc resolves next
# to aliases.c automatically.
CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fno-strict-aliasing -Db1nix -DWITHOUT_ICONV_FILTER
  -I$SRC_DIR/include -I$SRC_DIR/src -Wno-implicit-function-declaration"

SOURCES="
  src/charset/aliases.c
  src/charset/codec.c
  src/charset/codecs/codec_8859.c
  src/charset/codecs/codec_ascii.c
  src/charset/codecs/codec_ext8.c
  src/charset/codecs/codec_utf16.c
  src/charset/codecs/codec_utf8.c
  src/charset/encodings/utf16.c
  src/charset/encodings/utf8.c
  src/input/filter.c
  src/input/inputstream.c
  src/utils/buffer.c
  src/utils/errors.c
  src/utils/stack.c
  src/utils/vector.c
"

OBJS=""
for rel in $SOURCES; do
  obj="$OBJ_DIR/$(echo "$rel" | tr '/' '_').o"
  # shellcheck disable=SC2086
  clang $CFLAGS -c "$SRC_DIR/$rel" -o "$obj"
  OBJS="$OBJS $obj"
done

# shellcheck disable=SC2086
"$AR_BIN" rcs "$INSTALL_DIR/lib/libparserutils.a" $OBJS
cp -R "$SRC_DIR/include/parserutils" "$INSTALL_DIR/include/"

echo "$INSTALL_DIR"
