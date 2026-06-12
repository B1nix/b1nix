#ifndef B1NIX_FB_H
#define B1NIX_FB_H

#include <b1nix/types.h>

/* M47: /dev/fb0 — mmap-able linear framebuffer device.
 *
 * Userspace mmaps a kernel-owned contiguous shadow buffer (always 32bpp,
 * pitch == width*4 regardless of the hardware pitch), draws into it, then
 * issues B1NIX_FBIOFLUSH to push a dirty rectangle to the display (virtio-gpu
 * transfer+flush when available, else a copy to the boot framebuffer).
 * Userspace mirrors these definitions in userspace/include/b1nix/fb.h —
 * keep them matched. */

struct b1nix_fb_info {
  u32 width;
  u32 height;
  u32 pitch; /* bytes per shadow-buffer row (width * 4) */
  u32 bpp;   /* always 32 */
};

struct b1nix_fb_rect {
  u32 x;
  u32 y;
  u32 w;
  u32 h;
};

#define B1NIX_FBIOGET_INFO 0xFB01 /* arg: struct b1nix_fb_info * */
#define B1NIX_FBIOFLUSH 0xFB02    /* arg: struct b1nix_fb_rect * */

void fb_dev_init(void);

/* True once userspace has mapped the framebuffer: the kernel-side
 * compositor must stop flushing so it doesn't fight the userspace owner. */
int fb_dev_claimed(void);

#endif
