// SPDX-License-Identifier: GPL-2.0-only
/*
 * The kernel console on a DRM display, before any compositor exists.
 *
 * What Linux calls fbdev emulation: an in-kernel DRM client takes a
 * modesettable device, sets a mode on whatever is connected, allocates one
 * framebuffer, and hands its pixels to b1nix's framebuffer console -- so the
 * boot log is on the panel from the moment the driver has probed, not a black
 * screen until the compositor's first commit. A GPU handed to the guest with
 * VFIO has no bootloader framebuffer at all; this is the only console it can
 * have.
 *
 * When a userspace master appears (kwin, sway) the client keeps its buffer but
 * stops presenting: drm_master_internal_acquire() fails while a master holds
 * the device. When that master goes away the core calls restore, the modeset
 * is committed again and the whole buffer flushed, and the log continues
 * where it was.
 *
 * A real GPU that registers after an emulated one takes the console over:
 * with a GPU handed to the guest the emulated card is a mirror nobody looks
 * at, and the panel is where the log belongs. The new display is set up
 * before the old one is let go, so the console never points at freed memory.
 *
 * Only a Linux-side unit can talk to the DRM core; the console side is
 * reached through plain-C entry points declared here by hand, which is how
 * every other shim crosses that boundary.
 */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include <drm/drm_client.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_modes.h>
#include <drm/drm_print.h>
#include <drm/drm_rect.h>
#include <linux/iosys-map.h>
#include <linux/mutex.h>
#include <linux/string.h>
#pragma clang diagnostic pop

bool drm_master_internal_acquire(struct drm_device *dev);
void drm_master_internal_release(struct drm_device *dev);

/* b1nix side, see kernel/dev/fb_console.c. */
extern int fb_console_ready(void);
extern void fb_console_attach(void *pixels, unsigned pitch, unsigned width,
                              unsigned height, unsigned bpp,
                              void (*present)(unsigned x, unsigned y,
                                              unsigned w, unsigned h));
extern void fb_console_present_all(void);

/* A driver with no generic vmap maps its own: i915 (kernel/lkpi/i915_console.c). */
int __attribute__((weak)) drm_console_driver_map(struct drm_framebuffer *fb, void **vaddr)
{
	(void)fb; (void)vaddr;
	return -EOPNOTSUPP;
}

struct console_slot {
	struct drm_client_dev client;
	struct drm_client_buffer *buf;
	struct iosys_map map;
	unsigned width, height;
	int used;
	int registered;
};
static struct console_slot g_slot[2];
static int g_cur = -1;      /* slot the console draws into, -1 = none */
static int g_bootloader_fb = -1; /* the console had a bootloader framebuffer */

static void console_present(unsigned x, unsigned y, unsigned w, unsigned h)
{
	struct drm_rect rect = { .x1 = (int)x, .y1 = (int)y,
	                         .x2 = (int)(x + w), .y2 = (int)(y + h) };
	struct console_slot *s = g_cur >= 0 ? &g_slot[g_cur] : NULL;
	if (!s || !s->buf || !s->client.dev)
		return;
	if (!drm_master_internal_acquire(s->client.dev))
		return; /* a compositor owns the display */
	drm_client_framebuffer_flush(s->buf, &rect);
	drm_master_internal_release(s->client.dev);
}

static int console_restore(struct drm_client_dev *client)
{
	int ret;
	if (g_cur < 0 || client != &g_slot[g_cur].client)
		return 0; /* a display the console has moved away from */
	ret = drm_client_modeset_commit(client);
	if (ret == 0)
		fb_console_present_all();
	return ret;
}

static int console_hotplug(struct drm_client_dev *client)
{
	(void)client;
	return 0;
}

static void console_unregister(struct drm_client_dev *client)
{
	(void)client;
}

static const struct drm_client_funcs console_funcs = {
	.owner = THIS_MODULE,
	.unregister = console_unregister,
	.restore = console_restore,
	.hotplug = console_hotplug,
};

static int is_virtual(struct drm_device *dev)
{
	return dev && dev->driver && dev->driver->name &&
	       !strcmp(dev->driver->name, "virtio_gpu");
}

/* Only for a slot that never got registered. A registered client belongs
 * to the core until the device goes away; the most this side may do with
 * one is stop drawing into it (`active` = 0). */
static void slot_release(struct console_slot *s)
{
	if (!s->used || s->registered)
		return;
	if (s->buf)
		drm_client_framebuffer_delete(s->buf);
	s->buf = NULL;
	drm_client_release(&s->client);
	memset(s, 0, sizeof(*s));
}

static int slot_setup(struct console_slot *s, struct drm_device *dev)
{
	struct drm_mode_set *ms;
	int ret;

	memset(s, 0, sizeof(*s));
	ret = drm_client_init(dev, &s->client, "b1nix-console", &console_funcs);
	if (ret) {
		drm_info(dev, "console: client init failed (%d)\n", ret);
		return ret;
	}
	s->used = 1;

	/* Ask every connector what is attached before choosing a mode: the
	 * modeset probe reads the lists this fills. What drm_fb_helper does. */
	{
		struct drm_connector *connector;
		struct drm_connector_list_iter iter;
		mutex_lock(&dev->mode_config.mutex);
		drm_connector_list_iter_begin(dev, &iter);
		drm_for_each_connector_iter(connector, &iter)
			connector->funcs->fill_modes(connector, 8192, 8192);
		drm_connector_list_iter_end(&iter);
		mutex_unlock(&dev->mode_config.mutex);
	}
	ret = drm_client_modeset_probe(&s->client, 0, 0);
	if (ret) {
		drm_info(dev, "console: modeset probe failed (%d)\n", ret);
		goto fail;
	}
	mutex_lock(&s->client.modeset_mutex);
	drm_client_for_each_modeset(ms, &s->client) {
		if (ms->mode) {
			s->width = ms->mode->hdisplay;
			s->height = ms->mode->vdisplay;
			break;
		}
	}
	mutex_unlock(&s->client.modeset_mutex);
	if (!s->width || !s->height) {
		drm_info(dev, "console: nothing connected\n");
		ret = -ENODEV;
		goto fail;
	}
	s->buf = drm_client_framebuffer_create(&s->client, s->width, s->height,
	                                       DRM_FORMAT_XRGB8888);
	if (IS_ERR(s->buf)) {
		ret = PTR_ERR(s->buf);
		s->buf = NULL;
		drm_info(dev, "console: framebuffer failed (%d)\n", ret);
		goto fail;
	}
	ret = drm_client_buffer_vmap(s->buf, &s->map);
	if (ret) {
		void *vaddr = NULL;
		int r2 = drm_console_driver_map(s->buf->fb, &vaddr);
		if (r2) {
			drm_info(dev, "console: vmap failed (%d, driver map %d)\n", ret, r2);
			goto fail;
		}
		iosys_map_set_vaddr(&s->map, vaddr);
		ret = 0;
	}
	mutex_lock(&s->client.modeset_mutex);
	drm_client_for_each_modeset(ms, &s->client)
		ms->fb = s->buf->fb;
	mutex_unlock(&s->client.modeset_mutex);
	ret = drm_client_modeset_commit(&s->client);
	if (ret) {
		drm_info(dev, "console: modeset failed (%d)\n", ret);
		goto fail;
	}
	/* Registered last: after this the core owns the client's lifetime and
	 * drm_client_release() may no longer be called on it. */
	drm_client_register(&s->client);
	s->registered = 1;
	return 0;
fail:
	slot_release(s);
	return ret;
}

int drm_console_attach(struct drm_device *dev)
{
	int next, ret;

	if (!dev)
		return 0;
	if (!dev->mode_config.num_crtc) {
		drm_info(dev, "console: no crtc yet\n");
		return 0;
	}
	if (g_bootloader_fb < 0)
		g_bootloader_fb = fb_console_ready() ? 1 : 0;
	if (g_bootloader_fb) {
		drm_info(dev, "console: the bootloader framebuffer keeps the console\n");
		return 0;
	}
	if (g_cur >= 0) {
		if (!(is_virtual(g_slot[g_cur].client.dev) && !is_virtual(dev))) {
			drm_info(dev, "console: already on %s\n",
			         g_slot[g_cur].client.dev->driver->name);
			return 0; /* already on a display as good as this one */
		}
	}
	next = g_cur < 0 ? 0 : 1 - g_cur;
	if (g_slot[next].used)
		return 0; /* both slots taken: two takeovers is not a case */
	ret = slot_setup(&g_slot[next], dev);
	if (ret)
		return ret;
	drm_info(dev, "console: %ux%u on the panel\n", g_slot[next].width,
	         g_slot[next].height);
	fb_console_attach(g_slot[next].map.vaddr, g_slot[next].buf->fb->pitches[0],
	                  g_slot[next].width, g_slot[next].height, 32,
	                  console_present);
	g_cur = next; /* the old slot stays registered and idle: restore does
	               * nothing for a slot the console has left */
	return 0;
}
