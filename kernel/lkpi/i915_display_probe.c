/*
 * SPDX-License-Identifier: MIT
 *
 * What the display engine was actually told to fetch.
 *
 * A plane fault says the display engine's memory read failed, and nothing in
 * the driver's own logging says which address it tried. Inference had taken
 * this as far as it goes — DMA addresses, scatterlist lengths and GGTT sizes
 * all check out on paper — so this reads the two registers that settle it:
 * the plane's surface address, and the GGTT entry that address translates
 * through.
 *
 * Driver-specific on purpose, and kept apart from the driver-agnostic mirror
 * for that reason: it reaches into i915's uncore and GGTT, which no other
 * driver shares.
 */

#include "i915_drv.h"
#include "i915_reg.h"
#include "intel_uncore.h"
#include "gt/intel_gtt.h"

#include <drm/drm_device.h>
#include <linux/printk.h>

/*
 * struct drm_i915_private begins with its struct drm_device — checked here
 * rather than assumed, because a cast is not a layout guarantee.
 */
static struct drm_i915_private *to_i915_checked(struct drm_device *dev)
{
	_Static_assert(offsetof(struct drm_i915_private, drm) == 0,
	               "drm_i915_private must start with its drm_device");
	return (struct drm_i915_private *)dev;
}

/*
 * Report the primary plane's surface address on pipe A and what the GGTT says
 * about it.
 *
 * A GGTT entry with bit 0 clear is not present, which is exactly what a plane
 * fault means; a present entry pointing somewhere unexpected means the address
 * we mapped is not the address the device was given.
 */
void lkpi_i915_dump_plane_surface(struct drm_device *dev)
{
	struct drm_i915_private *i915 = to_i915_checked(dev);
	struct i915_ggtt *ggtt = to_gt(i915)->ggtt;
	u32 surf, ctl, stride;
	u64 pte = 0;

	ctl = intel_uncore_read(&i915->uncore, PLANE_CTL(PIPE_A, PLANE_PRIMARY));
	surf = intel_uncore_read(&i915->uncore, PLANE_SURF(PIPE_A, PLANE_PRIMARY));
	stride = intel_uncore_read(&i915->uncore, PLANE_STRIDE(PIPE_A, PLANE_PRIMARY));

	/*
	 * The GGTT's page tables are the GSM, mapped from the upper half of BAR0.
	 * Entry N covers GGTT address N << 12, and the plane's surface address is
	 * a GGTT address.
	 */
	if (ggtt && ggtt->gsm) {
		u64 __iomem *gsm = (u64 __iomem *)ggtt->gsm;

		pte = readq(&gsm[surf >> 12]);
	}

	pr_info("i915-probe: PLANE_CTL %08x SURF %08x STRIDE %08x GGTT[%u]=%llx present=%d\n",
	        ctl, surf, stride, surf >> 12, pte, (int)(pte & 1));
	/*
	 * How much of the frame is actually mapped.
	 *
	 * One present entry at the start proves nothing: the display engine reads
	 * the whole frame, so a binding that stopped early would map the first
	 * pages, translate them fine, and fault partway down every frame — which
	 * is exactly the symptom. 1920x1080x4 is 2025 pages.
	 */
	if (ggtt && ggtt->gsm) {
		u64 __iomem *gsm = (u64 __iomem *)ggtt->gsm;
		u32 first = surf >> 12;
		u32 want = (1920u * 1080u * 4u + 4095u) / 4096u;
		u32 present = 0, i;

		for (i = 0; i < want; i++)
			if (readq(&gsm[first + i]) & 1)
				present++;
		pr_info("i915-probe: %u of %u frame pages present in GGTT\n",
		        present, want);
	}

	pr_info("i915-probe: ggtt total %llx mappable %llx gsm %p\n",
	        ggtt ? (unsigned long long)ggtt->vm.total : 0ull,
	        ggtt ? (unsigned long long)ggtt->mappable_end : 0ull,
	        ggtt ? (void *)ggtt->gsm : NULL);
}
