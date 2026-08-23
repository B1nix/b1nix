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
#   B1NIX_GCC_ARCH       gcc arch name              x86_64 | aarch64
#   B1NIX_ROOTFS         per-arch sysroot           $PROJECT_DIR/build/<arch>/rootfs
#   TOOLCHAIN_BUILD_ROOT shared build parent        .../build/<arch>/toolchain (or $HOME/b1nix-toolchain)
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
    # Toolchain sources and tarballs are arch-independent (same LLVM/GCC source
    # tree is shared between x86 and x86_64 builds) so they live next to the
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

# ── C++ cross-compiler resolution (Clang frontend + LLVM libc++) ─────────────
# Source this after env.sh. Sets:
#   CXX_CROSS          clang++
#   CXXFLAGS_CROSS     compile flags (--target, -nostdinc, C++ stdlib includes)
#   STDLIB_CROSS_A     path to the C++ standard library (.a)
#   STDLIB_ABI_CROSS_A path to the C++ ABI library (.a)
#   LIBGCC_CROSS       always empty (GCC is not part of the toolchain)
#   LLVM_CRT_CROSS     path to libcompiler_rt.a
#   LLVM_UNW_CROSS     path to libunwind.a
resolve_cxx_cross() {
    local cross="$TOOLCHAIN_BUILD_HOME/cross"
    local cxx_frontend="clang"
    local cxx_stdlib="${B1NIX_CXX_STDLIB:-}"

    # Auto-detect C++ stdlib. The unified runtimes build (build-libcxx.sh) installs
    # BOTH libc++.a and libc++abi.a under libcxx-install/lib. Default to libc++
    # so we run GCC-free. Prefer the musl-built runtimes when present: the
    # legacy llvm-runtimes-build archives were compiled against the old b1nix
    # libc headers and reference `errno` as a data symbol musl does not export.
    local runtimes_home="$TOOLCHAIN_BUILD_HOME/llvm-runtimes-build-musl"
    if [ ! -f "$runtimes_home/libcxx-install/lib/libc++.a" ]; then
        if [ -x "$PROJECT_DIR/tools/ports/build-libcxx-musl.sh" ]; then
            echo "resolve_cxx_cross: building musl libc++ first..." >&2
            "$PROJECT_DIR/tools/ports/build-libcxx-musl.sh" >/dev/null
        fi
    fi
    if [ ! -d "$runtimes_home/libcxx-install" ] && [ -d "$TOOLCHAIN_BUILD_HOME/llvm-runtimes-build/libcxx-install" ]; then
        runtimes_home="$TOOLCHAIN_BUILD_HOME/llvm-runtimes-build"
    fi
    local libcxx_a="$runtimes_home/libcxx-install/lib/libc++.a"
    local libcxxabi_a="$runtimes_home/libcxx-install/lib/libc++abi.a"
    if [ -z "$cxx_stdlib" ]; then
        cxx_stdlib="libc++"
    fi

    # Resolve C++ stdlib archives
    case "$cxx_stdlib" in
        libc++)
            STDLIB_CROSS_A="$libcxx_a"
            STDLIB_ABI_CROSS_A="$libcxxabi_a"
            ;;
        *)
            echo "resolve_cxx_cross: only LLVM libc++ is supported; got '$cxx_stdlib'" >&2
            return 1
            ;;
    esac

    case "$cxx_frontend" in
        clang)
            CXX_CROSS="${B1NIX_CLANGXX:-$(command -v /opt/homebrew/opt/llvm/bin/clang++ 2>/dev/null || command -v clang++ 2>/dev/null || echo "clang++")}"
            if [ "$cxx_stdlib" = "libc++" ]; then
                # libc++: -nostdinc++ (drop only the C++ stdlib search) + the libc++
                # headers, letting clang resolve its own builtin C headers and the
                # b1nix libc headers through --sysroot. A blanket -nostdinc breaks
                # clang's stddef.h #include_next chain (::nullptr_t goes undefined),
                # so mirror exactly how libc++ itself + b1nix-c++ are built.
                local libcxx_hdr="$runtimes_home/libcxx-install/include/c++/v1"
                local sysroot="$TOOLCHAIN_BUILD_HOME/sysroot"
                CXXFLAGS_CROSS="--target=$B1NIX_TRIPLET --sysroot=$sysroot -nostdinc++ -isystem $libcxx_hdr"
                CXXFLAGS_CROSS="$CXXFLAGS_CROSS -O2 -ffunction-sections -fdata-sections -Db1nix"
            fi
            ;;
    esac

    LIBGCC_CROSS=""

    # LLVM runtimes: prefer compiler-rt + libunwind over libgcc when available.
    local llvm_rt="$TOOLCHAIN_BUILD_HOME/llvm-runtimes-build/install/lib"
    if [ -f "$llvm_rt/libcompiler_rt.a" ] && [ -f "$llvm_rt/libunwind.a" ]; then
        LLVM_CRT_CROSS="$llvm_rt/libcompiler_rt.a"
        LLVM_UNW_CROSS="$llvm_rt/libunwind.a"
    else
        LLVM_CRT_CROSS=""
        LLVM_UNW_CROSS=""
    fi
}
