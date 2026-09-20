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

/* The hardware cursor, when the display device composes one. Moving it redraws
 * nothing and covers nothing: a pointer painted into the framebuffer cannot be
 * moved away again, because the pixels underneath were not kept. `image` is
 * optional (0 keeps the loaded one) and points at image_w * image_h BGRA
 * pixels, at most 64x64. EOPNOTSUPP means the device has no hardware cursor. */
struct b1nix_fb_cursor {
  uint32_t visible;
  int32_t x;
  int32_t y;
  uint32_t hot_x;
  uint32_t hot_y;
  uint32_t image_w;
  uint32_t image_h;
  uint64_t image;
};

#define B1NIX_FBIOGET_INFO 0xFB01 /* arg: struct b1nix_fb_info * */
#define B1NIX_FBIOFLUSH 0xFB02    /* arg: struct b1nix_fb_rect * */
#define B1NIX_FBIOCURSOR 0xFB04   /* arg: struct b1nix_fb_cursor * */

#endif
