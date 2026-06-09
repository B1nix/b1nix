#!/usr/bin/env bash
# tools/build-busybox.sh - Porting/building upstream BusyBox 1.36.1 for b1nix
set -euo pipefail

BUSYBOX_VER="1.36.1"
BUSYBOX_TARBALL="busybox-${BUSYBOX_VER}.tar.bz2"
BUSYBOX_URL="https://busybox.net/downloads/${BUSYBOX_TARBALL}"
BUSYBOX_SHA256="b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Per-architecture build identity (B1NIX_ARCH -> triplet, per-triplet paths).
. "$PROJECT_DIR/tools/toolchain-env.sh"
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
    echo "Run tools/build-toolchain.sh first."
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

# b1nix is a Linux-like target, but the cross GCC predefines __b1nix__/__unix__,
# not __linux__. The procps applets (free/uptime, and the ps uptime helper)
# include <sys/sysinfo.h> and use `struct sysinfo` only under `#ifdef __linux__`,
# so without this they fail to compile ("storage size of 'info' isn't known").
# b1nix ships <sys/sysinfo.h> in its sysroot and implements the SYS_SYSINFO
# syscall, so widen those include guards to also fire for __b1nix__. Idempotent:
# keyed on the rewritten token so a re-extracted tree is patched exactly once.
for bb_src in procps/free.c procps/uptime.c procps/ps.c; do
    if [ -f "$SRC_DIR/$bb_src" ] && ! grep -q "__b1nix__" "$SRC_DIR/$bb_src"; then
        sed -i '' 's/#ifdef __linux__/#if defined(__linux__) || defined(__b1nix__)/' \
            "$SRC_DIR/$bb_src"
    fi
done

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
# Link using userspace library and load at 0x2000000
export EXTRA_CFLAGS="-fcommon --sysroot=$SYSROOT -isystem $SYSROOT/include"
export EXTRA_LDFLAGS="-static -Wl,-Ttext-segment=0x2000000 --sysroot=$SYSROOT"

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
