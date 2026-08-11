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
 * Resolve one page of a buffer object to a physical frame. `index` counts pages
 * from the start of the object.
 *
 * The driver supplies this because only the driver knows how its objects are
 * backed — a page array here, stolen memory or a GTT view on real hardware.
 * Upstream has no callback of this shape because Linux fills a VMA and faults;
 * b1nix asks page by page, so the shape is ours and the knowledge stays with
 * the driver rather than being guessed at in the bridge.
 *
 * Returns 0 with the frame in `*out_phys`, or a negative errno.
 */
typedef int (*lkpi_drm_page_fn)(struct drm_gem_object *obj, u64 index,
                                u64 *out_phys);

/* Publish the device userspace may open, and how to resolve its pages. One
 * device: b1nix exposes a single node from the imported core, and a second
 * driver would need the minor-to-device map this deliberately does without. */
void lkpi_drm_register_device(struct drm_device *dev, lkpi_drm_page_fn resolver);

#endif
