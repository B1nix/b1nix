#!/bin/sh
# Build libvpx's VP8 decoder as a static libvpx.a for the b1nix userspace ABI.
# libvpx has its own configure that generates vpx_config.h and the *_rtcd.h
# runtime-CPU-dispatch headers; we run it for the portable generic-gnu target
# (no asm, no runtime CPU detect) on the host to produce those headers, then
# recompile the C sources with the b1nix freestanding toolchain. VP8 is the WebM
# / browser video codec. Prints the install dir.
#
# M53 (NetSurf browser platform) full-motion video codec dependency.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
VPX_VERSION="${VPX_VERSION:-1.14.1}"
TARBALL="libvpx-${VPX_VERSION}.tar.gz"
URL="https://github.com/webmproject/libvpx/archive/refs/tags/v${VPX_VERSION}.tar.gz"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"
CC="clang"; command -v ccache >/dev/null 2>&1 && [ "${B1NIX_NO_CCACHE:-0}" != "1" ] && CC="ccache clang"

. "$ROOT_DIR/tools/toolchain/env.sh"

SRC_PARENT="$ROOT_DIR/build/src/libvpx"
SRC_DIR="$SRC_PARENT/libvpx-${VPX_VERSION}"
CFG_DIR="$SRC_PARENT/cfg"            # host configure output (generated headers)
BUILD_DIR="$ROOT_DIR/build/$B1NIX_ARCH/ports/libvpx"
OBJ_DIR="$BUILD_DIR/obj"
INSTALL_DIR="$BUILD_DIR/install"

mkdir -p "$SRC_PARENT" "$OBJ_DIR" "$INSTALL_DIR/include/vpx" "$INSTALL_DIR/lib"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$TARBALL"
  [ -f "$tmp" ] || curl -L "$URL" -o "$tmp" 1>&2
  tar -xzf "$tmp" -C "$SRC_PARENT" 1>&2
fi

# Generate vpx_config.h + the *_rtcd.h / generated *.c via libvpx's own
# configure+make for the portable target (host build, products are the headers).
if [ ! -f "$CFG_DIR/vpx_config.h" ]; then
  rm -rf "$CFG_DIR"
  mkdir -p "$CFG_DIR"
  ( cd "$CFG_DIR" && "$SRC_DIR/configure" --target=generic-gnu \
      --disable-vp9 --disable-vp8-encoder --enable-vp8-decoder \
      --disable-examples --disable-tools --disable-docs --disable-unit-tests \
      --disable-runtime-cpu-detect --disable-webm-io --disable-libyuv \
      --disable-postproc --disable-multithread 1>&2 )
  ( cd "$CFG_DIR" && make -j4 1>&2 ) || true
fi

if [ "$B1NIX_ARCH" = "x86" ]; then
  TARGET="i686-unknown-elf"
else
  TARGET="x86_64-unknown-elf"
fi

# generic-gnu uses portable C only. -DINLINE=inline because the generic config
# may not define a compiler-specific inline keyword for clang.
CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fno-strict-aliasing -fPIC -Db1nix -I$CFG_DIR -I$SRC_DIR
  -Wno-implicit-function-declaration -Wno-unused"

# The VP8-decode source set (the .c.o files libvpx's generic-gnu build produced).
# Sources live in the libvpx tree, except the three configure-generated ones in
# the cfg dir.
SRCS="vp8/common/alloccommon vp8/common/blockd vp8/common/dequantize \
vp8/common/entropy vp8/common/entropymode vp8/common/entropymv \
vp8/common/extend vp8/common/filter vp8/common/findnearmv \
vp8/common/generic/systemdependent vp8/common/idct_blk vp8/common/idctllm \
vp8/common/loopfilter_filters vp8/common/mbpitch vp8/common/modecont \
vp8/common/quant_common vp8/common/reconinter vp8/common/reconintra4x4 \
vp8/common/reconintra vp8/common/rtcd vp8/common/setupintrarecon \
vp8/common/swapyv12buffer vp8/common/treecoder vp8/common/vp8_loopfilter \
vp8/decoder/dboolhuff vp8/decoder/decodeframe vp8/decoder/decodemv \
vp8/decoder/detokenize vp8/decoder/onyxd_if vp8/vp8_dx_iface \
vpx_dsp/bitreader_buffer vpx_dsp/bitreader vpx_dsp/intrapred vpx_dsp/prob \
vpx_dsp/skin_detection vpx_mem/vpx_mem vpx_scale/generic/gen_scalers \
vpx_scale/generic/vpx_scale vpx_scale/generic/yv12config \
vpx_scale/generic/yv12extend vpx/src/vpx_codec vpx/src/vpx_decoder \
vpx/src/vpx_image vpx/src/vpx_tpl vpx_util/vpx_thread"
GEN_SRCS="vpx_config vpx_dsp/vpx_dsp_rtcd vpx_scale/vpx_scale_rtcd"

OBJS=""
for rel in $SRCS; do
  obj="$OBJ_DIR/$(echo "$rel" | tr / _).o"
  # shellcheck disable=SC2086
  $CC $CFLAGS -c "$SRC_DIR/$rel.c" -o "$obj"
  OBJS="$OBJS $obj"
done
for rel in $GEN_SRCS; do
  obj="$OBJ_DIR/gen_$(echo "$rel" | tr / _).o"
  src="$CFG_DIR/$rel.c"
  [ -f "$src" ] || src="$SRC_DIR/$rel.c"
  # shellcheck disable=SC2086
  $CC $CFLAGS -c "$src" -o "$obj"
  OBJS="$OBJS $obj"
done

# shellcheck disable=SC2086
"$AR_BIN" rcs "$INSTALL_DIR/lib/libvpx.a" $OBJS
cp -r "$SRC_DIR"/vpx/*.h "$INSTALL_DIR/include/vpx/" 2>/dev/null || true
cp "$CFG_DIR/vpx_config.h" "$INSTALL_DIR/include/"

echo "$INSTALL_DIR"
