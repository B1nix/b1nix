#!/bin/sh
# Build libwebp's decoder as a static libwebpdecoder.a for the b1nix userspace
# ABI. Freestanding compile of the portable decode subset (no autotools/CMake).
# b1nix userspace is soft-float and the kernel does not enable SSE state for
# userspace, so -mno-sse -mno-mmx forces libwebp's generic C paths (the SIMD dsp
# files are excluded entirely). Decodes both lossy (VP8 intra-frame) and lossless
# WebP. Prints the install dir.
#
# M53 (NetSurf browser platform) image/video-keyframe codec dependency.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
WEBP_VERSION="${WEBP_VERSION:-1.4.0}"
TARBALL="libwebp-${WEBP_VERSION}.tar.gz"
URL="https://github.com/webmproject/libwebp/archive/refs/tags/v${WEBP_VERSION}.tar.gz"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

. "$ROOT_DIR/tools/toolchain-env.sh"

SRC_PARENT="$ROOT_DIR/build/libwebp-src"
SRC_DIR="$SRC_PARENT/libwebp-${WEBP_VERSION}"
BUILD_DIR="$ROOT_DIR/build/libwebp-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
GEN_DIR="$BUILD_DIR/gen"
INSTALL_DIR="$BUILD_DIR/install"

mkdir -p "$SRC_PARENT" "$OBJ_DIR" "$GEN_DIR/src/webp" "$INSTALL_DIR/include/webp" \
  "$INSTALL_DIR/lib"

# Minimal config.h (HAVE_CONFIG_H path; included as "src/webp/config.h").
# Deliberately defines NO WEBP_HAVE_SSE2/SSE41/NEON and NO WEBP_USE_THREAD →
# portable generic C dsp, single-threaded. The bswap builtins exist in clang.
cat > "$GEN_DIR/src/webp/config.h" <<'EOF'
#define HAVE_BUILTIN_BSWAP16 1
#define HAVE_BUILTIN_BSWAP32 1
#define HAVE_BUILTIN_BSWAP64 1
#define PACKAGE_NAME "libwebp"
#define PACKAGE_VERSION "1.4.0"
EOF

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$TARBALL"
  [ -f "$tmp" ] || curl -L "$URL" -o "$tmp" 1>&2
  tar -xzf "$tmp" -C "$SRC_PARENT" 1>&2
fi

if [ "$B1NIX_ARCH" = "x86" ]; then
  TARGET="i686-unknown-elf"
else
  TARGET="x86_64-unknown-elf"
fi

# Hardware float/SSE like the other b1nix library ports (Mesa/pixman) — b1nix
# userspace enables SSE state. libwebp's cpu.h gates SIMD on
# (!HAVE_CONFIG_H || WEBP_HAVE_SSE2); compiling with -DHAVE_CONFIG_H and WITHOUT
# WEBP_HAVE_SSE2 selects the portable generic C dsp, so no *_sse2/*_sse41 files
# are needed and float returns still use the normal SSE ABI.
CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fno-strict-aliasing -Db1nix -DHAVE_CONFIG_H -I$GEN_DIR -I$SRC_DIR
  -Wno-implicit-function-declaration"

# Exactly libwebp's decoder file set (Makefile.am libwebpdecoder), generic only.
# Decoder file set (libwebp Makefile.am libwebpdecoder), generic only.
DEC="dec/alpha_dec dec/buffer_dec dec/frame_dec dec/idec_dec dec/io_dec \
dec/quant_dec dec/tree_dec dec/vp8_dec dec/vp8l_dec dec/webp_dec"
# Encoder file set — built too so the smoke can do a self-contained lossless
# roundtrip (the host has no WebP encoder to generate a test input).
ENC="enc/alpha_enc enc/analysis_enc enc/backward_references_cost_enc \
enc/backward_references_enc enc/config_enc enc/cost_enc enc/filter_enc \
enc/frame_enc enc/histogram_enc enc/iterator_enc enc/near_lossless_enc \
enc/picture_csp_enc enc/picture_enc enc/picture_psnr_enc enc/picture_rescale_enc \
enc/picture_tools_enc enc/predictor_enc enc/quant_enc enc/syntax_enc \
enc/token_enc enc/tree_enc enc/vp8l_enc enc/webp_enc"
DSP="dsp/alpha_processing dsp/cpu dsp/dec dsp/dec_clip_tables dsp/filters \
dsp/lossless dsp/rescaler dsp/upsampling dsp/yuv \
dsp/enc dsp/lossless_enc dsp/cost dsp/ssim"
UTILS="utils/bit_reader_utils utils/color_cache_utils utils/filters_utils \
utils/huffman_utils utils/quant_levels_dec_utils utils/random_utils \
utils/rescaler_utils utils/thread_utils utils/utils \
utils/bit_writer_utils utils/huffman_encode_utils utils/palette \
utils/quant_levels_utils"
SHARP="sharpyuv/sharpyuv sharpyuv/sharpyuv_cpu sharpyuv/sharpyuv_csp \
sharpyuv/sharpyuv_dsp sharpyuv/sharpyuv_gamma"

OBJS=""
for rel in $DEC $ENC $DSP $UTILS; do
  obj="$OBJ_DIR/$(echo "$rel" | tr / _).o"
  # shellcheck disable=SC2086
  clang $CFLAGS -c "$SRC_DIR/src/$rel.c" -o "$obj"
  OBJS="$OBJS $obj"
done
for rel in $SHARP; do
  obj="$OBJ_DIR/$(echo "$rel" | tr / _).o"
  # shellcheck disable=SC2086
  clang $CFLAGS -c "$SRC_DIR/$rel.c" -o "$obj"
  OBJS="$OBJS $obj"
done

# shellcheck disable=SC2086
"$AR_BIN" rcs "$INSTALL_DIR/lib/libwebp.a" $OBJS
cp "$SRC_DIR"/src/webp/decode.h "$SRC_DIR"/src/webp/encode.h \
   "$SRC_DIR"/src/webp/types.h "$INSTALL_DIR/include/webp/"

echo "$INSTALL_DIR"
