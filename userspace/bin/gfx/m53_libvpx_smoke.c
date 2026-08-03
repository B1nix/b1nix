/* M53 libvpx smoke: prove the ported libvpx VP8 decoder decodes real motion-video
 * bitstream data on b1nix. VP8 is the WebM / browser video codec. To get a valid
 * VP8 frame without embedding a binary blob, we lean on the already-ported
 * libwebp: a lossy WebP IS a VP8 intra (key) frame wrapped in a RIFF container,
 * so we encode a known image to lossy WebP, pull the raw VP8 bitstream out of the
 * "VP8 " chunk, and feed it to libvpx. We then check the decoder produced a
 * correctly-sized I420 frame whose luma (Y) plane reproduces the original image's
 * luminance within VP8's lossy tolerance — a garbled decode would be far off.
 * Nothing is faked. Markers (M53-VPX: ...) consumed by smoke.sh. */

#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <webp/encode.h>

static void emit(const char *s) { write(1, s, strlen(s)); }

#define DIM 64
#define CHANS 4

static uint8_t orig[DIM * DIM * CHANS];

static void fill_orig(void) {
  for (int y = 0; y < DIM; y++)
    for (int x = 0; x < DIM; x++) {
      uint8_t *p = &orig[(y * DIM + x) * CHANS];
      p[0] = (uint8_t)(x * 4);   /* R ramps across x */
      p[1] = (uint8_t)(y * 4);   /* G ramps down y */
      p[2] = 128;                /* B constant */
      p[3] = 255;
    }
}

/* Locate the raw VP8 bitstream inside a lossy WebP (RIFF "VP8 " chunk). */
static const uint8_t *find_vp8(const uint8_t *webp, size_t size, uint32_t *out_len) {
  if (size < 20 || memcmp(webp, "RIFF", 4) != 0 || memcmp(webp + 8, "WEBP", 4) != 0)
    return NULL;
  size_t off = 12;
  while (off + 8 <= size) {
    const uint8_t *c = webp + off;
    uint32_t clen = (uint32_t)c[4] | ((uint32_t)c[5] << 8) | ((uint32_t)c[6] << 16) |
                    ((uint32_t)c[7] << 24);
    if (memcmp(c, "VP8 ", 4) == 0) {
      if (off + 8 + clen > size) return NULL;
      *out_len = clen;
      return c + 8;
    }
    off += 8 + clen + (clen & 1); /* chunks are padded to even size */
  }
  return NULL;
}

int main(void) {
  emit("M53-VPX: start\n");
  fill_orig();

  /* Encode a lossy WebP (a VP8 keyframe) with libwebp. */
  uint8_t *webp = NULL;
  size_t webp_size = WebPEncodeRGBA(orig, DIM, DIM, DIM * CHANS, 90.0f, &webp);
  if (webp_size == 0 || webp == NULL) {
    emit("M53-VPX: fail webp-encode\n");
    return 1;
  }
  uint32_t vp8_len = 0;
  const uint8_t *vp8 = find_vp8(webp, webp_size, &vp8_len);
  if (!vp8 || vp8_len == 0) {
    emit("M53-VPX: fail vp8-extract\n");
    WebPFree(webp);
    return 1;
  }
  emit("M53-VPX: ok webp-vp8-frame\n");

  /* Decode the VP8 bitstream with libvpx. */
  vpx_codec_ctx_t codec;
  if (vpx_codec_dec_init(&codec, vpx_codec_vp8_dx(), NULL, 0) != VPX_CODEC_OK) {
    emit("M53-VPX: fail decode-init\n");
    WebPFree(webp);
    return 1;
  }
  emit("M53-VPX: ok decode-init\n");

  if (vpx_codec_decode(&codec, vp8, vp8_len, NULL, 0) != VPX_CODEC_OK) {
    emit("M53-VPX: fail decode\n");
    vpx_codec_destroy(&codec);
    WebPFree(webp);
    return 1;
  }
  vpx_codec_iter_t iter = NULL;
  vpx_image_t *img = vpx_codec_get_frame(&codec, &iter);
  if (!img || img->d_w != DIM || img->d_h != DIM || img->fmt != VPX_IMG_FMT_I420) {
    emit("M53-VPX: fail decode\n");
    vpx_codec_destroy(&codec);
    WebPFree(webp);
    return 1;
  }
  emit("M53-VPX: ok decode\n");

  /* Verify the decoded luma (Y) plane reproduces the original luminance. Y is
   * full-resolution and the least-lossy channel; a correct decode tracks the
   * original BT.601 luma closely, garbage does not. */
  const uint8_t *yplane = img->planes[VPX_PLANE_Y];
  int ystride = img->stride[VPX_PLANE_Y];
  long total = 0, maxd = 0;
  for (int y = 0; y < DIM; y++)
    for (int x = 0; x < DIM; x++) {
      const uint8_t *p = &orig[(y * DIM + x) * CHANS];
      /* BT.601 luma of the original pixel, integer approximation. */
      long luma = (77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8;
      long dec = yplane[y * ystride + x];
      long d = luma - dec;
      if (d < 0) d = -d;
      total += d;
      if (d > maxd) maxd = d;
    }
  long mean = total / (DIM * DIM);
  vpx_codec_destroy(&codec);
  WebPFree(webp);

  if (mean > 16 || maxd > 64) {
    emit("M53-VPX: fail luma\n");
    return 1;
  }
  emit("M53-VPX: ok luma\n");

  emit("M53-VPX: done\n");
  return 0;
}
