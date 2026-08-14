/* SPDX-License-Identifier: MIT */
#ifndef LKPI_DRM_BRIDGE_H
#define LKPI_DRM_BRIDGE_H

#include <b1nix/types.h>

/*
 * The driver's half of the character-device bridge.
 *
 * Separate from <lkpi/drmdev.h> because the two have different audiences:
 * drmdev.h is what b1nix's VFS sees and may name no Linux type at all, while
 * this is included only by driver code already compiled against the imported
 * headers. Keeping them apart is what lets the b1nix side stay ignorant of
 * `struct drm_gem_object`, which is the entire point of the boundary.
 */

struct drm_device;
struct drm_gem_object;

/*
 * Resolve one page of a mappable offset to a physical frame. `index` counts
 * pages from the start of whatever that offset names.
 *
 * The callback is handed the offset NODE, not an object, because the two are
 * not the same thing and only the driver knows which. Our own driver embeds the
 * node in its buffer object; i915 embeds it in a struct i915_mmap_offset, one
 * per mapping type, which merely points at the object. Casting the node to a
 * buffer object works for the first and lands in unrelated memory for the
 * second — where it read a size of 4503582447522665 pages and refused every
 * mapping a compositor asked for.
 *
 * Returns 0 with the frame in `*out_phys`, or a negative errno.
 */
struct drm_vma_offset_node;
typedef int (*lkpi_drm_page_fn)(struct drm_vma_offset_node *node, u64 index,
                                u64 *out_phys);

/* Publish the device userspace may open, and how to resolve its pages. One
 * device: b1nix exposes a single node from the imported core, and a second
 * driver would need the minor-to-device map this deliberately does without. */
void lkpi_drm_register_device(struct drm_device *dev, lkpi_drm_page_fn resolver);


/* Every registered device, so b1nix can publish a node per card. Index is
 * registration order, which is boot order. */
unsigned lkpi_drm_device_count(void);
int lkpi_drm_minor_at(unsigned index, u32 *out_minor);

#endif
