#!/bin/sh
# Build netbsd-curses — the NetBSD curses/terminfo libraries, as packaged for
# musl systems by sabotage-linux — for the b1nix userspace ABI.
#
# zsh needs a terminal-handling library and refuses to configure without one.
# b1nix had none: bash carried its own bundled termcap, so nothing in the tree
# ever provided tgetent/tigetstr. netbsd-curses fills that gap under a BSD
# licence, which is what the GNU-free goal wants — GNU ncurses is permissively
# licensed but is still a GNU package, and it is the heavier of the two.
#
# The library compiles the common terminal descriptions straight into
# libterminfo (compiled_terms.c), so a b1nix rootfs needs no /usr/share/terminfo
# database for xterm/vt100/ansi/linux to work.
#
# Prints the install dir on stdout (the port-script convention).
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT_DIR/tools/ports/drivers/common.sh"

VER="${NETBSD_CURSES_VERSION:-0.3.2}"
SRCNAME="netbsd-curses-$VER"
TARBALL="netbsd-curses-$VER.tar.gz"
URL="https://github.com/sabotage-linux/netbsd-curses/archive/refs/tags/v$VER.tar.gz"

SRC_PARENT="$ROOT_DIR/build/src/netbsd-curses"
SRC_DIR="$SRC_PARENT/$SRCNAME"
BUILD_DIR="$ROOT_DIR/build/$B1NIX_ARCH/ports/netbsd-curses"
INSTALL_DIR="$BUILD_DIR/install"
LOCKFILE="$BUILD_DIR/locks/build.lock"
CC_WRAPPER="$ROOT_DIR/tools/toolchain/bin/b1nix-musl-autotools-cc"
AR_BIN="$(port_ar)"
RANLIB_BIN="${RANLIB:-$(command -v llvm-ranlib 2>/dev/null || echo llvm-ranlib)}"

mkdir -p "$SRC_PARENT" "$BUILD_DIR" "$INSTALL_DIR" "$(dirname "$LOCKFILE")"

(
  flock -x 9
  if [ -f "$INSTALL_DIR/usr/lib/libterminfo.a" ]; then
    echo "$INSTALL_DIR"
    exit 0
  fi

  : > "$BUILD_DIR/build.log"
  exec 2>>"$BUILD_DIR/build.log"

  port_fetch_tarball "$URL" "$SRC_PARENT/$TARBALL" "$SRC_PARENT" "$SRC_DIR"
  [ -d "$SRC_DIR" ] || { echo "build-netbsd-curses: source not found at $SRC_DIR" >&2; exit 1; }

  if [ ! -f "$(port_musl_lib)/libc.so" ]; then
    B1NIX_ARCH="$B1NIX_ARCH" sh "$ROOT_DIR/tools/ports/build-musl.sh" 1>&2
  fi

  # config.mak is the build's own hook for extra flags. It matters that the
  # flags arrive this way rather than as `make CFLAGS=...`: a command-line
  # assignment overrides even the makefile's `+=`, which would drop the
  # per-object -D flags below and the tree's own warning settings.
  #
  # The two tic/ rules are a gap upstream: -DTERMINFO_COMPAT is applied to
  # libterminfo/compile.o but not to tic/compile.o (the same file, symlinked),
  # so the target `tic` fails to link on _ti_promote. Cross builds are the only
  # configuration that builds a target tic at all, which is why upstream does
  # not trip over it.
  cat > "$SRC_DIR/config.mak" <<'EOF'
CFLAGS += -O2 -fPIC
tic/compile.o: CPPFLAGS+= -DTERMINFO_COMPILE -DTERMINFO_DB -DTERMINFO_COMPAT
tic/tic.o: CPPFLAGS+= -DTERMINFO_COMPILE -DTERMINFO_DB -DTERMINFO_COMPAT
EOF

  # HOSTCC != CC is what switches the build into cross mode: tic and nbperf are
  # built twice, once for the host (to generate the hash tables and the compiled
  # terminal descriptions) and once for the target.
  make -C "$SRC_DIR" -j"${JOBS:-4}" \
    HOSTCC="cc" CC="$CC_WRAPPER" AR="$AR_BIN" RANLIB="$RANLIB_BIN" \
    PREFIX=/usr all-static 1>&2

  make -C "$SRC_DIR" \
    HOSTCC="cc" CC="$CC_WRAPPER" AR="$AR_BIN" RANLIB="$RANLIB_BIN" \
    PREFIX=/usr DESTDIR="$INSTALL_DIR" \
    install-headers install-stalibs 1>&2

  echo "$INSTALL_DIR"
) 9>"$LOCKFILE"
