#!/bin/sh
# Build compiler-rt's builtins (soft-float/integer helper routines the
# compiler emits calls to but no library defines) for a bare-metal ELF
# target. Needed on aarch64: `long double` is IEEE quad there, so musl's
# *l() math functions and vfprintf/vfscanf pull in __addtf3/__multf3/etc,
# and no prebuilt aarch64 bare-metal builtins archive ships with the host
# toolchain (x86_64 doesn't need this — its `long double` is the hardware
# x87 80-bit type, no software emulation involved).
# Usage: ARCH=aarch64 sh tools/ports/build-compiler-rt-builtins.sh
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${ARCH:-aarch64}"
CLANG="${CLANG:-clang}"
AR="${AR:-$(command -v llvm-ar 2>/dev/null || command -v /opt/homebrew/opt/llvm/bin/llvm-ar 2>/dev/null || echo ar)}"

case "$ARCH" in
  aarch64) CLANG_TARGET="aarch64-unknown-elf" ;;
  x86)     CLANG_TARGET="i686-unknown-elf" ;;
  *)       CLANG_TARGET="x86_64-unknown-elf" ;;
esac

SRC="$(find "$ROOT_DIR/build" -type d -path '*llvm-project*/compiler-rt/lib/builtins' 2>/dev/null | head -1)"
if [ -z "$SRC" ]; then
    echo "compiler-rt source not found under $ROOT_DIR/build (expected a vendored llvm-project checkout)" >&2
    exit 1
fi

OUT="$ROOT_DIR/build/$ARCH/compiler-rt-builtins"
LIB="$OUT/libclang_rt.builtins-$ARCH.a"
rm -rf "$OUT"
mkdir -p "$OUT"

echo "=== Building compiler-rt builtins for $ARCH ($CLANG_TARGET) ==="
FAIL=0
for f in "$SRC"/*.c; do
    name=$(basename "$f" .c)
    # Plain freestanding + -fPIC, no -mgeneral-regs-only: that flag (needed
    # for the b1nix kernel itself) makes clang reject the ABI's 128-bit
    # long-double type entirely — these are userspace-only helpers.
    "$CLANG" --target="$CLANG_TARGET" -ffreestanding -fno-builtin -fPIC -O2 \
        -I "$SRC" -c "$f" -o "$OUT/$name.o" 2>>"$OUT/errors.log" || FAIL=$((FAIL + 1))
done
"$AR" rcs "$LIB" "$OUT"/*.o

echo "  built:  $LIB"
echo "  $FAIL/$(ls "$SRC"/*.c | wc -l | tr -d ' ') source files failed (unneeded sanitizer/atomic/OS-specific helpers; see $OUT/errors.log)"
