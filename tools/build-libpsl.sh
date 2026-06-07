#!/bin/sh
# Build static libpsl for the b1nix userspace ABI.
#
# libpsl is built with --disable-runtime (no IDNA library dependency) so the
# only external dependencies are the b1nix libc itself.  The Public Suffix List
# data is compiled in as a DAFSA (--enable-builtin) using the upstream Python
# script; wget uses the built-in list at runtime.
#
# Prints the install prefix on stdout (build noise goes to stderr) so callers
# can capture it for -I/-L flags.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
VER="${LIBPSL_VERSION:-0.21.5}"
TARBALL="libpsl-${VER}.tar.gz"
URL="https://github.com/rockdaboot/libpsl/releases/download/${VER}/${TARBALL}"
WRAP="$ROOT_DIR/tools/b1nix-autotools-cc"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"
RANLIB_BIN="${RANLIB:-$(command -v llvm-ranlib 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ranlib)}"
PYTHON_BIN="${PYTHON:-$(command -v python3 2>/dev/null || command -v python 2>/dev/null || true)}"

if [ -z "$PYTHON_BIN" ]; then
  echo "tools/build-libpsl.sh: need host python3 (or set PYTHON=/path/to/python)" >&2
  exit 1
fi

# Per-architecture build identity + per-triplet source/build dirs.
. "$ROOT_DIR/tools/toolchain-env.sh"
HOST_TRIPLET="$B1NIX_TRIPLET"
SRC_PARENT="$ROOT_DIR/build/libpsl-src"
SRC_DIR="$SRC_PARENT/$HOST_TRIPLET/libpsl-${VER}"
BUILD_DIR="$ROOT_DIR/build/libpsl-b1nix/$HOST_TRIPLET"
INSTALL_DIR="$BUILD_DIR/install"

mkdir -p "$SRC_PARENT/$HOST_TRIPLET" "$BUILD_DIR"

# ── Fetch & unpack ────────────────────────────────────────────────────────────
if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/${TARBALL}"
  if [ ! -f "$tmp" ]; then
    if command -v curl >/dev/null 2>&1; then
      curl -L "$URL" -o "$tmp" 1>&2
    elif command -v wget >/dev/null 2>&1; then
      wget -O "$tmp" "$URL" 1>&2
    else
      echo "tools/build-libpsl.sh: need host curl or wget to fetch $URL" >&2
      exit 1
    fi
  fi
  tar -xzf "$tmp" -C "$SRC_PARENT/$HOST_TRIPLET" 1>&2
fi

# ── Patch config.sub to recognise the b1nix triplet ─────────────────────────
if ! grep -q 'b1nix\*' "$SRC_DIR/build-aux/config.sub"; then
  tmp_cs="$SRC_DIR/build-aux/config.sub.tmp"
  sed 's/twizzler\*/twizzler* | b1nix*/' "$SRC_DIR/build-aux/config.sub" > "$tmp_cs"
  mv "$tmp_cs" "$SRC_DIR/build-aux/config.sub"
  chmod +x "$SRC_DIR/build-aux/config.sub"
fi

# ── Fix autotools timestamp ordering (avoid autoconf/automake re-runs) ────────
# All dependency source files (configure.ac, *.am, *.m4, version.txt, etc.)
# get a date deep in the past; generated outputs (configure, aclocal.m4,
# Makefile.in, *.in, config.h.in) get a more-recent past date.
# Both must remain older than "now" so automake's own sanity check passes.
# NOTE: the m4 sources (acinclude.m4, m4/*.m4) MUST be in the "old" bucket too,
# otherwise aclocal.m4 looks stale relative to them and `make` fires an
# am--refresh that calls the exact-version aclocal-1.16 (absent on the host).
find "$SRC_DIR" \( -name 'configure.ac' -o -name '*.am' -o -name '*.m4' \
    -o -name '*.txt' -o -name 'config.guess' -o -name 'config.sub' \) \
  -exec touch -t 200001010000 {} + 1>&2
find "$SRC_DIR" \( -name 'configure' -o -name 'aclocal.m4' -o -name 'Makefile.in' -o -name '*.in' -o -name 'config.h.in' \) \
  -exec touch -t 202001010000 {} + 1>&2

# ── Ensure b1nix libc stubs are up-to-date ───────────────────────────────────
make -C "$ROOT_DIR/userspace" -s build/libb1nix.a build/crt/crt0.o 1>&2

# ── Clean build dir to avoid stale state from interrupted builds ─────────────
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

(
  cd "$BUILD_DIR"
  BUILD_TRIPLET="$("$SRC_DIR/build-aux/config.guess" 2>/dev/null || echo "$(uname -m)-pc-linux-gnu")"
  export cross_compiling=yes

  # --disable-runtime: no IDNA library needed; libpsl uses its own punycode
  #   fallback for IDN labels.  wget still benefits from the full built-in PSL
  #   table for cookie/suffix matching on ASCII domains.
  # --enable-builtin: compile the PSL DAFSA table into the library.
  # --disable-nls: no gettext in b1nix userspace.
  # --disable-man: no xsltproc in the cross environment.
  # --without-psl-distfile: do not embed a path to a runtime PSL file.
  "$SRC_DIR/configure" \
    --host="$HOST_TRIPLET" \
    --build="$BUILD_TRIPLET" \
    --prefix="$INSTALL_DIR" \
    --disable-shared --enable-static \
    --disable-runtime \
    --enable-builtin \
    --disable-nls \
    --disable-man \
    --without-psl-distfile \
    CC="$WRAP" AR="$AR_BIN" RANLIB="$RANLIB_BIN" 1>&2
)

# Generate the builtin PSL DAFSA header directly with Python (avoids going
# through make which can trigger automake reconstruction).
PSL_DAT="$SRC_DIR/list/public_suffix_list.dat"
DAFSA_H="$BUILD_DIR/src/suffixes_dafsa.h"
"$PYTHON_BIN" "$SRC_DIR/src/psl-make-dafsa" --output-format=cxx+ "$PSL_DAT" "$DAFSA_H" 1>&2

# Disable autotools maintainer-mode rebuild rules. The release tarball ships
# all generated files (configure, *.in, aclocal.m4) and we never edit the .ac/.am
# sources, so any am--refresh / automake-1.16 / autoconf trigger is spurious —
# and those exact-version tools are absent on a generic host. Passing them as
# command-line make variables (which override in-Makefile assignments and are
# inherited by sub-makes via MAKEFLAGS) turns each regen rule into a no-op.
AUTOGEN_OFF="ACLOCAL=: AUTOCONF=: AUTOMAKE=: AUTOHEADER=: MAKEINFO=: am__maybe_remake_makefiles="

# Build only the library (src/ subdirectory — that's where libpsl.la lives).
make -C "$BUILD_DIR/src" -j"${JOBS:-4}" $AUTOGEN_OFF libpsl.la 1>&2
# install-libLTLIBRARIES installs libpsl.a (extracted from libpsl.la) under
# $INSTALL_DIR/lib.
make -C "$BUILD_DIR/src" $AUTOGEN_OFF install-libLTLIBRARIES 1>&2
# Install the generated libpsl.h header.
make -C "$BUILD_DIR/include" $AUTOGEN_OFF install 1>&2

echo "$INSTALL_DIR"
