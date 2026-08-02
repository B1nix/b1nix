#ifndef B1NIX_FB_CONSOLE_H
#define B1NIX_FB_CONSOLE_H

#include <b1nix/types.h>

void fb_console_init(void);
void fb_console_clear(void);
void fb_console_putchar(char c);
void fb_console_write(const char *str);
void fb_console_blink_cursor(void);

int fb_console_ready(void);
u32 fb_console_width(void);
u32 fb_console_height(void);
u32 fb_console_pitch(void);
u8 fb_console_bpp(void);
volatile void *fb_console_frontbuffer(void);

/* M107 console fonts. `stride` is the per-glyph byte pitch of `glyphs` (32 for
 * the PIO_FONT layout, == height for a packed face); glyphs are 8 pixels wide,
 * MSB on the left, and the buffer must outlive the call (the console keeps the
 * pointer). Passing NULL restores the builtin 8x8 face. Returns 0, or -1 for
 * an unsupported geometry. */
int fb_console_set_font(const u8 *glyphs, u32 height, u32 stride, u32 count);
void fb_console_font_metrics(u32 *height, u32 *count);
const u8 *fb_console_builtin_font(void);

#endif
