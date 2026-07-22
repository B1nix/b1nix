#!/bin/sh
# Build libwebp's decoder+encoder as a static libwebp.a for the b1nix userspace
# ABI. Freestanding compile of the portable subset (no autotools/CMake), generic
# C dsp only (no SIMD). Decodes lossy (VP8 intra-frame) and lossless WebP; the
# encoder is built too so the smoke can do a self-contained lossless roundtrip.
# Prints the install dir.
#
# M53 (NetSurf browser platform) image codec dependency.
# Build logic lives in tools/ports/drivers/cport.sh; this is the manifest.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

CPORT_NAME=libwebp
CPORT_ARCHIVE=libwebp.a
CPORT_SRC_PARENT="$ROOT_DIR/build/src/libwebp"
VER="${WEBP_VERSION:-1.4.0}"
CPORT_SRCNAME="libwebp-${VER}"
CPORT_TARBALL="libwebp-${VER}.tar.gz"
CPORT_URL="https://github.com/webmproject/libwebp/archive/refs/tags/v${VER}.tar.gz"
CPORT_HEADERS="sub:webp:src/webp/decode.h sub:webp:src/webp/encode.h sub:webp:src/webp/types.h"

# Minimal config.h (HAVE_CONFIG_H path; included as "src/webp/config.h").
# Deliberately defines NO WEBP_HAVE_SSE2/SSE41/NEON and NO WEBP_USE_THREAD →
# portable generic C dsp, single-threaded.
port_pre_build() {
  mkdir -p "$GEN_DIR/src/webp"
  cat > "$GEN_DIR/src/webp/config.h" <<EOF
#define HAVE_BUILTIN_BSWAP16 1
#define HAVE_BUILTIN_BSWAP32 1
#define HAVE_BUILTIN_BSWAP64 1
#define PACKAGE_NAME "libwebp"
#define PACKAGE_VERSION "$VER"
EOF
  CPORT_CFLAGS="-DHAVE_CONFIG_H -I$GEN_DIR -I$SRC_DIR -Wno-implicit-function-declaration"
}

# Exactly libwebp's decoder + encoder + generic dsp/utils file sets (Makefile.am),
# generic only. DEC/ENC/DSP/UTILS live under src/; SHARP under sharpyuv/.
port_build() {
  DEC="dec/alpha_dec dec/buffer_dec dec/frame_dec dec/idec_dec dec/io_dec \
dec/quant_dec dec/tree_dec dec/vp8_dec dec/vp8l_dec dec/webp_dec"
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
  for rel in $DEC $ENC $DSP $UTILS; do cport_cc "$SRC_DIR/src/$rel.c"; done
  for rel in $SHARP; do cport_cc "$SRC_DIR/$rel.c"; done
}

. "$ROOT_DIR/tools/ports/drivers/cport.sh"
