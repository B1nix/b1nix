#!/bin/sh
# tools/ports/drivers/autotools.sh
#
# Generic driver for autotools-based ports (curl, dropbear, pcre2, wget, ...):
# fetch + extract, apply patches, configure via b1nix-autotools-cc with
# config.cache, make + install, emit pkg-config .pc, print install dir.
#
# A per-port manifest (tools/ports/build-<port>.sh) sets the AUTOTOOLS_*
# variables and may define hooks:
#   port_pre_configure   run after fetch+patch, before configure (set deps, etc.)
#   port_configure       override the default configure invocation
#   port_build           override the default make invocation
#   port_install         override the default make install
#   port_post_install    trailing fixups (e.g. emit .pc file)
#
# Manifest variables -------------------------------------------------------
#   AUTOTOOLS_NAME       port id; build dir = build/$AUTOTOOLS_NAME-b1nix/$TRIPLET
#   AUTOTOOLS_URL        source tarball URL
#   AUTOTOOLS_TARBALL    tarball filename (default: derived from URL)
#   AUTOTOOLS_VERSION    version string (used in tarball/dir naming)
#   AUTOTOOLS_SRCNAME    extracted dir name (default: ${AUTOTOOLS_NAME}-${VERSION})
#   AUTOTOOLS_SENTINEL   marker file inside src dir to skip re-extract
#   AUTOTOOLS_CONFIGURE  extra configure flags (default: --disable-shared --enable-static)
#   AUTOTOOLS_MAKE       make target(s) (default: all)
#   AUTOTOOLS_INSTALL    make install target(s) (default: install)
#   AUTOTOOLS_PREFIX     install prefix (default: $BUILD_DIR/install)
#   AUTOTOOLS_CC         compiler wrapper (default: b1nix-autotools-cc)
#   AUTOTOOLS_PC         pkg-config .pc file content (heredoc in manifest)

. "$ROOT_DIR/tools/ports/drivers/common.sh"

: "${AUTOTOOLS_NAME:?manifest must set AUTOTOOLS_NAME}"
: "${AUTOTOOLS_URL:?manifest must set AUTOTOOLS_URL}"
: "${AUTOTOOLS_VERSION:?manifest must set AUTOTOOLS_VERSION}"

AUTOTOOLS_TARBALL="${AUTOTOOLS_TARBALL:-${AUTOTOOLS_NAME}-${AUTOTOOLS_VERSION}.tar.gz}"
AUTOTOOLS_SRCNAME="${AUTOTOOLS_SRCNAME:-${AUTOTOOLS_NAME}-${AUTOTOOLS_VERSION}}"
AUTOTOOLS_CC="${AUTOTOOLS_CC:-$ROOT_DIR/tools/toolchain/bin/b1nix-autotools-cc}"
AR_BIN="$(port_ar)"
RANLIB_BIN="${RANLIB:-$(command -v llvm-ranlib 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ranlib)}"

SRC_PARENT="$ROOT_DIR/build/${AUTOTOOLS_NAME}-src"
SRC_DIR="$SRC_PARENT/$B1NIX_TRIPLET/$AUTOTOOLS_SRCNAME"
BUILD_DIR="$ROOT_DIR/build/${AUTOTOOLS_NAME}-b1nix/$B1NIX_TRIPLET"
INSTALL_DIR="${AUTOTOOLS_PREFIX:-$BUILD_DIR/install}"
mkdir -p "$SRC_PARENT/$B1NIX_TRIPLET" "$BUILD_DIR" "$INSTALL_DIR"

# --- fetch + extract ------------------------------------------------------
if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$AUTOTOOLS_TARBALL"
  if [ ! -f "$tmp" ]; then
    port_fetch_tarball "$AUTOTOOLS_URL" "$tmp" "$SRC_PARENT/$B1NIX_TRIPLET" \
      "${AUTOTOOLS_SENTINEL:-$SRC_DIR}"
  fi
  # Handle case where tarball extracts to a different name than expected
  if [ ! -d "$SRC_DIR" ]; then
    for d in "$SRC_PARENT/$B1NIX_TRIPLET"/*; do
      if [ -d "$d" ] && [ -f "$d/configure" -o -f "$d/configure.ac" ]; then
        mv "$d" "$SRC_DIR"
        break
      fi
    done
  fi
fi

# --- patches --------------------------------------------------------------
if [ -n "${PATCHES:-}" ]; then
  # shellcheck disable=SC2086
  port_apply_patches "$SRC_DIR" $PATCHES
fi

# --- patch config.sub to accept b1nix ------------------------------------
if [ -f "$SRC_DIR/config.sub" ] && ! grep -q 'b1nix' "$SRC_DIR/config.sub"; then
  tmp_sub="$SRC_DIR/config.sub.new"
  sed -e 's/| fiwix\* /| fiwix* | b1nix* /' \
      -e 's/| -mint\*/| -mint* | -b1nix*/' \
      -e 's/| -none\*/| -none* | -b1nix*/' \
      -e 's/| -elf\*/| -elf* | -b1nix*/' \
      -e 's/| -limine\*/| -limine* | -b1nix*/' \
      -e 's/| -os2\*/| -os2* | -b1nix*/' \
      "$SRC_DIR/config.sub" > "$tmp_sub"
  mv "$tmp_sub" "$SRC_DIR/config.sub"
fi

# --- touch autotools outputs to avoid re-running autoconf/automake ---------
# Only touch if configure is newer than the current time (to avoid touching
# files that already have correct timestamps from a previous build).
if [ -f "$SRC_DIR/configure" ] && [ "$(stat -c %Y "$SRC_DIR/configure" 2>/dev/null || echo 0)" -gt "$(date +%s)" ]; then
  find "$SRC_DIR" \( -name 'configure.ac' -o -name '*.am' -o -name '*.m4' \) \
    -exec touch -t 200001010000 {} + 1>&2
  find "$SRC_DIR" \( -name 'configure' -o -name '*.in' \) \
    -exec touch -t 202001010000 {} + 1>&2
fi

# --- ensure userspace libc is built --------------------------------------
make -C "$ROOT_DIR/userspace" -s "build/$B1NIX_ARCH/libb1nix.a" "build/$B1NIX_ARCH/crt/crt0.o" 1>&2

# --- pre-configure hook ---------------------------------------------------
if command -v port_pre_configure >/dev/null 2>&1; then
  # Port hooks may need these variables
  export B1NIX_ARCH
  port_pre_configure
fi

# --- configure ------------------------------------------------------------
if command -v port_configure >/dev/null 2>&1; then
  port_configure
else
  CONFIGURE_FLAGS="${AUTOTOOLS_CONFIGURE:---disable-shared --enable-static}"
  (
    cd "$BUILD_DIR"
    BUILD_TRIPLET="$("$SRC_DIR/config.guess" 2>/dev/null || echo "$(uname -m)-pc-linux-gnu")"
    export cross_compiling=yes
    "$SRC_DIR/configure" \
      --host="$B1NIX_TRIPLET" \
      --build="$BUILD_TRIPLET" \
      --prefix="$INSTALL_DIR" \
      $CONFIGURE_FLAGS \
      CC="$AUTOTOOLS_CC" AR="$AR_BIN" RANLIB="$RANLIB_BIN" \
      1>&2
  )
fi

# --- disable autotools maintainer-mode rebuild rules ----------------------
# Release tarballs ship all generated files (configure, *.in, aclocal.m4) and
# we never edit the .ac/.am sources, so any am--refresh / automake / autoconf
# trigger is spurious — and those exact-version tools are absent on the host.
# Export as environment variables so sub-makes and am--refresh recipes see them.
export ACLOCAL=":" AUTOCONF=":" AUTOMAKE=":" AUTOHEADER=":" MAKEINFO=":"
export am__maybe_remake_makefiles=""
_AUTOTOOLS_NOREGEN="ACLOCAL=: AUTOCONF=: AUTOMAKE=: AUTOHEADER=: MAKEINFO=: am__maybe_remake_makefiles="

# --- build ----------------------------------------------------------------
if command -v port_build >/dev/null 2>&1; then
  port_build
else
  # shellcheck disable=SC2086
  make -C "$BUILD_DIR" -j"${JOBS:-4}" $_AUTOTOOLS_NOREGEN ${AUTOTOOLS_MAKE:-all} 1>&2
fi

# --- install --------------------------------------------------------------
if command -v port_install >/dev/null 2>&1; then
  port_install
else
  # shellcheck disable=SC2086
  make -C "$BUILD_DIR" $_AUTOTOOLS_NOREGEN ${AUTOTOOLS_INSTALL:-install} 1>&2
fi

# --- pkg-config .pc file --------------------------------------------------
if [ -n "${AUTOTOOLS_PC:-}" ]; then
  _pc_dir="$INSTALL_DIR/lib/pkgconfig"
  mkdir -p "$_pc_dir"
  printf '%s\n' "$AUTOTOOLS_PC" > "$_pc_dir/${AUTOTOOLS_NAME}.pc"
fi

# --- post-install hook ----------------------------------------------------
if command -v port_post_install >/dev/null 2>&1; then
  port_post_install
fi

# Driver prints install dir unless manifest has already printed (e.g. crypto phase)
if [ -z "${AUTOTOOLS_ECHO+x}" ]; then
  echo "$INSTALL_DIR"
fi
