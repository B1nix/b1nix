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

#endif
