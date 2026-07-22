#!/usr/bin/env bash
# tools/toolchain/build-llvm-runtimes.sh
#
# Cross-compile LLVM compiler-rt builtins + libunwind for the b1nix target.
# These replace libgcc.a (builtins) and libgcc_eh.a (unwinder).
#
# Output (installed into the cross sysroot):
#   lib/libcompiler_rt.a    — compiler builtin functions (__mulodi3, etc.)
#   lib/libunwind.a         — C++ exception unwinding (_Unwind_* routines)
#   lib/libclang_rt.builtins-x86_64.a  — alias for compiler_rt
#
# These are consumed by:
#   - b1nix-c++ link step (replaces -lgcc and -lgcc_eh)
#   - b1nix-autotools-cc (replaces -lgcc)
#   - Demo/port scripts (replace LIBGCC)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$PROJECT_DIR/tools/toolchain/env.sh"

LLVM_VER="${LLVM_VER:-22.1.8}"
LLVM_URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VER}/llvm-project-${LLVM_VER}.src.tar.xz"
LLVM_SHA256="${LLVM_SHA256:-}"

CROSS="$TOOLCHAIN_BUILD_HOME/cross"
SYSROOT="$TOOLCHAIN_BUILD_HOME/sysroot"
# M90 is intentionally GCC-free.  LLVM runtimes only need the b1nix sysroot
# and are installed into the unversioned target lib directories below.

MUSL_USR="$PROJECT_DIR/build/$B1NIX_ARCH/ports/musl/install"
if [ ! -f "$MUSL_USR/include/stdlib.h" ]; then
    echo "build-llvm-runtimes.sh: building musl headers first..."
    "$PROJECT_DIR/tools/ports/build-musl.sh" >/dev/null
fi

mkdir -p "$SYSROOT/usr/include" "$SYSROOT/usr/lib" "$SYSROOT/include" "$SYSROOT/lib"
if [ -d "$MUSL_USR/include" ]; then
    cp -Rf "$MUSL_USR/include/"* "$SYSROOT/include/" 2>/dev/null || true
    cp -Rf "$MUSL_USR/include/"* "$SYSROOT/usr/include/" 2>/dev/null || true
fi
if [ -d "$MUSL_USR/lib" ]; then
    cp -Rf "$MUSL_USR/lib/"* "$SYSROOT/lib/" 2>/dev/null || true
    cp -Rf "$MUSL_USR/lib/"* "$SYSROOT/usr/lib/" 2>/dev/null || true
fi

OS="$(uname -s)"
if [ "$OS" = "Darwin" ]; then NPROC=$(sysctl -n hw.ncpu); else NPROC=$(nproc); fi

BUILD_HOME="$TOOLCHAIN_BUILD_HOME/llvm-runtimes-build"
SRC_DIR="$BUILD_HOME/llvm-project-${LLVM_VER}.src"
INSTALL_DIR="$BUILD_HOME/install"
mkdir -p "$BUILD_HOME" "$INSTALL_DIR/lib" "$INSTALL_DIR/include"

# ── 1. Download & extract LLVM sources ────────────────────────────────────────
if [ ! -d "$SRC_DIR" ]; then
    tarball="$BUILD_HOME/llvm-project-${LLVM_VER}.src.tar.xz"
    if [ ! -f "$tarball" ]; then
        echo "Downloading LLVM ${LLVM_VER}..."
        curl -fL --retry 3 "$LLVM_URL" -o "$tarball"
    fi
    echo "Extracting LLVM sources..."
    tar -xJf "$tarball" -C "$BUILD_HOME" 2>/dev/null
fi

CLANG_BIN="$(command -v clang 2>/dev/null || echo clang)"
CLANGXX_BIN="$(command -v clang++ 2>/dev/null || echo clang++)"
AR_BIN="$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)"
CMAKE_GENERATOR="Unix Makefiles"

# Common CMake flags for cross-compiling to b1nix
COMMON_CMAKE_ARGS=(
    -G "$CMAKE_GENERATOR"
    -DCMAKE_SYSTEM_NAME=Linux
    -DCMAKE_SYSTEM_PROCESSOR="$B1NIX_GCC_ARCH"
    -DCMAKE_C_COMPILER="$CLANG_BIN"
    -DCMAKE_CXX_COMPILER="$CLANGXX_BIN"
    -DCMAKE_AR="$AR_BIN"
    -DCMAKE_RANLIB="$(command -v llvm-ranlib 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ranlib)"
    -DCMAKE_C_COMPILER_TARGET="$B1NIX_TRIPLET"
    -DCMAKE_CXX_COMPILER_TARGET="$B1NIX_TRIPLET"
    # Enable the assembler too: libunwind's register save/restore lives in .S
    # files (UnwindRegisters{Save,Restore}.S). Without an ASM compiler set for the
    # cross target, CMake silently drops them and the archive ends up missing
    # __unw_getcontext / __libunwind_Registers_*_jumpto, breaking any C++ link.
    -DCMAKE_ASM_COMPILER="$CLANG_BIN"
    -DCMAKE_ASM_COMPILER_TARGET="$B1NIX_TRIPLET"
    -DCMAKE_SYSROOT="$SYSROOT"
    -DCMAKE_C_FLAGS="-D_GNU_SOURCE=1"
    -DCMAKE_CXX_FLAGS="-D_GNU_SOURCE=1"
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
    -DCMAKE_BUILD_TYPE=Release
    -DLLVM_ENABLE_ASSERTIONS=OFF
)

if command -v ccache >/dev/null 2>&1 && [ "${B1NIX_NO_CCACHE:-0}" != "1" ]; then
    COMMON_CMAKE_ARGS+=(
        -DCMAKE_C_COMPILER_LAUNCHER=ccache
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
    )
fi

# ── 2. Build compiler-rt builtins ─────────────────────────────────────────────
CRT_BUILD="$BUILD_HOME/build-compiler-rt"
if [ ! -f "$INSTALL_DIR/lib/libcompiler_rt.a" ]; then
    echo "Building compiler-rt builtins..."
    rm -rf "$CRT_BUILD"
    mkdir -p "$CRT_BUILD"
    cd "$CRT_BUILD"
    cmake "${COMMON_CMAKE_ARGS[@]}" \
        "$SRC_DIR/compiler-rt/lib/builtins" \
        -DCOMPILER_RT_BAREMETAL=ON \
        -DCOMPILER_RT_BUILD_BUILTINS=ON \
        -DCOMPILER_RT_BUILD_SANITIZERS=OFF \
        -DCOMPILER_RT_BUILD_CTX_PROFILE=OFF \
        -DCOMPILER_RT_BUILD_XRAY=OFF \
        -DCOMPILER_RT_BUILD_LIBFUZZER=OFF \
        -DCOMPILER_RT_BUILD_PROFILE=OFF \
        -DLLVM_INCLUDE_TESTS=OFF \
        2>&1 | tail -5
    make -j"$NPROC" 2>&1 | tail -5

    # Find the produced .a file. compiler-rt's builtins build can emit BOTH a
    # primary (x86_64) and a secondary 32-bit (i386) archive; pick the one that
    # matches our target arch, never let `head -1` grab the wrong width.
    CRT_A=$(find . -name "libclang_rt.builtins-${B1NIX_GCC_ARCH}.a" | head -1)
    [ -n "$CRT_A" ] || CRT_A=$(find . -name "libclang_rt.builtins-*.a" -o -name "libcompiler_rt.builtins.a" | head -1)
    if [ -n "$CRT_A" ]; then
        cp "$CRT_A" "$INSTALL_DIR/lib/libcompiler_rt.a"
        ln -sf libcompiler_rt.a "$INSTALL_DIR/lib/libclang_rt.builtins-${B1NIX_GCC_ARCH}.a"
        echo "  compiler-rt builtins: $CRT_A → $INSTALL_DIR/lib/libcompiler_rt.a"
    else
        # Build individual builtin objects when compiler-rt's CMake target does
        # not emit the archive. This is still LLVM compiler-rt; there is no GCC
        # or libgcc fallback by design.
        echo "  compiler-rt: cmake build didn't produce expected .a — trying individual build"
        CFLAGS_BUILTINS="--target=$B1NIX_TRIPLET -O2 -ffreestanding -fno-builtin -fPIC"
        OBJS=""
        for src in "$SRC_DIR/compiler-rt/lib/builtins"/{udivdi3,moddi3,udivmoddi4,mulodi3,muldi3,divdi3,divti3,modti3,udivti3,umodti3,udivmodti4,fixdfdi,fixunsdfdi,fixsfdi,fixunssfdi,fixtfdi,fixunstfdi,floatdidf,undf2df,df2unidf,adddf3,subdf3,muldf3,divdf3,negdf2,eqdf2,gedf2,ledf2,cmpled,cmpord,cmp*,popcount*,clz*,ctz*}.c; do
            [ -f "$src" ] || continue
            obj="$CRT_BUILD/$(basename "$src" .c).o"
            "$CLANG_BIN" $CFLAGS_BUILTINS -c "$src" -o "$obj" 2>/dev/null || true
            [ -f "$obj" ] && OBJS="$OBJS $obj"
        done
        if [ -n "$OBJS" ]; then
            "$AR_BIN" rcs "$INSTALL_DIR/lib/libcompiler_rt.a" $OBJS
            ln -sf libcompiler_rt.a "$INSTALL_DIR/lib/libclang_rt.builtins-${B1NIX_GCC_ARCH}.a"
            echo "  compiler-rt builtins: manually archived $(echo $OBJS | wc -w) objects"
        else
            echo "  ERROR: LLVM compiler-rt builtins build failed" >&2
            exit 1
        fi
    fi
fi

cd "$PROJECT_DIR"

# ── 3. Build libunwind ────────────────────────────────────────────────────────
UNW_BUILD="$BUILD_HOME/build-libunwind"
if [ ! -f "$INSTALL_DIR/lib/libunwind.a" ]; then
    echo "Building libunwind..."
    rm -rf "$UNW_BUILD"
    mkdir -p "$UNW_BUILD"
    cd "$UNW_BUILD"
    cmake "${COMMON_CMAKE_ARGS[@]}" \
        "$SRC_DIR/libunwind" \
        -DLIBUNWIND_ENABLE_SHARED=OFF \
        -DLIBUNWIND_ENABLE_STATIC=ON \
        -DLIBUNWIND_USE_COMPILER_RT=OFF \
        -DLIBUNWIND_INSTALL_HEADERS=ON \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
        -DLLVM_ENABLE_LIBCXX=OFF \
        -DLLVM_INCLUDE_TESTS=OFF \
        2>&1 | tail -5
    make -j"$NPROC" 2>&1 | tail -5
    make install 2>&1 | tail -5

    if [ -f "$INSTALL_DIR/lib/libunwind.a" ]; then
        echo "  libunwind: $INSTALL_DIR/lib/libunwind.a"
    else
        echo "  WARNING: libunwind build failed" >&2
    fi
fi

cd "$PROJECT_DIR"

# Strip the LLVM_DEPENDENT_LIBRARIES (.deplibs) section. compiler-rt/libunwind
# record "dl"/"pthread" there as autolink hints, but b1nix folds dl/pthread/rt into
# libc (no standalone libdl.a/libpthread.a), so a static consumer linked with
# ld.lld would otherwise fail: "unable to find library from dependent library
# specifier: dl". Removing it makes these archives link cleanly everywhere.
OBJCOPY_BIN="$(command -v llvm-objcopy 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-objcopy)"
for f in "$INSTALL_DIR"/lib/*.a; do
    [ -f "$f" ] && "$OBJCOPY_BIN" --remove-section=.deplibs "$f" 2>/dev/null || true
done

# ── 4. Install into cross sysroot ─────────────────────────────────────────────
echo ""
echo "Installing LLVM runtimes into cross sysroot..."
mkdir -p "$SYSROOT/usr/lib" "$SYSROOT/usr/include" "$CROSS/$B1NIX_TRIPLET/lib" "$CROSS/$B1NIX_TRIPLET/include"
for f in "$INSTALL_DIR"/lib/*.a; do
    [ -f "$f" ] || continue
    name="$(basename "$f")"
    cp -f "$f" "$SYSROOT/usr/lib/$name" 2>/dev/null || true
    cp -f "$f" "$MUSL_USR/lib/$name" 2>/dev/null || true
    cp -f "$f" "$CROSS/$B1NIX_TRIPLET/lib/$name" 2>/dev/null || true
    echo "  installed: $name"
done
# Install headers (unwind.h, etc.)
for hdr in "$INSTALL_DIR"/include/*.h; do
    [ -f "$hdr" ] || continue
    cp -f "$hdr" "$CROSS/$B1NIX_TRIPLET/include/" 2>/dev/null || true
    cp -f "$hdr" "$SYSROOT/usr/include/" 2>/dev/null || true
    echo "  header: $(basename "$hdr")"
done

echo ""
echo "LLVM runtimes build complete!"
echo "  compiler-rt: $INSTALL_DIR/lib/libcompiler_rt.a"
echo "  libunwind:   $INSTALL_DIR/lib/libunwind.a"
echo ""
echo "LLVM compiler-rt and libunwind are the only cross runtime."
