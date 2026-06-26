#!/bin/sh
# Build the IJG libjpeg as a static libjpeg.a for the b1nix userspace ABI.
# Freestanding compile of the portable C library (no SIMD, no autotools). A
# hand-written jconfig.h replaces the configure-generated one. The memory
# source/dest API (jpeg_mem_src/jpeg_mem_dest) is used by consumers, so no file
# I/O is required. Prints the install dir.
#
# M53 (NetSurf browser platform) image-codec dependency.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
JPEG_VERSION="${JPEG_VERSION:-9f}"
TARBALL="jpegsrc.v${JPEG_VERSION}.tar.gz"
URL="https://www.ijg.org/files/jpegsrc.v${JPEG_VERSION}.tar.gz"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"
CC="clang"; command -v ccache >/dev/null 2>&1 && [ "${B1NIX_NO_CCACHE:-0}" != "1" ] && CC="ccache clang"

. "$ROOT_DIR/tools/toolchain/env.sh"

SRC_PARENT="$ROOT_DIR/build/libjpeg-src"
SRC_DIR="$SRC_PARENT/jpeg-${JPEG_VERSION}"
BUILD_DIR="$ROOT_DIR/build/libjpeg-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
GEN_DIR="$BUILD_DIR/gen"
INSTALL_DIR="$BUILD_DIR/install"

mkdir -p "$SRC_PARENT" "$OBJ_DIR" "$GEN_DIR" "$INSTALL_DIR/include" "$INSTALL_DIR/lib"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$TARBALL"
  [ -f "$tmp" ] || curl -L "$URL" -o "$tmp" 1>&2
  tar -xzf "$tmp" -C "$SRC_PARENT" 1>&2
fi

# jconfig.h for a modern freestanding clang target (ANSI C, unsigned types
# available, stddef/stdlib present, no far pointers / DOS quirks).
cat > "$GEN_DIR/jconfig.h" <<'EOF'
#define HAVE_PROTOTYPES 1
#define HAVE_UNSIGNED_CHAR 1
#define HAVE_UNSIGNED_SHORT 1
#undef CHAR_IS_UNSIGNED
#define HAVE_STDDEF_H 1
#define HAVE_STDLIB_H 1
#undef NEED_BSD_STRINGS
#undef NEED_SYS_TYPES_H
#undef NEED_FAR_POINTERS
#undef NEED_SHORT_EXTERNAL_NAMES
#undef INCOMPLETE_TYPES_BROKEN
#define MEM_SRCDST_SUPPORTED 1
EOF

if [ "$B1NIX_ARCH" = "x86" ]; then
  TARGET="i686-unknown-elf"
else
  TARGET="x86_64-unknown-elf"
fi

CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fno-strict-aliasing -Db1nix -I$GEN_DIR -I$SRC_DIR
  -Wno-implicit-function-declaration -Wno-shift-negative-value"

# IJG library sources (the LIBSOURCES set from makefile.ansi) — no application
# or image-format-helper files. jmemnobs = no-backing-store memory manager.
LIB="jaricom jcapimin jcapistd jcarith jccoefct jccolor jcdctmgr jchuff jcinit \
jcmainct jcmarker jcmaster jcomapi jcparam jcprepct jcsample jctrans jdapimin \
jdapistd jdarith jdatadst jdatasrc jdcoefct jdcolor jddctmgr jdhuff jdinput \
jdmainct jdmarker jdmaster jdmerge jdpostct jdsample jdtrans jerror jfdctflt \
jfdctfst jfdctint jidctflt jidctfst jidctint jmemmgr jmemnobs jquant1 \
jquant2 jutils"
OBJS=""
for base in $LIB; do
  obj="$OBJ_DIR/$base.o"
  # shellcheck disable=SC2086
  $CC $CFLAGS -c "$SRC_DIR/$base.c" -o "$obj"
  OBJS="$OBJS $obj"
done

# shellcheck disable=SC2086
"$AR_BIN" rcs "$INSTALL_DIR/lib/libjpeg.a" $OBJS
cp "$SRC_DIR/jpeglib.h" "$SRC_DIR/jmorecfg.h" "$SRC_DIR/jerror.h" \
   "$GEN_DIR/jconfig.h" "$INSTALL_DIR/include/"

echo "$INSTALL_DIR"
