#!/usr/bin/env bash
# tools/ports/build-busybox.sh - Porting/building upstream BusyBox 1.38.0 for b1nix
set -euo pipefail

BUSYBOX_VER="1.38.0"
BUSYBOX_TARBALL="busybox-${BUSYBOX_VER}.tar.bz2"
BUSYBOX_URL="https://busybox.net/downloads/${BUSYBOX_TARBALL}"
BUSYBOX_SHA256="34f9ea6ff8636f2c9241153b9114eefa9e65674a45318ae1ef95bb5f31c53bb2"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Per-architecture build identity (B1NIX_ARCH -> triplet, per-triplet paths).
. "$PROJECT_DIR/tools/toolchain/env.sh"
TARGET="$B1NIX_TRIPLET"

OS="$(uname -s)"
if [ "$OS" = "Darwin" ]; then
    NPROC=$(sysctl -n hw.ncpu)
else
    NPROC=$(nproc)
fi

BUILD_HOME="$TOOLCHAIN_BUILD_HOME"
SRC_DIR="$TOOLCHAIN_SRC_DIR/busybox-${BUSYBOX_VER}"
BUILD_DIR="$PROJECT_DIR/build/busybox-b1nix/$B1NIX_TRIPLET"
CROSS_PREFIX="$BUILD_HOME/cross"
SYSROOT="$B1NIX_ROOTFS"
CONFIG_FRAGMENT="$PROJECT_DIR/tools/configs/busybox-${BUSYBOX_VER}.config"
INSTALL_DIR="${BUSYBOX_INSTALL_DIR:-$SYSROOT/opt/busybox/bin}"

if [ ! -f "$CROSS_PREFIX/bin/${TARGET}-gcc" ]; then
    echo "Error: cross-compiler not found at $CROSS_PREFIX/bin/${TARGET}-gcc"
    echo "Run tools/toolchain/build-toolchain.sh first."
    exit 1
fi

export PATH="$CROSS_PREFIX/bin:$PATH"

# ── 0. Build & install userspace libc and headers to sysroot ──
echo "Installing/updating userspace libc and headers in sysroot..."
make -C "$PROJECT_DIR/userspace" install-headers-libs

# Workaround for spaces in path (e.g. "Documents/GitHub"): build tools like
# BusyBox's make / autotools split EXTRA_*FLAGS on whitespace and don't
# tolerate --sysroot=/path/with spaces. Create a symlink at a space-free path.
SPACEFREE_SYSROOT="$TOOLCHAIN_BUILD_HOME/sysroot-$B1NIX_TRIPLET"
rm -f "$SPACEFREE_SYSROOT" 2>/dev/null || true
mkdir -p "$(dirname "$SPACEFREE_SYSROOT")"
ln -sf "$SYSROOT" "$SPACEFREE_SYSROOT"
SYSROOT="$SPACEFREE_SYSROOT"

# ── 1. Fetch (shared cache) + unpack BusyBox ──────────────────
mkdir -p "$TOOLCHAIN_DIST_DIR"
BUSYBOX_TAR="$TOOLCHAIN_DIST_DIR/${BUSYBOX_TARBALL}"
if [ ! -f "$BUSYBOX_TAR" ]; then
    echo "Fetching BusyBox ${BUSYBOX_VER}..."
    if command -v curl >/dev/null 2>&1; then
        curl -fL -o "$BUSYBOX_TAR.tmp" "$BUSYBOX_URL"
    else
        wget -O "$BUSYBOX_TAR.tmp" "$BUSYBOX_URL"
    fi
    mv "$BUSYBOX_TAR.tmp" "$BUSYBOX_TAR"
fi

if command -v sha256sum >/dev/null 2>&1; then
    actual_sha256="$(sha256sum "$BUSYBOX_TAR" | awk '{print $1}')"
else
    actual_sha256="$(shasum -a 256 "$BUSYBOX_TAR" | awk '{print $1}')"
fi
if [ "$actual_sha256" != "$BUSYBOX_SHA256" ]; then
    echo "Error: checksum mismatch for $BUSYBOX_TAR" >&2
    echo "Expected: $BUSYBOX_SHA256" >&2
    echo "Actual:   $actual_sha256" >&2
    exit 1
fi

if [ ! -d "$SRC_DIR" ]; then
    echo "Extracting BusyBox to $SRC_DIR..."
    mkdir -p "$TOOLCHAIN_SRC_DIR"
    tar xjf "$BUSYBOX_TAR" -C "$TOOLCHAIN_SRC_DIR"
fi

sh "$PROJECT_DIR/tools/patches/busybox/b1nix-config.sh" "$SRC_DIR"

# ── 2. Configure ────────────────────────────────────────────────────────────
mkdir -p "$BUILD_DIR"

echo "Generating BusyBox configuration from $CONFIG_FRAGMENT..."
make -C "$SRC_DIR" O="$BUILD_DIR" CROSS_COMPILE="${TARGET}-" allnoconfig < /dev/null
while IFS= read -r setting; do
    case "$setting" in
        CONFIG_*=*)
            key="${setting%%=*}"
            grep -v -e "^${key}=" -e "^# ${key} is not set$" \
                "$BUILD_DIR/.config" > "$BUILD_DIR/.config.tmp" || true
            echo "$setting" >> "$BUILD_DIR/.config.tmp"
            mv "$BUILD_DIR/.config.tmp" "$BUILD_DIR/.config"
            ;;
        "# CONFIG_"*" is not set")
            key="${setting#\# }"
            key="${key% is not set}"
            grep -v -e "^${key}=" -e "^# ${key} is not set$" \
                "$BUILD_DIR/.config" > "$BUILD_DIR/.config.tmp" || true
            echo "$setting" >> "$BUILD_DIR/.config.tmp"
            mv "$BUILD_DIR/.config.tmp" "$BUILD_DIR/.config"
            ;;
    esac
done < "$CONFIG_FRAGMENT"
make -C "$SRC_DIR" O="$BUILD_DIR" CROSS_COMPILE="${TARGET}-" oldconfig < /dev/null

# Force a clean rebuild when the b1nix sysroot changed since the last BusyBox
# build. BusyBox's make does not track the sysroot headers / libb1nix.a as
# dependencies, so an incremental build would silently keep object files
# compiled against stale headers — e.g. an inline syscall wrapper or a struct
# layout — and link them with no warning. Keeping the .config (only the objects
# are removed) lets the configure step above stand.
if [ -f "$BUILD_DIR/busybox" ] &&
   [ "$SYSROOT/lib/libb1nix.a" -nt "$BUILD_DIR/busybox" ]; then
    echo "b1nix sysroot changed since last BusyBox build; cleaning objects..."
    make -C "$BUILD_DIR" CROSS_COMPILE="${TARGET}-" clean < /dev/null ||
        rm -f "$BUILD_DIR/busybox"
fi

# ── 3. Build ────────────────────────────────────────────────────────────────
echo "Building BusyBox..."
# Link DYNAMICALLY against the shared libc.so.1 via /lib/ld-b1nix.so (the in-kernel
# M69 loader resolves it eagerly at spawn), instead of the historical fully-static
# link. The cross-gcc driver prefers the sysroot's libc.so (-> libc.so.1) over
# libc.a once -static is dropped, so the executable becomes a dynamically-linked
# ET_EXEC importing libc from libc.so.1 (COPY relocs for stdout/stderr/errno,
# JUMP_SLOTs for the functions). crt0's weak __register_frame_info reference goes
# through the GOT (see crt0.S), so a C-only port that does not pull libgcc_s.so
# leaves it resolved to 0 and skips eh-frame registration rather than trapping.
# -L$SYSROOT/lib is required so ld finds the sysroot's libc.so (shared) before the
# cross toolchain's own x86_64-b1nix/lib/libc.a (static): the gcc-internal lib dir
# holds only libc.a, so without this -L the link silently falls back to static
# libc even though -static was dropped.
export EXTRA_CFLAGS="-fcommon --sysroot=$SYSROOT -isystem $SYSROOT/include"
export EXTRA_LDFLAGS="-Wl,-Ttext-segment=0x2000000 -L$SYSROOT/lib --sysroot=$SYSROOT"

# Wire ccache into the busybox build (byte-identical objects, faster rebuilds).
if command -v ccache >/dev/null 2>&1 && [ "${B1NIX_NO_CCACHE:-0}" != "1" ]; then
  export CC="ccache ${TARGET}-gcc"
fi

make -C "$BUILD_DIR" -j"$NPROC" CROSS_COMPILE="${TARGET}-"

# ── 4. Install ──────────────────────────────────────────────────────────────
echo "Installing standalone BusyBox package..."
mkdir -p "$INSTALL_DIR"
cp "$BUILD_DIR/busybox" "$INSTALL_DIR/busybox"

# Remove links created by the earlier integration, but leave unrelated files
# and links in /bin untouched.
for applet in true false yes echo printf pwd basename dirname cat head tail wc \
    mkdir rmdir rm cp mv ln readlink touch chmod chown sync sleep date uname \
    kill test "[" sort uniq; do
    old_path="$SYSROOT/bin/$applet"
    if [ -L "$old_path" ]; then
        old_target="$(readlink "$old_path")"
        if [ "$old_target" = "busybox-real" ] ||
           [ "$old_target" = "/bin/busybox-real" ]; then
            rm -f "$old_path"
        fi
    fi
done
rm -f "$SYSROOT/bin/busybox-real"

echo "BusyBox ${BUSYBOX_VER} installed to $INSTALL_DIR/busybox"
