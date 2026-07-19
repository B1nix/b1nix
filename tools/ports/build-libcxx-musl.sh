#!/usr/bin/env bash
# tools/ports/build-libcxx-musl.sh
#
# Compile LLVM libc++ + libc++abi + libunwind for the b1nix target against musl.
#
# Output (installed into the musl sysroot):
#   usr/lib/libc++.a
#   usr/lib/libc++abi.a
#   usr/lib/libunwind.a
#   usr/lib/libc++.so.1
#   usr/lib/libc++abi.so.1
#   usr/include/c++/v1/
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$PROJECT_DIR/tools/toolchain/env.sh"

LLVM_VER="${LLVM_VER:-18.1.8}"
# Build in a separate folder to avoid colliding with regular build
BUILD_HOME="$TOOLCHAIN_BUILD_HOME/llvm-runtimes-build-musl"
SRC_DIR="$TOOLCHAIN_BUILD_HOME/llvm-runtimes-build/llvm-project-${LLVM_VER}.src"
INSTALL_DIR="$BUILD_HOME/libcxx-install"
RT_BUILD="$BUILD_HOME/build-runtimes"

MUSL_SYSROOT="$PROJECT_DIR/build/musl-b1nix/$B1NIX_TRIPLET/install"

OS="$(uname -s)"
if [ "$OS" = "Darwin" ]; then NPROC=$(sysctl -n hw.ncpu); else NPROC=$(nproc); fi

# Verify prerequisites
[ -d "$SRC_DIR" ] || { echo "LLVM sources not found at $SRC_DIR — run build-llvm-runtimes.sh first" >&2; exit 1; }
[ -f "$PROJECT_DIR/build/toolchain_build/$B1NIX_TRIPLET/llvm-runtimes-build/install/lib/libcompiler_rt.a" ] || { echo "compiler-rt not found — run build-llvm-runtimes.sh first" >&2; exit 1; }
[ -f "$MUSL_SYSROOT/usr/lib/libc.a" ] || { echo "musl libc not built — run build-musl.sh first" >&2; exit 1; }

mkdir -p "$BUILD_HOME" "$INSTALL_DIR"

CLANG_BIN="$(command -v clang 2>/dev/null || echo clang)"
CLANGXX_BIN="$(command -v clang++ 2>/dev/null || echo clang++)"
AR_BIN="$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)"
RANLIB_BIN="$(command -v llvm-ranlib 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ranlib)"

B1NIX_DEFINES="-D__B1NIX__=1 -D_POSIX_THREADS=1 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE=1"

if [ ! -f "$INSTALL_DIR/lib/libc++.a" ] || [ "${REBUILD:-0}" = "1" ]; then
    echo "Configuring LLVM runtimes for musl ($B1NIX_TRIPLET)..."
    rm -rf "$RT_BUILD"
    mkdir -p "$RT_BUILD"
    
    # We pass CMAKE_SYSROOT to point to the musl install dir so that the build finds musl headers & library
    cmake -G "Unix Makefiles" \
        -S "$SRC_DIR/runtimes" -B "$RT_BUILD" \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR="$B1NIX_GCC_ARCH" \
        -DCMAKE_C_COMPILER="$CLANG_BIN" \
        -DCMAKE_CXX_COMPILER="$CLANGXX_BIN" \
        -DCMAKE_AR="$AR_BIN" \
        -DCMAKE_RANLIB="$RANLIB_BIN" \
        -DCMAKE_C_COMPILER_TARGET="$B1NIX_TRIPLET" \
        -DCMAKE_CXX_COMPILER_TARGET="$B1NIX_TRIPLET" \
        -DCMAKE_ASM_COMPILER="$CLANG_BIN" \
        -DCMAKE_ASM_COMPILER_TARGET="$B1NIX_TRIPLET" \
        -DCMAKE_SYSROOT="$MUSL_SYSROOT" \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
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

# Normalize standard archives
for name in libc++.a libc++abi.a libunwind.a; do
    found=$(find "$INSTALL_DIR/lib" -name "$name" 2>/dev/null | head -1)
    if [ -n "$found" ] && [ "$found" != "$INSTALL_DIR/lib/$name" ]; then
        cp -f "$found" "$INSTALL_DIR/lib/$name"
    fi
done

[ -f "$INSTALL_DIR/lib/libc++.a" ]    || { echo "ERROR: libc++.a not produced" >&2; exit 1; }
[ -f "$INSTALL_DIR/lib/libc++abi.a" ] || { echo "ERROR: libc++abi.a not produced" >&2; exit 1; }
[ -f "$INSTALL_DIR/lib/libunwind.a" ] || { echo "ERROR: libunwind.a not produced" >&2; exit 1; }

# Strip .deplibs
OBJCOPY_BIN="$(command -v llvm-objcopy 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-objcopy)"
for name in libc++.a libc++abi.a libunwind.a; do
    "$OBJCOPY_BIN" --remove-section=.deplibs "$INSTALL_DIR/lib/$name" 2>/dev/null || true
done

# Drop libunwind from libc++.a (avoid double unwinder)
AR_BIN_DEL="$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)"
"$AR_BIN_DEL" d "$INSTALL_DIR/lib/libc++.a" \
    libunwind.cpp.o Unwind-EHABI.cpp.o Unwind-seh.cpp.o UnwindLevel1.c.o \
    UnwindLevel1-gcc-ext.c.o Unwind-sjlj.c.o Unwind-wasm.c.o \
    UnwindRegistersRestore.S.o UnwindRegistersSave.S.o 2>/dev/null || true
"$RANLIB_BIN" "$INSTALL_DIR/lib/libc++.a" 2>/dev/null || true

# Copy headers
LIBCXX_HDR_DIR="$INSTALL_DIR/include/c++/v1"
if [ ! -f "$LIBCXX_HDR_DIR/__config" ]; then
    alt=$(find "$INSTALL_DIR/include" -path '*/c++/v1/__config' 2>/dev/null | head -1)
    [ -n "$alt" ] && LIBCXX_HDR_DIR="$(dirname "$alt")"
fi

echo "Installing musl C++ static libraries and headers..."
mkdir -p "$MUSL_SYSROOT/usr/lib" "$MUSL_SYSROOT/usr/include/c++/v1"
for name in libc++.a libc++abi.a libunwind.a; do
    cp -f "$INSTALL_DIR/lib/$name" "$MUSL_SYSROOT/usr/lib/$name"
done
cp -R "$LIBCXX_HDR_DIR/"* "$MUSL_SYSROOT/usr/include/c++/v1/"

# ── Link shared libraries ──
LD_BIN="$(command -v ld.lld 2>/dev/null || echo ld.lld)"
CRT_A="$PROJECT_DIR/build/toolchain_build/$B1NIX_TRIPLET/llvm-runtimes-build/install/lib/libcompiler_rt.a"
LIBC_SO="$MUSL_SYSROOT/usr/lib/libc.so"
ABI_SO="$MUSL_SYSROOT/usr/lib/libc++abi.so.1"
CXX_SO="$MUSL_SYSROOT/usr/lib/libc++.so.1"

# version script to localize frame registration (avoid crash on throw)
VSCRIPT="$(mktemp)"
cat > "$VSCRIPT" <<'EOF'
{
  global: *;
  local:
    __register_frame;
    __deregister_frame;
};
EOF

echo "Linking shared libc++abi.so.1..."
"$LD_BIN" -shared --hash-style=sysv -soname libc++abi.so.1 --eh-frame-hdr -o "$ABI_SO" \
    --whole-archive "$MUSL_SYSROOT/usr/lib/libc++abi.a" --no-whole-archive \
    -L"$MUSL_SYSROOT/usr/lib" "$LIBC_SO" "$CRT_A" \
    --version-script "$VSCRIPT" \
    --allow-shlib-undefined
rm -f "$VSCRIPT"
ln -sf libc++abi.so.1 "$MUSL_SYSROOT/usr/lib/libc++abi.so"

echo "Linking shared libc++.so.1..."
"$LD_BIN" -shared --hash-style=sysv -soname libc++.so.1 --eh-frame-hdr -o "$CXX_SO" \
    --whole-archive "$MUSL_SYSROOT/usr/lib/libc++.a" --no-whole-archive \
    -L"$MUSL_SYSROOT/usr/lib" "$ABI_SO" "$LIBC_SO" "$CRT_A" \
    --allow-shlib-undefined
ln -sf libc++.so.1 "$MUSL_SYSROOT/usr/lib/libc++.so"

# patchelf sonames
if command -v patchelf >/dev/null 2>&1; then
    for so in "$ABI_SO" "$CXX_SO"; do
        for n in $(patchelf --print-needed "$so" 2>/dev/null); do
            case "$n" in
                */libc.so) patchelf --replace-needed "$n" libc.so "$so" 2>/dev/null || true ;;
                */libc++abi.so.1) patchelf --replace-needed "$n" libc++abi.so.1 "$so" 2>/dev/null || true ;;
            esac
        done
    done
fi

# stage to rootfs
for d in "$PROJECT_DIR/build/$B1NIX_ARCH/rootfs/lib"; do
    mkdir -p "$d"
    cp -f "$ABI_SO" "$d/libc++abi.so.1"; ln -sf libc++abi.so.1 "$d/libc++abi.so"
    cp -f "$CXX_SO"  "$d/libc++.so.1";   ln -sf libc++.so.1    "$d/libc++.so"
done

echo "build-libcxx-musl.sh: libc++ built and installed successfully for musl!"
