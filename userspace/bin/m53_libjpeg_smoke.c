/* M53 libjpeg smoke: prove the ported IJG libjpeg encodes and decodes real JPEG
 * data on b1nix. Builds a smooth 16x16 RGB gradient, compresses it to an
 * in-memory JPEG (jpeg_mem_dest), then decompresses it back (jpeg_mem_src) — the
 * decode path the NetSurf JPEG loader uses. JPEG is lossy, so the check is: exact
 * dimensions/component count, a valid SOI marker, every channel within a small
 * tolerance of the original, and a low mean error (a garbled decode would be far
 * off). Nothing is faked. Markers (M53-JPEG: ...) consumed by smoke.sh. */

#include <setjmp.h>
#include <stdio.h> /* libjpeg's jpeglib.h references FILE in its prototypes */
#include <string.h>
#include <unistd.h>

#include <jpeglib.h>

static void emit(const char *s) { write(1, s, strlen(s)); }

#define DIM 16
#define COMP 3 /* RGB */

static unsigned char orig[DIM * DIM * COMP];
static unsigned char back[DIM * DIM * COMP];
static unsigned char jpegbuf[65536];

/* A smooth low-frequency gradient — JPEG reproduces it with little error. */
static void fill_orig(void) {
  for (int y = 0; y < DIM; y++)
    for (int x = 0; x < DIM; x++) {
      unsigned char *p = &orig[(y * DIM + x) * COMP];
      p[0] = (unsigned char)(x * 16);
      p[1] = (unsigned char)(y * 16);
      p[2] = 128;
    }
}

/* setjmp-based error handling, the libjpeg-recommended pattern. */
struct jerr_mgr {
  struct jpeg_error_mgr pub;
  jmp_buf jb;
};
static void on_error(j_common_ptr cinfo) {
  struct jerr_mgr *e = (struct jerr_mgr *)cinfo->err;
  longjmp(e->jb, 1);
}

static int encode(unsigned long *out_len) {
  struct jpeg_compress_struct cinfo;
  struct jerr_mgr jerr;
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = on_error;
  if (setjmp(jerr.jb)) { jpeg_destroy_compress(&cinfo); return -1; }

  jpeg_create_compress(&cinfo);
  unsigned char *dst = jpegbuf;
  unsigned long dstlen = sizeof(jpegbuf);
  jpeg_mem_dest(&cinfo, &dst, &dstlen);
  cinfo.image_width = DIM;
  cinfo.image_height = DIM;
  cinfo.input_components = COMP;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, 95, TRUE);
  jpeg_start_compress(&cinfo, TRUE);
  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW row = &orig[cinfo.next_scanline * DIM * COMP];
    jpeg_write_scanlines(&cinfo, &row, 1);
  }
  jpeg_finish_compress(&cinfo);
  *out_len = dstlen;
  jpeg_destroy_compress(&cinfo);
  return 0;
}

static int decode(unsigned long len) {
  struct jpeg_decompress_struct cinfo;
  struct jerr_mgr jerr;
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = on_error;
  if (setjmp(jerr.jb)) { jpeg_destroy_decompress(&cinfo); return -1; }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, jpegbuf, len);
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }
  if (cinfo.image_width != DIM || cinfo.image_height != DIM ||
      cinfo.num_components != COMP) {
    emit("M53-JPEG: fail decode-header\n");
    jpeg_destroy_decompress(&cinfo);
    return -2;
  }
  emit("M53-JPEG: ok decode-header\n");

  jpeg_start_decompress(&cinfo);
  while (cinfo.output_scanline < cinfo.output_height) {
    JSAMPROW row = &back[cinfo.output_scanline * DIM * COMP];
    jpeg_read_scanlines(&cinfo, &row, 1);
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  return 0;
}

int main(void) {
  emit("M53-JPEG: start\n");
  fill_orig();

  unsigned long jlen = 0;
  if (encode(&jlen) != 0 || jlen < 2) {
    emit("M53-JPEG: fail encode\n");
    return 1;
  }
  /* Every JPEG begins with the SOI marker 0xFF 0xD8. */
  if (jpegbuf[0] != 0xFF || jpegbuf[1] != 0xD8) {
    emit("M53-JPEG: fail soi\n");
    return 1;
  }
  emit("M53-JPEG: ok encode\n");

  if (decode(jlen) != 0) {
    emit("M53-JPEG: fail decode\n");
    return 1;
  }

  /* Lossy: require a faithful (not byte-exact) reconstruction. */
  long total = 0, maxdiff = 0;
  for (unsigned i = 0; i < sizeof(orig); i++) {
    long d = (long)orig[i] - (long)back[i];
    if (d < 0) d = -d;
    total += d;
    if (d > maxdiff) maxdiff = d;
  }
  long mean = total / (long)sizeof(orig);
  if (maxdiff > 40 || mean > 8) {
    emit("M53-JPEG: fail pixels\n");
    return 1;
  }
  emit("M53-JPEG: ok decode\n");

  emit("M53-JPEG: done\n");
  return 0;
}
