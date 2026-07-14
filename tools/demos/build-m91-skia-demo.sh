#!/bin/sh
# Build the M91 Skia smoke demo — cross-compiled for b1nix.
# Links Mesa as shared .so (libOSMesa.so) — the kernel mounts rootfs.img
# at / so the dynamic linker finds the .so files at /lib/ at runtime.
# Skia itself is static (libskia.a), only Mesa/GL are shared.
set -eu

NAME="${1:?usage: build-m91-skia-demo.sh <name> <output>}"
OUTPUT="${2:?}"
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

. "$ROOT_DIR/tools/toolchain/env.sh"

CROSS="$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/cross"
SKIA="$ROOT_DIR/build/ports-src/skia"
SKIA_OUT="$SKIA/out/b1nix"
MESA="$ROOT_DIR/build/mesa-b1nix/$B1NIX_TRIPLET/install"
SYSROOT_LIB="$CROSS/$B1NIX_TRIPLET/lib"
WRAPPER="$ROOT_DIR/tools/ports/b1nix-cross-cc.sh"

# Source file: try .cpp first (C++ Skia API), then .c
SRC="$ROOT_DIR/userspace/bin/${NAME}.cpp"
[ -f "$SRC" ] || SRC="$ROOT_DIR/userspace/bin/${NAME}.c"
[ -f "$SRC" ] || { echo "build-m91-skia: source not found for $NAME" >&2; exit 1; }

mkdir -p "$(dirname "$OUTPUT")"
OUTPUT_ABS="$(cd "$(dirname "$OUTPUT")" 2>/dev/null && pwd)/$(basename "$OUTPUT")"
mkdir -p "$(dirname "$OUTPUT_ABS")"

# Dawn headers (for graphite-dawn compile check only)
DAWN_DIR="$SKIA/third_party/externals/dawn"
DAWN_GEN_DIR="$SKIA_OUT/gen/third_party/dawn"

# Compile
echo "Compiling $NAME (b1nix cross)..." >&2
"$WRAPPER" -std=c++20 -O2 -ffunction-sections -fdata-sections \
  -I "$SKIA" \
  -I "$MESA/include" \
  -I "$DAWN_DIR/include" \
  -I "$DAWN_GEN_DIR/include" \
  -c "$SRC" -o "${OUTPUT_ABS}.o"

# Link — Skia static, Mesa shared via -lOSMesa.
echo "Linking $NAME (b1nix cross, dynamic Mesa)..." >&2

LLD="${B1NIX_LLD:-$(command -v ld.lld 2>/dev/null || echo /usr/bin/ld.lld)}"
CROSS_DIR="$CROSS"
LIBCXX_RT="$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/llvm-runtimes-build/libcxx-install/lib"
LD_M="elf_x86_64"

# Skia static libs: core libskia.a + Skottie module + deps
SKIA_A="$SKIA_OUT/libskia.a"
[ -f "$SKIA_A" ] || { echo "build-m91-skia: missing $SKIA_A" >&2; exit 1; }

# Skottie and its transitive deps (all static, built with skia_enable_skottie=true)
SKOTTIE_LIBS=""
for lib in libskottie.a libsksg.a libskresources.a libjsonreader.a \
           libskshaper.a libskunicode_core.a libskunicode_icu.a \
           libicu.a libicu_bidi.a; do
    L="$SKIA_OUT/$lib"
    [ -f "$L" ] && SKOTTIE_LIBS="$SKOTTIE_LIBS $L"
done

# Mesa shared libs — libOSMesa.so is the only consumer-facing lib;
# shared-glapi=disabled means glapi is folded into libOSMesa.so.
MESA_LIB="$MESA/lib"

"$LLD" \
  --sysroot="$CROSS_DIR" \
  -m "$LD_M" \
  --hash-style=both \
  -T "$ROOT_DIR/userspace/linker.ld" \
  -z norelro \
  "$ROOT_DIR/userspace/build/$B1NIX_ARCH/crt/crt0.o" \
  -L "$SYSROOT_LIB" \
  -L "$LIBCXX_RT" \
  -L "$MESA_LIB" \
  -L "$ROOT_DIR/userspace/build/$B1NIX_ARCH" \
  "${OUTPUT_ABS}.o" \
  --start-group \
  "$SKIA_A" \
  $SKOTTIE_LIBS \
  "$SYSROOT_LIB/libEGL_b1nix.a" \
  --whole-archive -lb1nix_c11threads --no-whole-archive \
  "$SYSROOT_LIB/libfontconfig.a" "$SYSROOT_LIB/libfreetype.a" \
  "$SYSROOT_LIB/libz.a" "$SYSROOT_LIB/libexpat.a" \
  "$ROOT_DIR/userspace/build/$B1NIX_ARCH/libb1nix.a" \
  "$ROOT_DIR/userspace/build/$B1NIX_ARCH/libb1gui.a" \
  -lOSMesa \
  -Bstatic -lc++ -lc++abi -lunwind -lcompiler_rt -Bdynamic -lpthread -ldl -lm \
  --end-group \
  --export-dynamic \
  --gc-sections \
  -o "$OUTPUT_ABS"

# Strip debug info to reduce size
STRIP="$(command -v llvm-strip 2>/dev/null || command -v strip 2>/dev/null || echo strip)"
"$STRIP" --strip-unneeded "$OUTPUT_ABS" 2>/dev/null || true

rm -f "${OUTPUT_ABS}.o"
echo "Built: $OUTPUT_ABS ($(stat -c%s "$OUTPUT_ABS" 2>/dev/null || stat -f%z "$OUTPUT_ABS" 2>/dev/null || echo '?') bytes)" >&2
