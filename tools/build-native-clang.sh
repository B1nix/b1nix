#!/bin/sh
# Build a NATIVE clang/clang++ for b1nix (runs ON b1nix), reusing the b1nix
# libLLVM.so already cross-built for the Rust native toolchain.
#
# Two stages (cross-compiling clang needs HOST-native TableGen tools):
#   A. Host clang-tblgen + llvm-tblgen (system compiler) — clang's .td -> .inc.
#   B. Add clang to the existing b1nix LLVM CMake build (LLVM_ENABLE_PROJECTS=
#      clang) and `ninja clang clang-resource-headers`, reusing the LLVM .o and
#      linking the b1nix libLLVM dylib. Cross toolchain = the b1nix cross GCC,
#      same flags the Rust LLVM build used.
#
# Output: build/native-clang/b1nix/bin/clang (+ clang++ symlink) — an x86_64
# b1nix ELF, DT_NEEDED libLLVM.so, that the M69 loader can run.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_SRC="$ROOT/build/rust-native/rust-src-full/src/llvm-project"
B1NIX_LLVM="$ROOT/build/rust-native/rust-src-full/build/x86_64-unknown-b1nix/llvm/build"
CROSS="$ROOT/build/toolchain_build/x86_64-b1nix/cross"
HOST_TBLGEN="$ROOT/build/native-clang/host-tblgen"
JOBS="${NATIVE_CLANG_JOBS:-$(nproc)}"

[ -d "$LLVM_SRC/clang" ] || { echo "no clang source at $LLVM_SRC/clang" >&2; exit 1; }
[ -f "$B1NIX_LLVM/CMakeCache.txt" ] || { echo "no b1nix LLVM build at $B1NIX_LLVM" >&2; exit 1; }
[ -x "$CROSS/bin/x86_64-b1nix-gcc" ] || { echo "no cross gcc at $CROSS" >&2; exit 1; }

CCACHE=""
command -v ccache >/dev/null 2>&1 && CCACHE="ccache"

# ── Stage A: host TableGen tools (only built once) ────────────────────────────
if [ ! -x "$HOST_TBLGEN/bin/clang-tblgen" ] || [ ! -x "$HOST_TBLGEN/bin/llvm-tblgen" ]; then
	echo "[native-clang] Stage A: host clang-tblgen + llvm-tblgen"
	cmake -G Ninja -S "$LLVM_SRC/llvm" -B "$HOST_TBLGEN" \
		-DCMAKE_BUILD_TYPE=Release \
		-DLLVM_ENABLE_PROJECTS="clang" \
		-DLLVM_TARGETS_TO_BUILD="X86" \
		-DLLVM_ENABLE_ASSERTIONS=OFF \
		${CCACHE:+-DLLVM_CCACHE_BUILD=ON} \
		>/dev/null
	ninja -C "$HOST_TBLGEN" -j"$JOBS" clang-tblgen llvm-tblgen
else
	echo "[native-clang] Stage A: host tblgen already built, skipping"
fi

# ── Stage B: add clang to the b1nix LLVM build and cross-build it ─────────────
echo "[native-clang] Stage B: reconfigure b1nix LLVM build with clang"
CFLAGS_B1NIX="-ffunction-sections -fdata-sections -fPIC -m64 -D__linux__=1"
cmake -S "$LLVM_SRC/llvm" -B "$B1NIX_LLVM" \
	-DLLVM_ENABLE_PROJECTS="clang" \
	-DCLANG_TABLEGEN="$HOST_TBLGEN/bin/clang-tblgen" \
	-DLLVM_TABLEGEN="$HOST_TBLGEN/bin/llvm-tblgen" \
	-DCLANG_DEFAULT_LINKER="ld" \
	-DCLANG_DEFAULT_CXX_STDLIB="libstdc++" \
	-DCLANG_DEFAULT_RTLIB="libgcc" \
	-DLLVM_LINK_LLVM_DYLIB=ON \
	>/dev/null

echo "[native-clang] Stage B: ninja clang (cross, reuses LLVM .o, links libLLVM.so)"
ninja -C "$B1NIX_LLVM" -j"$JOBS" clang clang-resource-headers

CLANG="$B1NIX_LLVM/bin/clang"
if [ -x "$CLANG" ]; then
	echo "[native-clang] built: $CLANG"
	llvm-readelf -hd "$CLANG" 2>/dev/null | grep -iE "Type:|Machine:|NEEDED" | head
else
	echo "[native-clang] FAILED: no $CLANG" >&2
	exit 1
fi
