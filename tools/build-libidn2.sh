#!/bin/sh
# Build static libidn2 for the b1nix userspace ABI.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
VER="${LIBIDN2_VERSION:-2.3.7}"
TARBALL="libidn2-${VER}.tar.gz"
URL="https://ftp.gnu.org/gnu/libidn/${TARBALL}"
WRAP="$ROOT_DIR/tools/b1nix-autotools-cc"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"
RANLIB_BIN="${RANLIB:-$(command -v llvm-ranlib 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ranlib)}"
# Per-architecture build identity + per-triplet source/build dirs.
. "$ROOT_DIR/tools/toolchain-env.sh"
HOST_TRIPLET="$B1NIX_TRIPLET"
SRC_PARENT="$ROOT_DIR/build/libidn2-src"
SRC_DIR="$SRC_PARENT/$HOST_TRIPLET/libidn2-${VER}"
BUILD_DIR="$ROOT_DIR/build/libidn2-b1nix/$HOST_TRIPLET"
INSTALL_DIR="$BUILD_DIR/install"
UNISTR_PREFIX="$ROOT_DIR/build/libunistring-b1nix/$HOST_TRIPLET/install"
if [ ! -f "$UNISTR_PREFIX/lib/libunistring.a" ]; then
  "$ROOT_DIR/tools/build-libunistring.sh" >/dev/null
fi

mkdir -p "$SRC_PARENT/$HOST_TRIPLET" "$BUILD_DIR"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/${TARBALL}"
  if [ ! -f "$tmp" ]; then
    if command -v curl >/dev/null 2>&1; then
      curl -L "$URL" -o "$tmp"
    elif command -v wget >/dev/null 2>&1; then
      wget -O "$tmp" "$URL"
    else
      echo "tools/build-libidn2.sh: need host curl or wget to fetch $URL" >&2
      exit 1
    fi
  fi
  tar -xzf "$tmp" -C "$SRC_PARENT/$HOST_TRIPLET"
fi

if ! grep -q 'b1nix\*' "$SRC_DIR/build-aux/config.sub"; then
  tmp_config_sub="$SRC_DIR/build-aux/config.sub.tmp"
  sed 's/twizzler\*/twizzler\* | b1nix\*/' "$SRC_DIR/build-aux/config.sub" > "$tmp_config_sub"
  mv "$tmp_config_sub" "$SRC_DIR/build-aux/config.sub"
  chmod +x "$SRC_DIR/build-aux/config.sub"
fi

make -C "$ROOT_DIR/userspace" -s "build/$B1NIX_ARCH/libb1nix.a" "build/$B1NIX_ARCH/crt/crt0.o" 1>&2

# libidn2's autotools tree is brittle after interrupted/failed builds in this
# cross environment. Recreate the build dir to avoid incremental corruption.
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
(
  cd "$BUILD_DIR"
  BUILD_TRIPLET="$("$SRC_DIR/config.guess" 2>/dev/null || echo "$(uname -m)-pc-linux-gnu")"
  export cross_compiling=yes
  CPPFLAGS="-I$UNISTR_PREFIX/include" \
  LDFLAGS="-L$UNISTR_PREFIX/lib" \
  LIBS="-lunistring" \
  ac_cv_func_strchrnul=yes \
  ac_cv_have_decl_strchrnul=yes \
  gl_cv_onwards_func_strchrnul=yes \
  ac_cv_func_strverscmp=yes \
  ac_cv_func_rawmemchr=yes \
  ac_cv_func_getline=yes \
  ac_cv_func_getdelim=yes \
  ac_cv_func_getdtablesize=yes \
  ac_cv_func_basename=yes \
  ac_cv_func_strerror=yes \
  "$SRC_DIR/configure" \
    --host="$HOST_TRIPLET" \
    --build="$BUILD_TRIPLET" \
    --prefix="$INSTALL_DIR" \
    --disable-shared --enable-static \
    --with-libunistring-prefix="$UNISTR_PREFIX" \
    --with-included-libunistring=no \
    --disable-nls \
    --disable-doc \
    --disable-rpath \
    CC="$WRAP" AR="$AR_BIN" RANLIB="$RANLIB_BIN"
)

# Configure is now forced to use external libunistring, but keep single-job
# build because this cross tree is sensitive to jobserver propagation.
# Prevent building the `idn2` CLI tool: it links gnulib's strchrnul which
# conflicts with libb1nix.a's whole-archive'd copy. curl only needs libidn2.a.
make -C "$BUILD_DIR" -j1 1>&2
# Install only the library + headers (skip `idn2` CLI tool which conflicts
# with libb1nix.a's whole-archive'd strchrnul). The targets live in lib/.
make -C "$BUILD_DIR/lib" install-libLTLIBRARIES 1>&2
make -C "$BUILD_DIR/lib" install-includeHEADERS 1>&2
# Strip gnulib replacement objects from libidn2.a that duplicate symbols
# already provided by libb1nix.a (when linked --whole-archive). These
# gnulib objects are only needed on systems without a POSIX libc; b1nix
# libc already provides strerror, getline, etc.
#
# NOTE: do NOT strip strverscmp.o — gnulib renames it to rpl_strverscmp
# (config.h does `#define strverscmp rpl_strverscmp`), so libidn2's own
# version.c calls rpl_strverscmp. That object exports only rpl_strverscmp
# (not plain strverscmp), so it does not collide with libb1nix's strverscmp;
# stripping it leaves rpl_strverscmp undefined and breaks the nsfb/curl link.
LIBA="$INSTALL_DIR/lib/libidn2.a"
CONFLICTS="rawmemchr.o strerror.o strerror-override.o"
for obj in $CONFLICTS; do
  llvm-ar d "$LIBA" "libgnu_la-$obj" 2>/dev/null || true
  llvm-ar d "$LIBA" "libunistring_la-$obj" 2>/dev/null || true
done
# Also strip basename-lgpl.o (b1nix provides basename)
llvm-ar d "$LIBA" "libgnu_la-basename-lgpl.o" 2>/dev/null || true
echo "$INSTALL_DIR"
