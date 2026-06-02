#!/bin/sh
# Build Dropbear SSH (and its bundled libtomcrypt/libtommath crypto) for the
# b1nix userspace ABI.
#
# Two phases, selected by the first argument:
#   crypto  — configure + build only the bundled crypto archives
#             (libtomcrypt/libtomcrypt.a, libtommath/libtommath.a). Used by the
#             M32b crypto-baseline item.
#   all     — (default) build the full static dropbearmulti binary (server +
#             dropbearkey + client). Used by the sshd service item.
#
# Modelled on tools/build-openssl.sh / tools/build-wget.sh.

set -eu

PHASE="${1:-all}"

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DB_VERSION="${DROPBEAR_VERSION:-2022.83}"
DB_TARBALL="dropbear-${DB_VERSION}.tar.bz2"
DB_URL="https://matt.ucc.asn.au/dropbear/releases/${DB_TARBALL}"
WRAP="$ROOT_DIR/tools/b1nix-autotools-cc"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"
RANLIB_BIN="${RANLIB:-$(command -v llvm-ranlib 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ranlib)}"

# Per-architecture build identity (B1NIX_ARCH -> triplet).
. "$ROOT_DIR/tools/toolchain-env.sh"
HOST_TRIPLET="$B1NIX_TRIPLET"
# Per-triplet source tree so x86 and x86_64 never share objects.
SRC_PARENT="$ROOT_DIR/build/dropbear-src"
SRC_DIR="$SRC_PARENT/$HOST_TRIPLET/dropbear-${DB_VERSION}"

mkdir -p "$SRC_PARENT/$HOST_TRIPLET"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/${DB_TARBALL}"
  if [ ! -f "$tmp" ]; then
    if command -v curl >/dev/null 2>&1; then
      curl -L "$DB_URL" -o "$tmp"
    elif command -v wget >/dev/null 2>&1; then
      wget -O "$tmp" "$DB_URL"
    else
      echo "tools/build-dropbear.sh: need host curl or wget to fetch $DB_URL" >&2
      exit 1
    fi
  fi
  tar -xjf "$tmp" -C "$SRC_PARENT/$HOST_TRIPLET"
fi

# Patch config.sub to accept the x86_64-b1nix host triplet. Modern config.sub
# validate the OS against a big case list ending in "| fiwix* | mlibc* )".
if [ -f "$SRC_DIR/config.sub" ] && ! grep -q 'b1nix' "$SRC_DIR/config.sub"; then
  tmp_sub="$SRC_DIR/config.sub.new"
  sed 's/| fiwix\* | mlibc\* )/| fiwix* | mlibc* | b1nix* )/' \
    "$SRC_DIR/config.sub" > "$tmp_sub"
  mv "$tmp_sub" "$SRC_DIR/config.sub"
fi

# b1nix local options: turn off everything that needs OS facilities we lack
# (zlib, PAM, utmp/wtmp/lastlog, syslog) so the build is self-contained.
cat > "$SRC_DIR/localoptions.h" <<'EOF'
/* b1nix Dropbear build options (overrides default_options.h). */
#define DROPBEAR_SMALL_CODE 1
#define DO_HOST_LOOKUP 0
#define DROPBEAR_SYSLOG 0
#define DEBUG_TRACE 0
/* No utmp/wtmp/lastlog/PAM on b1nix. */
#define DROPBEAR_PASSWORD_AUTH 1
#define DROPBEAR_SVR_PASSWORD_AUTH 1
#define DROPBEAR_SVR_PUBKEY_AUTH 1
EOF

# Time discipline so make never tries to re-run autoconf/automake.
find "$SRC_DIR" -exec touch {} +

make -C "$ROOT_DIR/userspace" -s build/libb1nix.a build/crt/crt0.o 1>&2

# Configure only once: re-running configure regenerates config.h and has shown
# non-deterministic getpass detection (HAVE_GETPASS flipping), which then leaves
# stale objects referencing a non-existent rpl_getpass. Locking the first good
# config.h makes incremental rebuilds reproducible.
if [ ! -f "$SRC_DIR/config.h" ]; then
(
  cd "$SRC_DIR"
  ./configure \
    --host="$HOST_TRIPLET" \
    --disable-zlib \
    --disable-pam \
    --disable-syslog \
    --disable-lastlog \
    --disable-utmp --disable-utmpx \
    --disable-wtmp --disable-wtmpx \
    --disable-loginfunc \
    --disable-pututline --disable-pututxline \
    --disable-harden \
    CC="$WRAP" AR="$AR_BIN" RANLIB="$RANLIB_BIN" \
    1>&2
)
fi

if [ "$PHASE" = "crypto" ]; then
  make -C "$SRC_DIR" -j"${JOBS:-4}" \
    CC="$WRAP" AR="$AR_BIN" RANLIB="$RANLIB_BIN" \
    libtomcrypt/libtomcrypt.a libtommath/libtommath.a 1>&2
  echo "$SRC_DIR"
  exit 0
fi

# SSH daemon + key tools + client (dbclient), so the loopback handshake smoke
# can drive the server from inside b1nix. scp is omitted (deferred).
make -C "$SRC_DIR" -j"${JOBS:-4}" \
  CC="$WRAP" AR="$AR_BIN" RANLIB="$RANLIB_BIN" \
  PROGRAMS="dropbear dbclient dropbearkey dropbearconvert" \
  MULTI=1 dropbearmulti 1>&2

echo "$SRC_DIR"
