#!/bin/sh
# tools/ports/drivers/cport.sh
#
# Generic driver for the "freestanding curated compile" portable-C ports that
# bypass the upstream build system entirely (zlib, libpng, expat, libwebp,
# openlibm, pixman, freetype, tinygl, ...): fetch the source, generate any config
# header, compile a curated source set with freestanding clang against the b1nix
# userspace libc headers, archive into a static lib, copy the public headers.
# Identical in spirit to nslib.sh but with an arbitrary fetch URL and a free-form
# compile hook (these ports each pick their own source subset). ccache is wired
# into the compile path (byte-identical output). Prints the install dir.
#
# A per-port manifest (tools/ports/build-<port>.sh) sets the CPORT_* variables
# and usually defines:
#   port_pre_build        gen config headers / resolve deps / set CPORT_CFLAGS.
#                         Has $SRC_DIR $BUILD_DIR $OBJ_DIR $GEN_DIR $INSTALL_DIR
#                         $TARGET; may append to CPORT_CFLAGS / CPORT_SOURCES.
#   port_build            compile the sources (call cport_cc per file); used when
#                         the source selection has skip rules / name mapping that
#                         a flat CPORT_SOURCES list can't express. Mutually
#                         exclusive with CPORT_SOURCES.
#   port_install_headers  copy public headers (else CPORT_HEADERS tokens).
#   port_post_install     trailing fixups.
#
# Manifest variables -------------------------------------------------------
#   CPORT_NAME      port id; build dir = build/$CPORT_NAME-b1nix/$TRIPLET
#   CPORT_ARCHIVE   output archive filename (libz.a, libpng16.a, libm.a, ...)
#   CPORT_SRC_PARENT  source-cache parent (default build/ports-src)
#   CPORT_SRCNAME   extracted source dir name under CPORT_SRC_PARENT
#   CPORT_URL/CPORT_TARBALL/CPORT_SENTINEL  default fetch inputs (or port_fetch)
#   CPORT_CFLAGS    port-specific compile flags (-D / -I); base freestanding
#                   flags + -O2 + ccache are added by the driver
#   CPORT_SOURCES   curated source list (rel to $SRC_DIR) when no port_build hook
#   CPORT_HEADERS   header install tokens (same grammar as nslib NSLIB_HEADERS)
#   PATCHES         patch entries (tools/patches/<...>) applied before pre_build

. "$ROOT_DIR/tools/ports/drivers/common.sh"

: "${CPORT_NAME:?manifest must set CPORT_NAME}"
: "${CPORT_ARCHIVE:?manifest must set CPORT_ARCHIVE}"
CPORT_SRC_PARENT="${CPORT_SRC_PARENT:-$ROOT_DIR/build/ports-src}"

AR_BIN="$(port_ar)"
CC="$(port_ccache_prefix)clang"
TARGET="$(port_clang_target)"

SRC_DIR="$CPORT_SRC_PARENT/${CPORT_SRCNAME:-$CPORT_NAME}"
BUILD_DIR="$ROOT_DIR/build/$CPORT_NAME-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
GEN_DIR="$BUILD_DIR/gen"
INSTALL_DIR="$BUILD_DIR/install"
mkdir -p "$CPORT_SRC_PARENT" "$OBJ_DIR" "$GEN_DIR" "$INSTALL_DIR/include" "$INSTALL_DIR/lib"

# --- fetch ----------------------------------------------------------------
if command -v port_fetch >/dev/null 2>&1; then
  port_fetch
else
  : "${CPORT_URL:?manifest must set CPORT_URL or define port_fetch}"
  : "${CPORT_TARBALL:?manifest must set CPORT_TARBALL or define port_fetch}"
  port_fetch_tarball "$CPORT_URL" "$CPORT_SRC_PARENT/$CPORT_TARBALL" "$CPORT_SRC_PARENT" \
    "${CPORT_SENTINEL:+$SRC_DIR/$CPORT_SENTINEL}"
  [ -n "${CPORT_SENTINEL:-}" ] || port_fetch_tarball "$CPORT_URL" \
    "$CPORT_SRC_PARENT/$CPORT_TARBALL" "$CPORT_SRC_PARENT" "$SRC_DIR"
fi

# --- patches + codegen/deps hook ------------------------------------------
if [ -n "${PATCHES:-}" ]; then
  # shellcheck disable=SC2086
  port_apply_patches "$SRC_DIR" $PATCHES
fi

# Base freestanding CFLAGS (the port appends its -D/-I via CPORT_CFLAGS, and may
# extend CPORT_CFLAGS inside port_pre_build once gen dirs / dep dirs are known).
CFLAGS_BASE="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fno-strict-aliasing -fPIC -Db1nix"

if command -v port_pre_build >/dev/null 2>&1; then port_pre_build; fi

CFLAGS="$CFLAGS_BASE ${CPORT_CFLAGS:-}"

# --- compile --------------------------------------------------------------
OBJS=""
cport_cc() {  # $1 source path, $2.. extra flags; object name = unique per source
  _src="$1"; shift
  case "$_src" in
  "$SRC_DIR"/*) _rel="${_src#"$SRC_DIR"/}" ;;
  *)            _rel="$(basename "$_src")" ;;
  esac
  _obj="$OBJ_DIR/$(echo "$_rel" | tr '/' '_').o"
  # shellcheck disable=SC2086
  $CC $CFLAGS "$@" -c "$_src" -o "$_obj"
  OBJS="$OBJS $_obj"
}

if command -v port_build >/dev/null 2>&1; then
  port_build
else
  for _s in ${CPORT_SOURCES:?manifest must set CPORT_SOURCES or define port_build}; do
    case "$_s" in /*) cport_cc "$_s" ;; *) cport_cc "$SRC_DIR/$_s" ;; esac
  done
fi

# --- archive (clean rebuild) ----------------------------------------------
rm -f "$INSTALL_DIR/lib/$CPORT_ARCHIVE"
# shellcheck disable=SC2086
"$AR_BIN" rcs "$INSTALL_DIR/lib/$CPORT_ARCHIVE" $OBJS

# --- headers --------------------------------------------------------------
if command -v port_install_headers >/dev/null 2>&1; then
  port_install_headers
elif [ -n "${CPORT_HEADERS:-}" ]; then
  for _t in $CPORT_HEADERS; do
    _mode="${_t%%:*}"; _rest="${_t#*:}"
    case "$_mode" in
    flat) cp "$SRC_DIR/$_rest" "$INSTALL_DIR/include/" ;;
    gen)  cp "$GEN_DIR/$_rest" "$INSTALL_DIR/include/" ;;
    tree) cp -R "$SRC_DIR/$_rest" "$INSTALL_DIR/include/" ;;
    glob) cp "$SRC_DIR/$_rest"/*.h "$INSTALL_DIR/include/" ;;
    sub)  _sub="${_rest%%:*}"; _file="${_rest#*:}"
          mkdir -p "$INSTALL_DIR/include/$_sub"; cp "$SRC_DIR/$_file" "$INSTALL_DIR/include/$_sub/" ;;
    *) echo "build-$CPORT_NAME.sh: bad header token: $_t" >&2; exit 1 ;;
    esac
  done
fi

if command -v port_post_install >/dev/null 2>&1; then port_post_install; fi

echo "$INSTALL_DIR"
