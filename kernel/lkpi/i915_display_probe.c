/*
 * SPDX-License-Identifier: GPL-2.0-only
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
#include "display/intel_gmbus_regs.h"
#include "display/intel_cdclk.h"
#include "i915_reg.h"
#include "intel_uncore.h"
#include "gt/intel_gtt.h"

#include <drm/drm_device.h>
#include <drm/drm_edid.h>
#include <drm/drm_connector.h>
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
/*
 * The GMBUS controller's own registers.
 *
 * Every EDID read on this machine times out and falls back to bit-banging,
 * which takes minutes — long enough that a compositor gives up and drives the
 * monitor with a fallback mode. The first question is whether the controller is
 * being addressed at all: a wrong MMIO base reads as a bus that never becomes
 * ready, which is exactly what a timeout looks like from inside the driver.
 */
/*
 * The EDID a connector managed to read, as hex.
 *
 * Reading it over the wire costs minutes on this machine — GMBUS never
 * completes and the bit-banging fallback cannot see the lines — so the bytes
 * are worth capturing once and supplying from the kernel afterwards, the way
 * Linux's edid_firmware option does. This prints them in a form that can be
 * pasted back into a build.
 */
void lkpi_i915_dump_edid(struct drm_device *dev)
{
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;

	if (!dev)
		return;
	drm_connector_list_iter_begin(dev, &iter);
	drm_for_each_connector_iter(connector, &iter) {
		const struct drm_edid *edid;
		const struct edid *raw;
		usize len, i;
		char line[80];
		unsigned col = 0;

		if (connector->status != connector_status_connected)
			continue;
		edid = drm_edid_read(connector);
		if (!edid)
			continue;
		raw = drm_edid_raw(edid);
		len = raw ? (usize)(128 * (1 + raw->extensions)) : 0;
		pr_info("i915-probe: EDID for %s, %u bytes:\n", connector->name,
		        (unsigned)len);
		for (i = 0; i < len; i++) {
			static const char hex[] = "0123456789abcdef";
			const u8 *b = (const u8 *)raw;

			line[col++] = hex[b[i] >> 4];
			line[col++] = hex[b[i] & 0xf];
			if (col >= 64 || i + 1 == len) {
				line[col] = 0;
				pr_info("EDID %s\n", line);
				col = 0;
			}
		}
		drm_edid_free(edid);
	}
	drm_connector_list_iter_end(&iter);
}

/*
 * Release a GMBUS left mid-transfer by whoever ran before us.
 *
 * The controller carries a hardware semaphore (INUSE) and a cycle in progress
 * (ACTIVE, with a byte count still outstanding). Firmware — or a previous
 * owner of a passed-through card — can hand the device over in that state, and
 * nothing in the driver clears it: every transfer then waits for a bus that is
 * already busy, times out, and falls back to bit-banging the I2C lines, which
 * takes minutes per EDID. On this machine the card arrives exactly so, with
 * ACTIVE set and thirty-odd bytes outstanding.
 *
 * Abandoning the cycle and writing 1 to INUSE — the documented way to release
 * the semaphore — puts the controller back where a driver expects to find it.
 */
void lkpi_i915_gmbus_recover(struct drm_device *dev)
{
	struct drm_i915_private *i915 = to_i915_checked(dev);
	u32 stat;

	if (!i915)
		return;
	stat = intel_uncore_read(&i915->uncore, GMBUS2(i915));
	if (!(stat & (GMBUS_INUSE | GMBUS_ACTIVE)))
		return;

	pr_info("i915-probe: GMBUS held on handover (GMBUS2 %x), releasing\n",
	        (unsigned)stat);
	/* Abandon the cycle: clear the software-ready bit and any pending
	 * interrupt, deselect the pin, then drop the semaphore. */
	intel_uncore_write(&i915->uncore, GMBUS1(i915), GMBUS_SW_CLR_INT);
	intel_uncore_write(&i915->uncore, GMBUS1(i915), 0);
	intel_uncore_write(&i915->uncore, GMBUS0(i915), 0);
	intel_uncore_write(&i915->uncore, GMBUS4(i915), 0);
	intel_uncore_write(&i915->uncore, GMBUS2(i915), GMBUS_INUSE);
	intel_uncore_posting_read(&i915->uncore, GMBUS2(i915));
	pr_info("i915-probe: GMBUS after release: %x\n",
	        (unsigned)intel_uncore_read(&i915->uncore, GMBUS2(i915)));
}

void lkpi_i915_dump_gmbus(struct drm_device *dev)
{
	struct drm_i915_private *i915 = to_i915_checked(dev);
	u32 saved, probe;

	if (!i915)
		return;

	/*
	 * Does a write to this block land at all?
	 *
	 * Reads plainly work — the status register holds sensible values — but a
	 * transfer that never starts looks the same whether the controller ignored
	 * the command or answered it. Writing the port-select register and reading
	 * it back separates those two, and it is safe: the value is restored, and
	 * this runs before anything else drives the bus.
	 */
	saved = intel_uncore_read(&i915->uncore, GMBUS0(i915));
	intel_uncore_write(&i915->uncore, GMBUS0(i915), 0x4);
	probe = intel_uncore_read(&i915->uncore, GMBUS0(i915));
	intel_uncore_write(&i915->uncore, GMBUS0(i915), saved);
	pr_info("i915-probe: gmbus write test: wrote 4, read back %x (saved %x)\n",
	        (unsigned)probe, (unsigned)saved);
	/* The clock the GMBUS engine runs on. A controller with no reference clock
	 * accepts commands and never completes them, which is what a timeout looks
	 * like from the driver's side. */
	pr_info("i915-probe: rawclk reg %x, rawclk_freq %u kHz\n",
	        (unsigned)intel_uncore_read(&i915->uncore, PCH_RAWCLK_FREQ),
	        (unsigned)i915->display.cdclk.hw.ref);
	pr_info("i915-probe: gmbus base %x GMBUS0 %x GMBUS1 %x GMBUS2 %x GMBUS4 %x GMBUS5 %x\n",
	        (unsigned)GMBUS_MMIO_BASE(i915),
	        (unsigned)intel_uncore_read(&i915->uncore, GMBUS0(i915)),
	        (unsigned)intel_uncore_read(&i915->uncore, GMBUS1(i915)),
	        (unsigned)intel_uncore_read(&i915->uncore, GMBUS2(i915)),
	        (unsigned)intel_uncore_read(&i915->uncore, GMBUS4(i915)),
	        (unsigned)intel_uncore_read(&i915->uncore, GMBUS5(i915)));
}

void lkpi_i915_dump_port_state(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915_checked(dev);
	/*
	 * A display power reference for the duration of the dump.
	 *
	 * Registers in an unpowered domain read as zero rather than faulting, so a
	 * dump taken without one reports a display that is entirely switched off —
	 * which is indistinguishable from a modeset that never happened, and sent
	 * this investigation after the wrong thing twice.
	 */
	intel_wakeref_t dump_wakeref =
		intel_display_power_get(dev_priv, POWER_DOMAIN_DISPLAY_CORE);
	u32 func, ctrl1, ctrl2, status, sdeisr, buf1, buf2, iir;
	enum transcoder dump_transcoder;
	enum port port;
	unsigned id;

	/*
	 * Whichever transcoder is actually driving something.
	 *
	 * Reading transcoder A alone reported a dead pipe while a compositor was
	 * configuring pipe C — the numbers looked like "no modeset happened" when
	 * one plainly had. The enable bit picks the live one; A remains the
	 * fallback so the dump still says something on an idle machine.
	 */
	{
		static const enum transcoder candidates[] = {
			TRANSCODER_A, TRANSCODER_B, TRANSCODER_C
		};
		enum transcoder chosen = TRANSCODER_A;

		for (unsigned i = 0; i < 3; i++) {
			u32 conf = intel_uncore_read(&dev_priv->uncore,
			                             TRANSCONF(candidates[i]));

			if (conf & TRANSCONF_ENABLE) {
				chosen = candidates[i];
				break;
			}
		}
		dump_transcoder = chosen;
		pr_info("i915-probe: TRANSCONF A %x B %x C %x, dumping %d\n",
		        (unsigned)intel_uncore_read(&dev_priv->uncore,
		                                    TRANSCONF(TRANSCODER_A)),
		        (unsigned)intel_uncore_read(&dev_priv->uncore,
		                                    TRANSCONF(TRANSCODER_B)),
		        (unsigned)intel_uncore_read(&dev_priv->uncore,
		                                    TRANSCONF(TRANSCODER_C)),
		        (int)chosen);
	}

	func = intel_uncore_read(&dev_priv->uncore,
	                         TRANS_DDI_FUNC_CTL(dump_transcoder));
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
	/*
	 * The electrical half of the port: the buffer translation table.
	 *
	 * Everything read so far is the digital side — clock, timings, format,
	 * enables — and all of it can be right while the PHY drives no usable
	 * signal, because the voltage swing and de-emphasis for HDMI come from a
	 * table the driver writes here, taken from the VBT. Entries left at zero
	 * mean a port that is enabled, not idle, and electrically silent: the sink
	 * sees no clock and never wakes, which is exactly what the camera shows.
	 *
	 * The last entry (index 9 on this generation) is the HDMI one.
	 */
	{
		u32 lo0 = intel_uncore_read(&dev_priv->uncore, DDI_BUF_TRANS_LO(port, 0));
		u32 hi0 = intel_uncore_read(&dev_priv->uncore, DDI_BUF_TRANS_HI(port, 0));
		u32 lo9 = intel_uncore_read(&dev_priv->uncore, DDI_BUF_TRANS_LO(port, 9));
		u32 hi9 = intel_uncore_read(&dev_priv->uncore, DDI_BUF_TRANS_HI(port, 9));
		unsigned i, nonzero = 0;

		for (i = 0; i < 10; i++)
			if (intel_uncore_read(&dev_priv->uncore,
			                      DDI_BUF_TRANS_LO(port, i)) != 0)
				nonzero++;

		pr_info("i915-probe: DDI_BUF_TRANS[0] %08x/%08x [9=HDMI] %08x/%08x, "
		        "%u of 10 entries programmed\n",
		        lo0, hi0, lo9, hi9, nonzero);
	}

	/*
	 * The timings actually programmed, and the PLL that clocks them.
	 *
	 * A locked PLL is not a correct PLL: lock says the loop closed on whatever
	 * it was configured for, not that the frequency matches the mode. A sink
	 * fed a clock far from what the timings imply cannot lock a frame, and a
	 * sink that never locks can stay dark rather than complain.
	 *
	 * 1920x1080 at 60 Hz is HTOTAL 2200 and VTOTAL 1125 (the registers hold
	 * value-1), which is 148.5 MHz of pixel clock.
	 */
	{
		u32 ht = intel_uncore_read(&dev_priv->uncore, TRANS_HTOTAL(dump_transcoder));
		u32 vt = intel_uncore_read(&dev_priv->uncore, TRANS_VTOTAL(dump_transcoder));
		u32 c1 = intel_uncore_read(&dev_priv->uncore, DPLL_CFGCR1(SKL_DPLL1));
		u32 c2 = intel_uncore_read(&dev_priv->uncore, DPLL_CFGCR2(SKL_DPLL1));

		pr_info("i915-probe: HTOTAL %08x (active %u, total %u) "
		        "VTOTAL %08x (active %u, total %u)\n",
		        ht, (ht & 0xffff) + 1, ((ht >> 16) & 0xffff) + 1,
		        vt, (vt & 0xffff) + 1, ((vt >> 16) & 0xffff) + 1);
		pr_info("i915-probe: DPLL1_CFGCR1 %08x (enable %d, dco int %u, frac %u) "
		        "CFGCR2 %08x\n",
		        c1, (int)((c1 & DPLL_CFGCR1_FREQ_ENABLE) ? 1 : 0),
		        (unsigned)(c1 & DPLL_CFGCR1_DCO_INTEGER_MASK),
		        (unsigned)((c1 & DPLL_CFGCR1_DCO_FRACTION_MASK) >> 9), c2);
	}

	/*
	 * Which port clock the transcoder is fed.
	 *
	 * The pipe's timing generator runs off CDCLK, so scanline and frame count
	 * advance whether or not this is set — but what leaves the DDI is clocked
	 * from here, and TRANS_CLK_SEL_DISABLED means the transcoder is attached to
	 * no port clock at all. A sink then sees no TMDS clock and stays asleep,
	 * which is precisely the case a live pipe and an enabled port buffer cannot
	 * distinguish.
	 */
	{
		u32 cs = intel_uncore_read(&dev_priv->uncore,
		                           TRANS_CLK_SEL(dump_transcoder));

		pr_info("i915-probe: TRANS_CLK_SEL(A) %08x (port sel %u, %s)\n",
		        cs, (unsigned)(cs >> 29),
		        (cs >> 29) == 0 ? "DISABLED" : "attached");
	}

	/*
	 * The AVI InfoFrame the port is actually sending, byte for byte.
	 *
	 * Its enable bit was compared with the host's and matches; its contents
	 * never were. A sink is entitled to ignore a signal whose InfoFrame is
	 * malformed, and the way it ignores it is by staying asleep — the symptom
	 * here exactly. The packet carries its own checksum, defined so that the
	 * sum of every byte including the header is zero, so its validity can be
	 * decided without anything to compare against.
	 *
	 * The DIP data registers hold the packet little-endian, four bytes each,
	 * starting with the three header bytes.
	 */
	{
		u32 w[4];
		unsigned i, sum = 0;
		unsigned char b[16];

		for (i = 0; i < 4; i++)
			w[i] = intel_uncore_read(&dev_priv->uncore,
			                         HSW_TVIDEO_DIP_AVI_DATA(TRANSCODER_A, i));
		for (i = 0; i < 16; i++)
			b[i] = (unsigned char)((w[i / 4] >> ((i % 4) * 8)) & 0xff);
		/* Header (3 bytes) plus the 13 payload bytes an AVI InfoFrame has. */
		for (i = 0; i < 16; i++)
			sum += b[i];

		pr_info("i915-probe: AVI DIP %08x %08x %08x %08x\n",
		        w[0], w[1], w[2], w[3]);
		pr_info("i915-probe: AVI type %02x ver %02x len %02x checksum %02x, "
		        "byte sum %02x (0 = valid)\n",
		        b[0], b[1], b[2], b[3], sum & 0xff);
	}

	/*
	 * The AVI InfoFrame the port is actually sending, byte for byte.
	 *
	 * Its enable bit matches the host's; its contents were never read. A sink
	 * may lock onto a signal, find the InfoFrame malformed and drop it again —
	 * which is exactly what this monitor does, showing the frame for two or
	 * three seconds before going dark. The packet carries a checksum defined so
	 * that all its bytes sum to zero, so it can be judged on its own.
	 */
	{
		u32 w[8];
		unsigned i, sum = 0;
		unsigned char b[32];

		for (i = 0; i < 8; i++)
			w[i] = intel_uncore_read(&dev_priv->uncore,
			                         HSW_TVIDEO_DIP_AVI_DATA(TRANSCODER_A, i));
		for (i = 0; i < 32; i++)
			b[i] = (unsigned char)((w[i / 4] >> ((i % 4) * 8)) & 0xff);
		/* Header is three bytes; an AVI InfoFrame's body is thirteen. */
		for (i = 0; i < 17; i++)
			sum += b[i];

		pr_info("i915-probe: AVI DIP %08x %08x %08x %08x %08x %08x %08x %08x\n",
		        w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
		pr_info("i915-probe: AVI type %02x ver %02x len %02x sum %02x "
		        "(0 = valid)\n", b[0], b[1], b[2], sum & 0xff);
	}

	/*
	 * The General Control Packet, and the mute bit inside it.
	 *
	 * VIDEO_DIP_CTL says GCP is being sent. GCP carries AV_MUTE, which is a
	 * direct instruction to the sink to blank — a source sets it while changing
	 * something and clears it afterwards, and one that never clears it leaves a
	 * monitor that locks onto a perfectly good signal and shows nothing. That
	 * is close enough to what this monitor does to be worth reading rather than
	 * assuming.
	 */
	{
		u32 gcp = intel_uncore_read(&dev_priv->uncore,
		                            HSW_TVIDEO_DIP_GCP(TRANSCODER_A));

		pr_info("i915-probe: GCP %08x (av_mute %d, default_phase %d, "
		        "color_indication %d)\n", gcp,
		        (int)((gcp & GCP_AV_MUTE) ? 1 : 0),
		        (int)((gcp & GCP_DEFAULT_PHASE_ENABLE) ? 1 : 0),
		        (int)((gcp & GCP_COLOR_INDICATION) ? 1 : 0));
	}

	/*
	 * What the hardware says about itself, rather than what the driver set.
	 *
	 * SFUSE_STRAP is the PCH's straps: which DDIs the board reports as present.
	 * It is not written by any driver, so after the reset vfio performs on
	 * handover it answers the one question no configuration register can — does
	 * this device still believe port C exists. SKL_DFSM is the display fuse: it
	 * says which pipes and how much of the display engine are enabled at all.
	 */
	{
		u32 strap = intel_uncore_read(&dev_priv->uncore, SFUSE_STRAP);
		u32 dfsm = intel_uncore_read(&dev_priv->uncore, SKL_DFSM);

		pr_info("i915-probe: SFUSE_STRAP %08x (DDI B %d, C %d, D %d) DFSM %08x\n",
		        strap,
		        (int)((strap & SFUSE_STRAP_DDIB_DETECTED) ? 1 : 0),
		        (int)((strap & SFUSE_STRAP_DDIC_DETECTED) ? 1 : 0),
		        (int)((strap & SFUSE_STRAP_DDID_DETECTED) ? 1 : 0),
		        dfsm);
	}

	/*
	 * The pipe's colour pipeline.
	 *
	 * A sink that locks onto the signal and shows black is a different fault from
	 * one that sees no signal, and this is where the first kind lives: gamma and
	 * CSC sit between the plane and the transcoder, and a lookup table left at
	 * zero maps every colour to black while every other register still says the
	 * picture is being scanned out. The plane's own gamma-enable bit is the one
	 * place our modeset differed from the host's.
	 *
	 * The palette is read through an index/data pair: writing the index with
	 * auto-increment set and reading the data register walks the table.
	 */
	{
		u32 gm = intel_uncore_read(&dev_priv->uncore, GAMMA_MODE(PIPE_A));
		u32 csc = intel_uncore_read(&dev_priv->uncore, PIPE_CSC_MODE(PIPE_A));
		u32 pc = intel_uncore_read(&dev_priv->uncore,
		                           PLANE_CTL(PIPE_A, PLANE_PRIMARY));
		u32 e0, emid, elast;

		/* Bit 15 of the index register is the auto-increment enable. */
		intel_uncore_write(&dev_priv->uncore, PREC_PAL_INDEX(PIPE_A),
		                   (1u << 15) | 0);
		e0 = intel_uncore_read(&dev_priv->uncore, PREC_PAL_DATA(PIPE_A));
		intel_uncore_write(&dev_priv->uncore, PREC_PAL_INDEX(PIPE_A),
		                   (1u << 15) | 128);
		emid = intel_uncore_read(&dev_priv->uncore, PREC_PAL_DATA(PIPE_A));
		intel_uncore_write(&dev_priv->uncore, PREC_PAL_INDEX(PIPE_A),
		                   (1u << 15) | 255);
		elast = intel_uncore_read(&dev_priv->uncore, PREC_PAL_DATA(PIPE_A));

		pr_info("i915-probe: GAMMA_MODE %08x CSC_MODE %08x PLANE_CTL %08x "
		        "(plane gamma %d)\n",
		        gm, csc, pc, (int)((pc & PLANE_CTL_PIPE_GAMMA_ENABLE) ? 1 : 0));
		pr_info("i915-probe: palette[0] %08x [128] %08x [255] %08x\n",
		        e0, emid, elast);
	}

	/*
	 * The power wells, which are the last thing between a configured port and
	 * a dark cable.
	 *
	 * A well that is down does not stop the registers from reading back what
	 * was written to them, and does not stop the pipe from running: the port
	 * looks configured and enabled from every register above, and the PHY it
	 * drives is simply unpowered, so nothing reaches the connector. That is
	 * indistinguishable from a working link until these two are read.
	 *
	 * Each well has a request bit the driver sets and a state bit the hardware
	 * answers with; they differ exactly when the hardware refused. PW_2 covers
	 * the DDIs on this generation, and each DDI's IO has a well of its own.
	 */
	{
		u32 wc1 = intel_uncore_read(&dev_priv->uncore, HSW_PWR_WELL_CTL1);
		u32 wc2 = intel_uncore_read(&dev_priv->uncore, HSW_PWR_WELL_CTL2);

		pr_info("i915-probe: PWR_WELL_CTL1 %08x CTL2 %08x\n", wc1, wc2);
		pr_info("i915-probe: PW_2 req %d state %d; DDI_C io req %d state %d\n",
		        (int)((wc2 & HSW_PWR_WELL_CTL_REQ(SKL_PW_CTL_IDX_PW_2)) ? 1 : 0),
		        (int)((wc2 & HSW_PWR_WELL_CTL_STATE(SKL_PW_CTL_IDX_PW_2)) ? 1 : 0),
		        (int)((wc2 & HSW_PWR_WELL_CTL_REQ(SKL_PW_CTL_IDX_DDI_C)) ? 1 : 0),
		        (int)((wc2 & HSW_PWR_WELL_CTL_STATE(SKL_PW_CTL_IDX_DDI_C)) ? 1 : 0));
	}

	pr_info("i915-probe: TRANS_DDI mode %u bpc %u, pipe underrun %d, "
	        "SDEISR %08x\n",
	        (unsigned)((func & TRANS_DDI_MODE_SELECT_MASK) >> 24),
	        (unsigned)((func & TRANS_DDI_BPC_MASK) >> 20),
	        (int)((iir & GEN8_PIPE_FIFO_UNDERRUN) ? 1 : 0), sdeisr);
	lkpi_i915_dump_gmbus(dev);
	intel_display_power_put(dev_priv, POWER_DOMAIN_DISPLAY_CORE, dump_wakeref);
}

/*
 * Watch the pipe's CRC while the frame is supposed to be standing still.
 *
 * The picture reaches the monitor when the CPU is halted and never when the
 * kernel keeps running, which points at the frame itself rather than at the
 * link: a scanout whose memory is reused underneath it stays perfectly
 * configured while the pixels turn to noise, and a sink that has locked onto it
 * gives up. The pipe computes a CRC over what it emits, once per frame, so a
 * static frame must produce a constant value. One that drifts says the
 * framebuffer is being written by something other than us.
 */
void lkpi_i915_crc_watch(struct drm_device *dev, unsigned seconds)
{
	struct drm_i915_private *dev_priv = to_i915_checked(dev);
	u32 first = 0, first_w2 = 0, first_buf = 0;
	enum port port = PORT_C;
	unsigned i;

	intel_uncore_write(&dev_priv->uncore, PIPE_CRC_CTL(PIPE_A),
	                   PIPE_CRC_ENABLE | PIPE_CRC_SOURCE_DMUX_SKL);
	intel_uncore_posting_read(&dev_priv->uncore, PIPE_CRC_CTL(PIPE_A));

	for (i = 0; i < seconds; i++) {
		u32 crc, start;
		unsigned spins;

		msleep(1000);
		/*
		 * Tied to a frame boundary, or the read returns whatever was latched
		 * last — including a value from a previous session, which is what made
		 * the first version of this report a constant while the picture on the
		 * monitor was plainly something else.
		 */
		start = intel_uncore_read(&dev_priv->uncore,
		                          PIPE_FRMCOUNT_G4X(PIPE_A));
		for (spins = 0; spins < 200; spins++) {
			if (intel_uncore_read(&dev_priv->uncore,
			                      PIPE_FRMCOUNT_G4X(PIPE_A)) != start)
				break;
			udelay(1000);
		}
		if (spins == 200) {
			pr_info("i915-probe: crc-watch t=%us pipe stopped advancing\n", i);
			break;
		}
		crc = intel_uncore_read(&dev_priv->uncore,
		                        PIPE_CRC_RES_1_IVB(PIPE_A));
		/*
		 * The power wells and the port buffer alongside the CRC, so a report
		 * that the picture changed says which half moved: the pixels being
		 * scanned, or the output carrying them. A deferred power-domain release
		 * — intel_display_power_put_async_work — would leave the frame intact
		 * and take the port down, and the CRC alone cannot tell that from a
		 * link that never came up.
		 */
		{
			u32 w2 = intel_uncore_read(&dev_priv->uncore, HSW_PWR_WELL_CTL2);
			u32 buf = intel_uncore_read(&dev_priv->uncore, DDI_BUF_CTL(port));

			if (i == 0) {
				first = crc;
				first_w2 = w2;
				first_buf = buf;
			}
			if ((i % 10) == 0 || crc != first || w2 != first_w2 ||
			    buf != first_buf)
				pr_info("i915-probe: crc-watch t=%us crc %08x wells %08x "
				        "buf %08x%s\n", i, crc, w2, buf,
				        (crc != first || w2 != first_w2 ||
				         buf != first_buf) ? "  CHANGED" : "");
		}
	}
	intel_uncore_write(&dev_priv->uncore, PIPE_CRC_CTL(PIPE_A), 0);
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
static int i915_gem_page_phys(struct drm_vma_offset_node *node, u64 index,
                              u64 *out_phys)
{
	/* The node belongs to a struct i915_mmap_offset — one per object per
	 * mapping type — and only points at the object. Treating it as the object
	 * itself reads whatever happens to follow it in memory. */
	struct i915_mmap_offset *mmo;
	struct drm_i915_gem_object *bo;
	dma_addr_t addr;
	int ret;

	if (!node || !out_phys)
		return -EINVAL;
	mmo = container_of(node, struct i915_mmap_offset, vma_node);
	bo = mmo->obj;
	if (!bo)
		return -EINVAL;

	if (index >= (u64)(bo->base.size >> PAGE_SHIFT))
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
	 * Re-run the raw-clock setup before anything uses the bus.
	 *
	 * PCH_RAWCLK_FREQ reads zero on this machine although the driver believes
	 * the reference is 24 MHz: the value was computed during device-info init
	 * and did not reach the register. GMBUS derives its timing from that clock,
	 * so with the register at zero the controller accepts a transfer, never
	 * completes it, and every EDID read falls back to bit-banging. Running the
	 * driver's own routine again programs it from the strap, exactly as it
	 * would have been.
	 */
	{
		struct drm_i915_private *i915 = to_i915_checked(dev);

		if (i915) {
			u32 before = intel_uncore_read(&i915->uncore, PCH_RAWCLK_FREQ);
			u32 freq = intel_read_rawclk(i915);
			u32 after = intel_uncore_read(&i915->uncore, PCH_RAWCLK_FREQ);

			pr_info("i915-probe: rawclk %u kHz, PCH_RAWCLK_FREQ %x -> %x\n",
			        (unsigned)freq, (unsigned)before, (unsigned)after);
			if (after == 0)
				pr_info("i915: GMBUS has no reference clock — PCH_RAWCLK_FREQ "
				        "reads 0 and does not accept writes, so the controller "
				        "can never complete a transfer. Reading a display's "
				        "EDID over the wire will not work here. Seen under "
				        "QEMU's legacy IGD passthrough, where part of the PCH "
				        "register block is not forwarded to the guest.\n");
		}
	}

	lkpi_i915_gmbus_recover(dev);

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
