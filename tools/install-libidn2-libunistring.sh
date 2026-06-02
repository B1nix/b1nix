#!/bin/sh
# Build and install libunistring + libidn2 into a single prefix for b1nix.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="${1:-$ROOT_DIR/build/idn-b1nix/install}"

UNISTR_PREFIX="$ROOT_DIR/build/libunistring-b1nix/install"
IDN2_PREFIX="$ROOT_DIR/build/libidn2-b1nix/install"

if [ ! -f "$UNISTR_PREFIX/lib/libunistring.a" ]; then
  UNISTR_PREFIX="$("$ROOT_DIR/tools/build-libunistring.sh")"
fi
if [ ! -f "$IDN2_PREFIX/lib/libidn2.a" ]; then
  IDN2_PREFIX="$("$ROOT_DIR/tools/build-libidn2.sh")"
fi

mkdir -p "$PREFIX/include" "$PREFIX/lib"

cp -f "$UNISTR_PREFIX/lib/libunistring.a" "$PREFIX/lib/"
cp -f "$IDN2_PREFIX/lib/libidn2.a" "$PREFIX/lib/"

if [ -d "$UNISTR_PREFIX/include" ]; then
  cp -Rf "$UNISTR_PREFIX/include/." "$PREFIX/include/"
fi
if [ -d "$IDN2_PREFIX/include" ]; then
  cp -Rf "$IDN2_PREFIX/include/." "$PREFIX/include/"
fi

echo "Installed to: $PREFIX"
