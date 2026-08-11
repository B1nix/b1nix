/* SPDX-License-Identifier: MIT
 *
 * M101t: the imported DRM core's ioctl surface, driven from ring 3.
 *
 * The kernel-side proof in kernel/lkpi/drm_b1nix_kms.c showed that upstream's
 * modeset machinery runs — but it ran it from inside the kernel, through
 * drm_client. Nothing crossed a system call. This does: it opens the node, and
 * every step after that is the same sequence libdrm performs, against the same
 * argument structures, taken from the pinned upstream headers rather than from
 * a copy that could quietly disagree with them.
 *
 * The last check is the one that matters. Every ioctl here can return 0 while
 * the image never leaves the buffer it was drawn into, so success is not
 * "the calls succeeded": the test paints a known pattern, commits it, and then
 * reads the debugfs record the *plane update* wrote at the far end of the
 * commit — dimensions and five pixels. A modeset that reports success without
 * moving an image fails here.
 */

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define CARD "/dev/dri/card1"
#define RECORD "/sys/kernel/debug/dri/b1nix-scanout"

#define PIX_TL 0xFF102030u
#define PIX_TR 0xFF405060u
#define PIX_BL 0xFF708090u
#define PIX_BR 0xFFA0B0C0u
#define PIX_C  0xFF00FF00u

static int failures;

static void ok(const char *name) { printf("M101T-DRM: ok %s\n", name); }

static void fail(const char *name, long detail)
{
	printf("M101T-DRM: FAIL %s detail=%ld\n", name, detail);
	failures++;
}

static void report(const char *name, int good, long detail)
{
	if (good)
		ok(name);
	else
		fail(name, detail);
}

/* Read one unsigned field out of the record file. Returns -1 when the field is
 * absent, which is distinct from it being zero — an absent field means the
 * driver never wrote the record, and reporting that as 0 would turn "nothing
 * was displayed" into a plausible pixel value. */
static long record_field(const char *text, const char *key)
{
	const char *p = strstr(text, key);
	if (!p)
		return -1;
	p += strlen(key);
	while (*p == ' ')
		p++;
	if (!*p)
		return -1;
	return (long)strtoul(p, 0, 16);
}

static long record_dec(const char *text, const char *key)
{
	const char *p = strstr(text, key);
	if (!p)
		return -1;
	p += strlen(key);
	while (*p == ' ')
		p++;
	if (!*p)
		return -1;
	return (long)strtol(p, 0, 10);
}

int main(void)
{
	int fd = open(CARD, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fail("open", errno);
		printf("M101T-DRM: done\n");
		return 1;
	}
	ok("open");

	/* ── the device identifies itself ───────────────────────────── */
	{
		char name[32] = { 0 }, date[32] = { 0 }, desc[64] = { 0 };
		struct drm_version v;

		memset(&v, 0, sizeof(v));
		v.name = name;
		v.name_len = sizeof(name) - 1;
		v.date = date;
		v.date_len = sizeof(date) - 1;
		v.desc = desc;
		v.desc_len = sizeof(desc) - 1;
		if (ioctl(fd, DRM_IOCTL_VERSION, &v) != 0)
			fail("version", errno);
		else
			report("version", strcmp(name, "b1nix") == 0, (long)v.version_major);
	}

	/* ── resources: what the device has ─────────────────────────── */
	uint32_t crtc_id = 0, connector_id = 0;
	{
		struct drm_mode_card_res res;
		uint32_t crtcs[8], connectors[8], encoders[8], fbs[8];

		memset(&res, 0, sizeof(res));
		if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
			fail("getresources-count", errno);
			goto out;
		}
		if (res.count_crtcs == 0 || res.count_connectors == 0) {
			fail("getresources-count", (long)res.count_crtcs);
			goto out;
		}
		/* The two-pass shape libdrm uses: ask for counts, then hand back
		 * buffers of exactly that size. A core that ignored the second pass
		 * would leave these zero. */
		res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
		res.connector_id_ptr = (uint64_t)(uintptr_t)connectors;
		res.encoder_id_ptr = (uint64_t)(uintptr_t)encoders;
		res.fb_id_ptr = (uint64_t)(uintptr_t)fbs;
		if (res.count_crtcs > 8)
			res.count_crtcs = 8;
		if (res.count_connectors > 8)
			res.count_connectors = 8;
		if (res.count_encoders > 8)
			res.count_encoders = 8;
		if (res.count_fbs > 8)
			res.count_fbs = 8;
		if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
			fail("getresources", errno);
			goto out;
		}
		crtc_id = crtcs[0];
		connector_id = connectors[0];
		report("getresources", crtc_id != 0 && connector_id != 0,
		       (long)res.count_connectors);
	}

	/* ── the connector's mode ───────────────────────────────────── */
	struct drm_mode_modeinfo mode;
	memset(&mode, 0, sizeof(mode));
	{
		struct drm_mode_get_connector conn;
		struct drm_mode_modeinfo modes[16];
		uint32_t encoders[8];
		uint32_t props[32];
		uint64_t prop_values[32];

		memset(&conn, 0, sizeof(conn));
		conn.connector_id = connector_id;
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) != 0) {
			fail("getconnector-count", errno);
			goto out;
		}
		if (conn.count_modes == 0) {
			fail("getconnector-count", 0);
			goto out;
		}
		conn.modes_ptr = (uint64_t)(uintptr_t)modes;
		conn.encoders_ptr = (uint64_t)(uintptr_t)encoders;
		conn.props_ptr = (uint64_t)(uintptr_t)props;
		conn.prop_values_ptr = (uint64_t)(uintptr_t)prop_values;
		if (conn.count_modes > 16)
			conn.count_modes = 16;
		if (conn.count_encoders > 8)
			conn.count_encoders = 8;
		if (conn.count_props > 32)
			conn.count_props = 32;
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) != 0) {
			fail("getconnector", errno);
			goto out;
		}
		mode = modes[0];
		report("getconnector",
		       conn.connection == 1 && mode.hdisplay > 0 && mode.vdisplay > 0,
		       (long)mode.hdisplay);
	}

	/* ── a dumb buffer, mapped ──────────────────────────────────── */
	struct drm_mode_create_dumb create;
	memset(&create, 0, sizeof(create));
	create.width = mode.hdisplay;
	create.height = mode.vdisplay;
	create.bpp = 32;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
		fail("create-dumb", errno);
		goto out;
	}
	report("create-dumb", create.handle != 0 && create.pitch >= create.width * 4,
	       (long)create.pitch);

	struct drm_mode_map_dumb map;
	memset(&map, 0, sizeof(map));
	map.handle = create.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
		fail("map-dumb", errno);
		goto out;
	}
	report("map-dumb", map.offset != 0, (long)(map.offset >> 32));

	uint32_t *px = mmap(0, (size_t)create.size, PROT_READ | PROT_WRITE,
	                    MAP_SHARED, fd, (off_t)map.offset);
	if (px == MAP_FAILED) {
		fail("mmap", errno);
		goto out;
	}

	/* Written through the mapping, read back through the mapping: proves the
	 * pages are really there before anything is asked to display them. */
	uint32_t stride = create.pitch / 4;
	for (uint32_t y = 0; y < create.height; y++)
		for (uint32_t x = 0; x < create.width; x++)
			px[(size_t)y * stride + x] = 0xFF000000u;
	px[0] = PIX_TL;
	px[create.width - 1] = PIX_TR;
	px[(size_t)(create.height - 1) * stride] = PIX_BL;
	px[(size_t)(create.height - 1) * stride + create.width - 1] = PIX_BR;
	px[(size_t)(create.height / 2) * stride + create.width / 2] = PIX_C;
	report("mmap", px[0] == PIX_TL && px[create.width - 1] == PIX_TR,
	       (long)create.size);

	/* ── a framebuffer over it, and a modeset ───────────────────── */
	struct drm_mode_fb_cmd fbcmd;
	memset(&fbcmd, 0, sizeof(fbcmd));
	fbcmd.width = create.width;
	fbcmd.height = create.height;
	fbcmd.pitch = create.pitch;
	fbcmd.bpp = 32;
	fbcmd.depth = 24;
	fbcmd.handle = create.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fbcmd) != 0) {
		fail("addfb", errno);
		goto unmap;
	}
	report("addfb", fbcmd.fb_id != 0, (long)fbcmd.fb_id);

	struct drm_mode_crtc set;
	memset(&set, 0, sizeof(set));
	set.crtc_id = crtc_id;
	set.fb_id = fbcmd.fb_id;
	set.set_connectors_ptr = (uint64_t)(uintptr_t)&connector_id;
	set.count_connectors = 1;
	set.mode = mode;
	set.mode_valid = 1;
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set) != 0) {
		fail("setcrtc", errno);
		goto unmap;
	}
	ok("setcrtc");

	/* ── what actually reached the scanout ──────────────────────── */
	{
		char text[512];
		int rfd = open(RECORD, O_RDONLY);
		ssize_t n = rfd >= 0 ? read(rfd, text, sizeof(text) - 1) : -1;

		if (rfd >= 0)
			close(rfd);
		if (n <= 0) {
			fail("scanout-record", rfd < 0 ? errno : 0);
		} else {
			text[n] = 0;
			int good = record_dec(text, "presented=") == 1 &&
			           record_dec(text, "width=") == (long)create.width &&
			           record_dec(text, "height=") == (long)create.height &&
			           record_field(text, "tl=") == (long)PIX_TL &&
			           record_field(text, "tr=") == (long)PIX_TR &&
			           record_field(text, "bl=") == (long)PIX_BL &&
			           record_field(text, "br=") == (long)PIX_BR &&
			           record_field(text, "c=") == (long)PIX_C;
			if (!good) {
				/* The record itself, so a mismatch says which pixel and
				 * what arrived instead of only that something differed. */
				for (char *p = text; *p; p++)
					if (*p == '\n')
						*p = ' ';
				printf("M101T-DRM: record [%s]\n", text);
			}
			report("scanout-pixels", good, record_dec(text, "width="));
		}
	}

	/* ── a page flip, and the event it owes us ──────────────────── */
	{
		struct drm_mode_crtc_page_flip flip;

		memset(&flip, 0, sizeof(flip));
		flip.crtc_id = crtc_id;
		flip.fb_id = fbcmd.fb_id;
		flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
		flip.user_data = 0x101701;
		if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) != 0) {
			fail("page-flip", errno);
		} else {
			char buf[128];
			ssize_t n = read(fd, buf, sizeof(buf));

			if (n < (ssize_t)sizeof(struct drm_event_vblank)) {
				/* errno, not the return value: a short read and a refused
				 * one are different failures and need different fixes. */
				fail("flip-event", n < 0 ? (long)errno : (long)n);
			} else {
				struct drm_event_vblank ev;

				memcpy(&ev, buf, sizeof(ev));
				report("flip-event",
				       ev.base.type == DRM_EVENT_FLIP_COMPLETE &&
				           ev.user_data == 0x101701,
				       (long)ev.base.type);
			}
		}
	}

	/* ── teardown ───────────────────────────────────────────────── */
	{
		uint32_t fb_id = fbcmd.fb_id;
		struct drm_mode_destroy_dumb destroy;

		if (ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_id) != 0)
			fail("rmfb", errno);
		else
			ok("rmfb");

		memset(&destroy, 0, sizeof(destroy));
		destroy.handle = create.handle;
		if (ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) != 0)
			fail("destroy-dumb", errno);
		else
			ok("destroy-dumb");
	}

unmap:
	munmap(px, (size_t)create.size);
out:
	close(fd);
	printf("M101T-DRM: done (%d failure%s)\n", failures,
	       failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
