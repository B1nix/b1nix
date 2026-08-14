/* M51 rung 3: verify the ported FreeType rasterizes a real glyph from the
 * bundled TrueType font. */
#include <fcntl.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void mark(const char *s) { write(1, s, strlen(s)); }
static int fail(const char *s) {
  mark(s);
  return 1;
}

int main(int argc, char **argv) {
  /*
   * The path FreeType opens for itself, which nothing here had ever exercised.
   *
   * This test reads the file and hands FreeType the bytes; cairo does not — it
   * calls FT_New_Face with a file name and lets FreeType open and map it. That
   * is a different path through the OS, and it is the one that failed: pango
   * reported "file not found" for a font that fc-match resolved and the shell
   * could read. Given a path, this checks that path and says what FreeType
   * made of it.
   */
  if (argc > 1) {
    FT_Library l;
    FT_Face f;
    int err;

    if (FT_Init_FreeType(&l))
      return fail("M51-GFX: fail freetype-path (init)\n");
    err = FT_New_Face(l, argv[1], 0, &f);
    if (err) {
      char line[128];

      snprintf(line, sizeof(line),
               "M51-GFX: fail freetype-path (FT_New_Face=%d) %s\n", err,
               argv[1]);
      mark(line);
      return 1;
    }
    if (FT_Set_Pixel_Sizes(f, 0, 24) || FT_Load_Char(f, 'A', FT_LOAD_RENDER))
      return fail("M51-GFX: fail freetype-path (render)\n");
    FT_Done_Face(f);
    FT_Done_FreeType(l);
    mark("M51-GFX: ok freetype-path\n");
    return 0;
  }

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
