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
BUILD_DIR="$PROJECT_DIR/build/$B1NIX_ARCH/ports/busybox"
CROSS_PREFIX="$BUILD_HOME/cross"
SYSROOT="$B1NIX_ROOTFS"
CONFIG_FRAGMENT="$PROJECT_DIR/tools/configs/busybox-${BUSYBOX_VER}.config"
INSTALL_DIR="${BUSYBOX_INSTALL_DIR:-$SYSROOT/opt/busybox/bin}"

# ── musl paths ──
MUSL_INSTALL="$PROJECT_DIR/build/x86_64/ports/musl/install"
MUSL_INCLUDE="$MUSL_INSTALL/include"
MUSL_LIB="$MUSL_INSTALL/lib"

CC="${CC:-$PROJECT_DIR/tools/toolchain/bin/b1nix-musl-autotools-cc}"
if ! command -v "$CC" >/dev/null 2>&1 && [ ! -f "$CROSS_PREFIX/bin/${TARGET}-cc" ] && [ ! -f "$CROSS_PREFIX/bin/${TARGET}-clang" ]; then
    echo "Error: cross-compiler not found at $CC or $CROSS_PREFIX/bin/${TARGET}-cc"
    echo "Run tools/toolchain/build-toolchain.sh first."
    exit 1
fi

export PATH="$CROSS_PREFIX/bin:$PATH"

# ── 0. Install musl headers into sysroot ──
# With musl we don't build the b1nix native libc — just stage musl headers.
MUSL_INSTALL_HDR="$PROJECT_DIR/build/x86_64/ports/musl/install"
if [ "${B1NIX_HEADERS_INSTALLED:-0}" != "1" ]; then
  (
    flock -x 9
    mkdir -p "$SYSROOT/include" "$SYSROOT/lib" "$SYSROOT/usr"
    cp -r "$MUSL_INSTALL_HDR/include/"* "$SYSROOT/include/" 2>/dev/null || true
    ln -sfn ../include "$SYSROOT/usr/include"
    ln -sfn ../lib "$SYSROOT/usr/lib"
  ) 9>/tmp/b1nix-userspace-headers.lock
fi

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
make -C "$SRC_DIR" O="$BUILD_DIR" allnoconfig < /dev/null
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
# Feed newlines rather than EOF: enabling an applet in the manifest config can
# introduce sub-options the stored .config has no value for, and oldconfig
# prompts for each one. An empty line accepts the shown default; </dev/null
# made the first such prompt a hard error instead.
# Answer with blank lines rather than EOF: enabling an applet in the manifest
# config can pull in sub-options the stored .config has no value for, and
# oldconfig prompts for each one. A blank line accepts the shown default,
# whereas </dev/null turned the first such prompt into a hard error. The
# answers are generated without a pipe so `set -o pipefail` never sees the
# SIGPIPE a `yes | head` would produce.
awk 'BEGIN { for (i = 0; i < 20000; i++) print "" }' \
    > "$BUILD_DIR/.oldconfig-answers"
make -C "$SRC_DIR" O="$BUILD_DIR" oldconfig < "$BUILD_DIR/.oldconfig-answers"
rm -f "$BUILD_DIR/.oldconfig-answers"

# Force a clean rebuild when the b1nix sysroot changed since the last BusyBox
# build. BusyBox's make does not track the sysroot headers / libb1nix.a as
# dependencies, so an incremental build would silently keep object files
# compiled against stale headers — e.g. an inline syscall wrapper or a struct
# layout — and link them with no warning. Keeping the .config (only the objects
# are removed) lets the configure step above stand.
if [ -f "$BUILD_DIR/busybox" ] &&
   [ "$MUSL_LIB/libc.so" -nt "$BUILD_DIR/busybox" ]; then
    echo "musl libc changed since last BusyBox build; cleaning objects..."
    make -C "$BUILD_DIR" clean < /dev/null ||
        rm -f "$BUILD_DIR/busybox"
fi

# ── 3. Build ────────────────────────────────────────────────────────────────
echo "Building BusyBox (musl)..."
# Link via musl's libc.so through /lib/ld-musl-x86_64.so.1.
# Use clang with -fuse-ld=lld so the linker goes through ld.lld directly
# (no gcc/collect2 fallback). The musl wrapper supplies -isystem musl/include,
# -L musl/lib, -lc, and -Wl,-dynamic-linker.
# -include sys/sysinfo.h: busybox's libbb.h needs struct sysinfo.
# -Wno-error flags: match GCC baseline leniency for upstream code.
MUSL_CC="$PROJECT_DIR/tools/toolchain/bin/b1nix-musl-autotools-cc"
export EXTRA_CFLAGS="-fcommon -isystem $MUSL_INCLUDE -isystem $SYSROOT/include -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -D_GNU_SOURCE"
export EXTRA_LDFLAGS="-rtlib=compiler-rt -unwindlib=libunwind -L$MUSL_LIB -fuse-ld=lld -Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1 -lc"
# Build BusyBox as a musl PIE (ET_DYN), exactly like every other ported musl
# binary (js, id, displayd, ...). The kernel relocates a PIE to the high ASLR
# base (aslr_pie_base, ~0x500000000000), which the userspace ld.so relocates
# cleanly. The previous workaround forced a non-PIE ET_EXEC at 0x2000000 in the
# belief that the "large ET_DYN relocation path" was broken; in fact the opposite
# is true — PIE ET_DYN loads run, while a *dynamic* ET_EXEC main at 0x2000000 is
# the one ld.so cannot relocate (its PLT slots stay 0 -> the shell jumps to null
# at rip=0 before any code runs). PT_INTERP and DT_NEEDED are unchanged.

# BusyBox builds small host-side generators in the same make invocation.
HOST_CC="${B1NIX_HOST_CC:-$(command -v clang)}"
HOST_CXX="${B1NIX_HOST_CXX:-$(command -v clang++)}"
HOST_LD="${B1NIX_HOST_LD:-$(command -v ld)}"
TARGET_AR="${B1NIX_AR:-$(command -v llvm-ar 2>/dev/null || command -v /opt/homebrew/opt/llvm/bin/llvm-ar)}"
TARGET_RANLIB="${B1NIX_RANLIB:-$(command -v llvm-ranlib 2>/dev/null || command -v /opt/homebrew/opt/llvm/bin/llvm-ranlib)}"
TARGET_STRIP="${B1NIX_STRIP:-$(command -v llvm-strip 2>/dev/null || command -v /opt/homebrew/opt/llvm/bin/llvm-strip)}"

# Use the musl clang wrapper as CC (links against musl libc.so, PIE + ld.so).
CCACHE=""
[ "${B1NIX_NO_CCACHE:-0}" != "1" ] && command -v ccache >/dev/null 2>&1 && CCACHE="ccache"
export CC="$CCACHE $MUSL_CC"

# b1nix's musl loader now correctly adds the linker's symbols to the global
# lookup (unconditional add_syms(&ldso) in dynlink.c), so lazy PLT resolution
# works for all interfaces including weak signal functions.  -z now is no
# longer required and was actually harmful for large binaries.

make -C "$BUILD_DIR" -j"$NPROC" \
    HOSTCC="$HOST_CC" HOSTCXX="$HOST_CXX" HOSTLD="$HOST_LD" \
    CC="$CC" AR="$TARGET_AR" RANLIB="$TARGET_RANLIB" STRIP="$TARGET_STRIP"

# ── 4. Install ──────────────────────────────────────────────────────────────
echo "Installing standalone BusyBox package..."
mkdir -p "$INSTALL_DIR"
cp "$BUILD_DIR/busybox" "$INSTALL_DIR/busybox"
chmod 0755 "$INSTALL_DIR/busybox"

# M108: su/passwd/login need euid 0 to read /etc/shadow and to change identity.
# Rather than making THE multicall binary setuid — which would put a
# setuid-root path behind all 194 applets — install a second copy that is, the
# way Alpine splits busybox / busybox-suid. /bin/{su,passwd,login} point here;
# every other applet symlink keeps pointing at the plain, non-setuid busybox.
# Two lines of defence, both required:
#   1. only three applet names resolve to this inode at all, and
#   2. CONFIG_FEATURE_SUID=y makes libbb's check_suid() drop euid back to the
#      real uid for every applet that is not BB_SUID_REQUIRE, so even a
#      hand-made symlink pointing `sh` at this copy gets no privilege.
# A hard link cannot be used: the setuid bit lives in the inode, so the two
# names have to be two inodes. The real mode is stamped onto the ext4 image by
# the top-level Makefile (debugfs `sif`), since mke2fs -d does not reliably
# carry setuid bits; the chmod here keeps the staging tree honest.
cp "$BUILD_DIR/busybox" "$INSTALL_DIR/busybox-suid"
chmod 4755 "$INSTALL_DIR/busybox-suid"

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

# BusyBox's init applet, reachable as an alternative PID 1 (init=/opt/busybox/bin/init).
# The multicall binary dispatches on argv[0], so the name of the symlink is what
# selects the applet — /bin/init belongs to b1nix's own test orchestrator, so
# this one lives next to the busybox binary.
ln -sf busybox "$INSTALL_DIR/init"

echo "BusyBox ${BUSYBOX_VER} installed to $INSTALL_DIR/busybox"
