#ifndef B1NIX_VIRTIO_GPU_H
#define B1NIX_VIRTIO_GPU_H

#include <b1nix/types.h>

void virtio_gpu_init(void);
int virtio_gpu_ready(void);
int virtio_gpu_present(const u32 *src, u32 width, u32 height, u32 dirty_x, u32 dirty_y, u32 dirty_w, u32 dirty_h,
                       int cursor_x, int cursor_y, int cursor_visible);

#endif
