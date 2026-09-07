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
#include <drm/drm_modeset_lock.h>
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
extern void fb_console_set_hidden(int hidden);

/* A driver with no generic vmap maps its own: i915 (kernel/lkpi/i915_console.c). */
int __attribute__((weak)) drm_console_driver_map(struct drm_framebuffer *fb, void **vaddr)
{
	(void)fb; (void)vaddr;
	return -EOPNOTSUPP;
}

/*
 * Sleep for the whole time asked for.
 *
 * lkpi_sleep_jiffies() sleeps one jiffy and returns the remainder -- the way
 * schedule_timeout() is allowed to return early -- so imported callers loop on
 * it. Code on this side that wants a fixed delay has to loop too: the console's
 * retry did not, and its twenty "half-second" tries all ran inside a fifth of a
 * second, which is why a panel that answers at six seconds was never found.
 */
static void console_sleep_jiffies(u64 jiffies_count)
{
	while (jiffies_count)
		jiffies_count = lkpi_sleep_jiffies(jiffies_count);
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

static int console_present_seen;

/*
 * b1nix.drm-console=off      no console on a DRM display at all
 *                  =modeset  set the mode, but leave the text console on
 *                            serial: the half that says whether a wedge is in
 *                            the modeset or in the drawing that follows it
 *                  =on       the default
 */
static int console_mode(void)
{
	char v[16];

	if (!lkpi_bootopt_str("b1nix.drm-console", v, sizeof(v)) || !v[0])
		return 2;
	if (!strcmp(v, "off"))
		return 0;
	if (!strcmp(v, "modeset"))
		return 1;
	return 2;
}

static void console_present(unsigned x, unsigned y, unsigned w, unsigned h)
{
	struct drm_rect rect = { .x1 = (int)x, .y1 = (int)y,
	                         .x2 = (int)(x + w), .y2 = (int)(y + h) };
	struct console_slot *s = g_cur >= 0 ? &g_slot[g_cur] : NULL;
	if (!s || !s->buf || !s->client.dev)
		return;
	if (!drm_master_internal_acquire(s->client.dev)) {
		/* A compositor owns the display: the text console is invisible from
		 * here until it lets go, and drawing into it is pure cost. */
		fb_console_set_hidden(1);
		return;
	}
	if (!console_present_seen) {
		console_present_seen = 1;
		pr_info("drm: console: first present %ux%u+%u+%u\n", w, h, x, y);
	}
	drm_client_framebuffer_flush(s->buf, &rect);
	drm_master_internal_release(s->client.dev);
	if (console_present_seen == 1) {
		console_present_seen = 2;
		pr_info("drm: console: first present done\n");
	}
}

/*
 * Does this device actually put a DIFFERENT framebuffer on screen when asked?
 *
 * A compositor on the assigned card kept re-presenting one buffer, and from
 * outside there was no way to tell a client that will not rotate its swapchain
 * from a driver that will not take a second buffer. This asks directly, with
 * no compositor involved: two framebuffers filled with two colours, a modeset
 * onto the first, then flips between them -- and after each one, both what the
 * primary plane says it holds and what the person in front of the panel sees.
 *
 * It used to run inside the console's own set-up, which made it useless on the
 * hardware it was written for: every connector on the assigned card answers
 * "disconnected" for the first ten seconds, the console gives up, and the test
 * never ran at all. So it is its own client now, with its own connector probe,
 * its own modeset and its own thread that keeps asking until a display answers
 * (b1nix.drm-fliptest-wait seconds) -- it depends on nothing else having lit
 * the panel first.
 *
 * b1nix.drm-fliptest           run it
 * b1nix.drm-fliptest-wait=<s>  how long to wait for a connected display (30)
 * b1nix.drm-fliptest-hold=<ms> how long each buffer stays up (1000)
 * b1nix.drm-fliptest-cycles=<n> how many flips (6)
 * b1nix.drm-fliptest-bar      a moving bar instead of two flat colours
 *
 * The bar is what a camera can read. Two flat colours say nothing about
 * tearing through a lens: a rolling-shutter sensor reads its rows over a
 * thirtieth of a second, so a panel that changes colour mid-readout gives the
 * same horizontal split as a torn frame. A bar that moves a fixed step per
 * frame separates them -- the sensor shears a whole bar smoothly, while a
 * flip that lands mid-scanout breaks it into two offset pieces with a hard
 * edge between them.
 */
struct fliptest {
	struct drm_client_dev client;
	struct drm_client_buffer *buf[2];
	void *pix[2];
	unsigned width, height, pitch;
	int inited;
};
static struct fliptest g_fliptest;
static struct drm_device *fliptest_dev;

static int fliptest_map(struct drm_client_buffer *b, void **vaddr)
{
	struct iosys_map map;

	if (drm_client_buffer_vmap(b, &map) == 0) {
		*vaddr = map.vaddr;
		return 0;
	}
	return drm_console_driver_map(b->fb, vaddr);
}

/* A vertical bar at x, on black. Only the columns that change are touched:
 * the buffers live in an aperture mapping, where writing all eight megabytes
 * costs more than the frame it is meant to fit in. */
static void fliptest_bar(struct fliptest *f, int i, unsigned x, unsigned old_x,
                         unsigned bar_w)
{
	unsigned y, c;

	for (y = 0; y < f->height; y++) {
		u32 *row = (u32 *)((u8 *)f->pix[i] + (size_t)y * f->pitch);
		for (c = 0; c < bar_w; c++) {
			if (old_x + c < f->width)
				row[old_x + c] = 0x00000000u;
			if (x + c < f->width)
				row[x + c] = 0x00ffffffu;
		}
	}
}

static void fliptest_fill(struct fliptest *f, int i, u32 colour)
{
	unsigned x, y;

	for (y = 0; y < f->height; y++) {
		u32 *row = (u32 *)((u8 *)f->pix[i] + (size_t)y * f->pitch);
		for (x = 0; x < f->width; x++)
			row[x] = colour;
	}
}

/* Ask the hardware, not the cache: the cached status on an assigned card is
 * from a detect that ran before the panel answered. */
static int fliptest_probe_connectors(struct drm_device *dev)
{
	struct drm_connector *c;
	struct drm_connector_list_iter it;
	int found = 0;

	mutex_lock(&dev->mode_config.mutex);
	drm_connector_list_iter_begin(dev, &it);
	drm_for_each_connector_iter(c, &it) {
		c->status = drm_helper_probe_detect(c, NULL, true);
		c->funcs->fill_modes(c, 8192, 8192);
		if (c->status == connector_status_connected && !list_empty(&c->modes))
			found = 1;
	}
	drm_connector_list_iter_end(&it);
	mutex_unlock(&dev->mode_config.mutex);
	return found;
}

/* The test client is never registered with the core -- it drives the display
 * once and stops -- so none of these are ever called; the core still wants a
 * table. */
static void fliptest_unregister(struct drm_client_dev *client) { (void)client; }

static const struct drm_client_funcs fliptest_funcs = {
	.owner = THIS_MODULE,
	.unregister = fliptest_unregister,
};

static struct drm_crtc *fliptest_setup(struct fliptest *f, struct drm_device *dev)
{
	struct drm_crtc *crtc = NULL;
	struct drm_mode_set *ms;
	int i, ret;

	ret = drm_client_init(dev, &f->client, "b1nix-fliptest", &fliptest_funcs);
	if (ret) {
		pr_info("drm: fliptest: client init failed (%d)\n", ret);
		return NULL;
	}
	f->inited = 1;
	pr_info("drm: fliptest: client up, probing modesets\n");
	ret = drm_client_modeset_probe(&f->client, 0, 0);
	if (ret) {
		pr_info("drm: fliptest: modeset probe failed (%d)\n", ret);
		return NULL;
	}
	mutex_lock(&f->client.modeset_mutex);
	drm_client_for_each_modeset(ms, &f->client) {
		if (ms->mode && ms->crtc && ms->num_connectors) {
			f->width = ms->mode->hdisplay;
			f->height = ms->mode->vdisplay;
			crtc = ms->crtc;
			break;
		}
	}
	mutex_unlock(&f->client.modeset_mutex);
	if (!crtc || !f->width || !f->height) {
		pr_info("drm: fliptest: no usable modeset\n");
		return NULL;
	}
	pr_info("drm: fliptest: mode %ux%u, allocating\n", f->width, f->height);

	for (i = 0; i < 2; i++) {
		f->buf[i] = drm_client_framebuffer_create(&f->client, f->width,
		                                          f->height,
		                                          DRM_FORMAT_XRGB8888);
		if (IS_ERR(f->buf[i])) {
			pr_info("drm: fliptest: framebuffer %d refused (%ld)\n", i,
			        PTR_ERR(f->buf[i]));
			f->buf[i] = NULL;
			return NULL;
		}
		if (fliptest_map(f->buf[i], &f->pix[i]) || !f->pix[i]) {
			pr_info("drm: fliptest: framebuffer %d could not be mapped\n", i);
			return NULL;
		}
	}
	f->pitch = f->buf[0]->fb->pitches[0];
	pr_info("drm: fliptest: both framebuffers mapped, filling\n");
	if (lkpi_bootflag("b1nix.drm-fliptest-bar")) {
		fliptest_fill(f, 0, 0x00000000u);
		fliptest_fill(f, 1, 0x00000000u);
	} else {
		/* Two colours a person can name across a room. */
		fliptest_fill(f, 0, 0x00ff0000u); /* red   */
		fliptest_fill(f, 1, 0x0000ff00u); /* green */
	}

	mutex_lock(&f->client.modeset_mutex);
	drm_client_for_each_modeset(ms, &f->client)
		ms->fb = f->buf[0]->fb;
	mutex_unlock(&f->client.modeset_mutex);
	pr_info("drm: fliptest: committing own modeset\n");
	ret = drm_client_modeset_commit(&f->client);
	if (ret) {
		pr_info("drm: fliptest: own modeset failed (%d)\n", ret);
		return NULL;
	}
	pr_info("drm: fliptest: %ux%u on crtc %u, framebuffers %u (red) and %u (green)\n",
	        f->width, f->height, crtc->base.id, f->buf[0]->fb->base.id,
	        f->buf[1]->fb->base.id);
	return crtc;
}

static void fliptest_teardown(struct fliptest *f)
{
	int i;

	for (i = 0; i < 2; i++)
		if (f->buf[i])
			drm_client_framebuffer_delete(f->buf[i]);
	if (f->inited)
		drm_client_release(&f->client);
	memset(f, 0, sizeof(*f));
}

/* What the primary plane of this crtc holds right now. */
static u32 fliptest_plane_fb(struct drm_crtc *crtc)
{
	return (crtc->primary && crtc->primary->state && crtc->primary->state->fb)
	           ? crtc->primary->state->fb->base.id
	           : 0;
}

/*
 * One legacy page flip, with the acquire context the driver needs.
 *
 * The first shape of this held every modeset lock through drm_modeset_lock_all()
 * and then passed a NULL context down, which is what the legacy ioctl path
 * never does: an atomic driver's page flip locks the same ww_mutexes again
 * through the context it was handed, and with none it blocks on locks its own
 * caller is holding. The test thread hung on its first flip and printed
 * nothing. The locking here is now exactly what drm_mode_page_flip_ioctl does.
 */
/* Where the test thread is, for the watchdog: 0 sleeping between frames,
 * 1 about to take the modeset locks, 3 inside the driver's page flip, 2 back
 * out of it. A flip rate above the vertical blank stops the thread dead and
 * printing anything from the loop slows it enough to hide the stall, so the
 * observer has to be another thread. */
static volatile int fliptest_where;
static volatile unsigned fliptest_cycle;

static int fliptest_flip(struct drm_crtc *crtc, struct drm_framebuffer *fb,
                         u32 *plane)
{
	struct drm_device *dev = crtc->dev;
	struct drm_modeset_acquire_ctx ctx;
	int ret = 0, flip = -EOPNOTSUPP;

	DRM_MODESET_LOCK_ALL_BEGIN(dev, ctx, 0, ret);
	fliptest_where = 3;
	if (crtc->funcs && crtc->funcs->page_flip)
		flip = crtc->funcs->page_flip(crtc, fb, NULL, 0, &ctx);
	*plane = fliptest_plane_fb(crtc);
	DRM_MODESET_LOCK_ALL_END(dev, ctx, ret);
	return ret ? ret : flip;
}

static u32 fliptest_plane_fb_locked(struct drm_crtc *crtc)
{
	u32 id;

	drm_modeset_lock_all(crtc->dev);
	id = fliptest_plane_fb(crtc);
	drm_modeset_unlock_all(crtc->dev);
	return id;
}

static int fliptest_thread(void *arg)
{
	struct fliptest *f = &g_fliptest;
	struct drm_device *dev = arg;
	struct drm_crtc *crtc;
	unsigned wait_s = lkpi_bootopt_u32("b1nix.drm-fliptest-wait", 30);
	unsigned hold_ms = lkpi_bootopt_u32("b1nix.drm-fliptest-hold", 1000);
	unsigned cycles = lkpi_bootopt_u32("b1nix.drm-fliptest-cycles", 6);
	unsigned tries;
	unsigned i;
	int bar = lkpi_bootflag("b1nix.drm-fliptest-bar");
	unsigned bar_w = 120;

	pr_info("drm: fliptest: wait %u s, hold %u ms, %u cycles\n", wait_s, hold_ms,
	        cycles);

	/* A display, and no master: a compositor owns the device once it has
	 * opened it, and this test drives the hardware itself. */
	for (tries = 0; tries < wait_s; tries++) {
		int ready = 0;

		console_sleep_jiffies(100); /* a second, at the imported HZ of 100 */
		if (drm_master_internal_acquire(dev)) {
			ready = fliptest_probe_connectors(dev);
			drm_master_internal_release(dev);
		} else {
			pr_info("drm: fliptest: a master holds the device, waiting\n");
			continue;
		}
		if (ready)
			break;
	}
	if (tries >= wait_s) {
		pr_info("drm: fliptest: no display answered in %u s\n", wait_s);
		return 0;
	}
	pr_info("drm: fliptest: display answered after %u s\n", tries + 1);

	if (!drm_master_internal_acquire(dev)) {
		pr_info("drm: fliptest: a master took the device\n");
		return 0;
	}
	drm_master_internal_release(dev);
	crtc = fliptest_setup(f, dev);
	if (!crtc) {
		fliptest_teardown(f);
		return 0;
	}

	for (i = 0; i < cycles; i++) {
		struct drm_framebuffer *want = f->buf[i & 1]->fb;
		u32 got_flip = 0, got_settled = 0;
		int flip;

		/* Draw into the buffer that is NOT on screen, then show it: the
		 * order a compositor uses, and the one that puts a tear on the flip
		 * rather than on the drawing. */
		if (bar) {
			unsigned step = f->width / 24 ? f->width / 24 : 1;
			unsigned x = (i * step) % (f->width - bar_w);
			unsigned old = ((i >= 2 ? i - 2 : 0) * step) % (f->width - bar_w);

			fliptest_bar(f, (int)(i & 1), x, old, bar_w);
		}

		/* The master check is a check, not a lock to hold work under.
		 *
		 * This used to keep dev->master_mutex from before the flip until
		 * after the commit -- and drm_client_modeset_commit() takes that
		 * same mutex itself, so the thread waited for a lock it was holding
		 * and stopped for good. It only showed above about fifty flips a
		 * second, because only then did the plane sample lag far enough
		 * behind to send the test down the commit path at all. */
		if (!drm_master_internal_acquire(dev)) {
			pr_info("drm: fliptest: a master took the device mid-test\n");
			break;
		}
		drm_master_internal_release(dev);

		/* The driver's own page flip, which is what a legacy client's ioctl
		 * reaches. No event and no flags: this asks only whether the plane
		 * ends up holding what was asked for. */
		fliptest_where = 1;
		flip = fliptest_flip(crtc, want, &got_flip);
		fliptest_where = 2;

		fliptest_where = 0;
		fliptest_cycle = i;
		console_sleep_jiffies(hold_ms / 10 ? hold_ms / 10 : 1);

		/* A flip lands at the next vertical blank, so the plane is read
		 * again after the hold: sampling it the instant the ioctl returns
		 * reports the buffer that is still on screen and says nothing. */
		got_settled = fliptest_plane_fb_locked(crtc);
		if (i < 8 || got_settled != want->base.id)
			pr_info("drm: fliptest: %u asked for %u (%s), page_flip -> %d, "
			        "plane %u at once, %u after %u ms\n",
			        i, want->base.id, (i & 1) ? "green" : "red", flip,
			        got_flip, got_settled, hold_ms);
	}
	pr_info("drm: fliptest: done -- the panel should have alternated red and green\n");
	/* The buffers stay up: tearing them down would leave the panel pointing
	 * at freed pages, and this test is the last thing a fliptest boot does. */
	return 0;
}

static int fliptest_watchdog(void *arg)
{
	unsigned last = ~0u;
	int stuck = 0;

	(void)arg;
	for (;;) {
		console_sleep_jiffies(100); /* a second */
		if (fliptest_cycle == last) {
			stuck++;
			pr_info("drm: fliptest: watchdog: still on cycle %u after %d s, "
			        "thread at %d\n", fliptest_cycle, stuck, fliptest_where);
			if (stuck == 3)
				lkpi_dump_tasks(); /* who holds what, once */
		} else {
			last = fliptest_cycle;
			stuck = 0;
		}
	}
	return 0;
}

static void fliptest_start(struct drm_device *dev)
{
	if (!lkpi_bootflag("b1nix.drm-fliptest") || fliptest_dev)
		return;
	fliptest_dev = dev;
	lkpi_fs_kthread_run(fliptest_thread, dev, "drm-fliptest");
	lkpi_fs_kthread_run(fliptest_watchdog, NULL, "drm-fliptest-wd");
}

static int console_restore(struct drm_client_dev *client)
{
	int ret;
	if (g_cur < 0 || client != &g_slot[g_cur].client)
		return 0; /* a display the console has moved away from */
	ret = drm_client_modeset_commit(client);
	if (ret == 0) {
		fb_console_set_hidden(0); /* the master let go: draw again */
		fb_console_present_all();
	}
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
		console_sleep_jiffies(50); /* half a second, at the imported HZ of 100 */
		if (!console_retry_dev)
			return 0;
		if (g_cur >= 0 && g_slot[g_cur].buf)
			return 0; /* something came up in the meantime */
		/* The whole attach, not just the set-up: a slot with a framebuffer
		 * that the console was never pointed at shows nothing, which is what
		 * calling slot_setup() straight from here used to leave behind. */
		if (drm_console_attach(console_retry_dev) == 0 && g_cur >= 0) {
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
	drm_info(dev, "console: mode %ux%u, allocating\n", s->width, s->height);
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
	drm_info(dev, "console: committing modeset\n");
	ret = drm_client_modeset_commit(&s->client);
	if (ret) {
		drm_info(dev, "console: modeset failed (%d)\n", ret);
		goto fail;
	}
	drm_info(dev, "console: modeset done\n");
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
	/* Before every one of the console's own reasons to stand aside: the flip
	 * test drives the hardware itself and does not care whether this display
	 * ends up carrying the console. */
	fliptest_start(dev);
	if (console_mode() == 0)
		return 0;
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
	if (console_mode() < 2) {
		drm_info(dev, "console: mode set, text console stays on serial\n");
		g_cur = next;
		return 0;
	}
	drm_info(dev, "console: attaching the framebuffer console\n");
	fb_console_attach(g_slot[next].map.vaddr, g_slot[next].buf->fb->pitches[0],
	                  g_slot[next].width, g_slot[next].height, 32,
	                  console_present);
	g_cur = next; /* the old slot stays registered and idle: restore does
	               * nothing for a slot the console has left */
	return 0;
}
