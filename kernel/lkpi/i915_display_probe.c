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
#include <drm/drm_crtc.h>
#include <drm/drm_vblank.h>
#include <linux/printk.h>
#include <lkpi/env.h>

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

/*
 * Is the vblank interrupt armed, and is anything counting it?
 *
 * A commit that ends in "flip_done timed out" has failed to see one vblank.
 * Between the pipe and the wait there are four separate places that can drop
 * it, and only registers and core state distinguish them:
 *
 *   - the display engine's own mask: GEN8_DE_PIPE_IMR bit 0 clear and
 *     GEN8_DE_PIPE_IER bit 0 set mean the pipe is allowed to raise it;
 *   - the master enable: GEN8_MASTER_IRQ bit 31, which the handler clears and
 *     restores on every interrupt, so a stuck-clear one means the handler
 *     stopped half way;
 *   - delivery: GEN8_DE_PIPE_IIR bit 0 latched and staying latched means the
 *     pipe raised it and nobody serviced it;
 *   - the core's own bookkeeping: drm_vblank_crtc.enabled says the core asked
 *     for it, refcount says who is holding it on, and count only advances from
 *     drm_handle_vblank() — the very call flip_done needs.
 *
 * Sampled twice around a delay of a few frames, because every one of these is
 * a level or a counter rather than an event: what matters is which of them
 * moves.
 */
void lkpi_i915_dump_vblank_state(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915_checked(dev);
	u32 master1, imr1, ier1, iir1, master2, iir2;
	u64 seq1 = 0, seq2 = 0;
	int enabled = -1, refcount = -1;

	master1 = intel_uncore_read(&dev_priv->uncore, GEN8_MASTER_IRQ);
	imr1 = intel_uncore_read(&dev_priv->uncore, GEN8_DE_PIPE_IMR(PIPE_A));
	ier1 = intel_uncore_read(&dev_priv->uncore, GEN8_DE_PIPE_IER(PIPE_A));
	iir1 = intel_uncore_read(&dev_priv->uncore, GEN8_DE_PIPE_IIR(PIPE_A));

	if (dev->num_crtcs > 0 && dev->vblank) {
		seq1 = (u64)atomic64_read(&dev->vblank[0].count);
		enabled = dev->vblank[0].enabled ? 1 : 0;
		refcount = atomic_read(&dev->vblank[0].refcount);
	}

	/* Three frames at 60 Hz: a live, armed pipe raises several. */
	udelay(50000);

	master2 = intel_uncore_read(&dev_priv->uncore, GEN8_MASTER_IRQ);
	iir2 = intel_uncore_read(&dev_priv->uncore, GEN8_DE_PIPE_IIR(PIPE_A));
	if (dev->num_crtcs > 0 && dev->vblank)
		seq2 = (u64)atomic64_read(&dev->vblank[0].count);

	pr_info("i915-probe: MASTER_IRQ %08x->%08x DE_PIPE_A IMR %08x IER %08x "
	        "IIR %08x->%08x (vblank masked %d enabled-in-IER %d)\n",
	        master1, master2, imr1, ier1, iir1, iir2,
	        (int)(imr1 & GEN8_PIPE_VBLANK ? 1 : 0),
	        (int)(ier1 & GEN8_PIPE_VBLANK ? 1 : 0));
	pr_info("i915-probe: drm vblank crtcs %u immediate %d enabled %d "
	        "refcount %d count %llu->%llu\n",
	        dev->num_crtcs, (int)dev->vblank_disable_immediate,
	        enabled, refcount,
	        (unsigned long long)seq1, (unsigned long long)seq2);

	/*
	 * Asking for the interrupt directly.
	 *
	 * If the count did not move, the core either never turned the interrupt on
	 * or turned it on and got nothing. Taking a reference of our own drives
	 * enable_vblank() from here, with the pipe already running, so a count that
	 * moves afterwards puts the fault in whoever should have taken that
	 * reference during the commit, and a count that still does not move puts it
	 * in the interrupt path itself.
	 *
	 * Behind a flag because it drives the hardware rather than reading it, and
	 * a diagnostic that changes state has to be asked for.
	 */
	if (lkpi_bootflag("b1nix.vblank-force") && dev->num_crtcs > 0) {
		struct drm_crtc *crtc = drm_crtc_from_index(dev, 0);
		int ret;

		if (!crtc) {
			pr_info("i915-probe: vblank-force: no crtc 0\n");
			return;
		}
		ret = drm_crtc_vblank_get(crtc);
		if (ret) {
			pr_info("i915-probe: vblank-force: get failed %d\n", ret);
			return;
		}
		seq1 = (u64)atomic64_read(&dev->vblank[0].count);
		udelay(50000);
		seq2 = (u64)atomic64_read(&dev->vblank[0].count);
		iir2 = intel_uncore_read(&dev_priv->uncore, GEN8_DE_PIPE_IIR(PIPE_A));
		pr_info("i915-probe: vblank-force: IMR %08x IER %08x IIR %08x "
		        "count %llu->%llu\n",
		        intel_uncore_read(&dev_priv->uncore, GEN8_DE_PIPE_IMR(PIPE_A)),
		        intel_uncore_read(&dev_priv->uncore, GEN8_DE_PIPE_IER(PIPE_A)),
		        iir2, (unsigned long long)seq1, (unsigned long long)seq2);
		drm_crtc_vblank_put(crtc);
	}
}

/*
 * What is actually leaving the pipe, checked without a person in the room.
 *
 * A monitor reports nothing about the picture it receives, so "is there an
 * image on the cable" normally ends at somebody looking at a screen. The
 * display engine answers it directly instead: it computes a CRC over the
 * pixels the pipe emits, once per frame, from the same tap the port reads. A
 * CRC that is stable while the framebuffer is stable, and that changes when the
 * framebuffer is overwritten, says the bytes we painted are the bytes being
 * scanned out — through the plane, the pipe and the transcoder, all the way to
 * the port.
 *
 * It says nothing about the cable or the sink. Those are the only two links it
 * cannot reach, and no register on this side can.
 */
void lkpi_i915_crc_begin(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915_checked(dev);

	/* DMUX is the pipe's own output, past every plane and the blender —
	 * exactly what the transcoder takes. */
	intel_uncore_write(&dev_priv->uncore, PIPE_CRC_CTL(PIPE_A),
	                   PIPE_CRC_ENABLE | PIPE_CRC_SOURCE_DMUX_SKL);
	intel_uncore_posting_read(&dev_priv->uncore, PIPE_CRC_CTL(PIPE_A));
}

void lkpi_i915_crc_end(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915_checked(dev);

	intel_uncore_write(&dev_priv->uncore, PIPE_CRC_CTL(PIPE_A), 0);
}

/*
 * The CRC of the next whole frame.
 *
 * The result register is rewritten at the end of every frame, so a read has to
 * be tied to a frame boundary or it returns whichever frame happened to finish
 * — including a frame that was still being drawn into. Waiting for
 * PIPE_FRMCOUNT to move twice brackets one complete frame.
 */
u32 lkpi_i915_crc_sample(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915_checked(dev);
	u32 start, seen;
	unsigned spins;

	start = intel_uncore_read(&dev_priv->uncore, PIPE_FRMCOUNT_G4X(PIPE_A));
	for (seen = 0; seen < 2; seen++) {
		u32 target = start + seen + 1;

		/* A frame is 16 ms; 200 x 1 ms is an order of magnitude of headroom,
		 * and gives up rather than hanging if the pipe stops. */
		for (spins = 0; spins < 200; spins++) {
			if (intel_uncore_read(&dev_priv->uncore,
			                      PIPE_FRMCOUNT_G4X(PIPE_A)) >= target)
				break;
			udelay(1000);
		}
		if (spins == 200)
			return 0;
	}
	return intel_uncore_read(&dev_priv->uncore, PIPE_CRC_RES_1_IVB(PIPE_A));
}

/*
 * Whether the port is being driven as HDMI, and whether it is being told what
 * it is receiving.
 *
 * A sink that gets pixels with no AVI InfoFrame, or gets them in a format the
 * InfoFrame does not describe, is entitled to show nothing at all — which looks
 * exactly like a dead link from this side. VIDEO_DIP_CTL is where that is
 * armed, so its enable bits are the difference between "we sent a picture" and
 * "we sent a picture the monitor was willing to display".
 */
void lkpi_i915_dump_infoframes(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915_checked(dev);
	u32 dip = intel_uncore_read(&dev_priv->uncore,
	                            HSW_TVIDEO_DIP_CTL(TRANSCODER_A));

	pr_info("i915-probe: VIDEO_DIP_CTL %08x (enable %d, AVI %d)\n",
	        dip, (int)((dip & VIDEO_DIP_ENABLE) ? 1 : 0),
	        (int)((dip & VIDEO_DIP_ENABLE_AVI_HSW) ? 1 : 0));
}

/*
 * The last stretch that has any readback at all: the clock and the PHY.
 *
 * The pipe CRC proves the picture reaches the transcoder, and stops there —
 * it is sampled before the port. Past that point nothing reports what left the
 * connector, but three things say whether anything could have:
 *
 *   - the PLL that makes the link clock, and whether it locked. An unlocked
 *     PLL means no TMDS clock, and a sink with no clock shows nothing and does
 *     not even wake;
 *   - DPLL_CTRL2's per-port clock gate, which can be off with everything else
 *     configured, and the DDI's own buffer state. DDI_BUF_IS_IDLE clear means
 *     the buffer is driving the lines rather than parked;
 *   - the transcoder's output format: HDMI mode rather than DVI, and the bits
 *     per colour it is sending, since a sink that cannot take the format shows
 *     a black screen while every register on this side looks correct.
 *
 * Hotplug live state comes along too: the sink asserting HPD throughout the
 * scanout is the one thing the far end of the cable does say.
 */
void lkpi_i915_dump_port_state(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915_checked(dev);
	u32 func, ctrl1, ctrl2, status, sdeisr, buf1, buf2, iir;
	enum port port;
	unsigned id;

	func = intel_uncore_read(&dev_priv->uncore,
	                         TRANS_DDI_FUNC_CTL(TRANSCODER_A));
	/* Bits 30:28 name the port the transcoder feeds; 1 is DDI B. */
	port = (enum port)(((func >> 28) & 0x7) ? ((func >> 28) & 0x7) : 0);

	ctrl1 = intel_uncore_read(&dev_priv->uncore, DPLL_CTRL1);
	ctrl2 = intel_uncore_read(&dev_priv->uncore, DPLL_CTRL2);
	status = intel_uncore_read(&dev_priv->uncore, DPLL_STATUS);
	sdeisr = intel_uncore_read(&dev_priv->uncore, SDEISR);

	buf1 = intel_uncore_read(&dev_priv->uncore, DDI_BUF_CTL(port));
	udelay(20000);
	buf2 = intel_uncore_read(&dev_priv->uncore, DDI_BUF_CTL(port));
	iir = intel_uncore_read(&dev_priv->uncore, GEN8_DE_PIPE_IIR(PIPE_A));

	pr_info("i915-probe: DPLL_CTRL1 %08x CTRL2 %08x STATUS %08x "
	        "(locked:", ctrl1, ctrl2, status);
	for (id = 0; id < 4; id++)
		if (status & DPLL_LOCK(id))
			pr_info(" %u", id);
	pr_info(")\n");

	pr_info("i915-probe: port %c clk-off %d clk-sel %u DDI_BUF_CTL %08x->%08x "
	        "(idle %d->%d)\n",
	        'A' + (int)port,
	        (int)((ctrl2 & DPLL_CTRL2_DDI_CLK_OFF(port)) ? 1 : 0),
	        (unsigned)((ctrl2 >> DPLL_CTRL2_DDI_CLK_SEL_SHIFT(port)) & 3),
	        buf1, buf2,
	        (int)((buf1 & DDI_BUF_IS_IDLE) ? 1 : 0),
	        (int)((buf2 & DDI_BUF_IS_IDLE) ? 1 : 0));

	/*
	 * Mode 0 is HDMI, 1 is DVI, 2 is DP SST. The BPC field is 0 for 8 bits
	 * per colour, which is the only format every HDMI sink must accept.
	 */
	pr_info("i915-probe: TRANS_DDI mode %u bpc %u, pipe underrun %d, "
	        "SDEISR %08x\n",
	        (unsigned)((func & TRANS_DDI_MODE_SELECT_MASK) >> 24),
	        (unsigned)((func & TRANS_DDI_BPC_MASK) >> 20),
	        (int)((iir & GEN8_PIPE_FIFO_UNDERRUN) ? 1 : 0), sdeisr);
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
