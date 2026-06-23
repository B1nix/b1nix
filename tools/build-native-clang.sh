#!/bin/sh
# Build a NATIVE clang/clang++ for b1nix (runs ON b1nix), REUSING the b1nix
# libLLVM.so already cross-built for the Rust native toolchain — no LLVM rebuild.
#
# Two stages (cross-compiling clang needs HOST-native TableGen tools):
#   A. Host clang-tblgen + llvm-tblgen (system compiler) — clang's .td -> .inc.
#   B. STANDALONE clang build (cmake -S .../clang) against the installed b1nix
#      LLVM via LLVM_DIR, cross-compiled with the b1nix cross GCC, linking the
#      existing libLLVM.so. Builds ONLY clang (~1500 targets), reuses LLVM.
#
# Output: build/native-clang/b1nix/bin/clang (+ clang++ symlink) — an x86_64
# b1nix ELF, DT_NEEDED libLLVM.so, that the M69 loader can run.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_SRC="$ROOT/build/rust-native/rust-src-full/src/llvm-project"
# Installed (cross-built) b1nix LLVM: libLLVM.so + static libs + cmake exports.
B1NIX_LLVM="$ROOT/build/rust-native/rust-src-full/build/x86_64-unknown-b1nix/llvm"
CROSS="$ROOT/build/toolchain_build/x86_64-b1nix/cross"
HOST_TBLGEN="$ROOT/build/native-clang/host-tblgen"
CLANG_BUILD="$ROOT/build/native-clang/b1nix"
JOBS="${NATIVE_CLANG_JOBS:-$(nproc)}"

[ -d "$LLVM_SRC/clang" ] || { echo "no clang source at $LLVM_SRC/clang" >&2; exit 1; }
[ -f "$B1NIX_LLVM/lib/cmake/llvm/LLVMConfig.cmake" ] || { echo "no installed b1nix LLVM cmake at $B1NIX_LLVM/lib/cmake/llvm" >&2; exit 1; }
[ -x "$CROSS/bin/x86_64-b1nix-gcc" ] || { echo "no cross gcc at $CROSS" >&2; exit 1; }

# -isystem a private fixinclude dir holding the up-to-date b1nix errno.h ahead of
# the cross sysroot: the sysroot is SHARED with parallel worktrees whose header
# staging can overwrite errno.h (which would drop ENOTRECOVERABLE and break
# libstdc++ std::errc::state_not_recoverable in clangInterpreter). This makes the
# clang build immune to that clobber. Only errno.h is overridden; other headers
# fall through to the sysroot.
mkdir -p "$ROOT/build/native-clang/fixinclude"
cp "$ROOT/userspace/include/errno.h" "$ROOT/build/native-clang/fixinclude/errno.h"
CFLAGS_B1NIX="-isystem $ROOT/build/native-clang/fixinclude -ffunction-sections -fdata-sections -fPIC -m64 -D__linux__=1"

# ── Stage A: host TableGen tools (built once, cached) ─────────────────────────
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

# The standalone clang build resolves llvm-tblgen/clang-tblgen via the installed
# LLVM's LLVM_TOOLS_BINARY_DIR ($B1NIX_LLVM/bin), where the cross-built (b1nix)
# llvm-tblgen can't run on the host. Those tools are build-time-only and the
# b1nix copies are useless on the host, so point the imported paths at the
# host-native Stage-A tools (backing up the b1nix llvm-tblgen).
if [ ! -L "$B1NIX_LLVM/bin/llvm-tblgen" ]; then
	[ -e "$B1NIX_LLVM/bin/llvm-tblgen.b1nix-orig" ] || mv "$B1NIX_LLVM/bin/llvm-tblgen" "$B1NIX_LLVM/bin/llvm-tblgen.b1nix-orig"
	ln -sf "$HOST_TBLGEN/bin/llvm-tblgen" "$B1NIX_LLVM/bin/llvm-tblgen"
fi
ln -sf "$HOST_TBLGEN/bin/clang-tblgen" "$B1NIX_LLVM/bin/clang-tblgen"

# ── Stage B: STANDALONE clang against the installed b1nix LLVM (no LLVM rebuild) ─
echo "[native-clang] Stage B: configure standalone clang (reuse b1nix libLLVM.so)"
cmake -G Ninja -S "$LLVM_SRC/clang" -B "$CLANG_BUILD" \
	-DCMAKE_BUILD_TYPE=Release \
	-DLLVM_DIR="$B1NIX_LLVM/lib/cmake/llvm" \
	-DCMAKE_C_COMPILER="$CROSS/bin/x86_64-b1nix-gcc" \
	-DCMAKE_CXX_COMPILER="$CROSS/bin/x86_64-b1nix-g++" \
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
	-DCLANG_DEFAULT_LINKER="ld" \
	-DCLANG_DEFAULT_CXX_STDLIB="libstdc++" \
	-DCLANG_DEFAULT_RTLIB="libgcc" \
	>/dev/null

echo "[native-clang] Stage B: ninja clang (standalone, links the existing libLLVM.so)"
ninja -C "$CLANG_BUILD" -j"$JOBS" clang clang-resource-headers

CLANG="$CLANG_BUILD/bin/clang"
if [ -x "$CLANG" ]; then
	echo "[native-clang] built: $CLANG"
	llvm-readelf -hd "$CLANG" 2>/dev/null | grep -iE "Type:|Machine:|NEEDED" | head
else
	echo "[native-clang] FAILED: no $CLANG" >&2
	exit 1
fi
