#!/usr/bin/env bash
# tools/build-native-clang.sh
#
# Build a FULL NATIVE LLVM toolchain for b1nix (runs ON b1nix).
# Produces: clang, clang++, lld, llvm-ar, llvm-nm, llvm-objcopy, llvm-strip,
#           llvm-readelf, llvm-size, llvm-strings — all x86_64 b1nix ELFs.
#
# Two build strategies:
#   A) If Rust-built b1nix LLVM exists (build/rust-native/...): REUSE libLLVM.so,
#      only build clang + lld as standalone projects against it (fast, ~5 min).
#   B) If no pre-built LLVM: download LLVM 18.1.8 and build EVERYTHING
#      (llvm + clang + lld + compiler-rt) — full build (~20-40 min).
#
# Output: build/native-clang/b1nix/bin/{clang,clang++,lld,llvm-ar,...}
#
# Usage:
#   tools/build-native-clang.sh                         # auto-detect strategy
#   tools/build-native-clang.sh --full                  # force full LLVM build
#   tools/build-native-clang.sh --reuse-rust-llvm       # force reuse Rust LLVM
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/toolchain/env.sh" 2>/dev/null || true
B1NIX_TRIPLET="${B1NIX_TRIPLET:-x86_64-b1nix}"
CROSS="$ROOT/build/toolchain_build/$B1NIX_TRIPLET/cross"
HOST_TBLGEN="$ROOT/build/native-clang/host-tblgen"
CLANG_BUILD="$ROOT/build/native-clang/b1nix"
NATIVE_DEST="$ROOT/build/native-clang/installed"
JOBS="${NATIVE_CLANG_JOBS:-$(nproc 2>/dev/null || echo 4)}"
STRATEGY="${1:-auto}"

# ── Detect LLVM source availability ──────────────────────────────────────────
RUST_LLVM_SRC="$ROOT/build/rust-native/rust-src-full/src/llvm-project"
RUST_B1NIX_LLVM="$ROOT/build/rust-native/rust-src-full/build/x86_64-unknown-b1nix/llvm"
SELFHOST_LLVM_SRC="$ROOT/build/toolchain_build/llvm-runtimes-build/llvm-project-18.1.8.src"

LLVM_SRC=""
B1NIX_LLVM=""
case "$STRATEGY" in
  --reuse-rust-llvm)
    LLVM_SRC="$RUST_LLVM_SRC"
    B1NIX_LLVM="$RUST_B1NIX_LLVM"
    ;;
  --full)
    LLVM_SRC="$SELFHOST_LLVM_SRC"
    ;;
  auto|*)
    if [ -d "$RUST_LLVM_SRC/clang" ] && [ -f "$RUST_B1NIX_LLVM/lib/cmake/llvm/LLVMConfig.cmake" ]; then
      LLVM_SRC="$RUST_LLVM_SRC"
      B1NIX_LLVM="$RUST_B1NIX_LLVM"
      echo "[native-clang] auto: reusing Rust-built b1nix LLVM"
    elif [ -d "$SELFHOST_LLVM_SRC/clang" ]; then
      LLVM_SRC="$SELFHOST_LLVM_SRC"
      echo "[native-clang] auto: using downloaded LLVM sources"
    else
      echo "[native-clang] no LLVM sources found — downloading LLVM 18.1.8..."
      LLVM_VER="18.1.8"
      LLVM_URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VER}/llvm-project-${LLVM_VER}.src.tar.xz"
      LLVM_DL="$ROOT/build/native-clang/llvm-${LLVM_VER}.src.tar.xz"
      mkdir -p "$(dirname "$LLVM_DL")"
      [ -f "$LLVM_DL" ] || curl -fL --retry 3 "$LLVM_URL" -o "$LLVM_DL"
      echo "[native-clang] extracting LLVM sources..."
      tar -xJf "$LLVM_DL" -C "$ROOT/build/native-clang/" 2>/dev/null
      LLVM_SRC="$ROOT/build/native-clang/llvm-project-${LLVM_VER}.src"
    fi
    ;;
esac

[ -d "$LLVM_SRC/clang" ] || { echo "no clang source at $LLVM_SRC/clang" >&2; exit 1; }
[ -x "$CROSS/bin/$B1NIX_TRIPLET-gcc" ] || { echo "no cross gcc at $CROSS — run build-toolchain.sh first" >&2; exit 1; }

# fixinclude: protect against sysroot header clobbering
mkdir -p "$ROOT/build/native-clang/fixinclude"
cp "$ROOT/userspace/include/errno.h" "$ROOT/build/native-clang/fixinclude/errno.h" 2>/dev/null || true
CFLAGS_B1NIX="-isystem $ROOT/build/native-clang/fixinclude -ffunction-sections -fdata-sections -fPIC -m64 -D__linux__=1"

# ── Stage A: host TableGen tools (needed for cross-compiling clang) ──────────
if [ ! -x "$HOST_TBLGEN/bin/clang-tblgen" ] || [ ! -x "$HOST_TBLGEN/bin/llvm-tblgen" ]; then
  echo "[native-clang] Stage A: host clang-tblgen + llvm-tblgen"
  cmake -G Ninja -S "$LLVM_SRC/llvm" -B "$HOST_TBLGEN" \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_ENABLE_PROJECTS="clang" \
    -DLLVM_TARGETS_TO_BUILD="X86" \
    -DLLVM_ENABLE_ASSERTIONS=OFF >/dev/null
  ninja -C "$HOST_TBLGEN" -j"$JOBS" clang-tblgen llvm-tblgen
else
  echo "[native-clang] Stage A: host tblgen already built, reusing"
fi

# ── Stage B: cross-compile LLVM + Clang + LLD for b1nix ─────────────────────
if [ -n "$B1NIX_LLVM" ] && [ -f "$B1NIX_LLVM/lib/cmake/llvm/LLVMConfig.cmake" ]; then
  # Strategy A: Reuse existing b1nix libLLVM.so — build only clang + lld
  echo "[native-clang] Stage B: standalone clang + lld against installed b1nix LLVM"

  # Fix tblgen paths
  if [ ! -L "$B1NIX_LLVM/bin/llvm-tblgen" ]; then
    [ -e "$B1NIX_LLVM/bin/llvm-tblgen.b1nix-orig" ] || \
      mv "$B1NIX_LLVM/bin/llvm-tblgen" "$B1NIX_LLVM/bin/llvm-tblgen.b1nix-orig" 2>/dev/null || true
    ln -sf "$HOST_TBLGEN/bin/llvm-tblgen" "$B1NIX_LLVM/bin/llvm-tblgen"
  fi
  ln -sf "$HOST_TBLGEN/bin/clang-tblgen" "$B1NIX_LLVM/bin/clang-tblgen"

  # Build clang
  if [ ! -x "$CLANG_BUILD/bin/clang" ]; then
    cmake -G Ninja -S "$LLVM_SRC/clang" -B "$CLANG_BUILD" \
      -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_DIR="$B1NIX_LLVM/lib/cmake/llvm" \
      -DCMAKE_C_COMPILER="$CROSS/bin/$B1NIX_TRIPLET-gcc" \
      -DCMAKE_CXX_COMPILER="$CROSS/bin/$B1NIX_TRIPLET-g++" \
      -DCMAKE_C_FLAGS="$CFLAGS_B1NIX" \
      -DCMAKE_CXX_FLAGS="$CFLAGS_B1NIX" \
      -DCMAKE_CROSSCOMPILING=True \
      -DCMAKE_SYSTEM_NAME=Linux \
      -DLLVM_TABLEGEN="$HOST_TBLGEN/bin/llvm-tblgen" \
      -DCLANG_TABLEGEN="$HOST_TBLGEN/bin/clang-tblgen" \
      -DLLVM_NATIVE_TOOL_DIR="$HOST_TBLGEN/bin" \
      -DLLVM_LINK_LLVM_DYLIB=ON \
      -DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-unknown-b1nix \
      -DCLANG_INCLUDE_TESTS=OFF \
      -DLLVM_INCLUDE_TESTS=OFF \
      -DCLANG_DEFAULT_LINKER="lld" \
      -DCLANG_DEFAULT_CXX_STDLIB="libc++" \
      -DCLANG_DEFAULT_RTLIB="compiler-rt" \
      >/dev/null
    ninja -C "$CLANG_BUILD" -j"$JOBS" clang clang-resource-headers lld
  fi
else
  # Strategy B: Full LLVM build — llvm + clang + lld
  echo "[native-clang] Stage B: full LLVM build (llvm + clang + lld)"
  FULL_BUILD="$ROOT/build/native-clang/full-build"
  if [ ! -x "$FULL_BUILD/bin/clang" ]; then
    cmake -G Ninja -S "$LLVM_SRC/llvm" -B "$FULL_BUILD" \
      -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_ENABLE_PROJECTS="clang;lld" \
      -DLLVM_TARGETS_TO_BUILD="X86" \
      -DLLVM_ENABLE_ASSERTIONS=OFF \
      -DLLVM_LINK_LLVM_DYLIB=ON \
      -DLLVM_BUILD_LLVM_DYLIB=ON \
      -DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-unknown-b1nix \
      -DCLANG_DEFAULT_LINKER="lld" \
      -DCLANG_DEFAULT_CXX_STDLIB="libc++" \
      -DCLANG_DEFAULT_RTLIB="compiler-rt" \
      -DCLANG_INCLUDE_TESTS=OFF \
      -DLLVM_INCLUDE_TESTS=OFF \
      -DCMAKE_CROSSCOMPILING=True \
      -DCMAKE_SYSTEM_NAME=Linux \
      -DCMAKE_C_COMPILER="$CROSS/bin/$B1NIX_TRIPLET-gcc" \
      -DCMAKE_CXX_COMPILER="$CROSS/bin/$B1NIX_TRIPLET-g++" \
      -DCMAKE_C_FLAGS="$CFLAGS_B1NIX" \
      -DCMAKE_CXX_FLAGS="$CFLAGS_B1NIX" \
      -DLLVM_TABLEGEN="$HOST_TBLGEN/bin/llvm-tblgen" \
      -DCLANG_TABLEGEN="$HOST_TBLGEN/bin/clang-tblgen" \
      -DLLVM_NATIVE_TOOL_DIR="$HOST_TBLGEN/bin" \
      >/dev/null
    ninja -C "$FULL_BUILD" -j"$JOBS" clang clang-resource-headers lld llvm-ar llvm-nm llvm-objcopy llvm-strip llvm-readelf
    CLANG_BUILD="$FULL_BUILD"
  fi
fi

# ── Stage C: Install native toolchain ────────────────────────────────────────
echo "[native-clang] Stage C: installing native toolchain"
mkdir -p "$NATIVE_DEST/bin" "$NATIVE_DEST/lib" "$NATIVE_DEST/include"

CLANG="$CLANG_BUILD/bin/clang"
if [ -x "$CLANG" ]; then
  echo "  built: $CLANG"
  llvm-readelf -hd "$CLANG" 2>/dev/null | grep -iE "Type:|Machine:|NEEDED" | head || true
else
  echo "  FAILED: no $CLANG" >&2
  exit 1
fi

# Symlink clang++ → clang
for tool in clang lld llvm-ar llvm-nm llvm-objcopy llvm-strip llvm-readelf llvm-size llvm-strings llvm-objdump; do
  src="$CLANG_BUILD/bin/$tool"
  [ -x "$src" ] && ln -sf "$src" "$NATIVE_DEST/bin/$tool" 2>/dev/null || true
done
[ -x "$NATIVE_DEST/bin/clang" ] && ln -sf clang "$NATIVE_DEST/bin/clang++" 2>/dev/null || true

# Copy resource headers (needed by -nostdinc)
if [ -d "$CLANG_BUILD/lib/clang" ]; then
  cp -R "$CLANG_BUILD/lib/clang" "$NATIVE_DEST/lib/" 2>/dev/null || true
fi

echo ""
echo "Native Clang toolchain installed: $NATIVE_DEST/bin/"
ls -la "$NATIVE_DEST/bin/" 2>/dev/null | grep -v "^total" | head -15
echo ""
echo "To install into rootfs for b1nix self-host:"
echo "  make install-native-clang"
echo "Or manually:"
echo "  cp -R $NATIVE_DEST/* build/$B1NIX_ARCH/rootfs/usr/"
