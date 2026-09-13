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
#include <drm/drm_atomic_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include "display/intel_display_types.h"
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

void x86_watchpoint_write(u64 addr);

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
/*
 * When does the surface the display reads actually change?
 *
 * Everything the software side can say looked healthy while the picture tore:
 * the swapchain alternates, the commits are non-async, no commit overruns its
 * vblank-evasion window. So ask the display engine instead. PLANE_SURF reads
 * the ACTIVE value -- what the scanout is using now -- and PIPEDSL is the line
 * the beam is on. Sampling both fast and recording the line whenever the
 * surface changes says exactly one thing, and it is the definition of
 * tearing: a surface that changes while the beam is inside the visible area
 * is a frame made of two buffers.
 *
 * b1nix.drm-tearwatch, on the pipe that is actually on.
 */
void lkpi_i915_dump_vblank_state(struct drm_device *dev);

static struct drm_device *tearwatch_i915;

/* The active pipe's visible height, from the mode it is running -- not a
 * constant. A sampler that hardcodes 1080 is wrong on every other mode, and
 * the number is two dereferences away. */
/* The pipe that is actually driving the panel.
 *
 * Reading PIPE_A because it is the first one is how a sampler ends up
 * watching an idle plane: i915 has three pipes and the compositor's CRTC
 * need not be the first. An address that never changes is exactly what an
 * unused plane looks like. */
static int tearwatch_pipe(void)
{
	struct drm_crtc *c;

	if (!tearwatch_i915)
		return -1;
	drm_for_each_crtc(c, tearwatch_i915) {
		if (!c->state || !c->state->active)
			continue;
		return (int)to_intel_crtc(c)->pipe;
	}
	return -1;
}

static int i915_framedump_thread(void *arg);
static void *fd_map_ggtt(struct i915_ggtt *ggtt, u32 ggtt_addr);

/*
 * Read scanout memory the way the display sees it.
 *
 * The compositor writes its frames through a write-combining mapping, so its
 * stores land in memory without ever passing through the cache this direct-map
 * read hits. A plain load can therefore return a line cached from an earlier
 * look and report a buffer as unchanged while it is being repainted -- the
 * exact answer that would make a paint-during-scanout invisible to a detector
 * built to find it. Flushing the line first and fencing makes the read come
 * from memory.
 */
static u32 fd_read32(const volatile u32 *p)
{
	asm volatile("clflush (%0)" :: "r"(p) : "memory");
	asm volatile("mfence" ::: "memory");
	return *p;
}

/* Hash the surface the display is reading, every page of it: the earlier
 * sixteen-page sample could miss a repaint confined to one window. */
static u64 tearwatch_hash_live(struct drm_i915_private *i915, u32 surf,
                               u32 bytes, unsigned phase)
{
	struct i915_ggtt *ggtt = to_gt(i915)->ggtt;
	u64 h = 1469598103934665603ull;
	u32 off;

	if (!ggtt || !ggtt->gsm || !bytes)
		return 0;
	/*
	 * Eight dwords per page, but not the same eight every time.
	 *
	 * A fixed stride of 512 bytes always lands on the same fifteen columns
	 * of a 1920-pixel line, so anything narrower than that -- a caret, a
	 * cursor, a window border -- can be repainted under the display for a
	 * whole session without ever touching a sampled byte. The phase walks
	 * the offsets across the page from one look to the next, so over a run
	 * every column is covered while each single look stays cheap enough to
	 * fit inside one frame.
	 */
	for (off = 0; off < bytes; off += 4096) {
		const volatile u32 *p = fd_map_ggtt(ggtt, surf + off);
		unsigned i;

		if (!p)
			continue;
		for (i = 0; i < 8; i++)
			h = (h ^ fd_read32(&p[(i * 128 + phase * 16) % 1024])) *
			    1099511628211ull;
	}
	return h;
}

static u32 tearwatch_vdisplay(void)
{
	struct drm_crtc *c;

	if (!tearwatch_i915)
		return 0;
	drm_for_each_crtc(c, tearwatch_i915) {
		if (!c->state || !c->state->active)
			continue;
		return (u32)to_intel_crtc_state(c->state)->hw.adjusted_mode.crtc_vdisplay;
	}
	return 0;
}

static int i915_tearwatch_thread(void *arg)
{
	struct drm_i915_private *i915 = to_i915_checked(tearwatch_i915);
	struct drm_i915_private *dev_priv = i915; /* PIPEDSL() names it */
	u32 last_live = 0;
	int have_last = 0;
	u64 in_active = 0, in_blank = 0, changes = 0, straddle = 0, loose = 0;
	/* Where in the frame the latch fell: the top of the picture, its middle,
	 * the last few lines (harmless -- the beam is about to leave the visible
	 * area anyway) and the blanking interval (correct). A latch in the first
	 * two bands is a seam a person sees. */
	u64 band_top = 0, band_mid = 0, band_tail = 0;
	/* The end of the previous sample, so a change can be bounded in time as
	 * well as in place: a tight bracket says where WE looked, not when the
	 * hardware latched. Only when the gap since the last look is also tight
	 * does the pair of scanlines pin the latch down. */
	u32 prev_dsl = 0xffffffff;
	u64 gap_wide = 0;
	u64 painted_checks = 0, painted_while_shown = 0;
	unsigned phase = 0;
	u64 samples = 0;
	u32 worst_line = 0, min_line = 0xffffffff;
	u64 reports = 0;

	(void)arg;
	if (!i915)
		return 0;

	/*
	 * The cadence: how many frames each presented image is held for.
	 *
	 * A compositor drawing 22 images a second onto a 60 Hz panel shows each
	 * for two or three frames. Which of the two, frame after frame, is what
	 * motion looks like: a steady 3,3,3 reads as smooth, and 2,4,2,4 or
	 * 1,5,2,3 reads as text that jumps and smears -- the thing a person calls
	 * tearing when scrolling. Nothing measured so far describes it, because
	 * every other instrument asks whether a frame is whole, not when it
	 * arrives.
	 *
	 * b1nix.drm-cadence counts the frames between one latched surface and the
	 * next.
	 */
	if (lkpi_bootflag("b1nix.drm-cadence")) {
		u64 hist[12];
		u64 flips = 0, reports = 0;
		u32 prev_dsl = 0xffffffff, held = 0, last_surf = 0;
		u64 last_frame_ns = 0, min_ns = 0, max_ns = 0, sum_ns = 0, intervals = 0;
		unsigned i;

		for (i = 0; i < 12; i++)
			hist[i] = 0;
		for (;;) {
			int pipe = tearwatch_pipe();
			u32 dsl, surf;

			if (pipe < 0) {
				lkpi_sleep_ms(200);
				continue;
			}
			dsl = intel_uncore_read_fw(&i915->uncore,
			                           PIPEDSL(pipe)) & 0x1fffff;
			surf = intel_uncore_read_fw(&i915->uncore,
			                            PLANE_SURFLIVE(pipe, PLANE_PRIMARY));
			/* One count per frame: the scanline counter wrapping is the
			 * frame boundary. */
			if (prev_dsl != 0xffffffff && dsl < prev_dsl) {
				/* The panel's own beat, as the kernel sees it.
				 *
				 * An uneven cadence has two possible owners: a compositor
				 * that schedules badly, or a kernel whose idea of when the
				 * frame ends wobbles. The interval between frame boundaries
				 * separates them -- a steady 16.6 ms says the beat is fine
				 * and the unevenness is above it. */
				u64 now = lkpi_monotonic_ns();

				if (last_frame_ns) {
					u64 d = now - last_frame_ns;

					if (!min_ns || d < min_ns)
						min_ns = d;
					if (d > max_ns)
						max_ns = d;
					sum_ns += d;
					intervals++;
				}
				last_frame_ns = now;
				held++;
				if (surf != last_surf) {
					if (last_surf) {
						flips++;
						hist[held < 12 ? held : 11]++;
					}
					last_surf = surf;
					held = 0;
				}
			}
			prev_dsl = dsl;
			/* Give the CPU up between looks: a scanline poll that never
			 * yields competes with the compositor it is measuring. One
			 * jiffy is far shorter than a frame. */
			if ((dsl & 0x3f) == 0)
				lkpi_sleep_ms(1);

			if (flips && (flips % 200) == 0 && reports != flips) {
				reports = flips;
				pr_info("i915: cadence: %llu presented image(s); frames each "
				        "was held: 1x%llu 2x%llu 3x%llu 4x%llu 5x%llu "
				        "6+x%llu; frame interval min %llu us avg %llu us "
				        "max %llu us\n", (unsigned long long)flips,
				        (unsigned long long)hist[1],
				        (unsigned long long)hist[2],
				        (unsigned long long)hist[3],
				        (unsigned long long)hist[4],
				        (unsigned long long)hist[5],
				        (unsigned long long)(hist[6] + hist[7] + hist[8] +
				                             hist[9] + hist[10] + hist[11]),
				        (unsigned long long)(min_ns / 1000),
				        (unsigned long long)(intervals ? sum_ns / intervals /
				                             1000 : 0),
				        (unsigned long long)(max_ns / 1000));
			}
		}
	}

	/*
	 * When the GGTT entry under the scanned address is rewritten.
	 *
	 * The plane's address can stay put while the page it translates to is
	 * replaced: i915 binds a framebuffer into the GGTT when it is used, and
	 * two framebuffers can be given the same address in turn. A flip done
	 * that way never changes PLANE_SURF, so every check on the flip path
	 * calls it perfect -- and if the rewrite lands while the beam is inside
	 * the picture, the monitor is sent the top of one buffer and the bottom
	 * of another. That is a tear with no flip involved, which is why nothing
	 * watching flips could see it.
	 *
	 * b1nix.drm-bindwatch: sample the entry and the scanline together, the
	 * same way the surface watch samples the plane.
	 */
	if (lkpi_bootflag("b1nix.drm-bindwatch")) {
		struct i915_ggtt *ggtt = to_gt(i915)->ggtt;
		u64 prev = 0, changes = 0, in_active = 0, in_blank = 0, loose = 0;
		u64 samples = 0, reports = 0;
		u32 worst = 0, prev_dsl = 0xffffffff;
		u32 armed_idx = 0xffffffff;
		unsigned armings = 0;
		int self_tested = 0;
		u64 seen_active = 0, seen_blank = 0;
		unsigned detail = 0;
		int have_prev = 0;

		if (!ggtt || !ggtt->gsm)
			return 0;
		for (;;) {
			int pipe = tearwatch_pipe();
			u32 vdisplay = tearwatch_vdisplay();
			u32 surf, idx;
			unsigned spins;

			if (pipe < 0 || !vdisplay) {
				lkpi_sleep_ms(200);
				continue;
			}
			surf = intel_uncore_read_fw(&i915->uncore,
			                            PLANE_SURFLIVE(pipe, PLANE_PRIMARY));
			idx = surf >> 12;
			/*
			 * Name the writer instead of inferring it.
			 *
			 * b1nix.drm-gsmtrap puts a hardware data breakpoint on the entry
			 * this loop watches, so the next store to it reports the
			 * instruction that made it. Re-armed whenever the scanned address
			 * moves, because the interesting entry is whichever one the
			 * display is reading.
			 */
			if (lkpi_bootflag("b1nix.drm-gsmtrap") && idx != armed_idx) {
				u64 at = (u64)(uintptr_t)&((u64 __iomem *)ggtt->gsm)[idx];

				armed_idx = idx;
				x86_watchpoint_write(at);
				/*
				 * Prove the trap works before trusting its silence.
				 *
				 * The watchpoint reported nothing while another instrument
				 * saw the same entry change, and "nobody wrote it" and "the
				 * trap does not fire here" look identical from the log. So
				 * write the entry's own value back once: a trap that works
				 * reports this store, and one that does not says the silence
				 * means nothing.
				 */
				if (!self_tested) {
					self_tested = 1;
					lkpi_sleep_ms(20);	/* let every CPU arm on its tick */
					writeq(readq(&((u64 __iomem *)ggtt->gsm)[idx]),
					       &((u64 __iomem *)ggtt->gsm)[idx]);
					pr_info("i915: bindwatch: self-test store done\n");
				}
				if (armings < 6) {
					armings++;
					pr_info("i915: bindwatch: watchpoint armed on entry %u "
					        "(%llx)\n", idx, (unsigned long long)at);
				}
			}
			{
				unsigned long irqflags;

				local_irq_save(irqflags);
				for (spins = 0; spins < 4000; spins++) {
					u32 dsl_before = intel_uncore_read_fw(&i915->uncore,
					                     PIPEDSL(pipe)) & 0x1fffff;
					u64 pte = readq(&((u64 __iomem *)ggtt->gsm)[idx]);
					u32 dsl_after = intel_uncore_read_fw(&i915->uncore,
					                    PIPEDSL(pipe)) & 0x1fffff;

					samples++;
					if (!have_prev) {
						prev = pte;
						have_prev = 1;
						prev_dsl = dsl_after;
						continue;
					}
					if (pte != prev) {
						changes++;
						/* The first few in full: which entry, what it held,
						 * what replaced it, and what the plane was armed
						 * with at that moment. If the new page is the one
						 * the next flip is about to show, then a buffer is
						 * being bound over the address that is still being
						 * scanned. */
						if (detail < 8) {
							detail++;
							pr_info("i915: bindwatch: entry %u at line %u: "
							        "%llx -> %llx, plane armed %08x live "
							        "%08x\n", idx, dsl_after,
							        (unsigned long long)prev,
							        (unsigned long long)pte,
							        intel_uncore_read_fw(&i915->uncore,
							            PLANE_SURF(pipe, PLANE_PRIMARY)),
							        intel_uncore_read_fw(&i915->uncore,
							            PLANE_SURFLIVE(pipe, PLANE_PRIMARY)));
						}
						/* Where it was seen, tight or not: the timing rule
						 * below can only reject, and a rejected change still
						 * happened somewhere. */
						if (dsl_after < vdisplay)
							seen_active++;
						else
							seen_blank++;
						/* Only judge a change that is bracketed tightly and
						 * followed closely on the previous look, exactly as
						 * the surface watch does. */
						/* A change at the top of a frame follows a look
						 * taken in the blanking of the one before, so the
						 * scanline counter has wrapped between them. Treating
						 * a wrap as an untimed gap threw away exactly the
						 * changes that land where a flip does. */
						if (dsl_after >= dsl_before &&
						    dsl_after - dsl_before < 20u &&
						    (prev_dsl == 0xffffffff ||
						     (dsl_before >= prev_dsl &&
						      dsl_before - prev_dsl < 20u) ||
						     (dsl_before < prev_dsl && prev_dsl >= vdisplay &&
						      dsl_before < 20u))) {
							if (dsl_after < vdisplay) {
								in_active++;
								if (dsl_after > worst)
									worst = dsl_after;
							} else {
								in_blank++;
							}
						} else {
							loose++;
						}
						prev = pte;
					}
					prev_dsl = dsl_after;
				}
				local_irq_restore(irqflags);
			}
			if (++reports >= 20) {
				reports = 0;
				pr_info("i915: bindwatch: %llu sample(s), %llu change(s) of "
				        "the GGTT entry under the scanned address: %llu in "
				        "blanking, %llu WHILE THE PICTURE WAS BEING SCANNED "
				        "(worst line %u), %llu too loosely timed to judge; "
				        "seen at all: %llu inside the picture, %llu in "
				        "blanking\n",
				        (unsigned long long)samples,
				        (unsigned long long)changes,
				        (unsigned long long)in_blank,
				        (unsigned long long)in_active, worst,
				        (unsigned long long)loose,
				        (unsigned long long)seen_active,
				        (unsigned long long)seen_blank);
			}
			lkpi_sleep_ms(1);
		}
	}

	/*
	 * Within one frame.
	 *
	 * Two hardware readings disagree and only one of them can be right: the
	 * CRC says consecutive frames scanned from the SAME address differ, and
	 * the paint watch says that buffer's memory never changes while it is on
	 * screen. The paint watch compares two looks four milliseconds apart,
	 * which is a quarter of a frame and can fall either side of a repaint;
	 * this compares the top of a frame with the bottom of the SAME frame,
	 * with the surface address checked before and after. If those differ,
	 * the picture the monitor received was made of two moments, and that is
	 * the tear itself rather than an inference about it.
	 *
	 * b1nix.drm-scanwatch.
	 */
	if (lkpi_bootflag("b1nix.drm-scanwatch")) {
		u64 checks = 0, torn = 0, reports = 0;

		for (;;) {
			int pipe = tearwatch_pipe();
			u32 vdisplay = tearwatch_vdisplay();
			struct i915_ggtt *ggtt = to_gt(i915)->ggtt;
			u32 stride, surf_a, surf_b, dsl, prev_dsl = 0xffffffff;
			u64 h1 = 1469598103934665603ull, h2 = 1469598103934665603ull;
			u32 y;

			if (pipe < 0 || !vdisplay) {
				lkpi_sleep_ms(200);
				continue;
			}
			stride = (intel_uncore_read_fw(&i915->uncore,
			                               PLANE_STRIDE(pipe, PLANE_PRIMARY))
			          & 0x3ff) * 64u;
			if (!stride)
				continue;
			for (;;) {
				dsl = intel_uncore_read_fw(&i915->uncore,
				                           PIPEDSL(pipe)) & 0x1fffff;
				if (prev_dsl != 0xffffffff && dsl < prev_dsl)
					break;
				prev_dsl = dsl;
			}
			surf_a = intel_uncore_read_fw(&i915->uncore,
			                              PLANE_SURFLIVE(pipe, PLANE_PRIMARY));
			/* Coarse on purpose: both looks have to fit inside one frame,
			 * so this samples one dword every four cache lines. */
			for (y = 0; y < vdisplay; y += 8) {
				u32 x;

				for (x = 0; x < stride; x += 256) {
					const volatile u32 *p =
					    fd_map_ggtt(ggtt, surf_a + y * stride + x);

					if (p)
						h1 = (h1 ^ fd_read32(p)) * 1099511628211ull;
				}
			}
			/* Wait until the beam is near the bottom, then look again. */
			while ((intel_uncore_read_fw(&i915->uncore,
			                             PIPEDSL(pipe)) & 0x1fffff) <
			       vdisplay - 32u)
				;
			for (y = 0; y < vdisplay; y += 8) {
				u32 x;

				for (x = 0; x < stride; x += 256) {
					const volatile u32 *p =
					    fd_map_ggtt(ggtt, surf_a + y * stride + x);

					if (p)
						h2 = (h2 ^ fd_read32(p)) * 1099511628211ull;
				}
			}
			surf_b = intel_uncore_read_fw(&i915->uncore,
			                              PLANE_SURFLIVE(pipe, PLANE_PRIMARY));
			if (surf_a != surf_b)
				continue;	/* a flip landed: not one frame */
			checks++;
			if (h1 != h2)
				torn++;
			if (++reports >= 200) {
				reports = 0;
				pr_info("i915: scanwatch: %llu frame(s) read top and bottom "
				        "with the same buffer on screen, %llu changed "
				        "between the two looks\n",
				        (unsigned long long)checks,
				        (unsigned long long)torn);
			}
		}
	}

	/*
	 * Band mode.
	 *
	 * "How many frames differ" says a picture is changing but not HOW. A
	 * whole new frame changes every part of the screen at once; a frame
	 * assembled from two states changes some horizontal bands and leaves the
	 * rest holding the older picture, which is exactly what a seam is. So
	 * hash the buffer the display is reading in sixteen bands, once per
	 * frame, and count how many bands changed from one frame to the next.
	 * All sixteen, or none, is a clean presentation. Anything in between is
	 * a frame that shows two different moments at once.
	 *
	 * b1nix.drm-bandwatch.
	 */
	if (lkpi_bootflag("b1nix.drm-bandwatch")) {
		u64 prev[16];
		u64 frames = 0, clean = 0, still = 0, split = 0, reports = 0;
		u64 worst = 0;
		u64 hist[17];
		int have_prev = 0;

		for (frames = 0; frames < 17; frames++)
			hist[frames] = 0;
		frames = 0;

		for (;;) {
			int pipe = tearwatch_pipe();
			u32 vdisplay = tearwatch_vdisplay();
			struct i915_ggtt *ggtt = to_gt(i915)->ggtt;
			u32 stride, surf, dsl, prev_dsl = 0xffffffff;
			u64 cur[16];
			unsigned b, changed = 0;

			if (pipe < 0 || !vdisplay) {
				lkpi_sleep_ms(200);
				continue;
			}
			stride = (intel_uncore_read_fw(&i915->uncore,
			                               PLANE_STRIDE(pipe, PLANE_PRIMARY))
			          & 0x3ff) * 64u;
			if (!stride)
				continue;
			/* Start of a frame, so the whole hash is taken from one
			 * scanout rather than across a flip. */
			for (;;) {
				dsl = intel_uncore_read_fw(&i915->uncore,
				                           PIPEDSL(pipe)) & 0x1fffff;
				if (prev_dsl != 0xffffffff && dsl < prev_dsl)
					break;
				prev_dsl = dsl;
			}
			surf = intel_uncore_read_fw(&i915->uncore,
			                            PLANE_SURFLIVE(pipe, PLANE_PRIMARY));
			for (b = 0; b < 16; b++) {
				u32 y0 = (vdisplay * b) / 16u;
				u32 y1 = (vdisplay * (b + 1)) / 16u;
				u64 h = 1469598103934665603ull;
				u32 y;

				/* The band is the FULL width of the picture.
				 *
				 * Eight dwords spaced 400 bytes apart covered the first 2800
				 * bytes of a 7680-byte scanline, so the right two thirds of
				 * the screen were never hashed and a seam over there was
				 * invisible by construction. One dword per cache line spans
				 * the whole row instead, and it costs no more memory traffic:
				 * the flush that makes the read honest already pulls the
				 * whole line. The band edges are scaled rather than stepped
				 * so the last lines belong to the last band instead of
				 * falling outside every one. */
				for (y = y0; y < y1; y += 4) {
					u32 x;

					for (x = 0; x < stride; x += 64) {
						const volatile u32 *p =
						    fd_map_ggtt(ggtt, surf + y * stride + x);

						if (!p)
							continue;
						h = (h ^ fd_read32(p)) * 1099511628211ull;
					}
				}
				cur[b] = h;
			}
			if (surf != intel_uncore_read_fw(&i915->uncore,
			                                 PLANE_SURFLIVE(pipe,
			                                     PLANE_PRIMARY)))
				continue;	/* flipped while reading: not one frame */
			if (have_prev) {
				for (b = 0; b < 16; b++)
					if (cur[b] != prev[b])
						changed++;
				frames++;
				if (changed == 0)
					still++;
				else if (changed == 16)
					clean++;
				else {
					split++;
					if (changed > worst)
						worst = changed;
				}
				hist[changed]++;
			}
			for (b = 0; b < 16; b++)
				prev[b] = cur[b];
			have_prev = 1;

			if (++reports >= 300) {
				unsigned pl;

				reports = 0;
				/*
				 * Which planes the pipe is actually blending.
				 *
				 * The band hashes and the paint watch only ever look at the
				 * primary plane's buffer, so a picture that changes every
				 * frame with that buffer still could be coming from a sprite
				 * or the cursor. Naming the enabled planes says whether
				 * there is anywhere else for it to come from.
				 */
				for (pl = 0; pl < 4; pl++) {
					u32 ctl = intel_uncore_read_fw(&i915->uncore,
					                               PLANE_CTL(pipe, pl));

					if (!(ctl & PLANE_CTL_ENABLE))
						continue;
					pr_info("i915: bandwatch:   plane %u on, ctl %08x, "
					        "surface %08x\n", pl, ctl,
					        intel_uncore_read_fw(&i915->uncore,
					                             PLANE_SURFLIVE(pipe, pl)));
				}
				pr_info("i915: bandwatch:   cursor ctl %08x pos %08x "
				        "base %08x\n",
				        intel_uncore_read_fw(&i915->uncore, CURCNTR(pipe)),
				        intel_uncore_read_fw(&i915->uncore, CURPOS(pipe)),
				        intel_uncore_read_fw(&i915->uncore, CURBASE(pipe)));
				pr_info("i915: bandwatch: %llu frame(s): %llu unchanged, "
				        "%llu changed everywhere, %llu changed in PART of "
				        "the picture (worst %llu of 16 bands)\n",
				        (unsigned long long)frames, (unsigned long long)still,
				        (unsigned long long)clean, (unsigned long long)split,
				        (unsigned long long)worst);
				/* How many bands moved at once.
				 *
				 * A window redrawn whole moves the same number of bands
				 * every time -- the ones it covers. A count BELOW that,
				 * appearing between two of them, is a frame that caught the
				 * redraw half done, and that is the seam. */
				for (b = 1; b <= 16; b++)
					if (hist[b])
						pr_info("i915: bandwatch:   %u band(s) at once: "
						        "%llu frame(s)\n", b,
						        (unsigned long long)hist[b]);
			}
		}
	}

	/*
	 * Content-only mode.
	 *
	 * The scanline sampler spends nearly all of its time spinning on
	 * registers, so the content check runs for four milliseconds out of every
	 * fifty and a repaint of the visible buffer is caught at that duty cycle
	 * or not at all -- which turns a real rate into a rumour. With
	 * b1nix.drm-paintwatch the loop does nothing but the content check, back
	 * to back, and the count it reports is the rate.
	 */
	if (lkpi_bootflag("b1nix.drm-paintwatch")) {
		u64 checks = 0, hits = 0, secs = 0;
		unsigned phase = 0;

		for (;;) {
			int pipe = tearwatch_pipe();
			u32 vdisplay = tearwatch_vdisplay();
			u32 stride, surf_a, surf_b, bytes;
			u64 h1, h2;

			if (pipe < 0 || !vdisplay) {
				lkpi_sleep_ms(200);
				continue;
			}
			stride = intel_uncore_read_fw(&i915->uncore,
			                              PLANE_STRIDE(pipe, PLANE_PRIMARY));
			bytes = (stride & 0x3ff) * 64u * vdisplay;
			surf_a = intel_uncore_read_fw(&i915->uncore,
			                              PLANE_SURFLIVE(pipe, PLANE_PRIMARY));
			h1 = tearwatch_hash_live(i915, surf_a, bytes, phase);
			h2 = tearwatch_hash_live(i915, surf_a, bytes, phase);
			phase = (phase + 1) % 64u;
			surf_b = intel_uncore_read_fw(&i915->uncore,
			                              PLANE_SURFLIVE(pipe, PLANE_PRIMARY));
			if (surf_a == surf_b && h1 && h2) {
				checks++;
				if (h1 != h2)
					hits++;
			}
			if (++secs >= 200) {
				secs = 0;
				pr_info("i915: paintwatch: %llu look(s) at the buffer the "
				        "display was reading, %llu found it changing under "
				        "the display\n", (unsigned long long)checks,
				        (unsigned long long)hits);
			}
		}
	}

	for (;;) {
		u32 live, dsl_before, dsl_after;
		unsigned spins;
		u32 vdisplay;
		enum pipe pipe;

		/*
		 * The live surface, not the armed one.
		 *
		 * PLANE_SURF reads back what was WRITTEN: the driver arms a flip
		 * there and the hardware latches it at the vertical blank, so a
		 * change seen in it says only when the driver wrote, which is
		 * deliberately away from the blank and proves nothing. PLANE_SURFLIVE
		 * is the address the display engine is reading right now. A change in
		 * THAT while the beam is inside the visible area is a latch mid-frame,
		 * and that is tearing -- there is no other reading of it.
		 *
		 * Bracketed by two scanline reads: a change detected between two
		 * samples happened somewhere between them, so only a bracket that
		 * lies entirely inside the visible area proves where it happened.
		 */
		vdisplay = tearwatch_vdisplay();
		if (!vdisplay) {
			lkpi_sleep_ms(20);
			continue;
		}

		{
			int p = tearwatch_pipe();

			if (p < 0) {
				lkpi_sleep_ms(20);
				continue;
			}
			pipe = (enum pipe)p;
		}
		/*
		 * Sample in a burst with interrupts off.
		 *
		 * The question is WHEN the hardware latched, and the only way this
		 * thread can bound that is by looking often enough that two
		 * consecutive looks are microseconds apart. Preempted, it looks once
		 * a frame and can say nothing -- which is what "not timed" counted.
		 * A millisecond of uninterrupted sampling per iteration is a
		 * sixteenth of a frame, costs one CPU a fraction of its time, and
		 * holds no lock while it does it.
		 */
		{
			u64 irqflags;

			local_irq_save(irqflags);
		for (spins = 0; spins < 4000; spins++) {
			dsl_before = intel_uncore_read_fw(&i915->uncore,
			                                  PIPEDSL(pipe)) & 0x1fffff;
			live = intel_uncore_read_fw(&i915->uncore,
			                            PLANE_SURFLIVE(pipe, PLANE_PRIMARY));
			dsl_after = intel_uncore_read_fw(&i915->uncore,
			                                 PIPEDSL(pipe)) & 0x1fffff;
			samples++;
			if (!have_last) {
				/* The first reading is a starting point, not a change. */
				last_live = live;
				have_last = 1;
				continue;
			}
			if (live == last_live) {
				prev_dsl = dsl_after;
				continue;
			}
			last_live = live;
			changes++;
			{
				u32 gap = (prev_dsl == 0xffffffff || dsl_before < prev_dsl)
				              ? 0xffffffff
				              : dsl_before - prev_dsl;

				prev_dsl = dsl_after;
				if (gap > 20u) {
					/* The last look was too long ago: the latch could have
					 * happened anywhere since, including in the blanking. */
					gap_wide++;
					continue;
				}
			}
			/* Only a TIGHT bracket is evidence.
			 *
			 * The two scanline reads are microseconds apart when nothing
			 * interrupts this thread, and a whole frame apart when something
			 * does -- and a bracket that spans most of the frame cannot say
			 * which frame either reading belongs to, let alone where the
			 * latch fell. Twenty lines is about 0.3 ms at 60 Hz, far longer
			 * than three MMIO reads and far shorter than a frame. */
			if (dsl_after < dsl_before || dsl_after - dsl_before > 20) {
				loose++;
				continue;
			}
			if (dsl_before < vdisplay && dsl_after < vdisplay) {
				in_active++;
				if (dsl_after >= vdisplay - 8)
					band_tail++;
				else if (dsl_after < vdisplay / 2)
					band_top++;
				else
					band_mid++;
				if (dsl_before < min_line)
					min_line = dsl_before;
				if (dsl_after > worst_line)
					worst_line = dsl_after;
			} else if (dsl_before >= vdisplay && dsl_after >= vdisplay) {
				in_blank++;
			} else {
				/* The bracket crosses the boundary: it cannot say which
				 * side the latch fell on, and counting it either way would
				 * be inventing the answer. */
				straddle++;
			}
		}

			local_irq_restore(irqflags);
		}

		/*
		 * And the content of the buffer that is on screen, twice inside one
		 * frame. Between the two reads the display is still reading it: a
		 * change means the compositor painted the visible buffer, which is
		 * the one way left for a picture to tear when the flip path is
		 * right.
		 */
		{
			u32 stride = intel_uncore_read_fw(&i915->uncore,
			                                  PLANE_STRIDE(pipe,
			                                      PLANE_PRIMARY));
			u32 surf_a = intel_uncore_read_fw(&i915->uncore,
			                                  PLANE_SURFLIVE(pipe,
			                                      PLANE_PRIMARY));
			u32 bytes = (stride & 0x3ff) * 64u * vdisplay;
			u64 h1 = tearwatch_hash_live(i915, surf_a, bytes, phase);
			u32 surf_b;
			u64 h2;

			lkpi_sleep_ms(4);
			h2 = tearwatch_hash_live(i915, surf_a, bytes, phase);
			phase = (phase + 1) % 64u;
			surf_b = intel_uncore_read_fw(&i915->uncore,
			                              PLANE_SURFLIVE(pipe,
			                                  PLANE_PRIMARY));
			/* Only when the same buffer was on screen for both reads:
			 * a flip in between makes the comparison meaningless. */
			if (surf_a == surf_b && h1 && h2) {
				painted_checks++;
				if (h1 != h2)
					painted_while_shown++;
			}
		}

		/*
		 * No dump inside the loop.
		 *
		 * The vblank-state dump this used to call delays fifty milliseconds
		 * -- three frames -- and a latch that happens in that hole is
		 * attributed to whatever line the beam has reached by the next
		 * sample, which biases every count towards "mid-frame". The state it
		 * printed is available from the same thread's own numbers.
		 */
		if (++reports >= 20) {
			reports = 0;
			pr_info("i915: tearwatch: %llu sample(s), %llu change(s): %llu "
			        "in blanking, %llu in ACTIVE SCANOUT (lines %u..%u), %llu "
			        "across the boundary, %llu dropped for a bracket too "
			        "wide to judge\n",
			        (unsigned long long)samples, (unsigned long long)changes,
			        (unsigned long long)in_blank, (unsigned long long)in_active,
			        min_line == 0xffffffff ? 0 : min_line, worst_line,
			        (unsigned long long)straddle, (unsigned long long)loose);
			if (gap_wide)
				pr_info("i915: tearwatch: %llu change(s) not timed: the "
				        "previous look was too long ago to say when the "
				        "latch fell\n", (unsigned long long)gap_wide);
			gap_wide = 0;
			if (painted_checks)
				pr_info("i915: tearwatch: the on-screen buffer was read twice "
				        "%llu time(s) with no flip between; its content had "
				        "changed under the display %llu of those\n",
				        (unsigned long long)painted_checks,
				        (unsigned long long)painted_while_shown);
			painted_checks = painted_while_shown = 0;
			if (in_active)
				pr_info("i915: tearwatch: of those, %llu in the top half, "
				        "%llu in the bottom half, %llu in the last eight "
				        "lines\n", (unsigned long long)band_top,
				        (unsigned long long)band_mid,
				        (unsigned long long)band_tail);
			band_top = band_mid = band_tail = 0;
			/* And what the register actually holds. Zero, or a value that
			 * never moves while the compositor flips, means this is not the
			 * live address on this hardware and every count above is a count
			 * of nothing. */
			pr_info("i915: tearwatch: pipe %c SURFLIVE %08x, SURF %08x, "
			        "vdisplay %u\n", 'A' + (int)pipe,
			        intel_uncore_read_fw(&i915->uncore,
			                             PLANE_SURFLIVE(pipe, PLANE_PRIMARY)),
			        intel_uncore_read_fw(&i915->uncore,
			                             PLANE_SURF(pipe, PLANE_PRIMARY)),
			        vdisplay);
			{
				/* Which framebuffer the plane is supposed to be showing, and
				 * where that framebuffer actually lives in the graphics
				 * address space. An id that alternates while the address
				 * stands still says the two buffers were bound to the same
				 * place -- and then a flip changes nothing, the display keeps
				 * reading the memory the compositor is drawing into, and the
				 * picture tears on every change. */
				struct drm_crtc *c;

				drm_for_each_crtc(c, tearwatch_i915) {
					const struct intel_plane_state *ps;

					if (!c->state || !c->state->active || !c->primary ||
					    !c->primary->state || !c->primary->state->fb)
						continue;
					ps = to_intel_plane_state(c->primary->state);
					pr_info("i915: tearwatch: plane shows fb %u, ggtt %llx\n",
					        c->primary->state->fb->base.id,
					        ps->ggtt_vma ?
					            (unsigned long long)ps->ggtt_vma->node.start :
					            0ull);
					break;
				}
			}
			samples = 0;
			changes = in_active = in_blank = straddle = loose = 0;
			worst_line = 0;
			min_line = 0xffffffff;
		}
		lkpi_sleep_ms(1);
	}
	return 0;
}

/* The line the beam is on, for code outside this file. Forcewake-free: this
 * is a display register and the caller may be holding a lock. */

/*
 * Was the flip really finished when the compositor was told it was?
 *
 * A compositor treats the completion event as permission to draw into the
 * buffer the flip retired. If the event leaves before the hardware has
 * latched the new address, that permission arrives while the old buffer is
 * still being scanned, and the next repaint lands in the picture on the
 * glass. This returns the address the display engine is reading right now,
 * so the event path can compare it against the one the flip was armed with.
 */
u32 lkpi_i915_live_surface(void)
{
	struct drm_i915_private *i915 = to_i915_checked(tearwatch_i915);
	int pipe = tearwatch_pipe();

	if (!i915 || pipe < 0)
		return 0;
	return intel_uncore_read_fw(&i915->uncore,
	                            PLANE_SURFLIVE(pipe, PLANE_PRIMARY));
}

/* What the driver last armed, from the plane's own register. */
u32 lkpi_i915_armed_surface(void)
{
	struct drm_i915_private *i915 = to_i915_checked(tearwatch_i915);
	int pipe = tearwatch_pipe();

	if (!i915 || pipe < 0)
		return 0;
	return intel_uncore_read_fw(&i915->uncore,
	                            PLANE_SURF(pipe, PLANE_PRIMARY));
}

u32 lkpi_i915_scanline(void)
{
	struct drm_i915_private *dev_priv = to_i915_checked(tearwatch_i915);

	if (!dev_priv)
		return 0xffffffff;
	return intel_uncore_read_fw(&dev_priv->uncore, PIPEDSL(PIPE_A)) & 0x1fffff;
}

/*
 * What the commit asked for, and what the display engine ended up reading.
 *
 * Called from the atomic ioctl path once every so often. The plane's own
 * state says which framebuffer this commit put on the pipe and where that
 * buffer lives in the graphics address space; PLANE_SURF says what the
 * hardware was told, and PLANE_SURFLIVE what it is reading. Three numbers
 * that should agree, printed together, so a disagreement names itself.
 */
void lkpi_i915_note_commit(void)
{
	static unsigned n;
	struct drm_i915_private *dev_priv = to_i915_checked(tearwatch_i915);
	struct drm_crtc *c;

	if (!dev_priv || (!lkpi_bootflag("b1nix.drm-tearwatch") &&
	                  !lkpi_bootflag("b1nix.drm-bindwatch")))
		return;
	/*
	 * Which memory each framebuffer is made of, on every commit.
	 *
	 * The GGTT entry under the scanned address was seen swapping between two
	 * pages, so the question is whether a framebuffer keeps the same pages
	 * from one commit to the next. If the same framebuffer id reports a
	 * different first page each time it is shown, the object's page list is
	 * not stable and the binding is following it.
	 */
	if (lkpi_bootflag("b1nix.drm-bindwatch")) {
		static unsigned shown;
		struct drm_crtc *cc;

		drm_for_each_crtc(cc, tearwatch_i915) {
			struct drm_framebuffer *fb;
			struct drm_i915_gem_object *bo;

			if (!cc->state || !cc->state->active || !cc->primary ||
			    !cc->primary->state || !cc->primary->state->fb)
				continue;
			fb = cc->primary->state->fb;
			bo = fb->obj[0] ? to_intel_bo(fb->obj[0]) : NULL;
			if (bo && shown < 12 &&
			    !i915_gem_object_pin_pages_unlocked(bo)) {
				struct i915_vma *v = to_intel_plane_state(
				                         cc->primary->state)->ggtt_vma;
				/*
				 * Is this the same mapping as last time this framebuffer was
				 * shown?
				 *
				 * i915 caches one vma per (object, address space, view) and
				 * re-uses it, so a framebuffer keeps its GGTT node for as
				 * long as it lives. If the node moves from one commit to the
				 * next, the old node was freed -- and it is freed while the
				 * display is still reading it, which is what the binding
				 * watch sees rewritten mid-frame.
				 */
				{
					static struct { u32 id; void *vma; u64 start; } seen[4];
					unsigned k;

					for (k = 0; k < 4; k++) {
						if (seen[k].id == fb->base.id) {
							if (seen[k].vma != v ||
							    seen[k].start != (v ? v->node.start : 0))
								pr_info("i915: commit:   fb %u CHANGED "
								        "mapping: vma %p node %llx -> vma %p "
								        "node %llx\n", fb->base.id,
								        seen[k].vma,
								        (unsigned long long)seen[k].start,
								        (void *)v,
								        v ? (unsigned long long)v->node.start
								          : 0ull);
							seen[k].vma = v;
							seen[k].start = v ? v->node.start : 0;
							break;
						}
						if (!seen[k].id) {
							seen[k].id = fb->base.id;
							seen[k].vma = v;
							seen[k].start = v ? v->node.start : 0;
							break;
						}
					}
				}

				/* The whole mapping, not just where it starts: two
				 * framebuffers whose GGTT ranges overlap would explain an
				 * entry under one of them being rewritten with the other's
				 * pages, which is what the binding watch sees. */
				pr_info("i915: commit: fb %u obj page0 %llx size %llx, ggtt "
				        "%llx..%llx, plane offset %x, armed %08x live %08x\n",
				        fb->base.id,
				        (unsigned long long)
				            i915_gem_object_get_dma_address(bo, 0),
				        (unsigned long long)bo->base.size,
				        v ? (unsigned long long)v->node.start : 0ull,
				        v ? (unsigned long long)(v->node.start + v->node.size)
				          : 0ull,
				        to_intel_plane_state(
				            cc->primary->state)->view.color_plane[0].offset,
				        intel_uncore_read_fw(&dev_priv->uncore,
				            PLANE_SURF(to_intel_crtc(cc)->pipe, PLANE_PRIMARY)),
				        intel_uncore_read_fw(&dev_priv->uncore,
				            PLANE_SURFLIVE(to_intel_crtc(cc)->pipe,
				                           PLANE_PRIMARY)));
				/*
				 * And what the armed address actually maps right now.
				 *
				 * If the page under the address the plane was just given is
				 * the other framebuffer's memory, then the display was
				 * pointed at the wrong buffer and something repairs it a
				 * moment later -- in the middle of a frame, which is the
				 * tear. This prints the two side by side so the pairing is
				 * not an inference across two logs.
				 */
				{
					struct i915_ggtt *g = to_gt(dev_priv)->ggtt;
					u32 armed = intel_uncore_read_fw(&dev_priv->uncore,
					    PLANE_SURF(to_intel_crtc(cc)->pipe, PLANE_PRIMARY));

					if (g && g->gsm)
						pr_info("i915: commit:   armed %08x maps page %llx "
						        "(this fb's page0 is %llx)\n", armed,
						        (unsigned long long)(readq(
						            &((u64 __iomem *)g->gsm)[armed >> 12]) &
						            0x0000fffffffff000ull),
						        (unsigned long long)
						            i915_gem_object_get_dma_address(bo, 0));
				}
				i915_gem_object_unpin_pages(bo);
				shown++;
			}
			break;
		}
	}
	if (!lkpi_bootflag("b1nix.drm-tearwatch"))
		return;
	/* Every commit, for the first twenty after the desktop is up.
	 *
	 * Sampling one commit in sixty aliases with a two-buffer rotation: it
	 * lands on the same phase every time, and both the framebuffer id and the
	 * binding then look constant when they are alternating. Twenty in a row
	 * cannot hide that. */
	/* Print when the framebuffer CHANGES, not every n-th commit: a counter
	 * spends its budget on whatever ran first (the boot console, the flip
	 * test) and a fixed stride aliases with a two-buffer rotation. Forty
	 * transitions is plenty to see whether the buffers are two pieces of
	 * memory or one. */
	{
		static u32 last_fb;
		struct drm_crtc *cc;
		u32 id = 0;

		drm_for_each_crtc(cc, tearwatch_i915) {
			if (cc->state && cc->state->active && cc->primary &&
			    cc->primary->state && cc->primary->state->fb) {
				id = cc->primary->state->fb->base.id;
				break;
			}
		}
		if (!id || id == last_fb || n > 40u)
			return;
		last_fb = id;
		n++;
	}
	drm_for_each_crtc(c, tearwatch_i915) {
		const struct intel_plane_state *ps;

		if (!c->state || !c->state->active || !c->primary ||
		    !c->primary->state || !c->primary->state->fb)
			continue;
		ps = to_intel_plane_state(c->primary->state);
		/* What the driver would write is the vma's start plus the colour
		 * plane's offset; printing both says whether the register holds a
		 * stale address or a wrongly computed one. */
		/* update_planes is the bitmask i915's commit tail walks: a plane not
		 * in it is not programmed, however new its framebuffer is. A commit
		 * that names a new fb with an empty mask explains a surface address
		 * that never moves. */
		pr_info("i915: commit: update_planes %x, plane_mask %x, "
		        "needs_modeset %d\n",
		        to_intel_crtc_state(c->state)->update_planes,
		        c->state->plane_mask,
		        (int)drm_atomic_crtc_needs_modeset(c->state));
		{
			/* The physical page behind the buffer, not just its graphics
			 * address. Two framebuffers can be bound at two different GGTT
			 * addresses and still be one piece of memory -- and then flipping
			 * between them changes nothing, the compositor paints what the
			 * display is reading, and the picture tears on every change while
			 * every counter above looks right. */
			struct drm_i915_gem_object *bo =
			    c->primary->state->fb->obj[0]
			        ? to_intel_bo(c->primary->state->fb->obj[0])
			        : 0;
			dma_addr_t page0 = 0;

			if (bo && i915_gem_object_pin_pages_unlocked(bo) == 0) {
				page0 = i915_gem_object_get_dma_address(bo, 0);
				i915_gem_object_unpin_pages(bo);
			}
			pr_info("i915: commit: fb %u page0 %llx\n",
			        c->primary->state->fb->base.id,
			        (unsigned long long)page0);
		}
		pr_info("i915: commit: crtc %u (pipe %c) fb %u at ggtt %llx + off %x "
		        "= %llx; SURF %08x SURFLIVE %08x\n", c->base.id,
		        'A' + (int)to_intel_crtc(c)->pipe,
		        c->primary->state->fb->base.id,
		        ps->ggtt_vma ? (unsigned long long)ps->ggtt_vma->node.start
		                     : 0ull,
		        ps->view.color_plane[0].offset,
		        ps->ggtt_vma ? (unsigned long long)ps->ggtt_vma->node.start +
		                       ps->view.color_plane[0].offset : 0ull,
		        intel_uncore_read_fw(&dev_priv->uncore,
		                             PLANE_SURF(to_intel_crtc(c)->pipe,
		                                        PLANE_PRIMARY)),
		        intel_uncore_read_fw(&dev_priv->uncore,
		                             PLANE_SURFLIVE(to_intel_crtc(c)->pipe,
		                                            PLANE_PRIMARY)));
		return;
	}
}


/*
 * The frame the display actually sent, hashed by the hardware.
 *
 * Everything up to here is an argument from the driver's side of the wire:
 * which address the plane holds, when it latched, what the memory contained.
 * The pipe's CRC is taken after blending, on the pixels leaving for the
 * monitor, so it is the one witness downstream of all of it. Run against the
 * flip test's two flat colours it answers the question directly: whole frames
 * produce exactly two CRC values, and a frame assembled from two buffers
 * produces a third that belongs to neither.
 *
 * b1nix.drm-crcwatch.
 */
static int i915_crcwatch_thread(void *arg)
{
	struct drm_i915_private *i915 = to_i915_checked(tearwatch_i915);
	struct drm_i915_private *dev_priv = i915;
	u32 seen[64], count[64];
	unsigned n_seen = 0;
	u64 frames = 0, reports = 0;
	u32 prev_dsl = 0;
	unsigned waited = 0;
	u32 prev_crc = 0;
	u32 cur_ctl = 0, cur_base = 0, cur_pos = 0;
	u32 live_prev1 = 0, live_prev2 = 0;
	u64 crc_moved_same_buffer = 0, crc_moved_same_offset = 0;
	u32 prev_off = 0, prev_pos = 0;
	u64 prev_pte = 0, pte_moved = 0;
	u64 cur_moves = 0;
	int latch_checked = 0;
	u64 changed = 0, overflow = 0;
	int enabled = -1;

	(void)arg;
	if (!i915)
		return 0;
	pr_info("i915: crcwatch thread up\n");
	for (;;) {
		int pipe = tearwatch_pipe();
		u32 dsl, crc;
		unsigned i;

		if (pipe < 0) {
			if (++waited % 50 == 0)
				pr_info("i915: crcwatch: still no active pipe\n");
			lkpi_sleep_ms(200);
			continue;
		}
		if (enabled != pipe) {
			intel_uncore_write_fw(&i915->uncore, PIPE_CRC_CTL(pipe),
			                      PIPE_CRC_ENABLE |
			                      PIPE_CRC_SOURCE_DMUX_SKL);
			enabled = pipe;
			pr_info("i915: crcwatch: pipe %c CRC on, PIPE_MISC %08x "
			        "(dither bits tell whether identical pictures may hash "
			        "differently)\n", 'A' + pipe,
			        intel_uncore_read_fw(&i915->uncore, PIPE_MISC(pipe)));
			lkpi_sleep_ms(50);
		}

		/* One sample per frame: wait for the scanline counter to wrap. */
		dsl = intel_uncore_read_fw(&i915->uncore, PIPEDSL(pipe)) & 0x1fffff;
		if (dsl >= prev_dsl) {
			prev_dsl = dsl;
			continue;
		}
		prev_dsl = dsl;
		crc = intel_uncore_read_fw(&i915->uncore, PIPE_CRC_RES_1_IVB(pipe));
		frames++;
		/*
		 * The cursor, which is the one thing the CRC sees and the memory
		 * hashes do not.
		 *
		 * The pipe CRC is taken after blending, so a cursor that moves or
		 * animates changes it every frame while every hash of the primary
		 * plane's buffer reports the picture unchanged -- which is exactly
		 * the disagreement between these instruments. Its control, address
		 * and position are three reads, and they say whether the plane is
		 * even enabled and whether it is what is moving.
		 */
		{
			u32 ccntr = intel_uncore_read_fw(&i915->uncore, CURCNTR(pipe));
			u32 cbase = intel_uncore_read_fw(&i915->uncore, CURBASE(pipe));
			u32 cpos = intel_uncore_read_fw(&i915->uncore, CURPOS(pipe));

			if (ccntr != cur_ctl || cbase != cur_base || cpos != cur_pos) {
				cur_moves++;
				cur_ctl = ccntr;
				cur_base = cbase;
				cur_pos = cpos;
			}
		}
		/*
		 * Is the result register latched at the end of the frame, or does it
		 * accumulate as the frame is scanned?
		 *
		 * The whole test rests on one sample per frame being that frame's
		 * hash. If the register instead climbs while the beam moves, then
		 * two samples taken at different scanlines of the SAME frame differ,
		 * and a sampler whose timing jitters would report a stream of
		 * "different frames" from a picture that never changed. Checked
		 * once, on a frame where the two looks are provably inside the same
		 * scanout: the second look is taken before the counter wraps again.
		 */
		if (!latch_checked) {
			u32 dsl_a = intel_uncore_read_fw(&i915->uncore,
			                                 PIPEDSL(pipe)) & 0x1fffff;
			u32 crc_a = intel_uncore_read_fw(&i915->uncore,
			                                 PIPE_CRC_RES_1_IVB(pipe));
			u32 dsl_b, crc_b;

			while (((dsl_b = intel_uncore_read_fw(&i915->uncore,
			                                      PIPEDSL(pipe)) & 0x1fffff)
			        < 800) && dsl_b >= dsl_a)
				;
			crc_b = intel_uncore_read_fw(&i915->uncore,
			                             PIPE_CRC_RES_1_IVB(pipe));
			if (dsl_b > dsl_a) {
				latch_checked = 1;
				pr_info("i915: crcwatch: within one frame, line %u gives "
				        "%08x and line %u gives %08x -- %s\n", dsl_a, crc_a,
				        dsl_b, crc_b,
				        crc_a == crc_b ? "latched at frame end, one sample "
				                         "per frame is that frame"
				                       : "IT ACCUMULATES DURING THE FRAME, "
				                         "so a single sample is not a frame "
				                         "hash");
			}
		}
		/*
		 * Does the CRC move only when the picture does?
		 *
		 * Two hardware readings disagree: the band hashes say the scanned
		 * buffer changes about twenty times a second, the CRC says nearly
		 * every frame differs. They cannot both be describing the picture,
		 * so pair each CRC with the surface address it was taken over. A CRC
		 * that changes while the same buffer is on screen, on a load whose
		 * content only moves when the buffer flips, means the CRC is not a
		 * hash of the frame -- and every conclusion drawn from it has to go.
		 */
		{
			u32 live = intel_uncore_read_fw(&i915->uncore,
			                                PLANE_SURFLIVE(pipe,
			                                    PLANE_PRIMARY));

			/*
			 * Pair the CRC with the buffer it was actually taken over.
			 *
			 * The CRC read at the top of frame N is the hash of frame N-1,
			 * while the surface address read at the same moment is already
			 * the one frame N will use. Comparing those two directly reports
			 * "the picture changed but the buffer did not" every time a flip
			 * lands, which is an off-by-one in the sampler and not a fact
			 * about the hardware. The address that belongs with this CRC is
			 * the one seen a frame earlier.
			 */
			/*
			 * A frame can change without the buffer changing for an innocent
			 * reason: the plane can be re-pointed inside the same buffer.
			 * PLANE_OFFSET and PLANE_POS say where in it the picture starts
			 * and where on the pipe it lands, and a compositor is free to
			 * move either. Counting those separately keeps them out of the
			 * tear column.
			 */
			/*
			 * The GGTT address is not a name for a buffer.
			 *
			 * i915 binds a framebuffer into the GGTT when it is used, and two
			 * framebuffers can take the same address at different times. Then
			 * PLANE_SURFLIVE reads identical across a flip and every frame
			 * that differs looks like the same buffer changing under the
			 * beam. The page the address translates to is the buffer, so read
			 * the GGTT entry as well.
			 */
			struct i915_ggtt *ggtt = to_gt(i915)->ggtt;
			u64 pte = 0;

			if (ggtt && ggtt->gsm)
				pte = readq(&((u64 __iomem *)ggtt->gsm)[live >> 12]);
			if (frames > 1 && pte != prev_pte)
				pte_moved++;
			prev_pte = pte;

			u32 off = intel_uncore_read_fw(&i915->uncore,
			                               PLANE_OFFSET(pipe, PLANE_PRIMARY));
			u32 pos = intel_uncore_read_fw(&i915->uncore,
			                               PLANE_POS(pipe, PLANE_PRIMARY));

			if (frames > 1 && crc != prev_crc) {
				changed++;
				if (live_prev1 == live_prev2) {
					if (off != prev_off || pos != prev_pos)
						crc_moved_same_offset++;
					else
						crc_moved_same_buffer++;
				}
			}
			prev_off = off;
			prev_pos = pos;
			live_prev2 = live_prev1;
			live_prev1 = live;
		}
		prev_crc = crc;
		for (i = 0; i < n_seen; i++)
			if (seen[i] == crc)
				break;
		if (i == n_seen) {
			if (n_seen < 64) {
				seen[n_seen] = crc;
				count[n_seen] = 1;
				n_seen++;
			} else {
				overflow++;
			}
		} else {
			count[i]++;
		}

		/*
		 * Per window, not per session.
		 *
		 * A session-long table fills up during start-up -- every animation
		 * frame is its own CRC -- and after that it can no longer tell a new
		 * value from an old one. What the test needs is the count of
		 * *distinct frames the monitor was sent* while the picture had only
		 * two states, so the table is rebuilt each window and the window is
		 * judged on its own: two flat colours alternating must produce two
		 * values, and any third is a frame that was neither.
		 */
		if (++reports >= 240) {
			unsigned top = 0, second = 0, others = 0;

			for (i = 0; i < n_seen; i++) {
				if (count[i] > count[top]) {
					second = top;
					top = i;
				} else if (i != top &&
				           (second == top || count[i] > count[second])) {
					second = i;
				}
			}
			for (i = 0; i < n_seen; i++)
				if (i != top && i != second)
					others += count[i];
			pr_info("i915: crcwatch: window of %u frame(s): %u distinct "
			        "(+%llu past the table), %llu differed from the frame "
			        "before, commonest %08x x%u and %08x x%u, %u frame(s) "
			        "that were neither, cursor %s and changed in %llu of "
			        "them, %llu differed WITHOUT the buffer changing "
			        "(%llu of those moved the plane instead, %llu frames "
			        "had the GGTT entry repointed) (%llu total so far)\n",
			        (unsigned)reports, n_seen, (unsigned long long)overflow,
			        (unsigned long long)changed, seen[top], count[top],
			        n_seen > 1 ? seen[second] : 0,
			        n_seen > 1 ? count[second] : 0, others,
			        (cur_ctl & MCURSOR_MODE_MASK) ? "on" : "off",
			        (unsigned long long)cur_moves,
			        (unsigned long long)crc_moved_same_buffer,
			        (unsigned long long)crc_moved_same_offset,
			        (unsigned long long)pte_moved,
			        (unsigned long long)frames);
			/*
			 * Display FIFO underruns.
			 *
			 * A pipe that runs out of data mid-scan sends the monitor
			 * whatever is left in the FIFO -- repeated or torn lines -- from
			 * a framebuffer nobody wrote to. That is the one way the same
			 * buffer can produce a different frame, and it is invisible to
			 * every memory hash. i915 masks the underrun interrupt when it
			 * cannot service it, and the interrupt identity register latches
			 * the bit anyway, so it can be read here. Watermarks come from
			 * memory latencies the PCODE mailbox reports, which a passed
			 * through device may not answer.
			 */
			{
				u32 iir = intel_uncore_read_fw(&i915->uncore,
				                               GEN8_DE_PIPE_IIR(pipe));
				u32 imr = intel_uncore_read_fw(&i915->uncore,
				                               GEN8_DE_PIPE_IMR(pipe));

				pr_info("i915: crcwatch:   pipe IIR %08x IMR %08x, FIFO "
				        "underrun %s\n", iir, imr,
				        (iir & GEN8_PIPE_FIFO_UNDERRUN) ? "LATCHED" :
				        (imr & GEN8_PIPE_FIFO_UNDERRUN) ? "masked, none seen"
				                                        : "none");
			}
			/* The table only holds 64 values, so a window that fills it
			 * hides every later value behind one counter. Both numbers
			 * are printed because "62 were neither" means nothing on its
			 * own once the table saturated. */
			reports = 0;
			cur_moves = 0;
			crc_moved_same_buffer = 0;
			crc_moved_same_offset = 0;
			pte_moved = 0;
			n_seen = 0;
			changed = 0;
			overflow = 0;
		}
	}
	return 0;
}

void lkpi_i915_start_tearwatch(struct drm_device *dev)
{
	/*
	 * Which commit path this device actually runs.
	 *
	 * i915 programs its planes from its own commit tail: nothing else writes
	 * PLANE_SURF. If mode_config.funcs->atomic_commit is the generic helper
	 * rather than i915's, or the primary plane has no atomic_update helper,
	 * then a page flip updates the software state, completes, and never
	 * reaches the display engine -- which is exactly the frozen live surface
	 * the tear watch reports. The kernel's %p resolves through kallsyms, so
	 * these print as names.
	 */
	if (dev && dev->mode_config.funcs)
		pr_info("i915: commit path: atomic_commit %p, atomic_check %p\n",
		        dev->mode_config.funcs->atomic_commit,
		        dev->mode_config.funcs->atomic_check);
	if (dev) {
		struct drm_crtc *c;

		drm_for_each_crtc(c, dev) {
			if (!c->primary)
				continue;
			pr_info("i915: primary plane %u helper atomic_update %p, "
			        "atomic_check %p\n", c->primary->base.id,
			        c->primary->helper_private ?
			            c->primary->helper_private->atomic_update : 0,
			        c->primary->helper_private ?
			            c->primary->helper_private->atomic_check : 0);
			break;
		}
	}
	pr_info("i915: watch options: tearwatch %d paintwatch %d crcwatch %d "
	        "framedump %u\n", lkpi_bootflag("b1nix.drm-tearwatch"),
	        lkpi_bootflag("b1nix.drm-paintwatch"),
	        lkpi_bootflag("b1nix.drm-crcwatch"),
	        lkpi_bootopt_u32("b1nix.drm-framedump", 0));
	if (!dev || tearwatch_i915)
		return;
	if (!lkpi_bootflag("b1nix.drm-tearwatch") &&
	    !lkpi_bootflag("b1nix.drm-paintwatch") &&
	    !lkpi_bootflag("b1nix.drm-crcwatch") &&
	    !lkpi_bootflag("b1nix.drm-bandwatch") &&
	    !lkpi_bootflag("b1nix.drm-scanwatch") &&
	    !lkpi_bootflag("b1nix.drm-bindwatch") &&
	    !lkpi_bootflag("b1nix.drm-cadence") &&
	    /* Not a thread of its own: it only needs the device pointer so the
	     * event path can read the plane registers. */
	    !lkpi_bootflag("b1nix.drm-eventwatch") &&
	    !lkpi_bootopt_u32("b1nix.drm-framedump", 0))
		return;
	tearwatch_i915 = dev;
	if (lkpi_bootflag("b1nix.drm-tearwatch") ||
	    lkpi_bootflag("b1nix.drm-paintwatch") ||
	    lkpi_bootflag("b1nix.drm-bandwatch") ||
	    lkpi_bootflag("b1nix.drm-scanwatch") ||
	    lkpi_bootflag("b1nix.drm-bindwatch") ||
	    lkpi_bootflag("b1nix.drm-cadence"))
		lkpi_fs_kthread_run(i915_tearwatch_thread, 0, "i915-tearwatch");
	if (lkpi_bootflag("b1nix.drm-crcwatch"))
		lkpi_fs_kthread_run(i915_crcwatch_thread, 0, "i915-crcwatch");
	if (lkpi_bootopt_u32("b1nix.drm-framedump", 0)) {
		pr_info("i915: framedump: %u frame(s) from t+%us every %ums\n",
		        lkpi_bootopt_u32("b1nix.drm-framedump", 0),
		        lkpi_bootopt_u32("b1nix.drm-framedump-at", 45),
		        lkpi_bootopt_u32("b1nix.drm-framedump-every", 250));
		lkpi_fs_kthread_run(i915_framedump_thread, 0, "i915-framedump");
	}
}


/*
 * What is actually on the panel.
 *
 * Every measurement so far has been about addresses and timing: which buffer
 * the plane points at, and when the hardware latched it. All of them come out
 * clean while a person still sees the picture break, so the one thing left to
 * look at is the pixels themselves -- and they can be read from the same place
 * the display engine reads them, by translating the live surface address
 * through the GGTT page by page. That is the frame on the glass, not the
 * driver's idea of it.
 *
 * Output is a downscaled greyscale image over the serial log, one hex chunk
 * per line, reassembled on the host. b1nix.drm-framedump=<frames>, starting
 * b1nix.drm-framedump-at=<seconds> after boot, one frame every
 * b1nix.drm-framedump-every=<ms>.
 */
/* Sampling step, in pixels. Coarse by default because every pixel costs two
 * hex digits on a serial line; b1nix.drm-framedump-step=2 gives 960x540 and
 * =1 the full 1920x1080, at four and sixteen times the transfer. */
static unsigned fd_step(void)
{
	unsigned s = lkpi_bootopt_u32("b1nix.drm-framedump-step", 4);

	return (s == 1u || s == 2u || s == 4u || s == 8u) ? s : 4u;
}
#define FD_CHUNK  100u	/* pixels per log line: 200 hex chars, well under 512 */

static void *fd_map_ggtt(struct i915_ggtt *ggtt, u32 ggtt_addr)
{
	u64 __iomem *gsm;
	u64 pte;
	void *p;

	if (!ggtt || !ggtt->gsm)
		return NULL;
	gsm = (u64 __iomem *)ggtt->gsm;
	pte = readq(&gsm[ggtt_addr >> 12]);
	if (!(pte & 1))
		return NULL;
	p = lkpi_phys_to_virt(pte & 0x0000fffffffff000ull);
	if (!p)
		return NULL;
	return (char *)p + (ggtt_addr & 0xfffu);
}

static int i915_framedump_thread(void *arg)
{
	struct drm_i915_private *i915 = to_i915_checked(tearwatch_i915);
	struct drm_i915_private *dev_priv = i915;
	struct i915_ggtt *ggtt;
	u32 frames = lkpi_bootopt_u32("b1nix.drm-framedump", 1);
	u32 at = lkpi_bootopt_u32("b1nix.drm-framedump-at", 45);
	u32 every = lkpi_bootopt_u32("b1nix.drm-framedump-every", 250);
	u32 n;

	(void)arg;
	if (!i915)
		return 0;
	ggtt = to_gt(i915)->ggtt;
	pr_info("i915: framedump thread up\n");
	while (at--)
		lkpi_sleep_ms(1000);

	for (n = 0; n < frames; n++) {
		int pipe = tearwatch_pipe();
		u32 vdisplay = tearwatch_vdisplay();
		u32 surf, stride, dsl0, dsl1, y;
		u32 width;
		unsigned step = fd_step();

		if (pipe < 0 || !vdisplay) {
			lkpi_sleep_ms(every);
			continue;
		}
		surf = intel_uncore_read_fw(&i915->uncore,
		                            PLANE_SURFLIVE(pipe, PLANE_PRIMARY));
		/* PLANE_STRIDE counts 64-byte units on this generation. */
		stride = intel_uncore_read_fw(&i915->uncore,
		                              PLANE_STRIDE(pipe, PLANE_PRIMARY));
		stride = (stride & 0x3ff) * 64u;
		if (!stride) {
			lkpi_sleep_ms(every);
			continue;
		}
		width = stride / 4u;
		dsl0 = intel_uncore_read_fw(&i915->uncore, PIPEDSL(pipe));

		pr_info("FD: frame %u pipe %c surf %08x stride %u %ux%u step %ux%u "
		        "dsl %u\n", n, 'A' + pipe, surf, stride, width, vdisplay,
		        step, step, dsl0);

		for (y = 0; y < vdisplay; y += step) {
			u32 x = 0;

			while (x < width) {
				char line[2 * FD_CHUNK + 1];
				u32 got = 0;

				while (got < FD_CHUNK && x < width) {
					const volatile u32 *pw = fd_map_ggtt(ggtt,
					                           surf + y * stride + x * 4u);
					u32 lum = 0;

					if (pw) {
						u32 v = fd_read32(pw);
						const u8 px[4] = { (u8)v, (u8)(v >> 8),
						                   (u8)(v >> 16), (u8)(v >> 24) };
						/* XRGB8888: B, G, R. A rough luminance is
						 * enough to read the picture back. */
						lum = ((u32)px[2] * 77 + (u32)px[1] * 151 +
						       (u32)px[0] * 28) >> 8;
						if (lum > 255)
							lum = 255;
					}
					line[got * 2] = "0123456789abcdef"[lum >> 4];
					line[got * 2 + 1] = "0123456789abcdef"[lum & 15];
					got++;
					x += step;
				}
				line[got * 2] = 0;
				pr_info("FD%u %u %u %s\n", n, y,
				        (x - got * step) / step, line);
			}
		}
		dsl1 = intel_uncore_read_fw(&i915->uncore, PIPEDSL(pipe));
		pr_info("FD: frame %u end dsl %u surf now %08x\n", n, dsl1,
		        intel_uncore_read_fw(&i915->uncore,
		                             PLANE_SURFLIVE(pipe, PLANE_PRIMARY)));
		lkpi_sleep_ms(every);
	}
	pr_info("FD: done\n");
	return 0;
}

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
	        ctl, surf, stride, surf >> 12, (unsigned long long)pte, (int)(pte & 1));
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
	lkpi_i915_start_tearwatch(dev);
}

/*
 * Pin the display power-saving module parameters off before the driver probes.
 *
 * The vblank-evasion critical section (intel_pipe_update_start ..
 * intel_pipe_update_end) programs the flip registers with interrupts disabled
 * and must finish within the window just before vblank. A commit that arrives
 * after a power well has entered a deep DC state stalls inside that section
 * waking the well; under legacy IGD passthrough the wake is slow enough that
 * the section overruns the window, misses the flip and tears the frame -- seen
 * as "Atomic update failure on pipe A time 1548 us, min 1073". PSR and FBC add
 * their own commit-time handshakes for the same reason.
 *
 * Setting enable_dc/enable_psr/enable_fbc to 0 keeps the wells up: a little
 * more idle power for a commit that always meets the vblank deadline. The
 * imported defaults are -1 (auto, on for this generation); this only writes
 * i915_modparams, which i915_params_copy() folds into i915->params at probe.
 * Reachable because i915_drv.h declares the extern; nothing imported changes.
 */
void lkpi_i915_disable_display_power_saving(void)
{
	i915_modparams.enable_dc = 0;
	i915_modparams.enable_psr = 0;
	i915_modparams.enable_fbc = 0;
	pr_info("i915-probe: display power saving pinned off (DC/PSR/FBC) "
	        "for vblank-evasion determinism\n");
}

/*
 * Time a raw MMIO read of a display register, to tell a direct-mapped BAR
 * (KVM memslot, tens of ns per access) from a trapped one (a VM exit and a
 * VFIO round-trip per access, microseconds). The vblank-evasion critical
 * section writes a handful of double-buffered registers with interrupts off;
 * if each access is microseconds the section overruns its ~1 ms budget and
 * the frame tears. PIPEDSL is read-only and always live on an enabled pipe,
 * so hammering it changes nothing. b1nix.i915-mmio-bench.
 */
void lkpi_i915_mmio_bench(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915_checked(dev);
	const unsigned iters = 4000;
	volatile u32 sink = 0;
	u64 t0, t1;
	unsigned i;

	if (!dev_priv)
		return;

	/* Warm the mapping, then time the read loop with interrupts disabled so
	 * nothing else is charged to it. */
	sink += intel_uncore_read_fw(&dev_priv->uncore, PIPEDSL(PIPE_A));

	t0 = lkpi_monotonic_ns();
	for (i = 0; i < iters; i++)
		sink += intel_uncore_read_fw(&dev_priv->uncore, PIPEDSL(PIPE_A));
	t1 = lkpi_monotonic_ns();

	pr_info("i915-probe: mmio-bench %u raw reads in %llu ns = %llu ns/read "
	        "(sink %08x)\n",
	        iters, (unsigned long long)(t1 - t0),
	        (unsigned long long)((t1 - t0) / iters), (unsigned)sink);

	/*
	 * Reads are non-posted and cost a PCIe round-trip even on bare metal, so
	 * they cannot tell a trapped BAR from a direct one. Posted WRITES can: a
	 * direct write retires locally (~100 ns), a trapped one takes a VM exit
	 * and a VFIO round-trip (~1 us). SWF1 is a display scratch register i915
	 * saves and restores across suspend -- no hardware side effect -- so it is
	 * safe to hammer once its value is put back.
	 */
	{
		u32 orig = intel_uncore_read_fw(&dev_priv->uncore, SWF1(0));
		u64 w0, w1;

		w0 = lkpi_monotonic_ns();
		for (i = 0; i < iters; i++)
			intel_uncore_write_fw(&dev_priv->uncore, SWF1(0),
			                      0xb1000000u | i);
		w1 = lkpi_monotonic_ns();

		intel_uncore_write_fw(&dev_priv->uncore, SWF1(0), orig);

		pr_info("i915-probe: mmio-bench %u raw writes in %llu ns = %llu "
		        "ns/write\n",
		        iters, (unsigned long long)(w1 - w0),
		        (unsigned long long)((w1 - w0) / iters));
	}
}
