#ifndef B1NIX_FB_H
#define B1NIX_FB_H

#include <b1nix/types.h>

struct vfs_node;

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
/* arg: int * — number of live userspace mappings (0 == kernel console owns
 * scanout again). Lets a supervisor observe the claim hand-off that the
 * kernel-internal fb_dev_claimed() reports. */
#define B1NIX_FBIOGET_CLAIM 0xFB03

/* The hardware cursor, when the display device composes one (virtio-gpu does).
 * The pointer moves without the frame being redrawn and without anything being
 * painted over: a cursor drawn into the framebuffer cannot be moved away again,
 * because the pixels it covered were not kept. `image` is optional — 0 keeps
 * whatever image is loaded — and points at `image_w` * `image_h` BGRA pixels,
 * at most 64x64. -EOPNOTSUPP says the device has no hardware cursor rather than
 * quietly substituting a destructive one. */
struct b1nix_fb_cursor {
  u32 visible;  /* 0 hides the cursor; the position is still remembered */
  i32 x;
  i32 y;
  u32 hot_x;
  u32 hot_y;
  u32 image_w; /* 0 in both: keep the current image */
  u32 image_h;
  u64 image; /* userspace address of the BGRA pixels, or 0 */
};

#define B1NIX_FBIOCURSOR 0xFB04 /* arg: struct b1nix_fb_cursor * */

void fb_dev_init(void);

/* True once userspace has mapped the framebuffer: the kernel-side
 * compositor must stop flushing so it doesn't fight the userspace owner. */
int fb_dev_claimed(void);
void fb_dev_mapping_open(struct vfs_node *node);
void fb_dev_mapping_close(struct vfs_node *node);

#endif
