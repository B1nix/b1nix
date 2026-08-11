/*
 * SPDX-License-Identifier: MIT
 *
 * A monitor that is not there.
 *
 * Display bring-up stops at the cable: a connector with nothing plugged into it
 * reads as disconnected, so no modes are enumerated, no CRTC is configured and
 * none of the modeset path runs. That makes every display change untestable on
 * a headless machine — which is most machines a kernel gets built on, and all
 * of them when the GPU is passed through to a VM.
 *
 * This attaches a synthetic sink: an EDID we generate, and a connector forced
 * to report connected. The driver then enumerates our modes and will run a real
 * atomic modeset against them — pipe, transcoder, planes, watermarks and all.
 *
 * What it does NOT do, and this is the important part: it does not put a signal
 * on the wire. On DP and eDP the link still has to train against a sink that
 * does not answer, and it will fail. On HDMI there is no training, so the pipe
 * runs and scans out into nothing. So this exercises everything up to the
 * physical layer and nothing beyond it — useful for the driver, not a substitute
 * for a display.
 *
 * Enabled with b1nix.fake-monitor on the kernel command line.
 */

#include <drm/drm_connector.h>
#include <drm/drm_mode.h>
#include <lkpi/drm_bridge.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_file.h> /* struct drm_minor */
#include <drm/drm_modeset_lock.h>
#include <drm/drm_modes.h>
#include <drm/drm_probe_helper.h>
#include <linux/printk.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/types.h>

/*
 * Declared here rather than by including drm_internal.h: that header lives in
 * the core's source directory, which is not an include root for this file, and
 * adding it would put every private core header in scope for a file that wants
 * one symbol. drm_minor_acquire() is the registry lookup drm_open() itself
 * uses, and it is not static.
 */
struct drm_minor *drm_minor_acquire(unsigned int minor_id);

/*
 * Same reasoning for the EDID override: it is declared in the core's private
 * drm_crtc_internal.h. It is the entry point the debugfs edid_override file
 * uses, and setting an EDID from inside the kernel is exactly what it does.
 */
int drm_edid_override_set(struct drm_connector *connector, const void *edid,
                          size_t size);

/* Assembled at runtime rather than shipped as a blob, so the timings can be
 * chosen by whoever calls this and the checksum is always right. */
#define EDID_LEN 128

static void edid_put_le16(u8 *p, u16 v)
{
	p[0] = (u8)(v & 0xff);
	p[1] = (u8)(v >> 8);
}

/*
 * One 18-byte detailed timing descriptor.
 *
 * The packing is the awkward part of EDID: each of the four values is split
 * into a low byte and a nibble in a shared high byte, so a mistake shows up as
 * a mode that is subtly the wrong size rather than as a parse failure.
 */
static void edid_detailed_timing(u8 *d, u32 clock_khz,
                                 u16 hact, u16 hblank, u16 hsync_off, u16 hsync_w,
                                 u16 vact, u16 vblank, u16 vsync_off, u16 vsync_w)
{
	edid_put_le16(&d[0], (u16)(clock_khz / 10)); /* pixel clock, 10 kHz units */
	d[2] = (u8)(hact & 0xff);
	d[3] = (u8)(hblank & 0xff);
	d[4] = (u8)(((hact >> 8) << 4) | ((hblank >> 8) & 0xf));
	d[5] = (u8)(vact & 0xff);
	d[6] = (u8)(vblank & 0xff);
	d[7] = (u8)(((vact >> 8) << 4) | ((vblank >> 8) & 0xf));
	d[8] = (u8)(hsync_off & 0xff);
	d[9] = (u8)(hsync_w & 0xff);
	d[10] = (u8)((((vsync_off & 0xf) << 4)) | (vsync_w & 0xf));
	d[11] = (u8)(((hsync_off >> 8) << 6) | (((hsync_w >> 8) & 0x3) << 4) |
	             (((vsync_off >> 4) & 0x3) << 2) | ((vsync_w >> 4) & 0x3));
	d[12] = 0; /* horizontal image size, mm — 0 means unspecified */
	d[13] = 0;
	d[14] = 0;
	d[15] = 0; /* borders */
	d[16] = 0;
	/* Digital separate sync, positive on both, non-interlaced. */
	d[17] = 0x1e;
}

/*
 * A 1920x1080@60 EDID, using the CVT reduced-blanking timings a modern panel
 * reports. Fixed rather than parameterised because the point is a plausible
 * sink, not a mode generator; the numbers are the standard ones for this mode.
 */
static void build_edid(u8 *e)
{
	int i;
	u32 sum = 0;

	for (i = 0; i < EDID_LEN; i++)
		e[i] = 0;

	/* Header: the fixed 8-byte pattern every EDID starts with. */
	e[0] = 0x00;
	for (i = 1; i <= 6; i++)
		e[i] = 0xff;
	e[7] = 0x00;

	/*
	 * Manufacturer "BNX", packed as three 5-bit letters, big-endian, with
	 * 'A' == 1. Not a registered PNP id — it is deliberately not a real
	 * vendor's, so this sink cannot be mistaken for hardware.
	 */
	e[8] = (u8)(((('B' - 'A' + 1) & 0x1f) << 2) | ((('N' - 'A' + 1) >> 3) & 0x3));
	e[9] = (u8)(((('N' - 'A' + 1) & 0x07) << 5) | (('X' - 'A' + 1) & 0x1f));
	edid_put_le16(&e[10], 0x0001); /* product code */
	e[12] = 0; e[13] = 0; e[14] = 0; e[15] = 0; /* serial */
	e[16] = 1;  /* week */
	e[17] = 34; /* year - 1990 == 2024 */
	e[18] = 1;  /* EDID version 1 */
	e[19] = 4;  /* revision 4 */

	/* Digital input, 8 bits per colour, DisplayPort. */
	e[20] = 0xa5;
	e[21] = 52; /* horizontal size, cm */
	e[22] = 30; /* vertical size, cm */
	e[23] = 0;  /* gamma: unspecified */
	/* Digital display, preferred timing is the first detailed descriptor. */
	e[24] = 0x06;

	/* Chromaticity: sRGB primaries. Values are the standard encoding. */
	e[25] = 0xee; e[26] = 0x91; e[27] = 0xa3; e[28] = 0x54;
	e[29] = 0x4c; e[30] = 0x99; e[31] = 0x26; e[32] = 0x0f;
	e[33] = 0x50; e[34] = 0x54;

	/* No established or standard timings: the detailed block below is the
	 * only mode this sink claims, which keeps what the driver picks
	 * predictable. */
	for (i = 38; i < 54; i++)
		e[i] = 0x01; /* unused standard timing slots */

	/* Detailed descriptor 1: 1920x1080 @ 60 Hz, CVT reduced blanking.
	 * 138.5 MHz, hblank 160, hsync offset 48 width 32, vblank 45,
	 * vsync offset 3 width 5. */
	edid_detailed_timing(&e[54], 138500, 1920, 160, 48, 32, 1080, 45, 3, 5);

	/* Descriptor 2: monitor name. */
	e[72] = 0x00; e[73] = 0x00; e[74] = 0x00; e[75] = 0xfc; e[76] = 0x00;
	{
		static const char name[] = "b1nix virtual";
		for (i = 0; i < 13; i++)
			e[77 + i] = name[i] ? (u8)name[i] : 0x20;
	}

	/* Descriptor 3: range limits, so a driver that validates against them
	 * finds our mode inside. */
	e[90] = 0x00; e[91] = 0x00; e[92] = 0x00; e[93] = 0xfd; e[94] = 0x00;
	e[95] = 50;   /* min vertical Hz */
	e[96] = 75;   /* max vertical Hz */
	e[97] = 30;   /* min horizontal kHz */
	e[98] = 83;   /* max horizontal kHz */
	e[99] = 17;   /* max pixel clock / 10 MHz */
	e[100] = 0x00;
	for (i = 101; i < 108; i++)
		e[i] = 0x20;

	/* Descriptor 4: unused. */
	e[108] = 0x00; e[109] = 0x00; e[110] = 0x00; e[111] = 0x10; e[112] = 0x00;
	for (i = 113; i < 126; i++)
		e[i] = 0x20;

	e[126] = 0; /* no extension blocks */

	/* The whole 128 bytes must sum to zero mod 256. */
	for (i = 0; i < EDID_LEN - 1; i++)
		sum += e[i];
	e[127] = (u8)((256 - (sum & 0xff)) & 0xff);
}

/* The first registered DRM device. Found through the minor registry rather than
 * through a driver's drvdata, so this works for any driver and needs no
 * assumption about what that pointer points at. */
struct drm_device *lkpi_drm_first_device(void)
{
	unsigned int id;

	for (id = 0; id < 64; id++) {
		struct drm_minor *minor = drm_minor_acquire(id);

		if (IS_ERR_OR_NULL(minor))
			continue;
		if (minor->dev)
			return minor->dev;
	}
	return NULL;
}

/*
 * Attach the synthetic sink to every connector that is not already reporting a
 * display, of the given type. Type 0 means "the first one that is free".
 *
 * Returns the number of connectors it was attached to.
 */
_Static_assert(LKPI_DRM_CONNECTOR_HDMIA == DRM_MODE_CONNECTOR_HDMIA,
               "the connector type b1nix asks for must be the one DRM means");

/*
 * Force a connector on and let the driver supply the modes.
 *
 * The counterpart to the synthetic sink below, for the case where a display is
 * genuinely attached but its EDID cannot be read — which is what happens to a
 * passed-through GPU whose DDC transfers time out. Upstream exposes exactly
 * this as video=<connector>:e, and it works because a forced connector with no
 * EDID falls back to the driver's standard mode list, which carries the common
 * CEA and DMT modes a monitor of this era accepts.
 *
 * Preferred over the synthetic EDID when the display is real: that EDID
 * describes a 1920x1080 at CVT reduced blanking, 138.5 MHz, and a monitor
 * expecting the CEA timing of 148.5 MHz for the same size may refuse to sync.
 * Inventing a sink is right when there is none, and wrong when there is one.
 *
 * Returns the number of connectors forced.
 */
/*
 * Give a connector an EDID we obtained by other means.
 *
 * DDC on a passed-through GPU is unreliable here — GMBUS times out and the GPIO
 * readback the bit-banged fallback needs is dead — so the display's own EDID can
 * be read once from a machine that can reach it and handed to the driver at
 * boot. This is not an invented sink: the bytes are the monitor's, and with them
 * the driver picks the monitor's native timings and, seeing the CEA extension,
 * drives HDMI rather than DVI.
 *
 * Returns 1 when it was applied.
 */
int lkpi_drm_set_connector_edid(const char *name, const void *edid, usize size)
{
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;
	struct drm_device *dev = lkpi_drm_first_device();
	int applied = 0;

	if (!dev || !name || !edid || size < EDID_LEN)
		return 0;

	drm_connector_list_iter_begin(dev, &iter);
	drm_for_each_connector_iter(connector, &iter) {
		if (!connector->name || strcmp(connector->name, name) != 0)
			continue;
		if (drm_edid_override_set(connector, edid, size) == 0)
			applied = 1;
		break;
	}
	drm_connector_list_iter_end(&iter);

	return applied;
}

int lkpi_drm_force_connector_on(const char *name)
{
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;
	struct drm_device *dev = lkpi_drm_first_device();
	int forced = 0;

	if (!dev || !name || !name[0])
		return 0;

	drm_connector_list_iter_begin(dev, &iter);
	drm_for_each_connector_iter(connector, &iter) {
		/*
		 * Matched by name, not by type or by position in the list.
		 *
		 * Which connector a cable is in is a fact about the machine, and the
		 * first connector of a type is not it: this board's monitor is on the
		 * third HDMI. A name is what the user can read off the same list the
		 * kernel prints, and it is what upstream's video= takes.
		 */
		if (!connector->name || strcmp(connector->name, name) != 0)
			continue;

		connector->force = DRM_FORCE_ON;
		forced++;
		break;
	}
	drm_connector_list_iter_end(&iter);

	if (!forced)
		pr_info("drm: no connector named %s\n", name);

	if (!forced)
		return 0;

	drm_helper_hpd_irq_event(dev);

	drm_connector_list_iter_begin(dev, &iter);
	drm_for_each_connector_iter(connector, &iter) {
		struct drm_display_mode *mode;
		int modes = 0;

		if (connector->force != DRM_FORCE_ON)
			continue;
		if (connector->funcs && connector->funcs->fill_modes)
			connector->funcs->fill_modes(connector, 4096, 4096);
		list_for_each_entry(mode, &connector->modes, head)
			modes++;
		pr_info("drm: forced %s on: status %d, %d mode(s)%s%s\n",
		        connector->name ? connector->name : "(unnamed)",
		        (int)connector->status, modes,
		        modes ? ", preferred " : "",
		        modes ? list_first_entry(&connector->modes,
		                                 struct drm_display_mode, head)->name
		              : "");
	}
	drm_connector_list_iter_end(&iter);

	return forced;
}

int lkpi_drm_attach_virtual_monitor(int connector_type)
{
	u8 edid[EDID_LEN];
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;
	struct drm_device *dev = lkpi_drm_first_device();
	int attached = 0;

	if (!dev)
		return 0;

	build_edid(edid);

	drm_connector_list_iter_begin(dev, &iter);
	drm_for_each_connector_iter(connector, &iter) {
		if (connector_type && connector->connector_type != connector_type)
			continue;
		if (connector->status == connector_status_connected)
			continue; /* something real is there; leave it alone */

		/*
		 * One sink, not one per port.
		 *
		 * Forcing every connector on drives every pipe from the same
		 * framebuffer, which is three 1080p scanouts competing for display
		 * bandwidth and buffer to prove a point that one of them already
		 * proves. It also makes any failure three times harder to read.
		 */
		if (attached)
			break;

		if (drm_edid_override_set(connector, edid, sizeof(edid)) == 0) {
			/*
			 * Forced on, because an override EDID alone does not make the
			 * connector report connected — detection asks the hardware, and
			 * the hardware correctly says nothing is plugged in.
			 */
			connector->force = DRM_FORCE_ON;
			attached++;
		}
	}
	drm_connector_list_iter_end(&iter);

	if (!attached)
		return 0;

	/*
	 * Re-probe, then report what each forced connector actually produced.
	 *
	 * Reported from here rather than left to the DRM debug mask, because that
	 * mask makes the driver print thousands of lines through a 115200 serial
	 * port and the probe no longer finishes inside a test's timeout — so the
	 * one fact worth checking became the one fact that could not be observed.
	 * A mode count of zero means the sink was attached and the driver still
	 * found nothing usable, which is a different failure from not attaching.
	 */
	drm_helper_hpd_irq_event(dev);

	drm_connector_list_iter_begin(dev, &iter);
	drm_for_each_connector_iter(connector, &iter) {
		struct drm_display_mode *mode;
		int modes = 0;

		if (connector->force != DRM_FORCE_ON)
			continue;

		/*
		 * Fill the mode list explicitly. A hotplug event re-runs detection but
		 * does not enumerate modes — that happens when something asks, which
		 * is normally userspace through the getconnector ioctl. Asking here is
		 * what makes the result checkable without a display server.
		 */
		if (connector->funcs && connector->funcs->fill_modes)
			connector->funcs->fill_modes(connector, 4096, 4096);

		list_for_each_entry(mode, &connector->modes, head)
			modes++;
		pr_info("drm: virtual monitor on %s: status %d, %d mode(s)%s%s\n",
		        connector->name ? connector->name : "(unnamed)",
		        (int)connector->status, modes,
		        modes ? ", preferred " : "",
		        modes ? list_first_entry(&connector->modes,
		                                 struct drm_display_mode, head)->name
		              : "");
	}
	drm_connector_list_iter_end(&iter);

	return attached;
}
