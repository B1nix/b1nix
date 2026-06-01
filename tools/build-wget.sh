#!/bin/sh
# Build upstream GNU wget for the b1nix userspace ABI.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
WGET_VERSION="${WGET_VERSION:-1.21.4}"
WGET_TARBALL="wget-${WGET_VERSION}.tar.gz"
WGET_URL="https://ftpmirror.gnu.org/wget/${WGET_TARBALL}"
SRC_DIR="$ROOT_DIR/build/wget-src/wget-${WGET_VERSION}"
BUILD_DIR="$ROOT_DIR/build/wget-b1nix"
WRAP="$ROOT_DIR/tools/b1nix-autotools-cc"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"
RANLIB_BIN="${RANLIB:-$(command -v llvm-ranlib 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ranlib)}"
B1NIX_TLS="${B1NIX_TLS:-none}"

if [ "$B1NIX_TLS" != "none" ]; then
  echo "tools/build-wget.sh: TLS provider '$B1NIX_TLS' requested, but wget TLS wiring is not enabled yet; building HTTP-only wget." >&2
fi

# PCRE2 backs wget's --regex-type=pcre mode (--accept-regex/--reject-regex).
# Use the same static libpcre2-8 the standalone PCRE2 smoke is built against.
PCRE2_PREFIX="$ROOT_DIR/build/pcre2-b1nix/install"
if [ ! -f "$PCRE2_PREFIX/lib/libpcre2-8.a" ]; then
  if ! "$ROOT_DIR/tools/build-pcre2.sh" >/dev/null; then
    echo "tools/build-wget.sh: PCRE2 build failed" >&2
    exit 1
  fi
fi

mkdir -p "$ROOT_DIR/build/wget-src" "$BUILD_DIR"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$ROOT_DIR/build/wget-src/${WGET_TARBALL}"
  if [ ! -f "$tmp" ]; then
    if command -v curl >/dev/null 2>&1; then
      curl -L "$WGET_URL" -o "$tmp"
    elif command -v wget >/dev/null 2>&1; then
      wget -O "$tmp" "$WGET_URL"
    else
      echo "tools/build-wget.sh: need host curl or wget to fetch $WGET_URL" >&2
      exit 1
    fi
  fi
  tar -xzf "$tmp" -C "$ROOT_DIR/build/wget-src"
fi

if ! grep -q 'b1nix\*' "$SRC_DIR/build-aux/config.sub"; then
  tmp_config_sub="$SRC_DIR/build-aux/config.sub.tmp"
  sed 's/twizzler\*/twizzler\* | b1nix\*/' "$SRC_DIR/build-aux/config.sub" > "$tmp_config_sub"
  mv "$tmp_config_sub" "$SRC_DIR/build-aux/config.sub"
  chmod +x "$SRC_DIR/build-aux/config.sub"
fi

if ! grep -q 'defined b1nix' "$SRC_DIR/lib/fpurge.c"; then
  tmp_fpurge="$SRC_DIR/lib/fpurge.c.tmp"
  sed 's/# else/# elif defined b1nix\n  fp->has_unget = 0;\n  return 0;\n# else/' "$SRC_DIR/lib/fpurge.c" > "$tmp_fpurge"
  mv "$tmp_fpurge" "$SRC_DIR/lib/fpurge.c"
fi

if ! grep -q 'defined b1nix' "$SRC_DIR/lib/freading.c"; then
  tmp_freading="$SRC_DIR/lib/freading.c.tmp"
  sed 's/# else/# elif defined b1nix\n  (void)fp;\n  return true;\n# else/' "$SRC_DIR/lib/freading.c" > "$tmp_freading"
  mv "$tmp_freading" "$SRC_DIR/lib/freading.c"
fi

if ! grep -q 'defined b1nix' "$SRC_DIR/lib/fseeko.c"; then
  tmp_fseeko="$SRC_DIR/lib/fseeko.c.tmp"
  sed -e 's/#elif defined EPLAN9.*/#elif defined b1nix\n  if (!fp->has_unget)\n#elif defined EPLAN9/' \
      -e 's/#elif defined __MINT__/#elif defined b1nix\n      fp->eof = 0;\n#elif defined __MINT__/' \
      "$SRC_DIR/lib/fseeko.c" > "$tmp_fseeko"
  mv "$tmp_fseeko" "$SRC_DIR/lib/fseeko.c"
fi

if ! grep -q 'defined b1nix' "$SRC_DIR/lib/getdtablesize.c"; then
  tmp_getdtablesize="$SRC_DIR/lib/getdtablesize.c.tmp"
  sed 's/#else/#elif defined b1nix\nint\ngetdtablesize (void)\n{\n  return 1024;\n}\n#else/' "$SRC_DIR/lib/getdtablesize.c" > "$tmp_getdtablesize"
  mv "$tmp_getdtablesize" "$SRC_DIR/lib/getdtablesize.c"
fi

make -C "$ROOT_DIR/userspace" -s build/libb1nix.a build/crt/crt0.o

(
  cd "$BUILD_DIR"
  # PCRE2_CFLAGS/PCRE2_LIBS override pkg-config (absent in this cross env), so
  # configure trusts our static lib and defines HAVE_LIBPCRE2 -> --regex-type
  # pcre becomes available. Keep --disable-pcre (only PCRE2 is ported).
  "$SRC_DIR/configure" \
    --host=x86_64-b1nix \
    --disable-shared --enable-static \
    --without-ssl \
    --without-zlib \
    --disable-iri \
    --disable-pcre \
    --disable-threads \
    --disable-nls \
    --disable-ipv6 \
    --disable-ntlm \
    CC="$WRAP" AR="$AR_BIN" RANLIB="$RANLIB_BIN" \
    PCRE2_CFLAGS="-I$PCRE2_PREFIX/include" \
    PCRE2_LIBS="-L$PCRE2_PREFIX/lib -lpcre2-8"
)

make -C "$BUILD_DIR/lib" -j"${JOBS:-4}"
make -C "$BUILD_DIR/src" -j"${JOBS:-4}" wget
