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
#include <drm/drm_probe_helper.h>
#include <linux/kthread.h>
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

/*
 * Does this device actually put a DIFFERENT framebuffer on screen when asked?
 *
 * A compositor on the assigned card kept re-presenting one buffer, and from
 * outside there was no way to tell a client that will not rotate its swapchain
 * from a driver that will not take a second buffer. This asks directly, with
 * no compositor involved: two framebuffers, a legacy page flip to each in
 * turn, and a look at what the primary plane holds afterwards.
 *
 * Under b1nix.drm-fliptest, once, after the console has its own modeset.
 */
static void console_flip_test(struct console_slot *s)
{
	struct drm_client_buffer *second;
	struct drm_crtc *crtc = NULL;
	struct drm_mode_set *ms;
	u32 first_id, second_id;
	int i;

	if (!lkpi_bootflag("b1nix.drm-fliptest") || !s->buf)
		return;

	mutex_lock(&s->client.modeset_mutex);
	drm_client_for_each_modeset(ms, &s->client)
		if (ms->crtc && ms->num_connectors)
			crtc = ms->crtc;
	mutex_unlock(&s->client.modeset_mutex);
	if (!crtc) {
		pr_info("drm: fliptest: no modeset to flip on\n");
		return;
	}

	second = drm_client_framebuffer_create(&s->client, s->width, s->height,
	                                       DRM_FORMAT_XRGB8888);
	if (IS_ERR(second)) {
		pr_info("drm: fliptest: second framebuffer refused (%ld)\n",
		        PTR_ERR(second));
		return;
	}
	first_id = s->buf->fb->base.id;
	second_id = second->fb->base.id;
	pr_info("drm: fliptest: framebuffers %u and %u on crtc %u\n", first_id,
	        second_id, crtc->base.id);

	for (i = 0; i < 4; i++) {
		struct drm_framebuffer *want = (i & 1) ? s->buf->fb : second->fb;
		u32 got;
		int ret = -EINVAL;

		/* The driver's own page flip, which is what a legacy client's ioctl
		 * reaches. No event and no flags: this asks only whether the plane
		 * ends up holding what was asked for. */
		drm_modeset_lock_all(s->client.dev);
		if (crtc->funcs && crtc->funcs->page_flip)
			ret = crtc->funcs->page_flip(crtc, want, NULL, 0, NULL);
		got = (crtc->primary && crtc->primary->state &&
		       crtc->primary->state->fb)
		          ? crtc->primary->state->fb->base.id
		          : 0;
		drm_modeset_unlock_all(s->client.dev);
		pr_info("drm: fliptest: asked for %u, page_flip -> %d, plane holds %u\n",
		        want->base.id, ret, got);
		/* A flip lands at the next vertical blank; give it one. */
		lkpi_sleep_jiffies(2);
	}
	drm_client_framebuffer_delete(second);
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

static int slot_setup(struct console_slot *s, struct drm_device *dev);
int drm_console_attach(struct drm_device *dev);

/* The retry that looks for a display that was not ready at probe time.
 *
 * A thread with a sleep rather than delayed work: re-arming a delayed work
 * from inside its own handler is the shape this needs, and it only ever ran
 * four times that way. A loop that sleeps says exactly what it does. */
static struct drm_device *console_retry_dev;

static int console_retry_thread(void *arg)
{
	int tries;

	(void)arg;
	for (tries = 0; tries < 20; tries++) {
		lkpi_sleep_jiffies(50); /* half a second, at the imported HZ of 100 */
		if (!console_retry_dev)
			return 0;
		if (g_cur >= 0 && g_slot[g_cur].buf)
			return 0; /* something came up in the meantime */
		if (slot_setup(&g_slot[0], console_retry_dev) == 0) {
			g_cur = 0;
			drm_info(console_retry_dev,
			         "console: display answered after %d tries\n", tries + 1);
			return 0;
		}
	}
	return 0;
}

static int console_hotplug(struct drm_client_dev *client)
{
	struct console_slot *s;

	if (!client || !client->dev)
		return 0;
	if (g_cur < 0 || client != &g_slot[g_cur].client)
		return 0;
	s = &g_slot[g_cur];
	if (s->buf)
		return 0; /* already showing something */

	/*
	 * Try again for a display that was not there the first time.
	 *
	 * The console probes its connectors as soon as the driver is up, and on
	 * an assigned card that is often before the panel has answered: the probe
	 * finds nothing, the console gives up for good, and the machine has no
	 * console on a monitor that works perfectly a second later -- as the
	 * compositor then demonstrates. Hotplug is exactly the event that says
	 * "look again", and doing nothing with it was the whole of the bug.
	 */
	drm_client_release(&s->client);
	memset(s, 0, sizeof(*s));
	if (slot_setup(s, client->dev) == 0)
		drm_info(client->dev, "console: display appeared, console is up\n");
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
		drm_for_each_connector_iter(connector, &iter) {
			/* Ask the hardware, do not trust what it said last time.
			 *
			 * Every connector on the assigned card reported "disconnected"
			 * for the whole of the console's start-up while the compositor,
			 * moments later, drove the same panel: the cached status was
			 * from a detect that ran before the display answered, and
			 * nothing re-ran it. A forced detect is what a client's
			 * GETCONNECTOR does, and it is what this needs too. */
			connector->status =
			    drm_helper_probe_detect(connector, NULL, true);
			connector->funcs->fill_modes(connector, 8192, 8192);
		}
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
		/*
		 * Try again in a moment rather than give up for good.
		 *
		 * On an assigned card the panel often has not answered by the time
		 * the driver finishes probing -- the console found nothing at two
		 * seconds while the compositor found the same monitor at six and
		 * drove it perfectly. Hotplug would be the right event for this, but
		 * the driver sends none here, so the console asks again a few times
		 * and then stops. Bounded, because a machine with no display must not
		 * spend the rest of its life looking for one.
		 */
		{
			/* What each connector actually said, rather than only that the
			 * answer added up to nothing: "disconnected" and "connected with
			 * no modes" are different faults with different fixes. */
			struct drm_connector *c;
			struct drm_connector_list_iter it;

			mutex_lock(&dev->mode_config.mutex);
			drm_connector_list_iter_begin(dev, &it);
			drm_for_each_connector_iter(c, &it) {
				unsigned n = 0;
				struct drm_display_mode *m;

				list_for_each_entry(m, &c->modes, head)
					n++;
				drm_info(dev, "console: connector %u status %d, %u mode(s)\n",
				         c->base.id, (int)c->status, n);
			}
			drm_connector_list_iter_end(&it);
			mutex_unlock(&dev->mode_config.mutex);
		}
		drm_info(dev, "console: nothing connected\n");
		if (!console_retry_dev) {
			console_retry_dev = dev;
			lkpi_fs_kthread_run(console_retry_thread, NULL, "drm-console-retry");
		}
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
	if (ret == 0)
		console_flip_test(s);
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
