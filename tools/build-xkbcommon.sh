#!/bin/sh
# Build libxkbcommon (keymap compile + state, no X11) as a static
# libxkbcommon.a for the b1nix userspace ABI. Freestanding compile; meson
# bypassed. The keymap parser is generated from parser.y with a modern bison
# (brew). Prints the install dir.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
XKB_VERSION="${XKB_VERSION:-1.5.0}"
TARBALL="libxkbcommon-${XKB_VERSION}.tar.xz"
URL="https://xkbcommon.org/download/${TARBALL}"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"
BISON="${BISON:-$(command -v /opt/homebrew/opt/bison/bin/bison 2>/dev/null || command -v bison)}"

. "$ROOT_DIR/tools/toolchain-env.sh"

SRC_PARENT="$ROOT_DIR/build/ports-src"
SRC_DIR="$SRC_PARENT/libxkbcommon-${XKB_VERSION}"
S="$SRC_DIR/src"
BUILD_DIR="$ROOT_DIR/build/xkbcommon-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
GEN_DIR="$BUILD_DIR/gen"
INSTALL_DIR="$BUILD_DIR/install"

mkdir -p "$SRC_PARENT" "$OBJ_DIR" "$GEN_DIR" "$INSTALL_DIR/lib"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$TARBALL"
  [ -f "$tmp" ] || curl -L "$URL" -o "$tmp" 1>&2
  tar -xJf "$tmp" -C "$SRC_PARENT" 1>&2
fi

# Generate the keymap parser (parser.y needs bison >= 3). The -p prefix
# renames yylex/yyerror/yyparse to _xkbcommon_* to match scanner.c (this is
# what meson passes).
"$BISON" --defines="$GEN_DIR/parser.h" -o "$GEN_DIR/parser.c" -p _xkbcommon_ \
  "$S/xkbcomp/parser.y" 2>/dev/null

cat > "$GEN_DIR/config.h" <<EOF
#define HAVE_UNISTD_H 1
#define HAVE___BUILTIN_EXPECT 1
#define HAVE_STRNDUP 1
#define HAVE_ASPRINTF 1
#define HAVE_VASPRINTF 1
#define DFLT_XKB_CONFIG_ROOT "/share/X11/xkb"
#define DFLT_XKB_CONFIG_EXTRA_PATH "/etc/xkb"
#define XLOCALEDIR "/share/X11/locale"
#define DEFAULT_XKB_RULES "evdev"
#define DEFAULT_XKB_MODEL "pc105"
#define DEFAULT_XKB_LAYOUT "us"
#define DEFAULT_XKB_VARIANT NULL
#define DEFAULT_XKB_OPTIONS NULL
EOF

if [ "$B1NIX_ARCH" = "x86" ]; then
  TARGET="i686-unknown-elf"
else
  TARGET="x86_64-unknown-elf"
fi

CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fno-strict-aliasing -Db1nix -DHAVE_CONFIG_H
  -I$GEN_DIR -I$S -I$S/xkbcomp -I$SRC_DIR/include
  -Wno-implicit-function-declaration -Wno-incompatible-pointer-types -Wno-int-conversion -Wno-implicit-int -Wno-incompatible-function-pointer-types"

CORE="atom context context-priv keymap keymap-priv keysym keysym-utf state \
text utf8 util-list utils"
COMP="action ast-build compat expr include keycodes keymap keymap-dump \
keywords rules scanner symbols types vmod xkbcomp"

OBJS=""
cc1() {
  obj="$OBJ_DIR/$(echo "$1" | tr / _).o"
  clang $CFLAGS -c "$2" -o "$obj"
  OBJS="$OBJS $obj"
}
for c in $CORE; do cc1 "$c" "$S/$c.c"; done
for c in $COMP; do cc1 "xkbcomp_$c" "$S/xkbcomp/$c.c"; done
cc1 parser "$GEN_DIR/parser.c"

# shellcheck disable=SC2086
"$AR_BIN" rcs "$INSTALL_DIR/lib/libxkbcommon.a" $OBJS
cp -r "$SRC_DIR/include/"* "$INSTALL_DIR/" 2>/dev/null
mkdir -p "$INSTALL_DIR/include"
cp -r "$SRC_DIR/include/"* "$INSTALL_DIR/include/"

echo "$INSTALL_DIR"
