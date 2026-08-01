#!/usr/bin/env bash
# tools/toolchain/build-libcxx.sh
#
# Cross-compile LLVM libc++ + libc++abi + libunwind for the b1nix target as a
# unified LLVM "runtimes" build. These replace GCC's libstdc++ / libsupc++ /
# libgcc_eh as the C++ standard library + ABI + unwinder.
#
# Why the unified runtimes build (not three standalone CMake projects): since
# LLVM 16 the standalone libcxx/libcxxabi/libunwind builds are deprecated, and
# LLVM 18's libc++abi hard-errors when LIBCXXABI_USE_LLVM_UNWINDER=ON unless
# libunwind is part of the same LLVM_ENABLE_RUNTIMES configure. Building the
# three together from llvm-project/runtimes wires their inter-dependencies
# (libc++ -> libc++abi -> libunwind) correctly in one pass.
#
# Output is PIC static archives so they can be (a) linked straight into a static
# C++ executable, and (b) folded into the shared libc++.so / libc++abi.so /
# libunwind.so by tools/toolchain/build-libcxx-shared.sh.
#
# Prerequisites:
#   - Cross GCC toolchain (tools/toolchain/build-toolchain.sh) — for the sysroot
#   - b1nix userspace libc headers staged into the sysroot
#   - compiler-rt builtins (tools/toolchain/build-llvm-runtimes.sh)
#
# Output (installed into $INSTALL_DIR + the cross sysroot):
#   lib/libc++.a       — C++ standard library (PIC static)
#   lib/libc++abi.a    — C++ ABI support (exceptions, RTTI, atexit) (PIC static)
#   lib/libunwind.a    — DWARF unwinder (PIC static)
#   include/c++/v1/    — libc++ headers (incl. the generated __config_site)
#
# Usage:
#   tools/toolchain/build-libcxx.sh                 # build for default arch
#   B1NIX_ARCH=x86 tools/toolchain/build-libcxx.sh  # build for i686
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$PROJECT_DIR/tools/toolchain/env.sh"

LLVM_VER="${LLVM_VER:-22.1.8}"
BUILD_HOME="$TOOLCHAIN_BUILD_HOME/llvm-runtimes-build"
SRC_DIR="$BUILD_HOME/llvm-project-${LLVM_VER}.src"
INSTALL_DIR="$BUILD_HOME/libcxx-install"
RT_BUILD="$BUILD_HOME/build-runtimes"
CROSS="$TOOLCHAIN_BUILD_HOME/cross"
SYSROOT="$TOOLCHAIN_BUILD_HOME/sysroot"
GCC_INC="$CROSS/lib/gcc/$B1NIX_TRIPLET"
GCC_VER_DIR=$(ls "$GCC_INC" 2>/dev/null | head -1 || true)

OS="$(uname -s)"
if [ "$OS" = "Darwin" ]; then NPROC=$(sysctl -n hw.ncpu); else NPROC=$(nproc); fi

# Verify prerequisites
[ -d "$SRC_DIR" ] || { echo "LLVM sources not found at $SRC_DIR — run build-llvm-runtimes.sh first" >&2; exit 1; }
[ -f "$BUILD_HOME/install/lib/libcompiler_rt.a" ] || { echo "compiler-rt not found — run build-llvm-runtimes.sh first" >&2; exit 1; }
[ -d "$SRC_DIR/llvm" ] || { echo "llvm-project/llvm missing in $SRC_DIR" >&2; exit 1; }
[ -f "$SYSROOT/include/pthread.h" ] || { echo "b1nix sysroot headers not staged — run build-toolchain.sh first" >&2; exit 1; }

mkdir -p "$INSTALL_DIR/lib" "$INSTALL_DIR/include"

# Apple's /usr/bin/clang only implements the Darwin link driver (see
# build-llvm-runtimes.sh for the fuller writeup) — prefer the real Homebrew
# LLVM clang so any linking this bootstrap does (try_compile probes, etc.)
# doesn't silently default to Mach-O.
CLANG_BIN="$(command -v /opt/homebrew/opt/llvm/bin/clang 2>/dev/null || command -v clang 2>/dev/null || echo clang)"
CLANGXX_BIN="$(command -v /opt/homebrew/opt/llvm/bin/clang++ 2>/dev/null || command -v clang++ 2>/dev/null || echo clang++)"
AR_BIN="$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)"
RANLIB_BIN="$(command -v llvm-ranlib 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ranlib)"
# b1nix's own triple has no OS component, which makes clang's driver fall
# back to the Darwin toolchain for anything that links (see
# build-llvm-runtimes.sh). Use a real ELF/Linux triple for codegen instead.
CLANG_TARGET_TRIPLE="x86_64-unknown-linux-gnu"
LLD_PATH="$(command -v ld.lld 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/ld.lld)"

# b1nix-specific defines needed by libc++/libc++abi/libunwind:
#   _POSIX_THREADS / _POSIX_C_SOURCE — POSIX pthread + .1-2008 APIs
#   _GNU_SOURCE — dl_iterate_phdr prototype (libunwind DWARF index lookup)
#   __B1NIX__   — identify the target in any b1nix header guards
B1NIX_DEFINES="-D__B1NIX__=1 -D_POSIX_THREADS=1 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE=1"

if [ ! -f "$INSTALL_DIR/lib/libc++.a" ] || [ "${REBUILD:-0}" = "1" ]; then
    echo "Configuring LLVM runtimes (libunwind;libcxxabi;libcxx) for $B1NIX_TRIPLET..."
    rm -rf "$RT_BUILD"
    mkdir -p "$RT_BUILD"
    # Earlier debugging in this session chased "runtimes/ is gone in LLVM 22"
    # and "host tblgen not found" as if they were real API/version changes —
    # they weren't. The actual cause was a truncated/interrupted tar
    # extraction of llvm-project-*.src.tar.xz that silently left both
    # utils/TableGen/ and runtimes/ missing from the tree; a clean
    # re-extraction restored both. Back to the original, lighter invocation
    # (configuring only runtimes/, not the whole llvm/ project — no LLVMCodeGen
    # etc. dragged in as a "host tools" build).
    cmake -G "Unix Makefiles" \
        -S "$SRC_DIR/runtimes" -B "$RT_BUILD" \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR="$B1NIX_GCC_ARCH" \
        -DCMAKE_C_COMPILER="$CLANG_BIN" \
        -DCMAKE_CXX_COMPILER="$CLANGXX_BIN" \
        -DCMAKE_AR="$AR_BIN" \
        -DCMAKE_RANLIB="$RANLIB_BIN" \
        -DCMAKE_C_COMPILER_TARGET="$CLANG_TARGET_TRIPLE" \
        -DCMAKE_CXX_COMPILER_TARGET="$CLANG_TARGET_TRIPLE" \
        -DCMAKE_ASM_COMPILER="$CLANG_BIN" \
        -DCMAKE_ASM_COMPILER_TARGET="$CLANG_TARGET_TRIPLE" \
        -DCMAKE_SYSROOT="$SYSROOT" \
        -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=$LLD_PATH" \
        -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=$LLD_PATH" \
        -DCMAKE_MODULE_LINKER_FLAGS="-fuse-ld=$LLD_PATH" \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
        -DLLVM_FORCE_USE_OLD_TOOLCHAIN=ON \
        -DCMAKE_C_COMPILER_WORKS=1 \
        -DCMAKE_CXX_COMPILER_WORKS=1 \
        -DCMAKE_C_FLAGS="$B1NIX_DEFINES" \
        -DCMAKE_CXX_FLAGS="$B1NIX_DEFINES" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DLLVM_ENABLE_PIC=ON \
        -DLLVM_ENABLE_ASSERTIONS=OFF \
        -DLLVM_INCLUDE_TESTS=OFF \
        -DLLVM_ENABLE_RUNTIMES="libunwind;libcxxabi;libcxx" \
        -DLLVM_LIBC_FULL_BUILD=OFF \
        \
        -DLIBUNWIND_ENABLE_SHARED=OFF \
        -DLIBUNWIND_ENABLE_STATIC=ON \
        -DLIBUNWIND_USE_COMPILER_RT=ON \
        -DLIBUNWIND_INSTALL_HEADERS=ON \
        \
        -DLIBCXXABI_ENABLE_SHARED=OFF \
        -DLIBCXXABI_ENABLE_STATIC=ON \
        -DLIBCXXABI_USE_LLVM_UNWINDER=ON \
        -DLIBCXXABI_ENABLE_STATIC_UNWINDER=ON \
        -DLIBCXXABI_USE_COMPILER_RT=ON \
        -DLIBCXXABI_ENABLE_THREADS=ON \
        -DLIBCXXABI_ENABLE_EXCEPTIONS=ON \
        -DLIBCXXABI_INCLUDE_TESTS=OFF \
        -DLIBCXXABI_INSTALL_HEADERS=ON \
        \
        -DLIBCXX_ENABLE_SHARED=OFF \
        -DLIBCXX_ENABLE_STATIC=ON \
        -DLIBCXX_CXX_ABI=libcxxabi \
        -DLIBCXX_HAS_MUSL_LIBC=ON \
        -DLIBCXX_USE_COMPILER_RT=ON \
        -DLIBCXX_ENABLE_FILESYSTEM=ON \
        -DLIBCXX_ENABLE_TIME_ZONE_DATABASE=OFF \
        -DLIBCXX_ENABLE_EXCEPTIONS=ON \
        -DLIBCXX_ENABLE_RTTI=ON \
        -DLIBCXX_ENABLE_THREADS=ON \
        -DLIBCXX_HAS_PTHREAD_API=ON \
        -DLIBCXX_ENABLE_MONOTONIC_CLOCK=ON \
        -DLIBCXX_INCLUDE_TESTS=OFF \
        -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
        -DLIBCXX_INCLUDE_DOCS=OFF \
        2>&1 | tail -8

    echo "Building runtimes..."
    make -C "$RT_BUILD" -j"$NPROC" 2>&1 | tail -8
    echo "Installing runtimes..."
    make -C "$RT_BUILD" install 2>&1 | tail -8
fi

# The unified runtimes build installs the per-target libs under
# lib/<triplet>/ and headers under include/<triplet>/c++/v1 + include/c++/v1.
# Normalise the three archives into $INSTALL_DIR/lib/ for the consumers.
for name in libc++.a libc++abi.a libunwind.a; do
    found=$(find "$INSTALL_DIR/lib" -name "$name" 2>/dev/null | head -1)
    if [ -n "$found" ] && [ "$found" != "$INSTALL_DIR/lib/$name" ]; then
        cp -f "$found" "$INSTALL_DIR/lib/$name"
    fi
done

[ -f "$INSTALL_DIR/lib/libc++.a" ]    || { echo "ERROR: libc++.a not produced — check $RT_BUILD" >&2; exit 1; }
[ -f "$INSTALL_DIR/lib/libc++abi.a" ] || { echo "ERROR: libc++abi.a not produced — check $RT_BUILD" >&2; exit 1; }
[ -f "$INSTALL_DIR/lib/libunwind.a" ] || { echo "ERROR: libunwind.a not produced — check $RT_BUILD" >&2; exit 1; }

# Strip the LLVM_DEPENDENT_LIBRARIES (.deplibs) section. compiler-rt/libunwind
# record "dl" and "pthread" there as autolink hints, but b1nix folds dl/pthread/rt
# into libc (there are no standalone libdl.a/libpthread.a), so any static consumer
# linked with ld.lld would fail with "unable to find library from dependent library
# specifier: dl". Removing the section makes these archives link cleanly everywhere.
OBJCOPY_BIN="$(command -v llvm-objcopy 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-objcopy)"
for name in libc++.a libc++abi.a libunwind.a; do
    "$OBJCOPY_BIN" --remove-section=.deplibs "$INSTALL_DIR/lib/$name" 2>/dev/null || true
done

# The unified runtimes build folds the libunwind objects into libc++.a as well as
# libc++abi.a. Keeping them in libc++.a would mean a shared libc++.so AND a shared
# libc++abi.so each carry a full copy of the DWARF unwinder — two _Unwind_* / __unw_*
# instances with split FDE registries, which crashes the moment an exception is
# thrown (the personality routine in one .so unwinds against the other's state).
# The unwinder must live in exactly ONE place: libc++abi (which libc++ NEEDs). So
# drop the libunwind objects from libc++.a; its _Unwind_*/__unw_* references then
# resolve to libc++abi.so.1 at load time.
AR_BIN_DEL="$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)"
"$AR_BIN_DEL" d "$INSTALL_DIR/lib/libc++.a" \
    libunwind.cpp.o Unwind-EHABI.cpp.o Unwind-seh.cpp.o UnwindLevel1.c.o \
    UnwindLevel1-gcc-ext.c.o Unwind-sjlj.c.o Unwind-wasm.c.o \
    UnwindRegistersRestore.S.o UnwindRegistersSave.S.o 2>/dev/null || true
"$RANLIB_BIN" "$INSTALL_DIR/lib/libc++.a" 2>/dev/null || true

# The libc++ headers carry a generated __config_site; the runtimes build may
# install them under include/c++/v1 directly or include/<triplet>/c++/v1.
LIBCXX_HDR_DIR="$INSTALL_DIR/include/c++/v1"
if [ ! -f "$LIBCXX_HDR_DIR/__config" ]; then
    alt=$(find "$INSTALL_DIR/include" -path '*/c++/v1/__config' 2>/dev/null | head -1)
    [ -n "$alt" ] && LIBCXX_HDR_DIR="$(dirname "$alt")"
fi

# ── Install into cross sysroot ────────────────────────────────────────────────
echo ""
echo "Installing libc++/libc++abi/libunwind into cross sysroot..."
for name in libc++.a libc++abi.a libunwind.a; do
    f="$INSTALL_DIR/lib/$name"
    [ -f "$f" ] || continue
    cp -f "$f" "$SYSROOT/usr/lib/$name" 2>/dev/null || true
    cp -f "$f" "$CROSS/$B1NIX_TRIPLET/lib/$name" 2>/dev/null || true
    [ -n "$GCC_VER_DIR" ] && cp -f "$f" "$CROSS/lib/gcc/$B1NIX_TRIPLET/$GCC_VER_DIR/$name" 2>/dev/null || true
    echo "  lib: $name"
done

LIBCXX_SYSROOT_HDR="$CROSS/$B1NIX_TRIPLET/include/c++/v1"
mkdir -p "$LIBCXX_SYSROOT_HDR"
cp -R "$LIBCXX_HDR_DIR/"* "$LIBCXX_SYSROOT_HDR/" 2>/dev/null || true
echo "  headers → $LIBCXX_SYSROOT_HDR"

# The unified runtimes build is the authoritative source of libunwind.a — it
# compiles the register save/restore assembly (UnwindRegisters{Save,Restore}.S),
# which the standalone build in build-llvm-runtimes.sh omits. Install it over the
# LLVM_RT copy so every consumer that resolves -lunwind from there (b1nix-c++,
# build-mesa.sh, the M52 OSMesa demo) gets the COMPLETE archive, not one missing
# __unw_getcontext / __libunwind_Registers_*_jumpto.
LLVM_RT_LIB="$BUILD_HOME/install/lib"
if [ -d "$LLVM_RT_LIB" ]; then
    cp -f "$INSTALL_DIR/lib/libunwind.a" "$LLVM_RT_LIB/libunwind.a"
    echo "  libunwind → $LLVM_RT_LIB (authoritative, with asm)"
fi

echo ""
echo "libc++ build complete!"
echo "  libc++:    $INSTALL_DIR/lib/libc++.a"
echo "  libc++abi: $INSTALL_DIR/lib/libc++abi.a"
echo "  libunwind: $INSTALL_DIR/lib/libunwind.a"
echo "  headers:   $LIBCXX_HDR_DIR"
echo ""
echo "Next: tools/toolchain/build-libcxx-shared.sh to produce the shared .so set."
