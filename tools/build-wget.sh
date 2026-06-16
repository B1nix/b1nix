#!/bin/sh
# Build upstream GNU wget for the b1nix userspace ABI.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
WGET_VERSION="${WGET_VERSION:-1.21.4}"
WGET_TARBALL="wget-${WGET_VERSION}.tar.gz"
WGET_URL="https://ftpmirror.gnu.org/wget/${WGET_TARBALL}"
WRAP="$ROOT_DIR/tools/b1nix-autotools-cc"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"
RANLIB_BIN="${RANLIB:-$(command -v llvm-ranlib 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ranlib)}"
# Per-architecture build identity + per-triplet source/build dirs.
. "$ROOT_DIR/tools/toolchain-env.sh"
HOST_TRIPLET="$B1NIX_TRIPLET"
SRC_PARENT="$ROOT_DIR/build/wget-src"
SRC_DIR="$SRC_PARENT/$HOST_TRIPLET/wget-${WGET_VERSION}"
BUILD_DIR="$ROOT_DIR/build/wget-b1nix/$HOST_TRIPLET"

B1NIX_TLS="${B1NIX_TLS:-none}"

if [ "$B1NIX_TLS" != "none" ]; then
  echo "tools/build-wget.sh: TLS provider '$B1NIX_TLS' requested, but wget TLS wiring is not enabled yet; building HTTP-only wget." >&2
fi

# PCRE2 backs wget's --regex-type=pcre mode (--accept-regex/--reject-regex).
# Use the same static libpcre2-8 the standalone PCRE2 smoke is built against.
PCRE2_PREFIX="$ROOT_DIR/build/pcre2-b1nix/$HOST_TRIPLET/install"
if [ ! -f "$PCRE2_PREFIX/lib/libpcre2-8.a" ]; then
  if ! "$ROOT_DIR/tools/build-pcre2.sh" >/dev/null; then
    echo "tools/build-wget.sh: PCRE2 build failed" >&2
    exit 1
  fi
fi

OPENSSL_PREFIX="$ROOT_DIR/build/openssl-b1nix/$HOST_TRIPLET/install"
if [ ! -f "$OPENSSL_PREFIX/lib/libssl.a" ]; then
  if ! "$ROOT_DIR/tools/build-openssl.sh" >/dev/null; then
    echo "tools/build-wget.sh: OpenSSL build failed" >&2
    exit 1
  fi
fi

LIBIDN2_PREFIX="$ROOT_DIR/build/libidn2-b1nix/$HOST_TRIPLET/install"
LIBUNISTRING_PREFIX="$ROOT_DIR/build/libunistring-b1nix/$HOST_TRIPLET/install"
if [ ! -f "$LIBIDN2_PREFIX/lib/libidn2.a" ]; then
  if ! "$ROOT_DIR/tools/build-libidn2.sh" >/dev/null; then
    echo "tools/build-wget.sh: libidn2 build failed" >&2
    exit 1
  fi
fi

LIBPSL_PREFIX="$ROOT_DIR/build/libpsl-b1nix/$HOST_TRIPLET/install"
if [ ! -f "$LIBPSL_PREFIX/lib/libpsl.a" ]; then
  if ! "$ROOT_DIR/tools/build-libpsl.sh" >/dev/null; then
    echo "tools/build-wget.sh: libpsl build failed" >&2
    exit 1
  fi
fi

mkdir -p "$SRC_PARENT/$HOST_TRIPLET" "$BUILD_DIR"

SOURCE_READY="$SRC_DIR/.b1nix-source-ready"
if [ ! -f "$SOURCE_READY" ]; then
  tarball="$SRC_PARENT/${WGET_TARBALL}"
  if [ -f "$tarball" ] && ! gzip -t "$tarball" 2>/dev/null; then
    echo "tools/build-wget.sh: removing truncated cached archive $tarball" >&2
    rm -f "$tarball"
  fi
  if [ ! -f "$tarball" ]; then
    download="$tarball.part.$$"
    rm -f "$download"
    if command -v curl >/dev/null 2>&1; then
      curl -fL --retry 3 "$WGET_URL" -o "$download"
    elif command -v wget >/dev/null 2>&1; then
      wget -O "$download" "$WGET_URL"
    else
      echo "tools/build-wget.sh: need host curl or wget to fetch $WGET_URL" >&2
      exit 1
    fi
    if ! gzip -t "$download"; then
      echo "tools/build-wget.sh: downloaded archive is invalid: $WGET_URL" >&2
      rm -f "$download"
      exit 1
    fi
    mv "$download" "$tarball"
  fi
  rm -rf "$SRC_DIR"
  if ! tar -xzf "$tarball" -C "$SRC_PARENT/$HOST_TRIPLET"; then
    rm -rf "$SRC_DIR"
    exit 1
  fi
  touch "$SOURCE_READY"
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

make -C "$ROOT_DIR/userspace" -s "build/$B1NIX_ARCH/libb1nix.a" "build/$B1NIX_ARCH/crt/crt0.o"

# zlib for Content-Encoding (gzip/deflate) transfer support.
ZLIB_PREFIX="$("$ROOT_DIR/tools/build-zlib.sh" 2>/dev/null | tail -n 1)"
if [ -z "$ZLIB_PREFIX" ] || [ ! -f "$ZLIB_PREFIX/lib/libz.a" ]; then
  echo "tools/build-wget.sh: zlib build failed" >&2
  exit 1
fi

(
  cd "$BUILD_DIR"
  BUILD_TRIPLET="$("$SRC_DIR/build-aux/config.guess" 2>/dev/null || echo "$(uname -m)-pc-linux-gnu")"
  export cross_compiling=yes
  # PCRE2_CFLAGS/PCRE2_LIBS override pkg-config (absent in this cross env), so
  # configure trusts our static lib and defines HAVE_LIBPCRE2 -> --regex-type
  # pcre becomes available. Keep --disable-pcre (only PCRE2 is ported).
  "$SRC_DIR/configure" \
    --host="$HOST_TRIPLET" \
    --build="$BUILD_TRIPLET" \
    --disable-shared --enable-static \
    --with-ssl=openssl \
    --with-zlib \
    --with-libpsl \
    --enable-iri \
    --disable-pcre \
    --enable-threads=posix \
    --disable-nls \
    --enable-ipv6 \
    CC="$WRAP" AR="$AR_BIN" RANLIB="$RANLIB_BIN" \
    CPPFLAGS="-I$ZLIB_PREFIX/include" LDFLAGS="-L$ZLIB_PREFIX/lib" \
    gl_cv_func_getpass_good=yes \
    PCRE2_CFLAGS="-I$PCRE2_PREFIX/include" \
    PCRE2_LIBS="-L$PCRE2_PREFIX/lib -lpcre2-8" \
    OPENSSL_CFLAGS="-I$OPENSSL_PREFIX/include" \
    OPENSSL_LIBS="-L$OPENSSL_PREFIX/lib -lssl -lcrypto" \
    LIBIDN2_CFLAGS="-I$LIBIDN2_PREFIX/include -I$LIBUNISTRING_PREFIX/include" \
    LIBIDN2_LIBS="-L$LIBIDN2_PREFIX/lib -lidn2 -L$LIBUNISTRING_PREFIX/lib -lunistring" \
    LIBPSL_CFLAGS="-I$LIBPSL_PREFIX/include" \
    LIBPSL_LIBS="-L$LIBPSL_PREFIX/lib -lpsl"
)

make -C "$BUILD_DIR/lib" -j"${JOBS:-4}"
make -C "$BUILD_DIR/src" -j"${JOBS:-4}" wget
