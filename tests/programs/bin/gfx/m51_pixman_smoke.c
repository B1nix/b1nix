/* M51 rung 2: verify the ported pixman actually composites pixels. */
#include <pixman.h>
#include <string.h>
#include <unistd.h>

static void mark(const char *s) { write(1, s, strlen(s)); }

int main(void) {
  enum { W = 4, H = 4 };
  uint32_t buf[W * H];
  pixman_image_t *dst =
      pixman_image_create_bits(PIXMAN_a8r8g8b8, W, H, buf, W * 4);
  if (!dst) {
    mark("M51-GFX: fail pixman\n");
    return 1;
  }

  /* OP_SRC fill: opaque red over the whole image. */
  pixman_color_t red = {0xffff, 0x0000, 0x0000, 0xffff};
  pixman_box32_t box = {0, 0, W, H};
  pixman_image_fill_boxes(PIXMAN_OP_SRC, dst, &red, 1, &box);
  if (buf[0] != 0xffff0000u) {
    mark("M51-GFX: fail pixman\n");
    return 1;
  }

  /* OVER a half-alpha green (premultiplied: a=0x80, g=0x80). The result must
   * blend: red drops below full, green appears. This exercises the actual
   * combiner, not just a memset. */
  pixman_color_t green = {0x0000, 0x8080, 0x0000, 0x8080};
  pixman_image_t *src = pixman_image_create_solid_fill(&green);
  if (!src) {
    mark("M51-GFX: fail pixman\n");
    return 1;
  }
  pixman_image_composite32(PIXMAN_OP_OVER, src, NULL, dst, 0, 0, 0, 0, 0, 0, W,
                           H);
  uint32_t px = buf[0];
  unsigned r = (px >> 16) & 0xff, g = (px >> 8) & 0xff;
  if (!(r > 0 && r < 0xff) || g == 0) {
    mark("M51-GFX: fail pixman\n");
    return 1;
  }

  pixman_image_unref(src);
  pixman_image_unref(dst);
  mark("M51-GFX: ok pixman\n");
  return 0;
}
