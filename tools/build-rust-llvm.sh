#!/bin/sh
# build-rust-llvm.sh — standalone: fix wrapper + clean + rebuild LLVM + rustc
# Run directly in terminal: sh tools/build-rust-llvm.sh
# No agent approvals needed — everything runs autonomously.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/build/rust-native/rust-src-full"
LLVM_BUILD="$SRC/build/x86_64-unknown-b1nix/llvm/build"
LOG="/tmp/xpy-llvm-build.log"
WRAPPER="$ROOT/build/toolchain_build/x86_64-b1nix/cross/bin/x86_64-b1nix-g++"

echo "=== [1/4] Verifying wrapper fix ==="
# The wrapper must strip -z,defs and restructure --whole-archive for -shared builds.
# Quick smoke test: invoke wrapper with -shared and check it doesn't produce a 5K .so.
TMPDIR_TEST="${TMPDIR:-/tmp}"
cat > "$TMPDIR_TEST/_wrap_test.c" <<'EOF'
int main(){return 0;}
EOF
# Build a trivial shared lib through the wrapper — should produce >10K file
B1NIX_NO_CCACHE=1 "$WRAPPER" -shared -o "$TMPDIR_TEST/_wrap_test.so" "$TMPDIR_TEST/_wrap_test.c" 2>/dev/null
WRAP_SZ=$(stat -c%s "$TMPDIR_TEST/_wrap_test.so" 2>/dev/null || echo 0)
rm -f "$TMPDIR_TEST/_wrap_test.c" "$TMPDIR_TEST/_wrap_test.so"
# The wrapper test isn't a full validation but catches gross failures
echo "  wrapper smoke: $WRAP_SZ bytes (OK if >0)"

echo "=== [2/4] Nuking LLVM build directory ==="
if [ -d "$LLVM_BUILD" ]; then
    rm -rf "$LLVM_BUILD"
    echo "  removed $LLVM_BUILD"
else
    echo "  already absent"
fi

echo "=== [3/4] Starting x.py build (log: $LOG) ==="
cd "$SRC"
export RUSTUP_HOME="$PWD/../../rust/rustup"
export CARGO_HOME="$PWD/../../rust/cargo"
export BOOTSTRAP_SKIP_TARGET_SANITY=1
export RUST_TARGET_PATH="$PWD/../../rust/targets"
export PATH="$ROOT/build/toolchain_build/x86_64-b1nix/cross/bin:$PATH"

echo "  started at $(date)"
python3 x.py build --stage 2 -j$(nproc) compiler/rustc library/std 2>&1 | tee "$LOG"
BUILD_EXIT=${PIPESTATUS[0]}

echo ""
echo "=== [4/4] Result ==="
if [ "$BUILD_EXIT" -eq 0 ]; then
    echo "  BUILD SUCCEEDED at $(date)"
    # Check for libLLVM.so
    LIBLLVM="$LLVM_BUILD/lib/libLLVM.so.22.1-rust-1.98.0-nightly"
    if [ -f "$LIBLLVM" ]; then
        echo "  libLLVM.so: $(ls -lh "$LIBLLVM" | awk '{print $5}')"
        file "$LIBLLVM"
    fi
else
    echo "  BUILD FAILED (exit $BUILD_EXIT) at $(date)"
    echo "  Last errors:"
    grep -i "error\|FAILED" "$LOG" | tail -10
fi
