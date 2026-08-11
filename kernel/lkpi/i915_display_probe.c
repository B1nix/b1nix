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

/*
 * Is the display pipe actually scanning out?
 *
 * "flip_done timed out" says the commit's completion never arrived, and that
 * has two entirely different causes: the pipe never started, or it is running
 * and its vblank interrupt is not reaching the core. Nothing in the driver's
 * logging separates them, and the fixes have nothing in common.
 *
 * The hardware answers directly. TRANSCONF's enable bit says whether the
 * transcoder is on; PIPEDSL is the line the scanout is currently on and
 * PIPE_FRMCOUNT the frames it has completed. Sampled twice with a delay
 * between: both moving means the pipe is live and the missing piece is the
 * interrupt, both frozen means the pipe never came up.
 */
void lkpi_i915_dump_pipe_state(struct drm_device *dev)
{
	/*
	 * Named dev_priv, not i915: the _MMIO_PIPE2 macros these registers are
	 * built from index the per-platform register offsets through a variable of
	 * exactly that name, so any other name fails to compile.
	 */
	struct drm_i915_private *dev_priv = to_i915_checked(dev);
	u32 conf, dsl1, dsl2, frm1, frm2;

	conf = intel_uncore_read(&dev_priv->uncore, TRANSCONF(PIPE_A));
	dsl1 = intel_uncore_read(&dev_priv->uncore, PIPEDSL(PIPE_A));
	frm1 = intel_uncore_read(&dev_priv->uncore, PIPE_FRMCOUNT_G4X(PIPE_A));

	/* Longer than a frame at 60 Hz, so a running pipe must have advanced. */
	udelay(20000);

	dsl2 = intel_uncore_read(&dev_priv->uncore, PIPEDSL(PIPE_A));
	frm2 = intel_uncore_read(&dev_priv->uncore, PIPE_FRMCOUNT_G4X(PIPE_A));

	pr_info("i915-probe: TRANSCONF %08x (enabled %d) scanline %u->%u frame %u->%u\n",
	        conf, (int)((conf >> 31) & 1), dsl1, dsl2, frm1, frm2);

	/*
	 * Which output the transcoder was pointed at, and which port buffers are
	 * driving.
	 *
	 * A pipe can be enabled, timed correctly and scanning out, and still put
	 * nothing on a cable: the transcoder has to be routed to a DDI and that
	 * DDI's buffer has to be on. Which DDI belongs to which physical connector
	 * comes from the VBT, and there is no VBT here — so the driver is working
	 * from defaults, and a mismatch shows up as exactly this, a live pipe and a
	 * dark monitor.
	 *
	 * TRANS_DDI_FUNC_CTL bit 31 is the function enable and bits 30:28 select
	 * the port. DDI_BUF_CTL bit 31 is that port's buffer enable.
	 */
	{
		u32 func = intel_uncore_read(&dev_priv->uncore,
		                             TRANS_DDI_FUNC_CTL(TRANSCODER_A));
		enum port p;

		pr_info("i915-probe: TRANS_DDI_FUNC_CTL %08x (enabled %d, port select %u)\n",
		        func, (int)((func >> 31) & 1), (func >> 28) & 0x7);

		for (p = PORT_A; p <= PORT_F; p++) {
			u32 buf = intel_uncore_read(&dev_priv->uncore, DDI_BUF_CTL(p));

			pr_info("i915-probe: DDI_BUF_CTL(%c) %08x enabled %d idle %d\n",
			        'A' + (int)p, buf, (int)((buf >> 31) & 1),
			        (int)((buf >> 7) & 1));
		}
	}
}

/* ── mapping i915 objects into userspace ──────────────────────────── */

#include "gem/i915_gem_object.h"
#include <lkpi/drm_bridge.h>

/*
 * Where page `index` of an i915 object lives, for b1nix's DRM mmap path.
 *
 * The bridge asks page by page rather than filling a VMA and faulting — see the
 * note on lkpi_drm_page_fn — so this is the whole of what mapping an object
 * from userspace needs from the driver.
 *
 * The address is the DMA address, not the CPU physical one. On this machine the
 * two are equal: the guest has no IOMMU of its own, so nothing translates
 * between them, and the bounce path is never taken because guest memory is well
 * inside any device's reach. Asking for the DMA address rather than assuming
 * that keeps it right if either changes.
 *
 * Pages are pinned on the way in and stay pinned: a page that moved after being
 * handed to userspace would leave the process pointing at memory the object no
 * longer owns, and nothing here can revoke a mapping to tell it otherwise (see
 * zap_vma_ptes() in <linux/mm.h>).
 */
static int i915_gem_page_phys(struct drm_gem_object *obj, u64 index,
                              u64 *out_phys)
{
	struct drm_i915_gem_object *bo;
	dma_addr_t addr;
	int ret;

	if (!obj || !out_phys)
		return -EINVAL;
	bo = to_intel_bo(obj);

	if (index >= (u64)(obj->size >> PAGE_SHIFT))
		return -EINVAL;

	ret = i915_gem_object_pin_pages_unlocked(bo);
	if (ret)
		return ret;

	addr = i915_gem_object_get_dma_address(bo, (pgoff_t)index);
	if (!addr)
		return -EFAULT;

	*out_phys = (u64)addr;
	return 0;
}

/* Publish this device to the bridge, so userspace can open and map it. */
void lkpi_i915_register_card(struct drm_device *dev)
{
	if (!dev)
		return;

	/*
	 * Which south bridge the driver decided it is sitting next to.
	 *
	 * On this generation GMBUS completion and hotplug are serviced by the PCH,
	 * and a passed-through GPU has no PCH of its own — the VMM emulates one.
	 * i915 recognises an emulated bridge and assumes the matching real PCH, but
	 * only for the ids it knows; anything else leaves PCH_NONE, and then the
	 * south display interrupts are never serviced. That turns into an EDID read
	 * that waits for a completion nobody will signal, which reads as "monitor
	 * disconnected" rather than as a missing interrupt.
	 *
	 * PCH_NONE is 0. Printed unconditionally because it decides whether display
	 * detection can work at all on this machine type.
	 */
	pr_info("i915-probe: pch_type %d pch_id %04x\n",
	        (int)to_i915_checked(dev)->pch_type,
	        (unsigned)to_i915_checked(dev)->pch_id);

	lkpi_drm_register_device(dev, i915_gem_page_phys);
}
