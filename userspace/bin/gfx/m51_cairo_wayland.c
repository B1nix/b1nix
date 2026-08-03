/* M51 end-to-end: a Cairo Wayland app. Draws scalable text (B1nix Mono via the
 * FreeType backend) into a real wl_shm window buffer and presents it to
 * displayd over the Wayland protocol. */
#include <b1nix/gui.h>
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
  int fd = open("/share/fonts/B1nixMono-Regular.ttf", O_RDONLY);
  if (fd < 0)
    return fail("M51-GFX: fail cairo-wayland (font)\n");
  long fsz = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);
  unsigned char *blob = malloc(fsz);
  if (fsz <= 0 || !blob || read(fd, blob, fsz) != fsz)
    return fail("M51-GFX: fail cairo-wayland (font read)\n");
  close(fd);

  FT_Library lib;
  FT_Face ftface;
  if (FT_Init_FreeType(&lib) || FT_New_Memory_Face(lib, blob, fsz, 0, &ftface))
    return fail("M51-GFX: fail cairo-wayland (ft)\n");

  struct b1gui_window win;
  memset(&win, 0, sizeof(win));
  if (b1gui_connect(&win) < 0)
    return fail("M51-GFX: fail cairo-wayland (connect)\n");
  if (b1gui_create_window(&win, 160, 48, "M51 Cairo") < 0 || !win.pixels)
    return fail("M51-GFX: fail cairo-wayland (window)\n");

  /* Wrap the live wl_shm window buffer as a Cairo image surface. */
  cairo_surface_t *surf = cairo_image_surface_create_for_data(
      (unsigned char *)win.pixels, CAIRO_FORMAT_ARGB32, win.width, win.height,
      win.width * 4);
  cairo_t *cr = cairo_create(surf);
  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_paint(cr);

  cairo_font_face_t *face = cairo_ft_font_face_create_for_ft_face(ftface, 0);
  cairo_set_font_face(cr, face);
  cairo_set_font_size(cr, 28);
  cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
  cairo_move_to(cr, 6, 34);
  cairo_show_text(cr, "b1nix");
  cairo_surface_flush(surf);
  if (cairo_status(cr) != CAIRO_STATUS_SUCCESS)
    return fail("M51-GFX: fail cairo-wayland (draw)\n");

  if (b1gui_present(&win, 0, 0, win.width, win.height) < 0)
    return fail("M51-GFX: fail cairo-wayland (present)\n");

  /* Wait for displayd to composite the frame. */
  struct b1gui_event ev;
  int framed = 0;
  for (int i = 0; i < 200 && !framed; i++)
    if (b1gui_next_event(&win, &ev, 1000) == 1 && ev.type == B1GUI_EV_FRAME)
      framed = 1;
  if (!framed)
    return fail("M51-GFX: fail cairo-wayland (no-frame)\n");

  /* The presented buffer must contain the dark glyph ink we drew. */
  unsigned ink = 0;
  for (uint32_t i = 0; i < win.width * win.height; i++)
    if (((win.pixels[i] >> 16) & 0xff) < 0x60)
      ink++;
  if (ink < 20)
    return fail("M51-GFX: fail cairo-wayland (no-text)\n");

  cairo_font_face_destroy(face);
  cairo_destroy(cr);
  cairo_surface_destroy(surf);
  b1gui_destroy(&win);
  mark("M51-GFX: ok cairo-wayland\n");
  return 0;
}
