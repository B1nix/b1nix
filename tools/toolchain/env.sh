# SPDX-License-Identifier: GPL-2.0-only
# tools/toolchain/env.sh
#
# Shared, POSIX-sh sourceable helper that resolves the per-architecture build
# identity used by every b1nix toolchain and userspace-port build script.
#
# Source it (it is not executable) AFTER defining PROJECT_DIR (or ROOT_DIR).
# It reads B1NIX_ARCH (default x86_64) and exports:
#
#   B1NIX_ARCH           normalized arch key        x86_64 | aarch64
#   B1NIX_TRIPLET        cross/native host triplet  x86_64-b1nix | aarch64-b1nix
#   B1NIX_GCC_ARCH       GNU arch name              x86_64 | aarch64
#   B1NIX_ROOTFS         per-arch sysroot           $PROJECT_DIR/build/<arch>/rootfs
#   TOOLCHAIN_BUILD_ROOT shared build parent        .../build/<arch>/toolchain (or $HOME/b1nix-toolchain)
#   TOOLCHAIN_DIST_DIR   shared tarball cache        $TOOLCHAIN_BUILD_ROOT/dist
#   TOOLCHAIN_SRC_DIR    shared patched sources      $TOOLCHAIN_BUILD_ROOT/src
#   TOOLCHAIN_BUILD_HOME per-triplet build home     $TOOLCHAIN_BUILD_ROOT/<triplet>
#
# Keying the build home by triplet keeps the aarch64-b1nix and x86_64
# (x86_64-b1nix) cross + native build objects and installed outputs in
# completely separate directories, so switching ARCH never reuses stale objects
# compiled for the other architecture.
#
# The cross shims and runtimes are per-target, so each lives under its own
# <triplet>/cross. The downloaded source tarballs and patched source trees are
# shared, via TOOLCHAIN_DIST_DIR and TOOLCHAIN_SRC_DIR; only configure/build
# directories and installed toolchains are per triplet.

: "${PROJECT_DIR:=${ROOT_DIR:-}}"
if [ -z "$PROJECT_DIR" ]; then
    echo "tools/toolchain/env.sh: PROJECT_DIR (or ROOT_DIR) must be set before sourcing" >&2
    exit 1
fi

B1NIX_ARCH="${B1NIX_ARCH:-x86_64}"
case "$B1NIX_ARCH" in
    x86_64)
        B1NIX_TRIPLET="x86_64-b1nix"
        B1NIX_GCC_ARCH="x86_64"
        B1NIX_TARGET_ARCH="x86_64"
        ;;
    aarch64)
        B1NIX_TRIPLET="aarch64-b1nix"
        B1NIX_GCC_ARCH="aarch64"
        B1NIX_TARGET_ARCH="aarch64"
        ;;
    *)
        echo "tools/toolchain/env.sh: unsupported B1NIX_ARCH='$B1NIX_ARCH' (use x86_64 or aarch64)" >&2
        exit 1
        ;;
esac

B1NIX_ROOTFS="$PROJECT_DIR/build/$B1NIX_ARCH/rootfs"

# Port builds run through the compiler *wrappers* in tools/toolchain/bin, which
# append --target from B1NIX_ARCH. ccache never sees that flag — the command
# line it hashes is identical for every arch — so an x86_64 object was served
# straight back for the same source in an aarch64 build (mbedTLS shipped
# elf64-x86-64 members inside build/aarch64/.../libmbedtls.a). Give each arch
# its own ccache namespace so the key includes the arch.
CCACHE_NAMESPACE="b1nix-$B1NIX_ARCH"
export CCACHE_NAMESPACE

# GNU make's $(CURDIR) and libtool resolve all symlinks to the real path. On
# WSL the Windows-side filesystem (/mnt/c/...) can have spaces in usernames,
# which breaks libtool's unquoted paths. When the project path has spaces,
# build under the (space-free) Linux-side $HOME instead.
if echo "$PROJECT_DIR" | grep -q ' '; then
    TOOLCHAIN_BUILD_ROOT="$HOME/b1nix-toolchain/$B1NIX_ARCH"
    # In the spaces-in-path fallback keep src/dist co-located with the build tree.
    TOOLCHAIN_DIST_DIR="$TOOLCHAIN_BUILD_ROOT/dist"
    TOOLCHAIN_SRC_DIR="$TOOLCHAIN_BUILD_ROOT/src"
else
    TOOLCHAIN_BUILD_ROOT="$PROJECT_DIR/build/$B1NIX_ARCH/toolchain"
    # Toolchain sources and tarballs are arch-independent (the same LLVM source
    # tree is shared between aarch64 and x86_64 builds) so they live next to the
    # other port sources in build/src/ and build/dist/, not inside the per-arch
    # toolchain tree.
    TOOLCHAIN_DIST_DIR="$PROJECT_DIR/build/dist/toolchain"
    TOOLCHAIN_SRC_DIR="$PROJECT_DIR/build/src/toolchain"
fi
TOOLCHAIN_BUILD_HOME="$TOOLCHAIN_BUILD_ROOT"

# Allow ccache to use system default configuration (~/.config/ccache) by default.
# Set B1NIX_USE_LOCAL_CCACHE=1 to isolate cache under build/.ccache.
if [ "${B1NIX_USE_LOCAL_CCACHE:-0}" = "1" ] && [ -z "${CCACHE_DIR:-}" ] && [ -n "$PROJECT_DIR" ]; then
    export CCACHE_DIR="$PROJECT_DIR/build/.ccache"
fi

export B1NIX_ARCH B1NIX_TRIPLET B1NIX_GCC_ARCH B1NIX_TARGET_ARCH B1NIX_ROOTFS
export TOOLCHAIN_BUILD_ROOT TOOLCHAIN_BUILD_HOME TOOLCHAIN_DIST_DIR TOOLCHAIN_SRC_DIR

# ── Global Host GCC Protection & Centralized Compiler Helpers ────────────────
b1nix_host_cc() {
    if [ "${B1NIX_NO_CCACHE:-0}" != "1" ] && command -v ccache >/dev/null 2>&1; then
        echo "ccache clang"
    else
        echo "clang"
    fi
}

b1nix_host_cxx() {
    if [ "${B1NIX_NO_CCACHE:-0}" != "1" ] && command -v ccache >/dev/null 2>&1; then
        echo "ccache clang++"
    else
        echo "clang++"
    fi
}

b1nix_cc() {
    local cross="$TOOLCHAIN_BUILD_HOME/cross/bin/${B1NIX_TRIPLET}-cc"
    if [ -x "$cross" ]; then
        if [ "${B1NIX_NO_CCACHE:-0}" != "1" ] && command -v ccache >/dev/null 2>&1; then
            echo "ccache $cross"
        else
            echo "$cross"
        fi
    else
        b1nix_host_cc
    fi
}

b1nix_cxx() {
    local cross="$TOOLCHAIN_BUILD_HOME/cross/bin/${B1NIX_TRIPLET}-c++"
    if [ -x "$cross" ]; then
        if [ "${B1NIX_NO_CCACHE:-0}" != "1" ] && command -v ccache >/dev/null 2>&1; then
            echo "ccache $cross"
        else
            echo "$cross"
        fi
    else
        b1nix_host_cxx
    fi
}

if [ "$(uname -s 2>/dev/null || echo unknown)" = "Linux" ]; then
    export CC="${CC:-clang}"
    export CXX="${CXX:-clang++}"
    export HOSTCC="${HOSTCC:-clang}"
    export HOSTCXX="${HOSTCXX:-clang++}"
    if echo "${CC:-}" | grep -qE "(^|/)gcc$" || echo "${CXX:-}" | grep -qE "(^|/)g\+\+$"; then
        echo "tools/toolchain/env.sh: WARNING - Host GCC detected in CC/CXX. Enforcing Clang for GCC-free build." >&2
        export CC="clang"
        export CXX="clang++"
        export HOSTCC="clang"
        export HOSTCXX="clang++"
    fi
fi

