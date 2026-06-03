# tools/toolchain-env.sh
#
# Shared, POSIX-sh sourceable helper that resolves the per-architecture build
# identity used by every b1nix toolchain and userspace-port build script.
#
# Source it (it is not executable) AFTER defining PROJECT_DIR (or ROOT_DIR).
# It reads B1NIX_ARCH (default x86_64) and exports:
#
#   B1NIX_ARCH           normalized arch key        x86 | x86_64
#   B1NIX_TRIPLET        cross/native host triplet  i686-b1nix | x86_64-b1nix
#   B1NIX_GCC_ARCH       i386-family gcc arch       i686 | x86_64
#   B1NIX_ROOTFS         per-arch sysroot           $PROJECT_DIR/build/<arch>/rootfs
#   TOOLCHAIN_BUILD_ROOT shared build parent        .../build/toolchain_build (or $HOME/b1nix-toolchain)
#   TOOLCHAIN_DIST_DIR   shared tarball cache        $TOOLCHAIN_BUILD_ROOT/dist
#   TOOLCHAIN_SRC_DIR    shared patched sources      $TOOLCHAIN_BUILD_ROOT/src
#   TOOLCHAIN_BUILD_HOME per-triplet build home     $TOOLCHAIN_BUILD_ROOT/<triplet>
#
# Keying the build home by triplet keeps the x86 (i686-b1nix) and x86_64
# (x86_64-b1nix) cross + native build objects and installed outputs in
# completely separate directories, so switching ARCH never reuses stale objects
# compiled for the other architecture.
#
# The cross COMPILERS are necessarily per-target (x86_64-b1nix-gcc and
# i686-b1nix-gcc are different compilers, not one built twice — GCC is
# --disable-multilib), so each lives under its own <triplet>/cross. The
# downloaded source tarballs and the patched binutils/gcc source trees are
# shared, via TOOLCHAIN_DIST_DIR and TOOLCHAIN_SRC_DIR; only configure/build
# directories and installed toolchains are per triplet.

: "${PROJECT_DIR:=${ROOT_DIR:-}}"
if [ -z "$PROJECT_DIR" ]; then
    echo "tools/toolchain-env.sh: PROJECT_DIR (or ROOT_DIR) must be set before sourcing" >&2
    exit 1
fi

B1NIX_ARCH="${B1NIX_ARCH:-x86_64}"
case "$B1NIX_ARCH" in
    x86)
        B1NIX_TRIPLET="i686-b1nix"
        B1NIX_GCC_ARCH="i686"
        ;;
    x86_64)
        B1NIX_TRIPLET="x86_64-b1nix"
        B1NIX_GCC_ARCH="x86_64"
        ;;
    *)
        echo "tools/toolchain-env.sh: unsupported B1NIX_ARCH='$B1NIX_ARCH' (use x86 or x86_64)" >&2
        exit 1
        ;;
esac

B1NIX_ROOTFS="$PROJECT_DIR/build/$B1NIX_ARCH/rootfs"

# GNU make's $(CURDIR) and libtool resolve all symlinks to the real path. On
# WSL the Windows-side filesystem (/mnt/c/...) can have spaces in usernames,
# which breaks libtool's unquoted paths. When the project path has spaces,
# build under the (space-free) Linux-side $HOME instead.
if echo "$PROJECT_DIR" | grep -q ' '; then
    TOOLCHAIN_BUILD_ROOT="$HOME/b1nix-toolchain"
else
    TOOLCHAIN_BUILD_ROOT="$PROJECT_DIR/build/toolchain_build"
fi
TOOLCHAIN_BUILD_HOME="$TOOLCHAIN_BUILD_ROOT/$B1NIX_TRIPLET"
TOOLCHAIN_DIST_DIR="$TOOLCHAIN_BUILD_ROOT/dist"
TOOLCHAIN_SRC_DIR="$TOOLCHAIN_BUILD_ROOT/src"

export B1NIX_ARCH B1NIX_TRIPLET B1NIX_GCC_ARCH B1NIX_ROOTFS
export TOOLCHAIN_BUILD_ROOT TOOLCHAIN_BUILD_HOME TOOLCHAIN_DIST_DIR TOOLCHAIN_SRC_DIR
