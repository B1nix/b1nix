#!/bin/sh
# Build static libunistring for the b1nix userspace ABI.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
VER="${LIBUNISTRING_VERSION:-1.2}"
TARBALL="libunistring-${VER}.tar.gz"
URL="https://ftp.gnu.org/gnu/libunistring/${TARBALL}"
WRAP="$ROOT_DIR/tools/b1nix-autotools-cc"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"
RANLIB_BIN="${RANLIB:-$(command -v llvm-ranlib 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ranlib)}"
# Per-architecture build identity + per-triplet source/build dirs.
. "$ROOT_DIR/tools/toolchain-env.sh"
HOST_TRIPLET="$B1NIX_TRIPLET"
SRC_PARENT="$ROOT_DIR/build/libunistring-src"
SRC_DIR="$SRC_PARENT/$HOST_TRIPLET/libunistring-${VER}"
BUILD_DIR="$ROOT_DIR/build/libunistring-b1nix/$HOST_TRIPLET"
INSTALL_DIR="$BUILD_DIR/install"
mkdir -p "$SRC_PARENT/$HOST_TRIPLET" "$BUILD_DIR"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/${TARBALL}"
  if [ ! -f "$tmp" ]; then
    if command -v curl >/dev/null 2>&1; then
      curl -L "$URL" -o "$tmp"
    elif command -v wget >/dev/null 2>&1; then
      wget -O "$tmp" "$URL"
    else
      echo "tools/build-libunistring.sh: need host curl or wget to fetch $URL" >&2
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

if ! grep -q 'defined b1nix' "$SRC_DIR/lib/fseterr.c"; then
  tmp_fseterr="$SRC_DIR/lib/fseterr.c.tmp"
  sed 's/#elif 0                             \/\* unknown  \*\//#elif defined b1nix\n  fp->error = 1;\n#elif 0                             \/\* unknown  \*\//' "$SRC_DIR/lib/fseterr.c" > "$tmp_fseterr"
  mv "$tmp_fseterr" "$SRC_DIR/lib/fseterr.c"
fi

make -C "$ROOT_DIR/userspace" -s build/libb1nix.a build/crt/crt0.o 1>&2

mkdir -p "$BUILD_DIR"
(
  cd "$BUILD_DIR"
  BUILD_TRIPLET="$("$SRC_DIR/config.guess" 2>/dev/null || echo "$(uname -m)-pc-linux-gnu")"
  export cross_compiling=yes
  "$SRC_DIR/configure" \
    --host="$HOST_TRIPLET" \
    --build="$BUILD_TRIPLET" \
    --prefix="$INSTALL_DIR" \
    --disable-shared --enable-static \
    --disable-nls \
    CC="$WRAP" AR="$AR_BIN" RANLIB="$RANLIB_BIN"
)

make -C "$BUILD_DIR/lib" -j"${JOBS:-4}" 1>&2
make -C "$BUILD_DIR/lib" install 1>&2

echo "$INSTALL_DIR"
