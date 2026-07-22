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

CROSS="$TOOLCHAIN_BUILD_HOME/cross"
SKIA="$ROOT_DIR/build/src/skia"
SKIA_OUT="$ROOT_DIR/build/$B1NIX_ARCH/ports/skia/build/out/b1nix"
[ -d "$SKIA_OUT" ] || SKIA_OUT="$ROOT_DIR/build/src/skia/out/b1nix"
SKIA_INSTALL="$ROOT_DIR/build/$B1NIX_ARCH/ports/skia/install"
MESA="$ROOT_DIR/build/$B1NIX_ARCH/ports/mesa/install"
WRAPPER="$ROOT_DIR/tools/ports/b1nix-cross-cc.sh"

# Source file: try .cpp first (C++ Skia API), then .c
SRC="$ROOT_DIR/userspace/bin/${NAME}.cpp"
[ -f "$SRC" ] || SRC="$ROOT_DIR/userspace/bin/${NAME}.c"
[ -f "$SRC" ] || { echo "build-m91-skia: source not found for $NAME" >&2; exit 1; }

mkdir -p "$(dirname "$OUTPUT")"
OUTPUT_ABS="$(cd "$(dirname "$OUTPUT")" 2>/dev/null && pwd)/$(basename "$OUTPUT")"
mkdir -p "$(dirname "$OUTPUT_ABS")"

# Skia itself is an application library, but the executable uses the real
# dynamic musl runtime like the other C++ smoke binaries.
make B1NIX_ARCH="$B1NIX_ARCH" LINK=musl -C "$ROOT_DIR/userspace" -s \
  "build/$B1NIX_ARCH/libb1gui.a" 1>&2

# Dawn headers (for graphite-dawn compile check only). Dawn is optional: the
# CMake build may fail (WARNING in build-skia.sh) — only define HAVE_DAWN when
# the generated webgpu headers AND the combined static lib actually exist.
DAWN_DIR="$SKIA/third_party/externals/dawn"
DAWN_GEN_DIR="$SKIA_OUT/gen/third_party/dawn"
DAWN_FLAGS=""
DAWN_LIB=""
if [ -f "$DAWN_GEN_DIR/include/dawn/webgpu_cpp.h" ] && [ -f "$SKIA_OUT/libdawn_combined.a" ]; then
  DAWN_FLAGS="-DHAVE_DAWN -I $DAWN_DIR/include -I $DAWN_GEN_DIR/include"
  DAWN_LIB="$SKIA_OUT/libdawn_combined.a"
fi

# Compile main source and b1egl_mesa.c
echo "Compiling $NAME (b1nix cross)..." >&2
# shellcheck disable=SC2086
"$WRAPPER" -std=c++20 -O2 -ffunction-sections -fdata-sections \
  -I "$SKIA" \
  -I "$MESA/include" \
  $DAWN_FLAGS \
  -c "$SRC" -o "${OUTPUT_ABS}.o"

EGL_SRC="$ROOT_DIR/userspace/libegl/b1egl_mesa.c"
EGL_RES_SRC="$ROOT_DIR/userspace/libegl/egl_proc_resolver.c"
EGL_OBJ="${OUTPUT_ABS}_egl.o"
EGL_RES_OBJ="${OUTPUT_ABS}_egl_res.o"
"$WRAPPER" -O2 -ffunction-sections -fdata-sections \
  -I "$SKIA" \
  -I "$MESA/include" \
  -I "$ROOT_DIR/userspace/include" \
  -c "$EGL_SRC" -o "$EGL_OBJ"
"$WRAPPER" -O2 -ffunction-sections -fdata-sections \
  -I "$SKIA" \
  -I "$MESA/include" \
  -I "$ROOT_DIR/userspace/include" \
  -c "$EGL_RES_SRC" -o "$EGL_RES_OBJ"

# Link — Skia static, Mesa shared via -lOSMesa.
echo "Linking $NAME (b1nix cross, dynamic Mesa)..." >&2

LLD="${B1NIX_LLD:-$(command -v ld.lld 2>/dev/null || echo /usr/bin/ld.lld)}"
CROSS_DIR="$TOOLCHAIN_BUILD_HOME/cross"
LIBCXX_RT="$ROOT_DIR/build/$B1NIX_ARCH/ports/musl/install/lib"
[ -d "$LIBCXX_RT" ] || LIBCXX_RT="$TOOLCHAIN_BUILD_HOME/llvm-runtimes-build-musl/libcxx-install/lib"
LD_M="elf_x86_64"

# Skia static libs: core libskia.a + Skottie module + deps
SKIA_A="$SKIA_OUT/libskia.a"
[ -f "$SKIA_A" ] || SKIA_A="$SKIA_INSTALL/lib/libskia.a"
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
MUSL_LIB="$ROOT_DIR/build/$B1NIX_ARCH/ports/musl/install/lib"
USER_LIB="$ROOT_DIR/userspace/build/$B1NIX_ARCH"
FONTCONFIG_A="$ROOT_DIR/build/$B1NIX_ARCH/ports/fontconfig/install/lib/libfontconfig.a"
FREETYPE_A="$ROOT_DIR/build/$B1NIX_ARCH/ports/freetype/install/lib/libfreetype.a"
ZLIB_A="$ROOT_DIR/build/$B1NIX_ARCH/ports/zlib/install/lib/libz.a"
EXPAT_A="$ROOT_DIR/build/$B1NIX_ARCH/ports/expat/install/lib/libexpat.a"

EGL_A=""
[ -f "$USER_LIB/libEGL_b1nix.a" ] && EGL_A="$USER_LIB/libEGL_b1nix.a"
[ -f "$MUSL_LIB/libEGL_b1nix.a" ] && EGL_A="$MUSL_LIB/libEGL_b1nix.a"

C11_A=""
[ -f "$USER_LIB/libb1nix_c11threads.a" ] && C11_A="--whole-archive $USER_LIB/libb1nix_c11threads.a --no-whole-archive"
[ -f "$MUSL_LIB/libb1nix_c11threads.a" ] && C11_A="--whole-archive $MUSL_LIB/libb1nix_c11threads.a --no-whole-archive"

"$LLD" \
  --sysroot="$CROSS_DIR" \
  -m "$LD_M" \
  --hash-style=both \
  -pie \
  --dynamic-linker /lib/ld-musl-x86_64.so.1 \
  --allow-shlib-undefined \
  -z norelro \
  "$MUSL_LIB/Scrt1.o" "$MUSL_LIB/crti.o" \
  -L "$MUSL_LIB" \
  -L "$LIBCXX_RT" \
  -L "$USER_LIB" \
  -L "$MESA_LIB" \
  --start-group \
  "${OUTPUT_ABS}.o" "$EGL_OBJ" "$EGL_RES_OBJ" \
  "$SKIA_A" \
  $SKOTTIE_LIBS \
  $DAWN_LIB \
  $EGL_A \
  $C11_A \
  "$FONTCONFIG_A" "$FREETYPE_A" \
  "$ZLIB_A" "$EXPAT_A" \
  "$USER_LIB/libb1gui.a" \
  -lOSMesa \
  -Bstatic -lc++ -lc++abi -lunwind -lcompiler_rt -Bdynamic \
  -L "$MUSL_LIB" -lc -lpthread -ldl "$MUSL_LIB/crtn.o" \
  --end-group \
  --export-dynamic \
  --gc-sections \
  -o "$OUTPUT_ABS"

# Strip debug info to reduce size
STRIP="$(command -v llvm-strip 2>/dev/null || command -v strip 2>/dev/null || echo strip)"
"$STRIP" --strip-unneeded "$OUTPUT_ABS" 2>/dev/null || true

rm -f "${OUTPUT_ABS}.o"
echo "Built: $OUTPUT_ABS ($(stat -c%s "$OUTPUT_ABS" 2>/dev/null || stat -f%z "$OUTPUT_ABS" 2>/dev/null || echo '?') bytes)" >&2
