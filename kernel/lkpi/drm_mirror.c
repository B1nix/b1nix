/*
 * SPDX-License-Identifier: MIT
 *
 * Show what a passed-through GPU drew.
 *
 * A GPU handed to a VM scans out to a physical connector, and if nothing is
 * plugged into it there is nowhere for the picture to go — the driver does the
 * whole modeset, composites a frame, and the result leaves through a cable that
 * is not there. QEMU cannot help: for a plain assigned device it has no way to
 * read the framebuffer back (`vfio: device doesn't support any (known) display
 * method` — the gfx-plane query it wants exists only for mdev/vGPU).
 *
 * So the picture is fetched from the other side. This drives a real modeset on
 * the assigned GPU through the in-kernel DRM client — the same path fbdev
 * emulation uses — then reads the framebuffer back over the CPU mapping and
 * copies it into b1nix's own virtio-gpu scanout, which *is* a window QEMU
 * draws. What appears there is what the assigned GPU actually composited.
 *
 * The honest limits:
 *
 *   - It proves the pipeline up to the framebuffer, not the wire. The pixels
 *     are read from memory, so a fault in the transcoder, the PHY or the link
 *     would not show up here.
 *   - It is a copy, not a scanout. There is no page flip and no vblank
 *     synchronisation, so a frame captured mid-render will tear.
 *   - The two displays are different sizes. The copy is a top-left crop rather
 *     than a scale: scaling would invent pixels, and the point is to see what
 *     the GPU produced.
 *
 * Enabled with b1nix.mirror-display.
 */

#include <drm/drm_client.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_prime.h>
#include <asm/cacheflush.h>
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/err.h>
#include <linux/iosys-map.h>
#include <linux/slab.h>
#include <linux/types.h>

#include <lkpi/env.h>

struct drm_device *lkpi_drm_first_device(void);

/*
 * The DRM debug mask, enabled around the commit only.
 *
 * The modeset is the part worth seeing, and it is a few hundred lines. Leaving
 * the mask on for the whole boot is thousands of lines through a 115200 serial
 * port, which makes the driver miss every timeout it has — so the interesting
 * output becomes unobtainable precisely because it is switched on.
 */
extern unsigned long __drm_debug;
/*
 * The i915 display probe, which exists only when that driver is in the build —
 * this file is not, it mirrors whatever DRM device is present. Weak, so a
 * kernel built without i915 links, and called only when it resolved.
 */
void lkpi_i915_dump_plane_surface(struct drm_device *dev) __attribute__((weak));
void lkpi_i915_dump_pipe_state(struct drm_device *dev) __attribute__((weak));
void lkpi_i915_dump_vblank_state(struct drm_device *dev) __attribute__((weak));
void lkpi_i915_dump_infoframes(struct drm_device *dev) __attribute__((weak));
void lkpi_i915_dump_port_state(struct drm_device *dev) __attribute__((weak));
void lkpi_i915_crc_begin(struct drm_device *dev) __attribute__((weak));
u32 lkpi_i915_crc_sample(struct drm_device *dev) __attribute__((weak));
void lkpi_i915_crc_end(struct drm_device *dev) __attribute__((weak));
#define MIRROR_DEBUG_KMS 0x04UL

/* Kept for the life of the mirror: releasing the client would tear the modeset
 * down, and the framebuffer with it. */
static struct drm_client_dev g_client;
static struct drm_client_buffer *g_buffer;
/* Set when the mapping came from a dma-buf rather than from the client buffer,
 * so the right unmap is used. */
static struct dma_buf *g_dmabuf;
static u32 g_fb_width, g_fb_height;
static bool g_active;

static void mirror_unregister(struct drm_client_dev *client)
{
	(void)client;
	g_active = false;
}

static const struct drm_client_funcs mirror_client_funcs = {
	.unregister = mirror_unregister,
};

/*
 * Something recognisable, so a blank result can be told from a wrong one.
 *
 * A flat colour cannot: if the copy silently produced zeros it would look the
 * same as a black frame the GPU legitimately drew. Gradients plus a border give
 * a frame whose position, orientation and stride are all readable by eye, and
 * whose pixel values can be checked without looking.
 */
static void paint(u32 *px, u32 w, u32 h, u32 stride_px)
{
	for (u32 y = 0; y < h; y++) {
		u32 *row = px + (usize)y * stride_px;

		for (u32 x = 0; x < w; x++) {
			u32 r = (x * 255) / (w ? w : 1);
			u32 g = (y * 255) / (h ? h : 1);
			u32 b = 0x80;

			/* A four-pixel border, so a crop or an off-by-one stride is
			 * visible as a broken frame rather than as a slightly wrong
			 * gradient. */
			if (x < 4 || y < 4 || x >= w - 4 || y >= h - 4) {
				r = 0xff; g = 0xff; b = 0xff;
			}
			row[x] = (r << 16) | (g << 8) | b;
		}
	}
}


/*
 * Copy the mapped framebuffer onto b1nix's own scanout.
 *
 * A top-left crop when the two differ in size: scaling would invent pixels, and
 * the point is to see exactly what the GPU produced.
 */
static void present_to_local_display(const struct iosys_map *map, u32 out_w,
                                     u32 out_h)
{
	u32 copy_w = out_w < g_fb_width ? out_w : g_fb_width;
	u32 copy_h = out_h < g_fb_height ? out_h : g_fb_height;
	u32 pitch_px = (g_buffer->fb ? g_buffer->fb->pitches[0]
	                             : g_fb_width * 4) / 4;
	const u32 *src = map->vaddr;
	u32 *out;

	out = kzalloc((usize)out_w * out_h * 4, GFP_KERNEL);
	if (!out)
		return;
	for (u32 y = 0; y < copy_h; y++) {
		const u32 *s = src + (usize)y * pitch_px;
		u32 *d = out + (usize)y * out_w;

		for (u32 x = 0; x < copy_w; x++)
			d[x] = s[x];
	}
	g_active = lkpi_display_present(out, out_w, out_h) != 0;
	kfree(out);
	/*
	 * Announced here rather than by the caller: the modeset commit that runs
	 * next can take the machine down, and a capture that waits for a clean
	 * return would then never happen even though the frame is already on the
	 * local scanout.
	 */
	pr_info("drm: mirror: frame presented (%d) %ux%u from %ux%u\n",
	        g_active, out_w, out_h, g_fb_width, g_fb_height);
}

/*
 * Drive a modeset on the assigned GPU and copy the result to the local display.
 *
 * Returns 1 when a frame reached the local scanout.
 */
/*
 * What every connector on this device says about itself.
 *
 * A modeset probe that picks nothing gives one line of output for what can be
 * any of several unrelated failures: no connector at all, a connector that
 * reports disconnected, or one that is connected and still offers no usable
 * mode. Those need different fixes, and the DRM debug mask cannot be used to
 * tell them apart here — it prints thousands of lines through a 115200 serial
 * port and the probe stops finishing inside the timeout.
 *
 * fill_modes() is called rather than merely reading the list, because the list
 * is only populated when something asks; normally that is userspace through the
 * getconnector ioctl, and nothing has asked yet at this point in boot.
 */
/* Is any connector reporting a display right now? */
static bool any_connected(struct drm_device *dev)
{
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;
	bool found = false;

	drm_connector_list_iter_begin(dev, &iter);
	drm_for_each_connector_iter(connector, &iter) {
		if (connector->funcs && connector->funcs->fill_modes)
			connector->funcs->fill_modes(connector, 4096, 4096);
		if (connector->status == connector_status_connected &&
		    !list_empty(&connector->modes)) {
			found = true;
			break;
		}
	}
	drm_connector_list_iter_end(&iter);
	return found;
}

/*
 * Poll for a display over a bounded window instead of trusting the first probe.
 *
 * A driver normally learns a display is there from a hotplug interrupt, and
 * re-probes when it fires. A passed-through GPU has no working hotplug here —
 * the interrupt originates in the PCH, which the VM does not own — so the first
 * probe is the only answer the boot would ever get. That makes a display that
 * needs a moment after the device reset indistinguishable from one that is not
 * plugged in.
 *
 * Bounded, and it does not manufacture a result: a connector still has to
 * report connected and produce modes from a real EDID read. If the window
 * expires, detection has genuinely failed and the caller reports that.
 */
static void wait_for_a_display(struct drm_device *dev)
{
	const unsigned attempts = 10; /* about five seconds */
	unsigned i;

	for (i = 0; i < attempts; i++) {
		if (any_connected(dev)) {
			if (i)
				pr_info("drm: mirror: a display answered after %u retries\n",
				        i);
			return;
		}
		msleep(500);
	}
	pr_info("drm: mirror: no display answered in %u attempts\n", attempts);
}

static void report_connectors(struct drm_device *dev)
{
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;
	unsigned long saved_debug;

	/*
	 * Whether interrupts are on, reported alongside the detection results.
	 *
	 * Reading an EDID waits on the GMBUS completion interrupt, and the modeset
	 * that follows waits on vblank. With interrupts off, neither wake can ever
	 * be delivered on this CPU: both waits run to their full timeout and report
	 * a fault that looks like the display's, not ours. It is worth one line to
	 * know which of those two worlds the driver is running in.
	 */
	pr_info("drm: mirror: interrupts %s at detect, %llu device IRQs so far\n",
	        lkpi_irqs_enabled() ? "on" : "OFF",
	        (unsigned long long)lkpi_device_irq_count());

	/*
	 * The debug mask, on for detection only.
	 *
	 * Detection is where the driver says why a connector came back empty — a
	 * GMBUS timeout, a fallback to bit-banging, a live-status read of zero —
	 * and none of that is visible otherwise. Bounded to this loop because the
	 * same mask left on for the whole boot floods a 115200 serial line badly
	 * enough that the driver misses its own timeouts.
	 */
	saved_debug = __drm_debug;
	if (lkpi_bootflag("b1nix.drm-debug"))
		__drm_debug |= MIRROR_DEBUG_KMS;

	drm_connector_list_iter_begin(dev, &iter);
	drm_for_each_connector_iter(connector, &iter) {
		struct drm_display_mode *mode;
		int modes = 0;

		if (connector->funcs && connector->funcs->fill_modes)
			connector->funcs->fill_modes(connector, 4096, 4096);

		list_for_each_entry(mode, &connector->modes, head)
			modes++;

		pr_info("drm: mirror: connector %s: status %d, %d mode(s)%s%s\n",
		        connector->name ? connector->name : "(unnamed)",
		        (int)connector->status, modes,
		        modes ? ", preferred " : "",
		        modes ? list_first_entry(&connector->modes,
		                                 struct drm_display_mode, head)->name
		              : "");
	}
	drm_connector_list_iter_end(&iter);

	pr_info("drm: mirror: %llu device IRQs after detect\n",
	        (unsigned long long)lkpi_device_irq_count());

	__drm_debug = saved_debug;
}

int lkpi_drm_mirror_to_display(void)
{
	struct drm_device *dev = lkpi_drm_first_device();
	struct iosys_map map;
	u32 out_w = 0, out_h = 0;
	u32 *out;
	int rc;

	/* Each exit says which one it was. A silent zero here means the mirror did
	 * not happen and gives no way to tell "no device" from "the modeset was
	 * rejected", which are entirely different problems. */
	if (!dev) {
		pr_info("drm: mirror: no DRM device registered\n");
		return 0;
	}
	if (!lkpi_display_get_mode(&out_w, &out_h) || !out_w || !out_h) {
		pr_info("drm: mirror: no local display to mirror onto\n");
		return 0;
	}

	rc = drm_client_init(dev, &g_client, "b1nix-mirror", &mirror_client_funcs);
	if (rc) {
		pr_info("drm: mirror: client init failed (%d)\n", rc);
		return 0;
	}

	wait_for_a_display(dev);
	report_connectors(dev);

	/*
	 * Probed without a size limit: the mode comes from the connector, and
	 * constraining it to the local display's size would reject the only mode
	 * the sink offers. The size difference is handled by the crop below.
	 */
	rc = drm_client_modeset_probe(&g_client, 0, 0);
	if (rc) {
		pr_info("drm: mirror: modeset probe failed (%d)\n", rc);
		drm_client_release(&g_client);
		return 0;
	}

	/* The framebuffer is the size of the mode that was chosen, which is the
	 * first modeset's mode. */
	/*
	 * The framebuffer is sized from the first CRTC the probe actually gave a
	 * mode to, which is not necessarily the first CRTC.
	 *
	 * A modeset entry exists for every CRTC on the device and only the ones
	 * driving a connected display carry a mode; which pipe that is depends on
	 * which port the cable is in. Reading modesets[0] alone worked only as long
	 * as the display was a synthetic one attached to the first free connector,
	 * and reported "no mode on any CRTC" the moment a real monitor came up on a
	 * later pipe.
	 */
	{
		unsigned i;

		for (i = 0; g_client.modesets && g_client.modesets[i].crtc; i++)
			if (g_client.modesets[i].mode)
				break;

		if (!g_client.modesets || !g_client.modesets[i].crtc ||
		    !g_client.modesets[i].mode) {
			pr_info("drm: mirror: probe chose no mode on any CRTC\n");
			drm_client_release(&g_client);
			return 0;
		}
		g_fb_width = g_client.modesets[i].mode->hdisplay;
		g_fb_height = g_client.modesets[i].mode->vdisplay;
		pr_info("drm: mirror: CRTC %u drives %ux%u\n", i, g_fb_width,
		        g_fb_height);
	}

	g_buffer = drm_client_framebuffer_create(&g_client, g_fb_width, g_fb_height,
	                                         DRM_FORMAT_XRGB8888);
	if (IS_ERR_OR_NULL(g_buffer)) {
		pr_info("drm: mirror: framebuffer create failed (%d) for %ux%u\n",
		        g_buffer ? (int)PTR_ERR(g_buffer) : -ENOMEM,
		        g_fb_width, g_fb_height);
		drm_client_release(&g_client);
		return 0;
	}

	/*
	 * The generic GEM vmap first, then dma-buf.
	 *
	 * i915 does not implement the GEM ->vmap callback — it has its own
	 * pin_map — so drm_client_buffer_vmap() reports -EOPNOTSUPP on its
	 * buffers. The same memory is reachable through the driver's dma-buf
	 * export, which every DRM driver that can share buffers implements, so
	 * that is the fallback rather than reaching into i915's internals and
	 * making this file work for exactly one driver.
	 */
	rc = drm_client_buffer_vmap(g_buffer, &map);
	if (rc != 0) {
		int vmap_rc = rc;

		if (!g_buffer->gem) {
			pr_info("drm: mirror: vmap %d and the buffer has no GEM object\n",
			        vmap_rc);
		} else {
			/*
			 * The driver's own exporter when it has one.
			 *
			 * drm_gem_prime_export() builds a dma_buf whose vmap calls back
			 * into the GEM ->vmap callback — the very one i915 does not
			 * implement — so going straight to it just reproduces the failure
			 * one level down. i915's ->export installs its own ops, whose vmap
			 * is its pin_map. This is the same choice drm_gem_prime_handle_to_fd
			 * makes internally.
			 */
			if (g_buffer->gem->funcs && g_buffer->gem->funcs->export)
				g_dmabuf = g_buffer->gem->funcs->export(g_buffer->gem, 0);
			else
				g_dmabuf = drm_gem_prime_export(g_buffer->gem, 0);
			if (IS_ERR_OR_NULL(g_dmabuf)) {
				pr_info("drm: mirror: vmap %d, dma-buf export failed (%d)\n",
				        vmap_rc,
				        g_dmabuf ? (int)PTR_ERR(g_dmabuf) : -ENOMEM);
				g_dmabuf = NULL;
			} else {
				rc = dma_buf_vmap(g_dmabuf, &map);
				pr_info("drm: mirror: vmap %d, via dma-buf %d\n", vmap_rc, rc);
				if (rc != 0) {
					dma_buf_put(g_dmabuf);
					g_dmabuf = NULL;
				}
			}
		}
	}
	if (rc != 0) {
		pr_info("drm: mirror: cannot map the framebuffer (%d)\n", rc);
		drm_client_framebuffer_delete(g_buffer);
		drm_client_release(&g_client);
		return 0;
	}

	{
		u32 pitch = g_buffer->fb ? g_buffer->fb->pitches[0]
		                         : g_fb_width * 4;
		paint(map.vaddr, g_fb_width, g_fb_height, pitch / 4);
	}

	/*
	 * Copy to the local display BEFORE committing.
	 *
	 * The frame is already in the GPU's framebuffer at this point, so the
	 * picture does not depend on the commit succeeding — and the commit drives
	 * a great deal of hardware that can fail on a machine with nothing plugged
	 * in. Presenting first means a commit failure costs the modeset, not the
	 * image.
	 */
	present_to_local_display(&map, out_w, out_h);

	/*
	 * Point every modeset at this framebuffer, then commit — the real atomic
	 * modeset, on the real hardware.
	 *
	 * b1nix.mirror-noplane leaves the framebuffer off, which is how the pipe
	 * is told apart from the plane: if the display engine still faults with no
	 * plane to fetch, the fault is not about the memory the plane reads.
	 */
	if (!lkpi_bootflag("b1nix.mirror-noplane"))
		/*
		 * Only the CRTCs that were given a mode.
		 *
		 * A modeset entry exists for every CRTC, and the ones without a mode
		 * are the pipes the commit is about to turn off. Handing those a
		 * framebuffer describes a disabled pipe that is nonetheless scanning
		 * out of memory — the core says so itself with WARN_ON(set->fb) on the
		 * disable path — and the commit then works from a state no hardware
		 * configuration corresponds to.
		 */
		for (unsigned i = 0; g_client.modesets && g_client.modesets[i].crtc; i++)
			if (g_client.modesets[i].mode)
				g_client.modesets[i].fb = g_buffer->fb;

	{
		unsigned long saved_debug = __drm_debug;

		u64 irqs_before = lkpi_device_irq_count();

		pr_info("drm: mirror: interrupts %s at commit, %llu device IRQs\n",
		        lkpi_irqs_enabled() ? "on" : "OFF",
		        (unsigned long long)irqs_before);

		/*
		 * Only when asked for.
		 *
		 * The commit prints a few hundred lines with this mask on, through a
		 * 115200 serial port — about a second of output interleaved with the
		 * steps of a modeset whose own waits are measured in tens of
		 * milliseconds. Turning it on to watch the commit is what made the
		 * commit miss flip_done, so the observation destroyed what it was
		 * meant to observe.
		 */
		if (lkpi_bootflag("b1nix.drm-debug"))
			__drm_debug |= MIRROR_DEBUG_KMS;
		rc = drm_client_modeset_commit(&g_client);
		__drm_debug = saved_debug;
		/*
		 * How many interrupts arrived while the commit ran.
		 *
		 * The commit ends by waiting for flip_done, which is signalled from the
		 * vblank interrupt. That wait timing out has two causes that look
		 * identical from here: the pipe is not raising the interrupt, or it is
		 * and nothing delivers it. A count of zero across a commit that spans
		 * several frames settles which.
		 */
		pr_info("drm: mirror: %llu device IRQs during commit\n",
		        (unsigned long long)(lkpi_device_irq_count() - irqs_before));
		/* What the display engine was actually told to fetch — see
		 * kernel/lkpi/i915_display_probe.c. Absent unless i915 is built. */
		if (lkpi_i915_dump_plane_surface)
			lkpi_i915_dump_plane_surface(dev);
		if (lkpi_i915_dump_pipe_state)
			lkpi_i915_dump_pipe_state(dev);
		/* Whether the vblank the commit waited for is armed, raised and
		 * counted — see lkpi_i915_dump_vblank_state(). */
		if (lkpi_i915_dump_vblank_state)
			lkpi_i915_dump_vblank_state(dev);
		if (lkpi_i915_dump_infoframes)
			lkpi_i915_dump_infoframes(dev);
		if (lkpi_i915_dump_port_state)
			lkpi_i915_dump_port_state(dev);

		/*
		 * Proving the picture without a witness.
		 *
		 * The pipe's own CRC is the only readback of what the port is being
		 * fed. Two samples of the frame just committed must agree — a stable
		 * image gives a stable CRC — and a third, taken after the framebuffer
		 * has been overwritten with a different pattern, must differ. Together
		 * those say the plane is fetching this buffer and the pipe is emitting
		 * what is in it, which is everything up to the cable.
		 *
		 * The buffer is repainted afterwards, because the frame left on the
		 * monitor should be the one worth looking at.
		 */
		if (lkpi_i915_crc_begin && lkpi_i915_crc_sample &&
		    lkpi_i915_crc_end && rc == 0 && g_active) {
			u32 pitch_px = (g_buffer->fb ? g_buffer->fb->pitches[0]
			                             : g_fb_width * 4) / 4;
			u32 same_a, same_b, changed;

			lkpi_i915_crc_begin(dev);
			same_a = lkpi_i915_crc_sample(dev);
			same_b = lkpi_i915_crc_sample(dev);

			/* A flat mid-grey: as different from a bordered gradient as a
			 * frame can be, and cheap to write. */
			for (u32 y = 0; y < g_fb_height; y++) {
				u32 *row = (u32 *)map.vaddr + (usize)y * pitch_px;

				for (u32 x = 0; x < g_fb_width; x++)
					row[x] = 0x00404040;
			}
			clflush_cache_range(map.vaddr,
			                    (usize)g_fb_height * pitch_px * 4);
			changed = lkpi_i915_crc_sample(dev);

			paint(map.vaddr, g_fb_width, g_fb_height, pitch_px);
			clflush_cache_range(map.vaddr,
			                    (usize)g_fb_height * pitch_px * 4);
			lkpi_i915_crc_end(dev);

			pr_info("drm: mirror: pipe CRC %08x %08x, after repaint %08x "
			        "(stable %d, responds %d)\n",
			        same_a, same_b, changed,
			        (int)(same_a && same_a == same_b),
			        (int)(changed && changed != same_a));
		}
	}

	/*
	 * Copy to the local display whether or not the commit succeeded.
	 *
	 * The commit drives the wire; the framebuffer is already painted either
	 * way. On a machine with nothing plugged in the commit is expected to fail
	 * somewhere past the pipe, and refusing to show the frame because of that
	 * would hide the very thing this exists to show.
	 */
	{
		int presented = g_active ? 1 : 0;

		if (g_dmabuf) {
			dma_buf_vunmap(g_dmabuf, &map);
			dma_buf_put(g_dmabuf);
			g_dmabuf = NULL;
		} else {
			drm_client_buffer_vunmap(g_buffer);
		}

		pr_info("drm: mirror %ux%u -> %ux%u: commit %d, presented %d\n",
		        g_fb_width, g_fb_height, out_w, out_h, rc, presented);
		return presented;
	}
}
