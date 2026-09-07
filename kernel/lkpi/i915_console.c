// SPDX-License-Identifier: GPL-2.0-only
/*
 * The console's framebuffer on i915, mapped the way intel_fbdev maps its own.
 *
 * i915 offers no generic GEM vmap, so drm_client_buffer_vmap() answers
 * -EOPNOTSUPP there. What the driver's own fbdev does instead is pin the
 * framebuffer into the GGTT and map it through the aperture, write-combined:
 * the display engine on these parts is not coherent with the CPU caches, so
 * a plain cached mapping of the pages would leave stale pixels on the panel
 * and this one does not.
 */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_framebuffer.h>
#include "i915_drv.h"
#include "i915_vma.h"
#include "display/intel_fb_pin.h"
#pragma clang diagnostic pop

int drm_console_driver_map(struct drm_framebuffer *fb, void **vaddr)
{
	struct i915_gtt_view view = { .type = I915_GTT_VIEW_NORMAL };
	unsigned long flags = 0;
	struct i915_vma *vma;
	void __iomem *p;

	if (!fb || !fb->dev || !fb->dev->driver ||
	    strcmp(fb->dev->driver->name, "i915"))
		return -EOPNOTSUPP;
	vma = intel_pin_and_fence_fb_obj(fb, false, &view, false, &flags);
	if (IS_ERR(vma))
		return PTR_ERR(vma);
	p = i915_vma_pin_iomap(vma);
	if (IS_ERR(p)) {
		intel_unpin_fb_vma(vma, flags);
		return PTR_ERR(p);
	}
	*vaddr = (void __force *)p;
	return 0;
}
