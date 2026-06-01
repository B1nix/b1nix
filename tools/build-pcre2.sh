#!/bin/sh
# Build upstream PCRE2 (8-bit static library) for the b1nix userspace ABI.
#
# Prints the install prefix on stdout (build noise goes to stderr) so callers
# can capture it for -I/-L flags, mirroring tools/build-mbedtls.sh.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PCRE2_VERSION="${PCRE2_VERSION:-10.44}"
PCRE2_TARBALL="pcre2-${PCRE2_VERSION}.tar.gz"
PCRE2_URL="https://github.com/PCRE2Project/pcre2/releases/download/pcre2-${PCRE2_VERSION}/${PCRE2_TARBALL}"
SRC_DIR="$ROOT_DIR/build/pcre2-src/pcre2-${PCRE2_VERSION}"
BUILD_DIR="$ROOT_DIR/build/pcre2-b1nix"
INSTALL_DIR="$BUILD_DIR/install"
WRAP="$ROOT_DIR/tools/b1nix-autotools-cc"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"
RANLIB_BIN="${RANLIB:-$(command -v llvm-ranlib 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ranlib)}"

mkdir -p "$ROOT_DIR/build/pcre2-src" "$BUILD_DIR"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$ROOT_DIR/build/pcre2-src/${PCRE2_TARBALL}"
  if [ ! -f "$tmp" ]; then
    if command -v curl >/dev/null 2>&1; then
      curl -L "$PCRE2_URL" -o "$tmp" 1>&2
    elif command -v wget >/dev/null 2>&1; then
      wget -O "$tmp" "$PCRE2_URL" 1>&2
    else
      echo "tools/build-pcre2.sh: need host curl or wget to fetch $PCRE2_URL" >&2
      exit 1
    fi
  fi
  tar -xzf "$tmp" -C "$ROOT_DIR/build/pcre2-src" 1>&2
fi

# Force the autotools-generated files strictly newer than their sources so
# make never tries to re-run autoconf/automake (which are not installed). Use
# fixed timestamps to be immune to the host/NTP clock: sources in 2000,
# generated outputs in 2030.
# Generated files must be newer than their sources (so no autotools rerun) but
# still in the past relative to "now" (or automake's build-sanity check, which
# compares a freshly-created file against the distributed ones, fails).
find "$SRC_DIR" \( -name 'configure.ac' -o -name '*.am' -o -name '*.m4' \) \
  -exec touch -t 200001010000 {} + 1>&2
find "$SRC_DIR" \( -name 'configure' -o -name '*.in' \) \
  -exec touch -t 202001010000 {} + 1>&2

if ! grep -q 'b1nix\*' "$SRC_DIR/config.sub"; then
  tmp_config_sub="$SRC_DIR/config.sub.tmp"
  sed 's/twizzler\*/twizzler* | b1nix*/' "$SRC_DIR/config.sub" > "$tmp_config_sub"
  mv "$tmp_config_sub" "$SRC_DIR/config.sub"
fi

make -C "$ROOT_DIR/userspace" -s build/libb1nix.a build/crt/crt0.o 1>&2

(
  cd "$BUILD_DIR"
  "$SRC_DIR/configure" \
    --host=x86_64-b1nix \
    --prefix="$INSTALL_DIR" \
    --disable-shared --enable-static \
    --disable-jit \
    --disable-pcre2-16 --disable-pcre2-32 \
    --disable-pcre2grep-libz --disable-pcre2grep-libbz2 \
    --disable-pcre2test-libedit --disable-pcre2test-libreadline \
    CC="$WRAP" AR="$AR_BIN" RANLIB="$RANLIB_BIN" 1>&2
)

# Build and install only the 8-bit library (skip pcre2test/pcre2grep, which
# need host-grade stdio/file facilities b1nix userspace does not fully provide).
make -C "$BUILD_DIR" -j"${JOBS:-4}" libpcre2-8.la 1>&2
make -C "$BUILD_DIR" install-libLTLIBRARIES install-nodist_includeHEADERS 1>&2 \
  || make -C "$BUILD_DIR" install 1>&2

echo "$INSTALL_DIR"
