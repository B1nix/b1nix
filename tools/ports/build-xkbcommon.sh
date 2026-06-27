#!/bin/sh
# Build libxkbcommon (keymap compile + state, no X11) as a static
# libxkbcommon.a for the b1nix userspace ABI. Uses the cport driver.
# The keymap parser is generated from parser.y with a modern bison (brew).
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

CPORT_NAME=xkbcommon
CPORT_ARCHIVE=libxkbcommon.a
CPORT_SRCNAME="libxkbcommon-${XKB_VERSION:-1.5.0}"
CPORT_URL="https://xkbcommon.org/download/libxkbcommon-${XKB_VERSION:-1.5.0}.tar.xz"
CPORT_TARBALL="libxkbcommon-${XKB_VERSION:-1.5.0}.tar.xz"
CPORT_HEADERS="tree:include"
BISON="${BISON:-$(command -v /opt/homebrew/opt/bison/bin/bison 2>/dev/null || command -v bison)}"

port_pre_build() {
  S="$SRC_DIR/src"
  # Generate the keymap parser (parser.y needs bison >= 3)
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

  CPORT_CFLAGS="-DHAVE_CONFIG_H -I$GEN_DIR -I$S -I$S/xkbcomp -I$SRC_DIR/include \
    -Wno-implicit-function-declaration -Wno-incompatible-pointer-types \
    -Wno-int-conversion -Wno-implicit-int -Wno-incompatible-function-pointer-types"
}

port_build() {
  CORE="atom context context-priv keymap keymap-priv keysym keysym-utf state \
text utf8 util-list utils"
  COMP="action ast-build compat expr include keycodes keymap keymap-dump \
keywords rules scanner symbols types vmod xkbcomp"

  for c in $CORE; do cport_cc "$S/$c.c"; done
  for c in $COMP; do cport_cc "$S/xkbcomp/$c.c"; done
  cport_cc "$GEN_DIR/parser.c"
}

. "$ROOT_DIR/tools/ports/drivers/cport.sh"
