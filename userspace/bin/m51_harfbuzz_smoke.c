/* M51 rung 6: verify the ported HarfBuzz shapes text with the built-in
 * OpenType shaper using the bundled B1nix Mono font. */
#include <fcntl.h>
#include <hb-ot.h>
#include <hb.h>
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

int main(void) {
  int fd = open("/share/fonts/B1nixMono-Regular.ttf", O_RDONLY);
  if (fd < 0)
    return fail("M51-GFX: fail harfbuzz (open)\n");
  long size = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);
  char *blobdata = malloc(size);
  if (size <= 0 || !blobdata || read(fd, blobdata, size) != size)
    return fail("M51-GFX: fail harfbuzz (read)\n");
  close(fd);

  hb_blob_t *blob =
      hb_blob_create(blobdata, (unsigned)size, HB_MEMORY_MODE_READONLY, 0, 0);
  hb_face_t *face = hb_face_create(blob, 0);
  hb_font_t *font = hb_font_create(face);
  hb_ot_font_set_funcs(font); /* built-in OpenType shaper, no FreeType */

  hb_buffer_t *buf = hb_buffer_create();
  const char *text = "b1nix";
  hb_buffer_add_utf8(buf, text, -1, 0, -1);
  hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
  hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
  hb_buffer_set_language(buf, hb_language_from_string("en", -1));
  hb_shape(font, buf, NULL, 0);

  unsigned n = hb_buffer_get_length(buf);
  hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, NULL);
  hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, NULL);
  if (n != strlen(text) || !info || !pos)
    return fail("M51-GFX: fail harfbuzz (count)\n");

  /* Every cluster must map to a real glyph (codepoint != .notdef) and advance. */
  unsigned mapped = 0, advanced = 0;
  for (unsigned i = 0; i < n; i++) {
    if (info[i].codepoint != 0)
      mapped++;
    if (pos[i].x_advance > 0)
      advanced++;
  }
  if (mapped != n || advanced != n)
    return fail("M51-GFX: fail harfbuzz (shape)\n");

  hb_buffer_destroy(buf);
  hb_font_destroy(font);
  hb_face_destroy(face);
  hb_blob_destroy(blob);
  free(blobdata);
  mark("M51-GFX: ok harfbuzz\n");
  return 0;
}
