#!/bin/sh
# Build static libidn2 for the b1nix userspace ABI.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
VER="${LIBIDN2_VERSION:-2.3.7}"
TARBALL="libidn2-${VER}.tar.gz"
URL="https://ftp.gnu.org/gnu/libidn/${TARBALL}"
SRC_DIR="$ROOT_DIR/build/libidn2-src/libidn2-${VER}"
BUILD_DIR="$ROOT_DIR/build/libidn2-b1nix"
INSTALL_DIR="$BUILD_DIR/install"
WRAP="$ROOT_DIR/tools/b1nix-autotools-cc"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"
RANLIB_BIN="${RANLIB:-$(command -v llvm-ranlib 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ranlib)}"
B1NIX_ARCH="${B1NIX_ARCH:-x86_64}"
if [ "$B1NIX_ARCH" = "x86" ]; then
  HOST_TRIPLET="i686-b1nix"
else
  HOST_TRIPLET="x86_64-b1nix"
fi
UNISTR_PREFIX="$ROOT_DIR/build/libunistring-b1nix/install"
if [ ! -f "$UNISTR_PREFIX/lib/libunistring.a" ]; then
  "$ROOT_DIR/tools/build-libunistring.sh" >/dev/null
fi

mkdir -p "$ROOT_DIR/build/libidn2-src" "$BUILD_DIR"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$ROOT_DIR/build/libidn2-src/${TARBALL}"
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
  tar -xzf "$tmp" -C "$ROOT_DIR/build/libidn2-src"
fi

if ! grep -q 'b1nix\*' "$SRC_DIR/build-aux/config.sub"; then
  tmp_config_sub="$SRC_DIR/build-aux/config.sub.tmp"
  sed 's/twizzler\*/twizzler\* | b1nix\*/' "$SRC_DIR/build-aux/config.sub" > "$tmp_config_sub"
  mv "$tmp_config_sub" "$SRC_DIR/build-aux/config.sub"
  chmod +x "$SRC_DIR/build-aux/config.sub"
fi

make -C "$ROOT_DIR/userspace" -s build/libb1nix.a build/crt/crt0.o 1>&2

# libidn2's autotools tree is brittle after interrupted/failed builds in this
# cross environment. Recreate the build dir to avoid incremental corruption.
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
(
  cd "$BUILD_DIR"
  CPPFLAGS="-I$UNISTR_PREFIX/include" \
  LDFLAGS="-L$UNISTR_PREFIX/lib" \
  LIBS="-lunistring" \
  "$SRC_DIR/configure" \
    --host="$HOST_TRIPLET" \
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
make -C "$BUILD_DIR" -j1 1>&2
make -C "$BUILD_DIR" install 1>&2

echo "$INSTALL_DIR"
