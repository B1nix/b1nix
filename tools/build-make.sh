#!/usr/bin/env bash
# tools/build-make.sh - Cross-build GNU Make for the x86_64-b1nix target.
#
# Produces a native `make` that runs INSIDE b1nix, alongside the ported
# gcc/binutils (see build-native-toolchain.sh). This replaces the old in-kernel
# `nmake` toy: GNU Make can drive a real Makefile, which closes roadmap item 416
# (in-guest `make` workflow) — the kernel self-build can then run
# `make -f Makefile` in-guest instead of the flat tools/inguest/build-kernel.sh.
#
# Version choice: GNU Make 3.82. It bundles its own glob/fnmatch (glob/ subdir),
# so it does NOT need libc <glob.h>/<fnmatch.h> (which b1nix lacks), is C89 with
# no gnulib dependency, and is the well-trodden choice for minimal-libc OS
# ports. The b1nix libc now provides the rest of what Make needs: fork/execvp/
# waitpid, stat (st_mtime), opendir/readdir/closedir (over SYS_GETDENTS),
# signal/sigaction, getcwd/chdir, getenv/setenv, malloc family, printf family.
#
# Requires: cross-toolchain (tools/build-toolchain.sh) + userspace libc
# installed into the sysroot/rootfs (`make -C userspace install`).
#
# NOTE: this script COMPILES Make with the cross compiler on the build host; the
# resulting binary RUNS only inside b1nix (verify on the KVM rig, like the rest
# of the in-guest self-host flow — the macOS host is TCG-only).
set -euo pipefail

TARGET="x86_64-b1nix"
MAKE_VER="3.82"
MAKE_URL="https://ftp.gnu.org/gnu/make/make-${MAKE_VER}.tar.gz"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Platform detection (matches build-native-toolchain.sh) ──────────────────
OS="$(uname -s)"
if [ "$OS" = "Darwin" ]; then
    NPROC=$(sysctl -n hw.ncpu)
    SED_INPLACE() { sed -i '' "$@"; }
else
    NPROC=$(nproc)
    SED_INPLACE() { sed -i "$@"; }
fi

# ── Space-free build directory (matches the toolchain scripts) ──────────────
if echo "$PROJECT_DIR" | grep -q ' '; then
    BUILD_HOME="$HOME/b1nix-toolchain"
else
    BUILD_HOME="$PROJECT_DIR/build/toolchain_build"
fi

SRC_DIR="$BUILD_HOME/src"
WORK_DIR="$BUILD_HOME/native_build"
CROSS_PREFIX="$BUILD_HOME/cross"
NATIVE_DEST="$BUILD_HOME/native_root"
# The cross toolchain's sysroot is a symlink to build/x86_64/rootfs (see
# build-toolchain.sh); the userspace libc + headers must already be installed
# there via `make -C userspace install`.
SYSROOT="$PROJECT_DIR/build/x86_64/rootfs"

# ── Sanity checks ───────────────────────────────────────────────────────────
if [ ! -f "$CROSS_PREFIX/bin/${TARGET}-gcc" ]; then
    echo "Error: cross-compiler not found at $CROSS_PREFIX/bin/${TARGET}-gcc"
    echo "Run tools/build-toolchain.sh first."
    exit 1
fi
if [ ! -f "$SYSROOT/lib/libb1nix.a" ] && [ ! -f "$SYSROOT/usr/lib/libb1nix.a" ]; then
    echo "Warning: libb1nix.a not found under $SYSROOT/{lib,usr/lib}."
    echo "Run 'make -C userspace install' (and 'make install' at the top level)"
    echo "so the sysroot has the current libc (incl. opendir/readdir + limits.h)."
fi

export PATH="$CROSS_PREFIX/bin:$PATH"
export ac_cv_c_bigendian=no

CC_VAL="${TARGET}-gcc"
AR_VAL="${TARGET}-ar"
RANLIB_VAL="${TARGET}-ranlib"
CFLAGS_VAL="-isystem $SYSROOT/include -Wl,-Ttext-segment=0x2000000"
CPPFLAGS_VAL="-isystem $SYSROOT/include"
LDFLAGS_VAL="-Wl,-Ttext-segment=0x2000000"
LIBS_VAL="-lb1nix -lgcc"

mkdir -p "$SRC_DIR" "$WORK_DIR" "$NATIVE_DEST"

# ── 1. Fetch + unpack GNU Make ──────────────────────────────────────────────
if [ ! -d "$SRC_DIR/make-${MAKE_VER}" ]; then
    echo "Fetching GNU Make ${MAKE_VER}..."
    cd "$SRC_DIR"
    if command -v curl >/dev/null 2>&1; then
        curl -L -o "make-${MAKE_VER}.tar.gz" "$MAKE_URL"
    else
        wget -O "make-${MAKE_VER}.tar.gz" "$MAKE_URL"
    fi
    tar xf "make-${MAKE_VER}.tar.gz"
fi

# ── 2. Teach Make's config.sub about the b1nix OS ──────────────────────────
#   Make 3.82 ships a 2010-vintage config/config.sub that rejects an unknown
#   OS ("system `b1nix' not recognized"). Rather than sed a fragile pattern
#   into it, drop in the cross toolchain's already-b1nix-aware config.sub /
#   config.guess (binutils' or gcc's modern version), which canonicalises
#   x86_64-b1nix -> x86_64-pc-b1nix. The newer config.sub is backward
#   compatible with the older configure script.
MK_CFGDIR="$SRC_DIR/make-${MAKE_VER}/config"
GOOD_CFG=""
for c in "$SRC_DIR"/binutils-*/config.sub "$SRC_DIR"/gcc-*/config.sub; do
    if [ -f "$c" ] && bash "$c" x86_64-b1nix >/dev/null 2>&1; then
        GOOD_CFG="$(dirname "$c")"
        break
    fi
done
if [ -n "$GOOD_CFG" ]; then
    echo "Installing b1nix-aware config.sub/config.guess from $GOOD_CFG ..."
    cp "$GOOD_CFG/config.sub" "$MK_CFGDIR/config.sub"
    [ -f "$GOOD_CFG/config.guess" ] && cp "$GOOD_CFG/config.guess" "$MK_CFGDIR/config.guess"
else
    echo "Warning: no b1nix-aware config.sub found in $SRC_DIR; configure may reject --host."
fi

# ── 3. Configure + build ────────────────────────────────────────────────────
if [ ! -f "$WORK_DIR/build-make/Makefile" ]; then
    rm -rf "$WORK_DIR/build-make"
    mkdir -p "$WORK_DIR/build-make"
    cd "$WORK_DIR/build-make"
    "$SRC_DIR/make-${MAKE_VER}/configure" \
        CC="$CC_VAL" \
        AR="$AR_VAL" \
        RANLIB="$RANLIB_VAL" \
        CFLAGS="$CFLAGS_VAL" \
        CPPFLAGS="$CPPFLAGS_VAL" \
        LDFLAGS="$LDFLAGS_VAL" \
        LIBS="$LIBS_VAL" \
        --host="$TARGET" \
        --prefix="" \
        --without-guile \
        --disable-nls
else
    cd "$WORK_DIR/build-make"
fi

make -j"$NPROC"
make install DESTDIR="$NATIVE_DEST"

echo ""
echo "GNU Make ${MAKE_VER} cross-built for ${TARGET}."
echo "Installed: $NATIVE_DEST/bin/make"
echo "Run 'make install-native-toolchain' to copy it into the rootfs"
echo "(it lands at /persist/bin/make in-guest)."
