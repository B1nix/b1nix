#!/bin/sh
# Build zsh for the b1nix userspace ABI — the interactive shell that replaces
# GNU bash (GPLv3). zsh ships under its own permissive MIT-like licence.
#
# zsh is an autotools project, so this follows the pattern the other autotools
# ports use: the musl cc wrapper compiles and links a dynamic PIE, and a
# preseeded config.cache answers the run-time probes a cross-compile cannot
# execute. It additionally needs a terminal library, which b1nix gained with
# tools/ports/build-netbsd-curses.sh.
#
# Prints the install dir on stdout (the port-script convention); the binary is
# $INSTALL_DIR/bin/zsh.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT_DIR/tools/ports/drivers/common.sh"

VER="${ZSH_VERSION_NUM:-5.9}"
SRCNAME="zsh-$VER"
TARBALL="zsh-$VER.tar.xz"
URL="https://sourceforge.net/projects/zsh/files/zsh/$VER/$TARBALL/download"

SRC_PARENT="$ROOT_DIR/build/src/zsh/$B1NIX_TRIPLET"
SRC_DIR="$SRC_PARENT/$SRCNAME"
BUILD_DIR="$ROOT_DIR/build/$B1NIX_ARCH/ports/zsh"
INSTALL_DIR="$BUILD_DIR/install"
LOCKFILE="$BUILD_DIR/locks/build.lock"
CC_WRAPPER="$ROOT_DIR/tools/toolchain/bin/b1nix-musl-autotools-cc"
AR_BIN="$(port_ar)"
RANLIB_BIN="${RANLIB:-$(command -v llvm-ranlib 2>/dev/null || echo llvm-ranlib)}"
STRIP_BIN="${STRIP:-$(command -v llvm-strip 2>/dev/null || echo llvm-strip)}"

mkdir -p "$SRC_PARENT" "$BUILD_DIR" "$INSTALL_DIR" "$(dirname "$LOCKFILE")"

(
  flock -x 9
  if [ -x "$INSTALL_DIR/bin/zsh" ]; then
    echo "$INSTALL_DIR"
    exit 0
  fi

  : > "$BUILD_DIR/build.log"
  exec 2>>"$BUILD_DIR/build.log"

  port_fetch_tarball "$URL" "$SRC_PARENT/$TARBALL" "$SRC_PARENT" "$SRC_DIR"
  [ -d "$SRC_DIR" ] || { echo "build-zsh: source not found at $SRC_DIR" >&2; exit 1; }

  if [ ! -f "$(port_musl_lib)/libc.so" ]; then
    B1NIX_ARCH="$B1NIX_ARCH" sh "$ROOT_DIR/tools/ports/build-musl.sh" 1>&2
  fi

  CURSES_DIR="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/ports/build-netbsd-curses.sh")/usr"
  [ -f "$CURSES_DIR/lib/libterminfo.a" ] || {
    echo "build-zsh: netbsd-curses did not produce libterminfo.a at $CURSES_DIR" >&2; exit 1; }

  # zsh's config.sub (2020 vintage) validates the OS against a case list; add
  # b1nix so --host=x86_64-b1nix is accepted.
  if ! grep -q 'b1nix' "$SRC_DIR/config.sub"; then
    sed -i.bak 's/| os2\* | vos\* | palmos\* | uclinux\* | nucleus\* \\/| os2* | vos* | palmos* | uclinux* | nucleus* | b1nix* \\/' \
      "$SRC_DIR/config.sub"
    rm -f "$SRC_DIR/config.sub.bak"
  fi

  # Run-time probes a cross-compile cannot execute. These describe b1nix:
  # FIFOs, select and sigset_t all work (ash and bash both used them), /dev/fd
  # is present, realpath(NULL) and getcwd(NULL) allocate. The dynamic-module
  # answers are all "no" because this build is --disable-dynamic: zsh's modules
  # are linked into the binary rather than dlopen'd, which avoids depending on
  # zsh's own module loader on top of our ld.so.
  cat > "$SRC_DIR/config.cache" <<'EOF'
zsh_cv_sys_nis=no
zsh_cv_sys_nis_plus=no
zsh_cv_c_broken_signed_to_unsigned_casting=no
zsh_cv_sys_fifos=yes
zsh_cv_sys_fifo=yes
zsh_cv_sys_select=yes
zsh_cv_date_adj=no
zsh_cv_sys_path_dev_fd=/dev/fd
zsh_cv_sys_dynamic_clash_ok=no
zsh_cv_sys_dynamic_rtld_global=no
zsh_cv_sys_dynamic_execsyms=no
zsh_cv_sys_dynamic_strip_exe=no
zsh_cv_sys_dynamic_strip_lib=no
zsh_cv_func_tgetent_accepts_null=no
zsh_cv_func_tgetent_zero_success=no
zsh_cv_func_realpath_accepts_null=yes
zsh_cv_getcwd_malloc=yes
zsh_cv_shared_environ=yes
zsh_cv_sys_sigset_t=yes
zsh_cv_c_have_union_init=yes
zsh_cv_c_broken_isprint=no
zsh_cv_c_variable_length_arrays=yes
zsh_cv_sys_signed_to_unsigned_casting=yes
EOF
  # Which header the signal-name table is scraped from. configure probes the
  # BUILD host for this and on macOS settled on the SDK's sys/signal.h, so zsh
  # would have been built with the HOST's signal numbers. Point it at musl's,
  # where the numbered SIG* defines actually live for the target.
  printf 'zsh_cv_path_signal_h=%s/bits/signal.h\n' "$(port_musl_include)" \
    >> "$SRC_DIR/config.cache"

  # -D_GNU_SOURCE: configure's link probes find setresuid/setresgid in musl, but
  # musl only DECLARES them under _GNU_SOURCE, so options.c would otherwise call
  # them implicitly — an error for current clang.
  #
  # --without-tcsetpgrp: the probe needs to run a program on the target. b1nix
  # has tcsetpgrp (job control is exercised by M13-JC), and zsh falls back to
  # the TIOCSPGRP ioctl path, which is the same kernel call underneath.
  CPPFLAGS_ALL="-I$CURSES_DIR/include -D_GNU_SOURCE"
  ( cd "$SRC_DIR"
    BUILD_TRIPLET="$(./config.guess 2>/dev/null || echo "$(uname -m)-pc-linux-gnu")"
    export cross_compiling=yes
    B1NIX_ARCH="$B1NIX_ARCH" ./configure \
      --host="$B1NIX_TRIPLET" \
      --build="$BUILD_TRIPLET" \
      --prefix=/usr \
      --cache-file=config.cache \
      --disable-gdbm \
      --without-tcsetpgrp \
      --disable-dynamic \
      --enable-multibyte \
      --with-term-lib="terminfo" \
      CC="$CC_WRAPPER" AR="$AR_BIN" RANLIB="$RANLIB_BIN" \
      CPP="$CC_WRAPPER -E" \
      CPPFLAGS="$CPPFLAGS_ALL" LDFLAGS="-L$CURSES_DIR/lib" 1>&2 )

  # With --disable-dynamic, configure marks every optional module link=no, which
  # silently drops shell features rather than build steps. zsh/regex is the one
  # that matters most — without it [[ str =~ re ]] is a parse-time failure, and
  # that operator is part of what a bash replacement has to offer. The rest are
  # the small, dependency-free modules an interactive shell is expected to have.
  # Link them into the binary instead.
  for _mod in zsh/regex zsh/mathfunc zsh/stat zsh/system zsh/files zsh/zselect; do
    sed -i.bak "s|^name=$_mod \(.*\)link=no|name=$_mod \1link=static|" \
      "$SRC_DIR/config.modules"
    rm -f "$SRC_DIR/config.modules.bak"
  done

  # CPP again on the make line: signames.c is generated by preprocessing a
  # generated `#include <signal.h>`, and autoconf's default when $CPP is unset
  # is a BARE `cpp` — Apple's, reading the macOS SDK. That both produced the
  # wrong signal set and failed outright on the SDK's AvailabilityInternal.h,
  # which left /bin/zsh a stub and every SSH login (root's shell is zsh) dead.
  B1NIX_ARCH="$B1NIX_ARCH" make -C "$SRC_DIR" -j"${JOBS:-4}" \
    CPP="$CC_WRAPPER -E" CPPFLAGS="$CPPFLAGS_ALL" 1>&2
  [ -x "$SRC_DIR/Src/zsh" ] || { echo "build-zsh: no zsh binary produced" >&2; exit 1; }

  mkdir -p "$INSTALL_DIR/bin"
  cp -f "$SRC_DIR/Src/zsh" "$INSTALL_DIR/bin/zsh"
  "$STRIP_BIN" -S "$INSTALL_DIR/bin/zsh" 2>/dev/null || true

  # zsh's shell functions (completion, prompt themes) are plain scripts read at
  # runtime from $fpath. Ship them so an interactive zsh is more than the core.
  mkdir -p "$INSTALL_DIR/share/zsh/$VER"
  if [ -d "$SRC_DIR/Functions" ]; then
    cp -R "$SRC_DIR/Functions" "$INSTALL_DIR/share/zsh/$VER/functions"
  fi

  echo "$INSTALL_DIR"
) 9>"$LOCKFILE"
