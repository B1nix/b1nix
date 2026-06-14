/* M53 libpng smoke: prove the ported libpng (over the ported zlib) encodes and
 * decodes real PNG data on b1nix. Builds a known 8x8 RGBA image, writes it to an
 * in-memory PNG via libpng (a real, standard-conformant PNG byte stream), then
 * decodes it back through the same library used by the NetSurf image loader and
 * verifies the pixels are byte-for-byte identical. Every marker is gated on a
 * verified result; nothing is faked. Markers (M53-PNG: ...) consumed by smoke.sh.
 */

#include <string.h>
#include <unistd.h>

#include <png.h>

static void emit(const char *s) { write(1, s, strlen(s)); }

#define DIM 8
#define CHANS 4 /* RGBA */

static unsigned char orig[DIM][DIM * CHANS];
static unsigned char back[DIM][DIM * CHANS];

/* A simple deterministic pattern: per-pixel (x*37, y*29, x^y, 255). */
static void fill_orig(void) {
  for (int y = 0; y < DIM; y++)
    for (int x = 0; x < DIM; x++) {
      unsigned char *p = &orig[y][x * CHANS];
      p[0] = (unsigned char)(x * 37);
      p[1] = (unsigned char)(y * 29);
      p[2] = (unsigned char)(x ^ y) << 4;
      p[3] = 255;
    }
}

/* Growable in-memory PNG byte sink/source. */
static unsigned char pngbuf[16384];
static unsigned long png_len;   /* bytes written */
static unsigned long png_pos;   /* read cursor */

static void mem_write(png_structp png, png_bytep data, png_size_t len) {
  (void)png;
  if (png_len + len > sizeof(pngbuf)) {
    emit("M53-PNG: fail buffer-overflow\n");
    _exit(1);
  }
  memcpy(pngbuf + png_len, data, len);
  png_len += len;
}
static void mem_flush(png_structp png) { (void)png; }

static void mem_read(png_structp png, png_bytep data, png_size_t len) {
  (void)png;
  if (png_pos + len > png_len) {
    emit("M53-PNG: fail read-past-end\n");
    _exit(1);
  }
  memcpy(data, pngbuf + png_pos, len);
  png_pos += len;
}

static int encode(void) {
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png) return -1;
  png_infop info = png_create_info_struct(png);
  if (!info) { png_destroy_write_struct(&png, NULL); return -1; }
  if (setjmp(png_jmpbuf(png))) { png_destroy_write_struct(&png, &info); return -1; }

  png_set_write_fn(png, NULL, mem_write, mem_flush);
  png_set_IHDR(png, info, DIM, DIM, 8, PNG_COLOR_TYPE_RGBA,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);
  png_bytep rows[DIM];
  for (int y = 0; y < DIM; y++) rows[y] = orig[y];
  png_write_image(png, rows);
  png_write_end(png, NULL);
  png_destroy_write_struct(&png, &info);
  return 0;
}

static int decode(void) {
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png) return -1;
  png_infop info = png_create_info_struct(png);
  if (!info) { png_destroy_read_struct(&png, NULL, NULL); return -1; }
  if (setjmp(png_jmpbuf(png))) { png_destroy_read_struct(&png, &info, NULL); return -1; }

  png_pos = 0;
  png_set_read_fn(png, NULL, mem_read);
  png_read_info(png, info);

  png_uint_32 w = 0, h = 0;
  int depth = 0, color = 0;
  png_get_IHDR(png, info, &w, &h, &depth, &color, NULL, NULL, NULL);
  if (w != DIM || h != DIM || depth != 8 || color != PNG_COLOR_TYPE_RGBA) {
    emit("M53-PNG: fail decode-header\n");
    png_destroy_read_struct(&png, &info, NULL);
    return -1;
  }
  emit("M53-PNG: ok decode-header\n");

  png_bytep rows[DIM];
  for (int y = 0; y < DIM; y++) rows[y] = back[y];
  png_read_image(png, rows);
  png_read_end(png, NULL);
  png_destroy_read_struct(&png, &info, NULL);
  return 0;
}

int main(void) {
  emit("M53-PNG: start\n");
  fill_orig();

  png_len = 0;
  if (encode() != 0) {
    emit("M53-PNG: fail encode\n");
    return 1;
  }
  /* A valid PNG starts with the 8-byte signature 89 50 4E 47 0D 0A 1A 0A. */
  if (png_len < 8 || pngbuf[0] != 0x89 || pngbuf[1] != 'P' || pngbuf[2] != 'N' ||
      pngbuf[3] != 'G') {
    emit("M53-PNG: fail signature\n");
    return 1;
  }
  emit("M53-PNG: ok encode\n");

  if (decode() != 0) {
    emit("M53-PNG: fail decode\n");
    return 1;
  }

  if (memcmp(orig, back, sizeof(orig)) != 0) {
    emit("M53-PNG: fail pixels\n");
    return 1;
  }
  emit("M53-PNG: ok decode\n");

  emit("M53-PNG: done\n");
  return 0;
}
