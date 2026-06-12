#ifndef _B1NIX_FB_H
#define _B1NIX_FB_H

#include <stdint.h>

/* M47 /dev/fb0 ABI — userspace mirror of kernel/include/b1nix/fb.h.
 * Keep the two files matched. */

struct b1nix_fb_info {
  uint32_t width;
  uint32_t height;
  uint32_t pitch; /* bytes per row (width * 4) */
  uint32_t bpp;   /* always 32 */
};

struct b1nix_fb_rect {
  uint32_t x;
  uint32_t y;
  uint32_t w;
  uint32_t h;
};

#define B1NIX_FBIOGET_INFO 0xFB01 /* arg: struct b1nix_fb_info * */
#define B1NIX_FBIOFLUSH 0xFB02    /* arg: struct b1nix_fb_rect * */

#endif
