#!/bin/sh
# Build upstream libffi as a static library for the b1nix userspace ABI.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
LIBFFI_VERSION="${LIBFFI_VERSION:-3.5.2}"
LIBFFI_TARBALL="libffi-${LIBFFI_VERSION}.tar.gz"
LIBFFI_URL="https://github.com/libffi/libffi/releases/download/v${LIBFFI_VERSION}/${LIBFFI_TARBALL}"
WRAP="$ROOT_DIR/tools/b1nix-autotools-cc"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"
RANLIB_BIN="${RANLIB:-$(command -v llvm-ranlib 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ranlib)}"

. "$ROOT_DIR/tools/toolchain-env.sh"
HOST_TRIPLET="$B1NIX_TRIPLET"
SRC_PARENT="$ROOT_DIR/build/libffi-src"
SRC_DIR="$SRC_PARENT/$HOST_TRIPLET/libffi-${LIBFFI_VERSION}"
BUILD_DIR="$ROOT_DIR/build/libffi-b1nix/$HOST_TRIPLET"
INSTALL_DIR="$BUILD_DIR/install"

mkdir -p "$SRC_PARENT/$HOST_TRIPLET" "$BUILD_DIR"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$LIBFFI_TARBALL"
  if [ ! -f "$tmp" ]; then
    curl -L "$LIBFFI_URL" -o "$tmp" 1>&2
  fi
  tar -xzf "$tmp" -C "$SRC_PARENT/$HOST_TRIPLET" 1>&2
fi

if ! grep -q 'b1nix\*' "$SRC_DIR/config.sub"; then
  tmp_config_sub="$SRC_DIR/config.sub.tmp"
  sed 's/twizzler\*/twizzler* | b1nix*/' "$SRC_DIR/config.sub" > "$tmp_config_sub"
  mv "$tmp_config_sub" "$SRC_DIR/config.sub"
fi

make -C "$ROOT_DIR/userspace" -s "build/$B1NIX_ARCH/libb1nix.a" \
  "build/$B1NIX_ARCH/crt/crt0.o" 1>&2

if [ ! -f "$BUILD_DIR/Makefile" ]; then
  (
    cd "$BUILD_DIR"
    BUILD_TRIPLET="$("$SRC_DIR/config.guess" 2>/dev/null || echo "$(uname -m)-pc-linux-gnu")"
    export cross_compiling=yes
    "$SRC_DIR/configure" \
      --host="$HOST_TRIPLET" \
      --build="$BUILD_TRIPLET" \
      --prefix="$INSTALL_DIR" \
      --disable-shared --enable-static \
      --disable-docs --disable-multi-os-directory \
      CC="$WRAP" AR="$AR_BIN" RANLIB="$RANLIB_BIN" 1>&2
  )
fi

# libffi's Makefile has a parallel-build race: the archiving step (ar) can fire
# before src/x86/sysv.o is assembled, so a clean -j build fails with
# "src/x86/sysv.o: No such file". libffi is tiny — build it serially to avoid
# the race entirely. ponytail: -j1 here, the few seconds lost are irrelevant.
make -C "$BUILD_DIR" -j1 1>&2
make -C "$BUILD_DIR" install 1>&2
echo "$INSTALL_DIR"
