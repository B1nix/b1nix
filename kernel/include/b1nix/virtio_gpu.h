#ifndef B1NIX_VIRTIO_GPU_H
#define B1NIX_VIRTIO_GPU_H

#include <b1nix/types.h>

void virtio_gpu_init(void);
void virtio_gpu_dev_init(void);
int virtio_gpu_ready(void);
/* Test mode only: drive commands through the blocking wait and report whether
 * the completion interrupt is what ended them. Needs the live scheduler. */
void virtio_gpu_irq_selftest(void);
void virtio_gpu_get_mode(u32 *width, u32 *height);
int virtio_gpu_present(const u32 *src, u32 width, u32 height, u32 dirty_x, u32 dirty_y, u32 dirty_w, u32 dirty_h,
                       int cursor_x, int cursor_y, int cursor_visible);

/*
 * The hardware cursor. The device composes it on the host side, so moving it
 * costs one small command and leaves the scanout resource untouched — unlike
 * painting a pointer into the frame, which destroys the pixels underneath.
 * Every one of these returns -1 when the device has no cursor resource, so a
 * caller learns that instead of getting a destructive substitute.
 */
int virtio_gpu_cursor_ready(void);
/* Up to 64x64 BGRA pixels; a smaller image is placed at the resource's origin
 * and the rest left transparent. The hot spot must lie inside the image. */
int virtio_gpu_set_cursor_image(const u32 *bgra, u32 width, u32 height,
                                u32 hot_x, u32 hot_y);
int virtio_gpu_show_cursor(int x, int y);
int virtio_gpu_move_cursor(int x, int y);
int virtio_gpu_hide_cursor(void);

#endif
