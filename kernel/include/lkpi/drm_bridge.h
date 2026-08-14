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


/* Attach a synthetic sink to unconnected connectors, so the modeset path can
 * run on a machine with nothing plugged in. `connector_type` is a
 * DRM_MODE_CONNECTOR_* value, or 0 for any. Returns how many it attached to.
 * See kernel/lkpi/drm_virtual_monitor.c for what this does and does not test. */
int lkpi_drm_attach_virtual_monitor(int connector_type);

/* Force a real-but-undetected display on, modes from the driver's own list.
 * For a display that is attached but whose EDID cannot be read. */
int lkpi_drm_force_connector_on(const char *name);

/* Hand a connector an EDID read elsewhere, for a display whose DDC will not
 * answer. See the note in drm_virtual_monitor.c. */
int lkpi_drm_set_connector_edid(const char *name, const void *edid, usize size);

/*
 * DRM_MODE_CONNECTOR_HDMIA, repeated because callers on b1nix's side cannot
 * include the DRM headers. drm_virtual_monitor.c asserts the two agree, so a
 * change upstream is a build error rather than a connector nobody matches.
 */
#define LKPI_DRM_CONNECTOR_HDMIA 11


/* Every registered device, so b1nix can publish a node per card. Index is
 * registration order, which is boot order. */
unsigned lkpi_drm_device_count(void);
int lkpi_drm_minor_at(unsigned index, u32 *out_minor);

#endif
