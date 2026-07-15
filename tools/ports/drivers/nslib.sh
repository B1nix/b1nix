#!/bin/sh
# tools/ports/drivers/nslib.sh
#
# Generic driver for the NetSurf-style "freestanding curated compile" libraries:
# fetch a -src.tar.gz from download.netsurf-browser.org (or pull the subtree out
# of the netsurf-all bundle), compile the portable C sources with freestanding
# clang against the b1nix userspace libc headers, archive into a static lib and
# copy the public headers. Prints the install dir as its only stdout line.
#
# A per-port manifest (tools/ports/build-<lib>.sh) sets the NSLIB_* variables
# below, then sources this file. It may also define optional hook functions:
#
#   port_pre_build        run after extract+patch, before the compile loop
#                         (code generation: perl / gperf / bison / flex / assets).
#                         Has $SRC_DIR $BUILD_DIR $OBJ_DIR $INSTALL_DIR $TARGET.
#   port_compile_extra    compile additional objects (use nslib_cc), e.g. a
#                         source outside $SRC_DIR or one needing per-file flags.
#   port_install_headers  override the default header install (NSLIB_HEADERS).
#   port_post_install     run after archive + headers (shims / sysroot copies).
#
# Manifest variables -------------------------------------------------------
#   NSLIB_NAME            port id; build dir = build/$NSLIB_NAME-b1nix/$TRIPLET
#   NSLIB_ARCHIVE         output archive filename (e.g. librosprite.a, liblwc.a)
#   NSLIB_SOURCE          "tarball" (default) | "bundle"
#     tarball:  NSLIB_VERSION  (required)
#               NSLIB_TARBALL  (default ${NSLIB_NAME}-${NSLIB_VERSION}-src.tar.gz)
#               NSLIB_URL      (default netsurf releases URL for the tarball)
#               NSLIB_SRCNAME  (default ${NSLIB_NAME}-${NSLIB_VERSION})
#     bundle:   NSLIB_BUNDLE_SUBDIR  subdir under build/netsurf-src/netsurf-all-*/
#               NSLIB_SENTINEL       a file under that subdir used to validate it
#   NSLIB_SOURCES         explicit space/newline list of *.c rel to $SRC_DIR
#   NSLIB_FIND_DIR        if NSLIB_SOURCES empty: find *.c under $SRC_DIR/<dir>
#   NSLIB_FIND_EXCLUDE    optional -name pattern excluded from the find
#   NSLIB_EXTRA_SOURCES   extra rel sources appended to the list
#   NSLIB_INC_DIRS        dirs rel to $SRC_DIR added as -I (default "include src")
#   NSLIB_DEPS            sibling build-*.sh scripts to build+stage; each adds
#                         -I<installdir>/include to CFLAGS (resolved in order)
#   NSLIB_CFLAGS          extra raw compile flags (defines, -include, -Wno-*)
#   NSLIB_HEADERS         header install tokens (see nslib_install_headers)
#   PATCHES               optional patch entries (tools/patches/<...>); applied
#                         by port_apply_patches before port_pre_build

. "$ROOT_DIR/tools/ports/drivers/common.sh"

: "${NSLIB_NAME:?manifest must set NSLIB_NAME}"
: "${NSLIB_ARCHIVE:?manifest must set NSLIB_ARCHIVE}"
NSLIB_SOURCE="${NSLIB_SOURCE:-tarball}"
NSLIB_INC_DIRS="${NSLIB_INC_DIRS:-include src}"

AR_BIN="$(port_ar)"
CC="$(port_ccache_prefix)clang"
TARGET="$(port_clang_target)"

SRC_PARENT="$ROOT_DIR/build/netsurf-src"
BUILD_DIR="$ROOT_DIR/build/$NSLIB_NAME-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
INSTALL_DIR="$BUILD_DIR/install"
LOCKFILE="$ROOT_DIR/build/$NSLIB_NAME-$B1NIX_TRIPLET.lock"
mkdir -p "$(dirname "$LOCKFILE")"

(
  flock -x 9
  if [ -f "$INSTALL_DIR/lib/$NSLIB_ARCHIVE" ]; then
    echo "$INSTALL_DIR"
    exit 0
  fi
  mkdir -p "$SRC_PARENT" "$OBJ_DIR" "$INSTALL_DIR/include" "$INSTALL_DIR/lib"

  # --- locate / fetch the source tree ---------------------------------------
  if [ "$NSLIB_SOURCE" = "bundle" ]; then
    : "${NSLIB_BUNDLE_SUBDIR:?bundle manifest must set NSLIB_BUNDLE_SUBDIR}"
    SRC_DIR="$(ls -d "$SRC_PARENT"/netsurf-all-*/"$NSLIB_BUNDLE_SUBDIR" 2>/dev/null | head -1)"
    if [ -z "$SRC_DIR" ] || { [ -n "${NSLIB_SENTINEL:-}" ] && [ ! -f "$SRC_DIR/$NSLIB_SENTINEL" ]; }; then
      echo "build-$NSLIB_NAME.sh: source not found (run build-netsurf-fb.sh first)" >&2
      exit 1
    fi
  else
    : "${NSLIB_VERSION:?tarball manifest must set NSLIB_VERSION}"
    NSLIB_TARBALL="${NSLIB_TARBALL:-${NSLIB_NAME}-${NSLIB_VERSION}-src.tar.gz}"
    NSLIB_URL="${NSLIB_URL:-https://download.netsurf-browser.org/libs/releases/${NSLIB_TARBALL}}"
    NSLIB_SRCNAME="${NSLIB_SRCNAME:-${NSLIB_NAME}-${NSLIB_VERSION}}"
    SRC_DIR="$SRC_PARENT/$NSLIB_SRCNAME"
    port_fetch_tarball "$NSLIB_URL" "$SRC_PARENT/$NSLIB_TARBALL" "$SRC_PARENT" "$SRC_DIR"
  fi

  # --- patches (idempotent) + codegen hook ----------------------------------
  if [ -n "${PATCHES:-}" ]; then
    # shellcheck disable=SC2086
    port_apply_patches "$SRC_DIR" $PATCHES
  fi
  if command -v port_pre_build >/dev/null 2>&1; then port_pre_build; fi

  # --- compose CFLAGS -------------------------------------------------------
  CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
    -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
    -O2 -fno-strict-aliasing -Db1nix"
  for _d in $NSLIB_INC_DIRS; do CFLAGS="$CFLAGS -I$SRC_DIR/$_d"; done
  for _dep in ${NSLIB_DEPS:-}; do
    _inst="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/ports/$_dep" | tail -1)"
    CFLAGS="$CFLAGS -I$_inst/include"
  done
  CFLAGS="$CFLAGS ${NSLIB_CFLAGS:-}"

  # --- compile helper (hooks may call it for extra/bespoke objects) ---------
  OBJS=""
  nslib_cc() {  # $1 source path, $2.. extra flags
    _src="$1"; shift
    case "$_src" in
    "$SRC_DIR"/*) _rel="${_src#"$SRC_DIR"/}" ;;
    *)           _rel="$(basename "$_src")" ;;
    esac
    _obj="$OBJ_DIR/$(echo "$_rel" | tr '/' '_').o"
    # shellcheck disable=SC2086
    $CC $CFLAGS "$@" -c "$_src" -o "$_obj"
    OBJS="$OBJS $_obj"
  }

  # --- assemble the source list ---------------------------------------------
  SOURCES="${NSLIB_SOURCES:-}"
  if [ -z "$SOURCES" ] && [ -n "${NSLIB_FIND_DIR:-}" ]; then
    if [ -n "${NSLIB_FIND_EXCLUDE:-}" ]; then
      SOURCES="$(find "$SRC_DIR/$NSLIB_FIND_DIR" -name '*.c' ! -name "$NSLIB_FIND_EXCLUDE" | sort)"
    else
      SOURCES="$(find "$SRC_DIR/$NSLIB_FIND_DIR" -name '*.c' | sort)"
    fi
  fi

  for _s in $SOURCES; do
    case "$_s" in
    /*) nslib_cc "$_s" ;;               # absolute (from find)
    *)  nslib_cc "$SRC_DIR/$_s" ;;      # relative to SRC_DIR (explicit list)
    esac
  done
  for _s in ${NSLIB_EXTRA_SOURCES:-}; do
    case "$_s" in /*) nslib_cc "$_s" ;; *) nslib_cc "$SRC_DIR/$_s" ;; esac
  done
  if command -v port_compile_extra >/dev/null 2>&1; then port_compile_extra; fi

  # --- archive --------------------------------------------------------------
  rm -f "$INSTALL_DIR/lib/$NSLIB_ARCHIVE"
  # shellcheck disable=SC2086
  "$AR_BIN" rcs "$INSTALL_DIR/lib/$NSLIB_ARCHIVE" $OBJS

  # --- headers --------------------------------------------------------------
  nslib_install_headers() {
    for _t in $NSLIB_HEADERS; do
      _mode="${_t%%:*}"; _rest="${_t#*:}"
      case "$_mode" in
      flat) cp "$SRC_DIR/$_rest" "$INSTALL_DIR/include/" ;;
      tree) cp -R "$SRC_DIR/$_rest" "$INSTALL_DIR/include/" ;;
      glob) cp "$SRC_DIR/$_rest"/*.h "$INSTALL_DIR/include/" ;;
      sub)  _sub="${_rest%%:*}"; _file="${_rest#*:}"
            mkdir -p "$INSTALL_DIR/include/$_sub"
            cp "$SRC_DIR/$_file" "$INSTALL_DIR/include/$_sub/" ;;
      *) echo "build-$NSLIB_NAME.sh: bad header token: $_t" >&2; exit 1 ;;
      esac
    done
  }
  if command -v port_install_headers >/dev/null 2>&1; then
    port_install_headers
  elif [ -n "${NSLIB_HEADERS:-}" ]; then
    nslib_install_headers
  fi

  if command -v port_post_install >/dev/null 2>&1; then port_post_install; fi

  echo "$INSTALL_DIR"
) 9>"$LOCKFILE"
