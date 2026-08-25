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


/* ── virgl, for the DRM node ─────────────────────────────────────────────
 *
 * Mesa speaks the Linux DRM ioctls, which are served in the shim
 * (kernel/lkpi/) -- a layer that may not include b1nix headers. These are the
 * plain entry points it reaches the transport through, via lkpi_virgl_*.
 */

/* What RESOURCE_CREATE_3D needs, without dragging the wire structs across. */
struct virtio_gpu_res_params {
	u32 target, format, bind, width, height, depth;
	u32 array_size, last_level, nr_samples, flags;
};

int virtio_gpu_virgl_available(void);
/* Claim a resource id (0 when the table is full). The same table the
 * character device draws from, so the two front doors cannot collide. */
u32 virtio_gpu_virgl_res_alloc(void);
/* Create the resource, bind it to the context and attach `npages` pages as its
 * backing. The pages need not be contiguous: adjacent ones are coalesced and
 * the rest become separate entries. */
int virtio_gpu_virgl_res_create(u32 ctx_id, const struct virtio_gpu_res_params *p,
				const u64 *phys, u32 npages, u32 res_id);
int virtio_gpu_virgl_ctx_create(u32 ctx_id);
int virtio_gpu_virgl_submit(u32 ctx_id, const u32 *cmd, u32 bytes);
/* box6 is x, y, z, w, h, d. */
int virtio_gpu_virgl_transfer(int to_host, u32 ctx_id, u32 res_id, u32 level,
			      const u32 *box6, u64 offset);
int virtio_gpu_virgl_unref(u32 res_id);
/* Read a capset blob. Without it Mesa cannot create a virgl screen at all. */
int virtio_gpu_virgl_capset(u32 index, u32 want_id, u32 want_ver, void *out,
			    u32 *len);
/* Bitmask of the capset ids the host offers. Mesa gives up on a zero. */
int virtio_gpu_virgl_capset_ids(u64 *mask);

#endif
