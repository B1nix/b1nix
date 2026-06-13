/* M51 rung 3: verify the ported FreeType rasterizes a real glyph from the
 * bundled TrueType font. */
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
    return fail("M51-GFX: fail freetype (open)\n");
  long size = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);
  if (size <= 0)
    return fail("M51-GFX: fail freetype (size)\n");
  unsigned char *font = malloc(size);
  if (!font || read(fd, font, size) != size)
    return fail("M51-GFX: fail freetype (read)\n");
  close(fd);

  FT_Library lib;
  if (FT_Init_FreeType(&lib))
    return fail("M51-GFX: fail freetype (init)\n");
  FT_Face face;
  if (FT_New_Memory_Face(lib, font, size, 0, &face))
    return fail("M51-GFX: fail freetype (face)\n");
  if (FT_Set_Pixel_Sizes(face, 0, 48))
    return fail("M51-GFX: fail freetype (size)\n");
  if (FT_Load_Char(face, 'A', FT_LOAD_RENDER))
    return fail("M51-GFX: fail freetype (load)\n");

  FT_Bitmap *bm = &face->glyph->bitmap;
  if (bm->width == 0 || bm->rows == 0)
    return fail("M51-GFX: fail freetype (empty)\n");
  /* The glyph must actually have ink, not just a blank box. */
  unsigned ink = 0;
  for (unsigned y = 0; y < bm->rows; y++)
    for (unsigned x = 0; x < bm->width; x++)
      if (bm->buffer[y * bm->pitch + x])
        ink++;
  if (ink == 0)
    return fail("M51-GFX: fail freetype (no-ink)\n");

  FT_Done_Face(face);
  FT_Done_FreeType(lib);
  free(font);
  mark("M51-GFX: ok freetype\n");
  return 0;
}
