#!/bin/sh
# Build samurai — the 0BSD C reimplementation of Ninja — for b1nix.
#
# Ships as /bin/samu with a /bin/ninja alias, giving the in-guest toolchain a
# build-graph executor with no GNU code in it. Samurai is ~4k lines of plain
# C99 with no configure step and no dependencies beyond POSIX, so this builds it
# by naming its translation units directly and letting the musl cc wrapper
# produce the usual dynamic PIE (PT_INTERP=/lib/ld-musl-x86_64.so.1).
#
# Prints the install dir on stdout (the port-script convention).
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT_DIR/tools/ports/drivers/common.sh"

VER="${SAMURAI_VERSION:-1.2}"
SRCNAME="samurai-$VER"
TARBALL="$SRCNAME.tar.gz"
URL="https://github.com/michaelforney/samurai/releases/download/$VER/$TARBALL"

SRC_PARENT="$ROOT_DIR/build/src/samurai"
SRC_DIR="$SRC_PARENT/$SRCNAME"
BUILD_DIR="$ROOT_DIR/build/$B1NIX_ARCH/ports/samurai"
INSTALL_DIR="$BUILD_DIR/install"
LOCKFILE="$BUILD_DIR/locks/build.lock"
CC_WRAPPER="$ROOT_DIR/tools/toolchain/bin/b1nix-musl-autotools-cc"

# samurai's own build.ninja/Makefile list these; keeping the list here avoids
# depending on a make implementation to build the make replacement.
SOURCES="build.c deps.c env.c graph.c htab.c log.c parse.c samu.c scan.c tool.c tree.c util.c"

mkdir -p "$SRC_PARENT" "$BUILD_DIR" "$INSTALL_DIR/bin" "$(dirname "$LOCKFILE")"

(
  flock -x 9
  if [ -x "$INSTALL_DIR/bin/samu" ]; then
    echo "$INSTALL_DIR"
    exit 0
  fi

  : > "$BUILD_DIR/build.log"
  exec 2>>"$BUILD_DIR/build.log"

  port_fetch_tarball "$URL" "$SRC_PARENT/$TARBALL" "$SRC_PARENT" "$SRC_DIR"
  [ -d "$SRC_DIR" ] || { echo "build-samurai: source not found at $SRC_DIR" >&2; exit 1; }

  # musl must exist before the wrapper can link against libc.so + Scrt1.o.
  if [ ! -f "$(port_musl_lib)/libc.so" ]; then
    B1NIX_ARCH="$B1NIX_ARCH" sh "$ROOT_DIR/tools/ports/build-musl.sh" 1>&2
  fi

  objs=""
  for s in $SOURCES; do
    o="$BUILD_DIR/${s%.c}.o"
    B1NIX_ARCH="$B1NIX_ARCH" "$CC_WRAPPER" -std=c99 -O2 -D_POSIX_C_SOURCE=200809L \
      -c "$SRC_DIR/$s" -o "$o" 1>&2
    objs="$objs $o"
  done
  # shellcheck disable=SC2086
  B1NIX_ARCH="$B1NIX_ARCH" "$CC_WRAPPER" -o "$INSTALL_DIR/bin/samu" $objs 1>&2

  echo "$INSTALL_DIR"
) 9>"$LOCKFILE"
