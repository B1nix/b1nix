# tools/toolchain/env.sh
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
    echo "tools/toolchain/env.sh: PROJECT_DIR (or ROOT_DIR) must be set before sourcing" >&2
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
        echo "tools/toolchain/env.sh: unsupported B1NIX_ARCH='$B1NIX_ARCH' (use x86 or x86_64)" >&2
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

# ── C++ cross-compiler resolution (Clang frontend + GCC libstdc++ headers/libs) ──
# Source this after env.sh. Sets:
#   CXX_CROSS          clang++ (or g++ with B1NIX_CXX_FRONTEND=gcc)
#   CXXFLAGS_CROSS     compile flags (--target, -nostdinc, C++ stdlib includes)
#   STDLIB_CROSS_A     path to the C++ standard library (.a)
#   STDLIB_ABI_CROSS_A path to the C++ ABI library (.a)
#   LIBGCC_CROSS       path to libgcc.a (from cross GCC, fallback CRT)
#   LLVM_CRT_CROSS     path to libcompiler_rt.a (LLVM builtins, or empty)
#   LLVM_UNW_CROSS     path to libunwind.a (LLVM unwinder, or empty)
resolve_cxx_cross() {
    local cross="$TOOLCHAIN_BUILD_HOME/cross"
    local gxx="$cross/bin/$B1NIX_TRIPLET-g++"
    local cxx_frontend="${B1NIX_CXX_FRONTEND:-clang}"
    local cxx_stdlib="${B1NIX_CXX_STDLIB:-}"

    # Auto-detect C++ stdlib
    local libcxx_a="$TOOLCHAIN_BUILD_HOME/llvm-runtimes-build/libcxx-install/lib/libc++.a"
    local libcxxabi_a="$TOOLCHAIN_BUILD_HOME/llvm-runtimes-build/libcxxabi-install/lib/libc++abi.a"
    if [ -z "$cxx_stdlib" ]; then
        if [ -f "$libcxx_a" ] && [ -f "$libcxxabi_a" ]; then
            cxx_stdlib="libc++"
        else
            cxx_stdlib="libstdc++"
        fi
    fi

    # Resolve C++ stdlib archives
    case "$cxx_stdlib" in
        libc++)
            STDLIB_CROSS_A="$libcxx_a"
            STDLIB_ABI_CROSS_A="$libcxxabi_a"
            ;;
        libstdc++|*)
            STDLIB_CROSS_A="$("$gxx" -print-file-name=libstdc++.a 2>/dev/null)"
            STDLIB_ABI_CROSS_A="$("$gxx" -print-file-name=libsupc++.a 2>/dev/null)"
            ;;
    esac

    case "$cxx_frontend" in
        gcc)
            CXX_CROSS="$gxx"
            CXXFLAGS_CROSS="-O2 -ffunction-sections -fdata-sections -Db1nix"
            ;;
        clang|*)
            CXX_CROSS="${B1NIX_CLANGXX:-$(command -v /opt/homebrew/opt/llvm/bin/clang++ 2>/dev/null || command -v clang++ 2>/dev/null || echo "$gxx")}"
            local ver
            ver="$("$gxx" -dumpversion 2>/dev/null || echo 13.2.0)"
            local gccroot="$cross/lib/gcc/$B1NIX_TRIPLET/$ver"
            local res
            res="$("$CXX_CROSS" -print-resource-dir 2>/dev/null || true)"
            CXXFLAGS_CROSS="--target=$B1NIX_TRIPLET -O2 -ffunction-sections -fdata-sections -Db1nix -nostdinc"
            [ -n "$res" ] && CXXFLAGS_CROSS="$CXXFLAGS_CROSS -isystem $res/include"

            if [ "$cxx_stdlib" = "libc++" ]; then
                local libcxx_hdr="$TOOLCHAIN_BUILD_HOME/llvm-runtimes-build/libcxx-install/include/c++/v1"
                CXXFLAGS_CROSS="$CXXFLAGS_CROSS -isystem $libcxx_hdr"
            else
                local cxxroot="$cross/$B1NIX_TRIPLET/include/c++/$ver"
                CXXFLAGS_CROSS="$CXXFLAGS_CROSS -isystem $cxxroot -isystem $cxxroot/$B1NIX_TRIPLET -isystem $cxxroot/backward"
            fi
            CXXFLAGS_CROSS="$CXXFLAGS_CROSS -isystem $gccroot/include -isystem $gccroot/include-fixed"
            CXXFLAGS_CROSS="$CXXFLAGS_CROSS -isystem $cross/$B1NIX_TRIPLET/include"
            ;;
    esac

    LIBGCC_CROSS="$("$gxx" -print-libgcc-file-name 2>/dev/null)"

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
