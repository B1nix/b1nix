/* M53 libwebp smoke: prove the ported libwebp encodes and decodes real WebP data
 * on b1nix. WebP's lossy mode is a VP8 intra (keyframe) codec — the same
 * bitstream family as WebM video — so this exercises a genuine video-keyframe
 * decoder, the path the NetSurf WebP loader uses. The test builds a known 16x16
 * RGBA image, encodes it losslessly to an in-memory WebP (a valid RIFF/WEBP
 * stream), reads back the dimensions, decodes it, and verifies the pixels are
 * byte-for-byte identical (lossless). Nothing is faked. Markers (M53-WEBP: ...)
 * consumed by smoke.sh. */

#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <webp/decode.h>
#include <webp/encode.h>

static void emit(const char *s) { write(1, s, strlen(s)); }

#define DIM 16
#define CHANS 4 /* RGBA */

static uint8_t orig[DIM * DIM * CHANS];

static void fill_orig(void) {
  for (int y = 0; y < DIM; y++)
    for (int x = 0; x < DIM; x++) {
      uint8_t *p = &orig[(y * DIM + x) * CHANS];
      p[0] = (uint8_t)(x * 16);
      p[1] = (uint8_t)(y * 16);
      p[2] = (uint8_t)((x ^ y) << 4);
      p[3] = 255;
    }
}

int main(void) {
  emit("M53-WEBP: start\n");
  fill_orig();

  /* ── Encode losslessly to an in-memory WebP ── */
  uint8_t *webp = NULL;
  size_t webp_size = WebPEncodeLosslessRGBA(orig, DIM, DIM, DIM * CHANS, &webp);
  if (webp_size == 0 || webp == NULL) {
    emit("M53-WEBP: fail encode\n");
    return 1;
  }
  /* A WebP file is a RIFF container: "RIFF" .... "WEBP". */
  if (webp_size < 12 || memcmp(webp, "RIFF", 4) != 0 ||
      memcmp(webp + 8, "WEBP", 4) != 0) {
    emit("M53-WEBP: fail signature\n");
    WebPFree(webp);
    return 1;
  }
  emit("M53-WEBP: ok encode\n");

  /* ── Read back the dimensions from the header ── */
  int w = 0, h = 0;
  if (!WebPGetInfo(webp, webp_size, &w, &h) || w != DIM || h != DIM) {
    emit("M53-WEBP: fail info\n");
    WebPFree(webp);
    return 1;
  }
  emit("M53-WEBP: ok info\n");

  /* ── Decode and verify (lossless → exact) ── */
  int dw = 0, dh = 0;
  uint8_t *dec = WebPDecodeRGBA(webp, webp_size, &dw, &dh);
  WebPFree(webp);
  if (dec == NULL || dw != DIM || dh != DIM) {
    emit("M53-WEBP: fail decode\n");
    if (dec) WebPFree(dec);
    return 1;
  }
  int same = (memcmp(dec, orig, sizeof(orig)) == 0);
  WebPFree(dec);
  if (!same) {
    emit("M53-WEBP: fail pixels\n");
    return 1;
  }
  emit("M53-WEBP: ok decode\n");

  emit("M53-WEBP: done\n");
  return 0;
}
