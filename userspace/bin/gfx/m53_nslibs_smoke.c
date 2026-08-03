/* M53 NetSurf helper/decoder smoke: prove the ported libnsutils, libnsgif,
 * libnsbmp and libnslog work on b1nix. These are the leaf utilities and image
 * decoders the NetSurf core links against. Nothing faked:
 *  - libnsutils: base64 encode->decode round-trips, monotonic clock advances,
 *  - libnsgif: decodes a hand-built 1x1 GIF to an opaque-red R8G8B8A8 pixel,
 *  - libnsbmp: decodes a hand-built 2x2 24-bit BMP to opaque-red pixels,
 *  - libnslog: a logged line is delivered verbatim to a render callback.
 * Markers (M53-NSUTILS / M53-NSGIF / M53-NSBMP / M53-NSLOG) read by smoke.sh. */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <nsutils/base64.h>
#include <nsutils/time.h>
#include <nsgif.h>
#include <libnsbmp.h>
#include <nslog/nslog.h>

static void emit(const char *s) { write(1, s, strlen(s)); }

/* Shared client bitmap: a width*height RGBA8888 buffer. */
struct cbmp {
  int w, h;
  uint8_t *buf;
};

/* ── libnsgif / libnsbmp bitmap callbacks ── */
static void *bmp_make(int w, int h) {
  struct cbmp *b = malloc(sizeof(*b));
  if (b == NULL)
    return NULL;
  b->w = w;
  b->h = h;
  b->buf = calloc((size_t)w * h, 4);
  if (b->buf == NULL) {
    free(b);
    return NULL;
  }
  return b;
}
static nsgif_bitmap_t *gif_create(int w, int h) { return bmp_make(w, h); }
static void gif_destroy(void *p) {
  struct cbmp *b = p;
  free(b->buf);
  free(b);
}
static uint8_t *gif_get_buffer(void *p) { return ((struct cbmp *)p)->buf; }
static const nsgif_bitmap_cb_vt gif_vt = {
    .create = gif_create,
    .destroy = gif_destroy,
    .get_buffer = gif_get_buffer,
};

static void *nsbmp_create(int w, int h, unsigned int state) {
  (void)state;
  return bmp_make(w, h);
}
static void nsbmp_destroy(void *p) { gif_destroy(p); }
static unsigned char *nsbmp_get_buffer(void *p) {
  return ((struct cbmp *)p)->buf;
}

/* A red RGBA8888 pixel is the bytes FF 00 00 FF for both decoders. */
static int pixel_is_red(const uint8_t *px) {
  return px[0] == 0xFF && px[1] == 0x00 && px[2] == 0x00 && px[3] == 0xFF;
}

/* ── libnslog category + capture ── */
NSLOG_DEFINE_CATEGORY(m53test, "M53 NetSurf helper smoke");
static char log_capture[256];
static int log_hit;
static void log_render(void *context, nslog_entry_context_t *ctx,
                       const char *fmt, va_list args) {
  (void)context;
  (void)ctx;
  vsnprintf(log_capture, sizeof(log_capture), fmt, args);
  log_hit = 1;
}

int main(void) {
  emit("M53-NSLIBS: start\n");

  /* ── libnsutils: base64 round-trip ── */
  const uint8_t plain[] = "b1nix browser platform";
  uint8_t *enc = NULL;
  size_t enc_len = 0;
  if (nsu_base64_encode_alloc(plain, sizeof(plain) - 1, &enc, &enc_len) !=
          NSUERROR_OK ||
      enc == NULL || enc_len == 0) {
    emit("M53-NSUTILS: fail base64-encode\n");
    return 1;
  }
  uint8_t *dec = NULL;
  size_t dec_len = 0;
  if (nsu_base64_decode_alloc(enc, enc_len, &dec, &dec_len) != NSUERROR_OK ||
      dec == NULL || dec_len != sizeof(plain) - 1 ||
      memcmp(dec, plain, dec_len) != 0) {
    emit("M53-NSUTILS: fail base64-roundtrip\n");
    return 1;
  }
  free(enc);
  free(dec);
  emit("M53-NSUTILS: ok base64\n");

  /* monotonic clock must return a value and not go backwards */
  uint64_t t0 = 0, t1 = 0;
  if (nsu_getmonotonic_ms(&t0) != NSUERROR_OK ||
      nsu_getmonotonic_ms(&t1) != NSUERROR_OK || t1 < t0) {
    emit("M53-NSUTILS: fail monotonic\n");
    return 1;
  }
  emit("M53-NSUTILS: ok monotonic\n");
  emit("M53-NSUTILS: done\n");

  /* ── libnsgif: decode a 1x1 red GIF ── */
  static const uint8_t gif[] = {
      0x47, 0x49, 0x46, 0x38, 0x39, 0x61, /* GIF89a */
      0x01, 0x00, 0x01, 0x00,             /* 1x1 */
      0x80, 0x00, 0x00,                   /* GCT (2 colours), bg, aspect */
      0xFF, 0x00, 0x00,                   /* colour 0: red */
      0x00, 0x00, 0x00,                   /* colour 1: black */
      0x2C, 0x00, 0x00, 0x00, 0x00,       /* image descriptor, pos */
      0x01, 0x00, 0x01, 0x00, 0x00,       /* 1x1, no LCT */
      0x02, 0x02, 0x44, 0x01, 0x00,       /* LZW: minsz 2, [0x44 0x01] */
      0x3B,                               /* trailer */
  };
  nsgif_t *g = NULL;
  if (nsgif_create(&gif_vt, NSGIF_BITMAP_FMT_R8G8B8A8, &g) != NSGIF_OK ||
      g == NULL) {
    emit("M53-NSGIF: fail create\n");
    return 1;
  }
  if (nsgif_data_scan(g, sizeof(gif), gif) != NSGIF_OK) {
    emit("M53-NSGIF: fail scan\n");
    return 1;
  }
  nsgif_data_complete(g);
  const nsgif_info_t *gi = nsgif_get_info(g);
  if (gi == NULL || gi->width != 1 || gi->height != 1) {
    emit("M53-NSGIF: fail info\n");
    return 1;
  }
  emit("M53-NSGIF: ok info\n");
  nsgif_bitmap_t *gbm = NULL;
  if (nsgif_frame_decode(g, 0, &gbm) != NSGIF_OK || gbm == NULL ||
      !pixel_is_red(gif_get_buffer(gbm))) {
    emit("M53-NSGIF: fail decode\n");
    return 1;
  }
  nsgif_destroy(g);
  emit("M53-NSGIF: ok decode\n");
  emit("M53-NSGIF: done\n");

  /* ── libnsbmp: decode a 2x2 red 24-bit BMP ── */
  static uint8_t bmp[] = {
      /* BITMAPFILEHEADER */
      'B', 'M', 0x46, 0x00, 0x00, 0x00, /* "BM", filesize=70 */
      0x00, 0x00, 0x00, 0x00,           /* reserved */
      0x36, 0x00, 0x00, 0x00,           /* pixel data offset = 54 */
      /* BITMAPINFOHEADER */
      0x28, 0x00, 0x00, 0x00, /* header size = 40 */
      0x02, 0x00, 0x00, 0x00, /* width = 2 */
      0x02, 0x00, 0x00, 0x00, /* height = 2 */
      0x01, 0x00,             /* planes = 1 */
      0x18, 0x00,             /* bpp = 24 */
      0x00, 0x00, 0x00, 0x00, /* compression = BI_RGB */
      0x10, 0x00, 0x00, 0x00, /* image size = 16 */
      0x00, 0x00, 0x00, 0x00, /* x ppm */
      0x00, 0x00, 0x00, 0x00, /* y ppm */
      0x00, 0x00, 0x00, 0x00, /* colours used */
      0x00, 0x00, 0x00, 0x00, /* important colours */
      /* pixel data: BGR per pixel, rows padded to 4 bytes, bottom-up */
      0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, /* row */
      0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, /* row */
  };
  bmp_bitmap_callback_vt bmp_cb = {
      .bitmap_create = nsbmp_create,
      .bitmap_destroy = nsbmp_destroy,
      .bitmap_get_buffer = nsbmp_get_buffer,
  };
  bmp_image bi;
  bmp_create(&bi, &bmp_cb);
  if (bmp_analyse(&bi, sizeof(bmp), bmp) != BMP_OK) {
    emit("M53-NSBMP: fail analyse\n");
    return 1;
  }
  if (bi.width != 2 || bi.height != 2) {
    emit("M53-NSBMP: fail info\n");
    return 1;
  }
  emit("M53-NSBMP: ok info\n");
  if (bmp_decode(&bi) != BMP_OK || bi.bitmap == NULL ||
      !pixel_is_red(nsbmp_get_buffer(bi.bitmap))) {
    emit("M53-NSBMP: fail decode\n");
    bmp_finalise(&bi);
    return 1;
  }
  bmp_finalise(&bi);
  emit("M53-NSBMP: ok decode\n");
  emit("M53-NSBMP: done\n");

  /* ── libnslog: a logged message reaches the render callback ── */
  if (nslog_set_render_callback(log_render, NULL) != NSLOG_NO_ERROR) {
    emit("M53-NSLOG: fail callback\n");
    return 1;
  }
  nslog_uncork();
  NSLOG(m53test, WARNING, "render check %d", 1234);
  if (!log_hit || strcmp(log_capture, "render check 1234") != 0) {
    emit("M53-NSLOG: fail deliver\n");
    return 1;
  }
  emit("M53-NSLOG: ok deliver\n");
  emit("M53-NSLOG: done\n");

  emit("M53-NSLIBS: done\n");
  return 0;
}
