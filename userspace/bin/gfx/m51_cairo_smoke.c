/* M51 rung 5: Cairo draws real text onto an image surface using the bundled
 * B1nix Mono font via the FreeType backend. The integration test for the whole
 * stack (cairo -> pixman compositing + freetype rasterization). */
#include <cairo-ft.h>
#include <cairo.h>
#include <fcntl.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void mark(const char *s) { write(1, s, strlen(s)); }
static int fail(const char *s) {
  mark(s);
  return 1;
}

int main(void) {
  /* Load the bundled font through FreeType. */
  int fd = open("/share/fonts/B1nixMono-Regular.ttf", O_RDONLY);
  if (fd < 0)
    return fail("M51-GFX: fail cairo (font open)\n");
  long size = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);
  unsigned char *blob = malloc(size);
  if (size <= 0 || !blob || read(fd, blob, size) != size)
    return fail("M51-GFX: fail cairo (font read)\n");
  close(fd);

  FT_Library lib;
  FT_Face ftface;
  if (FT_Init_FreeType(&lib) || FT_New_Memory_Face(lib, blob, size, 0, &ftface))
    return fail("M51-GFX: fail cairo (ft)\n");

  enum { W = 96, H = 48 };
  cairo_surface_t *surf =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
  if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS)
    return fail("M51-GFX: fail cairo (surface)\n");
  cairo_t *cr = cairo_create(surf);

  /* White background. */
  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_paint(cr);

  /* Black text in B1nix Mono. */
  cairo_font_face_t *face = cairo_ft_font_face_create_for_ft_face(ftface, 0);
  cairo_set_font_face(cr, face);
  cairo_set_font_size(cr, 32);
  cairo_set_source_rgb(cr, 0, 0, 0);
  cairo_move_to(cr, 4, 36);
  cairo_show_text(cr, "Ab1");

  if (cairo_status(cr) != CAIRO_STATUS_SUCCESS)
    return fail("M51-GFX: fail cairo (status)\n");

  cairo_surface_flush(surf);
  uint32_t *px = (uint32_t *)cairo_image_surface_get_data(surf);
  int stride = cairo_image_surface_get_stride(surf) / 4;

  /* Count dark pixels: the glyphs must have painted ink over the white bg. */
  unsigned ink = 0;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint32_t p = px[y * stride + x];
      unsigned r = (p >> 16) & 0xff;
      if (r < 0x40)
        ink++;
    }
  if (ink < 20)
    return fail("M51-GFX: fail cairo (no-text)\n");

  cairo_font_face_destroy(face);
  cairo_destroy(cr);
  cairo_surface_destroy(surf);
  FT_Done_Face(ftface);
  FT_Done_FreeType(lib);
  free(blob);
  mark("M51-GFX: ok cairo\n");
  return 0;
}
