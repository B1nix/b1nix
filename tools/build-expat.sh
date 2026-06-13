#!/bin/sh
# Build expat (XML parser) as a static libexpat.a for the b1nix userspace ABI.
# Dependency of Fontconfig. Freestanding compile of the 3 core sources. Prints
# the install dir.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
EXPAT_VERSION="${EXPAT_VERSION:-2.6.2}"
TARBALL="expat-${EXPAT_VERSION}.tar.xz"
URL="https://github.com/libexpat/libexpat/releases/download/R_2_6_2/${TARBALL}"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

. "$ROOT_DIR/tools/toolchain-env.sh"

SRC_PARENT="$ROOT_DIR/build/ports-src"
SRC_DIR="$SRC_PARENT/expat-${EXPAT_VERSION}"
BUILD_DIR="$ROOT_DIR/build/expat-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
GEN_DIR="$BUILD_DIR/gen"
INSTALL_DIR="$BUILD_DIR/install"

mkdir -p "$SRC_PARENT" "$OBJ_DIR" "$GEN_DIR" "$INSTALL_DIR/include" \
  "$INSTALL_DIR/lib"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$TARBALL"
  [ -f "$tmp" ] || curl -L "$URL" -o "$tmp" 1>&2
  tar -xJf "$tmp" -C "$SRC_PARENT" 1>&2
fi

cat > "$GEN_DIR/expat_config.h" <<EOF
#define XML_GE 1
#define XML_DTD 1
#define XML_NS 1
#define XML_CONTEXT_BYTES 1024
#define BYTEORDER 1234
#define HAVE_MEMMOVE 1
#define XML_POOR_ENTROPY 1
#define PACKAGE_VERSION "$EXPAT_VERSION"
EOF

if [ "$B1NIX_ARCH" = "x86" ]; then
  TARGET="i686-unknown-elf"
else
  TARGET="x86_64-unknown-elf"
fi

# XML_POOR_ENTROPY: b1nix has no getrandom/arc4random; expat falls back to a
# weak hash salt (fine for parsing trusted local config). XML_GE=1 enables
# general entities (Fontconfig uses them).
CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fno-strict-aliasing -Db1nix
  -DXML_GE=1 -DXML_DTD -DXML_NS -DBYTEORDER=1234 -DXML_POOR_ENTROPY
  -DHAVE_MEMMOVE=1 -DHAVE_EXPAT_CONFIG_H=1 -I$GEN_DIR -I$SRC_DIR/lib
  -Wno-implicit-function-declaration"

OBJS=""
for base in xmlparse xmlrole xmltok; do
  obj="$OBJ_DIR/$base.o"
  clang $CFLAGS -c "$SRC_DIR/lib/$base.c" -o "$obj"
  OBJS="$OBJS $obj"
done

# shellcheck disable=SC2086
"$AR_BIN" rcs "$INSTALL_DIR/lib/libexpat.a" $OBJS
cp "$SRC_DIR/lib/expat.h" "$SRC_DIR/lib/expat_external.h" "$INSTALL_DIR/include/"

echo "$INSTALL_DIR"
