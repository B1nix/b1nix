/* SPDX-License-Identifier: MIT
 *
 * M101: a real driver on the imported DRM core, scanning out through virtio-gpu.
 *
 * Linking the core proves the symbols resolved. Calling a few of its helpers
 * proves the shim's data structures behave. Neither proves the thing the
 * milestone is actually about: that upstream's *machinery* runs here — mode
 * configuration, the atomic state machine, GEM objects and handles, framebuffer
 * creation, the probe helpers, and an in-kernel client driving all of it.
 *
 * So this is a driver, written the way a vendor driver is written and standing
 * on nothing but the imported core:
 *
 *   - a drm_device registered with drm_dev_register, so it goes through minor
 *     allocation and the sysfs class;
 *   - a connector reporting one fixed mode, the size virtio-gpu is scanning out;
 *   - a simple display pipe, which is upstream's CRTC + encoder + primary plane
 *     assembled by drm_simple_kms_helper;
 *   - dumb buffers backed by plain kernel memory, exposed as GEM objects with
 *     handles, mapped through drm_gem_vmap;
 *   - drm_client, the in-kernel modeset client — the same path fbdev emulation
 *     takes — to probe the connector, pick the mode, allocate a framebuffer and
 *     commit it atomically.
 *
 * The pipe's update callback is the only place b1nix appears: it takes the
 * pixels the committed framebuffer points at and hands them to the scanout.
 * Everything between the client's commit and that callback is upstream's code
 * running unmodified.
 *
 * The check the self-test makes is deliberately not "the calls returned 0". It
 * writes a known pattern into the buffer, commits, and then asks the scanout
 * what it actually received: dimensions, and the pixels at four corners and the
 * centre. A commit that returns success without the pixels reaching the display
 * is the failure this is built to catch.
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_client.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/iosys-map.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <lkpi/drm_bridge.h>
#include <lkpi/env.h>
#include <lkpi/page.h>

/* The DRM core's own init function is static and registered with module_init,
 * which the shim turns into a wrapper under a predictable name — the kernel
 * calls initcalls directly rather than a loader finding them. */
int lkpi_initcall_drm_core_init(void);

/* Defined at the bottom of this file; called from the bringup above it. */
void drm_core_bringup(void);

/* ── GEM: dumb buffers over a page array ────────────────────────── */

/*
 * Backed by individually allocated pages rather than one contiguous run, and
 * deliberately so: userspace maps these a page at a time, and a buffer that
 * happened to be physically linear would let a bug that assumes linearity pass
 * here and fail on hardware. The CPU view is a vmap over the same pages, so the
 * two never disagree about what the buffer contains.
 */
struct b1nix_gem {
	struct drm_gem_object base;
	struct page **pages;
	usize npages;
	void *vaddr;
	usize size;
};

static struct b1nix_gem *to_b1nix_gem(struct drm_gem_object *obj)
{
	return container_of(obj, struct b1nix_gem, base);
}

static void b1nix_gem_free(struct drm_gem_object *obj)
{
	struct b1nix_gem *bo = to_b1nix_gem(obj);

	drm_gem_private_object_fini(obj);
	if (bo->vaddr)
		lkpi_vunmap(bo->vaddr);
	if (bo->pages)
		shmem_free_pages(bo->pages, bo->npages);
	kfree(bo);
}

/* The driver's half of the mmap bridge: which frame backs page `index`. The
 * bridge has already resolved the fake offset to this object and checked the
 * caller is allowed it; all that is left is knowledge only the driver has. */
static int b1nix_gem_page_phys(struct drm_vma_offset_node *node, u64 index,
                               u64 *out_phys)
{
	/* This driver embeds the offset node in the object itself, so the node is
	 * the object. i915 does not — see the note on lkpi_drm_page_fn. */
	struct drm_gem_object *obj =
		container_of(node, struct drm_gem_object, vma_node);
	struct b1nix_gem *bo = to_b1nix_gem(obj);

	if (!bo->pages || index >= (u64)bo->npages)
		return -EINVAL;
	*out_phys = page_to_phys(bo->pages[(usize)index]);
	return 0;
}

static int b1nix_gem_vmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	struct b1nix_gem *bo = to_b1nix_gem(obj);

	if (!bo->vaddr)
		return -ENOMEM;
	iosys_map_set_vaddr(map, bo->vaddr);
	return 0;
}

static void b1nix_gem_vunmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	/* The mapping is the allocation itself; there is nothing to undo, and
	 * clearing the map keeps a stale pointer from being reused. */
	(void)obj;
	iosys_map_clear(map);
}

static const struct drm_gem_object_funcs b1nix_gem_funcs = {
	.free = b1nix_gem_free,
	.vmap = b1nix_gem_vmap,
	.vunmap = b1nix_gem_vunmap,
};

static int b1nix_dumb_create(struct drm_file *file, struct drm_device *dev,
                             struct drm_mode_create_dumb *args)
{
	struct b1nix_gem *bo;
	u32 handle;
	int ret;

	if (args->bpp != 32)
		return -EINVAL; /* the only format the pipe advertises */

	args->pitch = args->width * 4;
	args->size = (u64)args->pitch * args->height;
	if (!args->size)
		return -EINVAL;

	/* Whole pages: userspace maps this, and a mapping is only ever handed out
	 * a page at a time. Rounding here rather than at mmap keeps the object's
	 * size and the number of frames behind it the same number. */
	args->size = (args->size + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return -ENOMEM;
	bo->size = (usize)args->size;
	bo->npages = bo->size / PAGE_SIZE;
	bo->pages = shmem_alloc_pages(bo->npages);
	if (!bo->pages) {
		kfree(bo);
		return -ENOMEM;
	}
	bo->vaddr = lkpi_vmap(bo->pages, bo->npages, LKPI_PROT_RW);
	if (!bo->vaddr) {
		shmem_free_pages(bo->pages, bo->npages);
		kfree(bo);
		return -ENOMEM;
	}
	memset(bo->vaddr, 0, bo->size);

	bo->base.funcs = &b1nix_gem_funcs;
	drm_gem_private_object_init(dev, &bo->base, (usize)args->size);

	/* The handle is what makes this a *dumb buffer* rather than a kernel
	 * allocation: the caller names the object by handle from here on, and the
	 * core owns the object's lifetime through it. */
	ret = drm_gem_handle_create(file, &bo->base, &handle);
	drm_gem_object_put(&bo->base);
	if (ret)
		return ret;

	args->handle = handle;
	return 0;
}

/* ── the display pipe ───────────────────────────────────────────── */

struct b1nix_drm {
	struct device parent;
	struct drm_device *drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct drm_client_dev client;
	u32 width, height;
};

static struct b1nix_drm g_b1nix_drm;

/* What the scanout last received, recorded for the self-test. Sampled rather
 * than copied: five pixels answer "did the right image arrive" without keeping
 * a second framebuffer alive. */
struct b1nix_drm_scanout_record {
	int presented;
	u32 width, height;
	u32 top_left, top_right, bottom_left, bottom_right, centre;
};

static struct b1nix_drm_scanout_record g_record;

/*
 * The record, published where userspace can read it.
 *
 * A test in ring 3 can drive the whole ioctl surface and still not know whether
 * anything was displayed — every call can return 0 while the image never
 * leaves the buffer it was drawn into. This file is the answer to that: it is
 * written by the plane update, at the far end of the commit, from the
 * framebuffer the atomic state actually installed. A test that reads its own
 * pattern back out of here knows the pixels crossed the whole path.
 */
static isize b1nix_scanout_show(void *ctx, char *buf, usize cap)
{
	(void)ctx;
	/* `key=value`, not `key value`: the values are hex, and a reader looking
	 * for the key "c" finds one inside "ffa0b0c0" first. The '=' is not
	 * decoration — it is what makes each key unfindable inside a value. */
	return (isize)lkpi_snprintf(buf, cap,
	                            "presented=%d\nwidth=%u\nheight=%u\n"
	                            "tl=%08x\ntr=%08x\nbl=%08x\nbr=%08x\nc=%08x\n",
	                            g_record.presented, g_record.width,
	                            g_record.height, g_record.top_left,
	                            g_record.top_right, g_record.bottom_left,
	                            g_record.bottom_right, g_record.centre);
}

static void b1nix_scanout_debugfs_register(void)
{
	void *root = lkpi_sysfs_debug_root();
	void *dir = root ? lkpi_sysfs_dir(root, "dri") : 0;

	if (!dir)
		return;
	/* Read-only, and owner-only: the contents say what is on screen. */
	lkpi_sysfs_attr(dir, "b1nix-scanout", 0400, b1nix_scanout_show, 0, 0, 0);
}

static void b1nix_pipe_enable(struct drm_simple_display_pipe *pipe,
                              struct drm_crtc_state *crtc_state,
                              struct drm_plane_state *plane_state)
{
	(void)pipe;
	(void)crtc_state;
	(void)plane_state;
	/* Nothing to power on: the scanout is already live, and this driver's job
	 * is to feed it. */
}

static void b1nix_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	(void)pipe;
}

/*
 * Where the committed framebuffer becomes visible pixels. Called by the atomic
 * helpers at the end of a commit, with the plane's new state already installed.
 */
static void b1nix_pipe_update(struct drm_simple_display_pipe *pipe,
                              struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_framebuffer *fb = state ? state->fb : 0;
	struct iosys_map map;
	struct drm_gem_object *obj;

	(void)old_state;
	if (!fb)
		return;

	obj = fb->obj[0];
	if (!obj)
		return;
	if (drm_gem_vmap_unlocked(obj, &map) != 0)
		return;

	const u32 *src = map.vaddr;
	u32 w = fb->width, h = fb->height;

	g_record.presented = 1;
	g_record.width = w;
	g_record.height = h;
	g_record.top_left = src[0];
	g_record.top_right = src[w - 1];
	g_record.bottom_left = src[(usize)(h - 1) * (fb->pitches[0] / 4)];
	g_record.bottom_right =
		src[(usize)(h - 1) * (fb->pitches[0] / 4) + (w - 1)];
	g_record.centre = src[(usize)(h / 2) * (fb->pitches[0] / 4) + (w / 2)];

	lkpi_scanout_present(src, w, h);
	drm_gem_vunmap_unlocked(obj, &map);
}

static const struct drm_simple_display_pipe_funcs b1nix_pipe_funcs = {
	.enable = b1nix_pipe_enable,
	.disable = b1nix_pipe_disable,
	.update = b1nix_pipe_update,
};

/* ── the connector ──────────────────────────────────────────────── */

static int b1nix_connector_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	/* One mode, the size the scanout is already running at. A driver that
	 * reported modes the display cannot do would commit them and then show
	 * nothing. */
	mode = drm_cvt_mode(connector->dev, g_b1nix_drm.width, g_b1nix_drm.height,
	                    60, false, false, false);
	if (!mode)
		return 0;
	mode->type |= DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);
	return 1;
}

static const struct drm_connector_helper_funcs b1nix_connector_helper_funcs = {
	.get_modes = b1nix_connector_get_modes,
};

static const struct drm_connector_funcs b1nix_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/* ── the device ─────────────────────────────────────────────────── */

static const struct drm_mode_config_funcs b1nix_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const u32 b1nix_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static const struct drm_driver b1nix_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.name = "b1nix",
	.desc = "b1nix scanout on the imported DRM core",
	.date = "20260808",
	.major = 1,
	.minor = 0,
	.dumb_create = b1nix_dumb_create,
};

static const struct drm_client_funcs b1nix_client_funcs = {
	.owner = 0,
};

/*
 * Bring the device up. Split from the proof below so the failure of any single
 * step is reported as itself rather than as "the picture was wrong".
 */
static int b1nix_drm_bringup(struct b1nix_drm *b)
{
	struct drm_device *drm;
	int ret;

	if (b->drm)
		return 0; /* already up: the device outlives any one caller */

	lkpi_scanout_mode(&b->width, &b->height);
	if (!b->width || !b->height)
		return -ENODEV;

	/* The DRM device hangs off a parent device, the way a PCI driver's does.
	 * b1nix's virtio-gpu is not modelled as a `struct device`, so this driver
	 * owns one and names it after the scanout it drives — enough for the core's
	 * dev_name() and for the sysfs entry to land somewhere honest. */
	device_initialize(&b->parent);
	dev_set_name(&b->parent, "virtio-gpu");

	drm = drm_dev_alloc(&b1nix_drm_driver, &b->parent);
	if (IS_ERR(drm))
		return PTR_ERR(drm);
	b->drm = drm;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.min_width = 0;
	drm->mode_config.min_height = 0;
	drm->mode_config.max_width = b->width;
	drm->mode_config.max_height = b->height;
	drm->mode_config.funcs = &b1nix_mode_config_funcs;
	drm->mode_config.preferred_depth = 24;

	drm_connector_helper_add(&b->connector, &b1nix_connector_helper_funcs);
	ret = drm_connector_init(drm, &b->connector, &b1nix_connector_funcs,
	                         DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret)
		return ret;
	b->connector.status = connector_status_connected;

	ret = drm_simple_display_pipe_init(drm, &b->pipe, &b1nix_pipe_funcs,
	                                   b1nix_formats,
	                                   ARRAY_SIZE(b1nix_formats), 0,
	                                   &b->connector);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	/* From here the device is openable: the bridge knows which minor to hand
	 * drm_open and how to turn one of this driver's objects into a frame. */
	lkpi_drm_register_device(drm, b1nix_gem_page_phys);
	b1nix_scanout_debugfs_register();
	return 0;
}

/*
 * Bring the imported core and this device up on every boot, not only under
 * b1nix.test=1.
 *
 * The device is what userspace opens; gating it on test mode would mean the
 * node exists exactly when nothing is there to use it. The self-test below is
 * still test-only — it is a check, not a device.
 */
void drm_kms_device_init(void)
{
	drm_core_bringup();
	if (!lkpi_scanout_ready())
		return;
	int rc = b1nix_drm_bringup(&g_b1nix_drm);
	if (rc != 0)
		lkpi_printk("drm: imported-core device bringup failed (%d)\n", rc);
}

/* ── the proof ──────────────────────────────────────────────────── */

static void import_report(const char *name, int ok, u64 detail)
{
	lkpi_printk("%s%s detail=%lu\n",
	            ok ? "M101-KMS: ok " : "M101-KMS: FAIL ", name,
	            (unsigned long)detail);
}

#define PIX_TL 0xFF112233u
#define PIX_TR 0xFF445566u
#define PIX_BL 0xFF778899u
#define PIX_BR 0xFFAABBCCu
#define PIX_C  0xFF00FF00u

static int paint_pattern(struct drm_client_buffer *buffer, u32 w, u32 h)
{
	struct iosys_map map;
	u32 *px;
	usize stride;

	if (drm_client_buffer_vmap(buffer, &map) != 0)
		return -EIO;
	px = map.vaddr;
	if (!px)
		return -EIO;

	stride = buffer->fb->pitches[0] / 4;
	for (u32 y = 0; y < h; y++) {
		for (u32 x = 0; x < w; x++)
			px[(usize)y * stride + x] = 0xFF000000u;
	}
	px[0] = PIX_TL;
	px[w - 1] = PIX_TR;
	px[(usize)(h - 1) * stride] = PIX_BL;
	px[(usize)(h - 1) * stride + (w - 1)] = PIX_BR;
	px[(usize)(h / 2) * stride + (w / 2)] = PIX_C;

	drm_client_buffer_vunmap(buffer);
	return 0;
}

void drm_kms_selftest(void)
{
	struct b1nix_drm *b = &g_b1nix_drm;
	int ok = 1;

	if (!lkpi_test_mode())
		return;
	if (!lkpi_scanout_ready()) {
		/* No virtio-gpu on this run: report absence rather than success, so a
		 * missing device can never read as a passing test. */
		lkpi_printk("M101-KMS: skip no-scanout\n");
		return;
	}

	int rc = b1nix_drm_bringup(b);
	if (rc != 0) {
		import_report("device-register", 0, (u64)(-rc));
		return;
	}
	import_report("device-register", b->drm->primary != 0,
	              (u64)b->drm->mode_config.num_connector);

	/* The in-kernel client: probes the connector, chooses the mode and drives
	 * the atomic commit — upstream's own modeset path, end to end. */
	rc = drm_client_init(b->drm, &b->client, "b1nix-proof", &b1nix_client_funcs);
	if (rc != 0) {
		import_report("client-init", 0, (u64)(-rc));
		return;
	}

	rc = drm_client_modeset_probe(&b->client, b->width, b->height);
	if (rc != 0) {
		import_report("modeset-probe", 0, (u64)(-rc));
		drm_client_release(&b->client);
		return;
	}
	import_report("modeset-probe", 1, b->width);

	struct drm_client_buffer *buffer = drm_client_framebuffer_create(
		&b->client, b->width, b->height, DRM_FORMAT_XRGB8888);
	if (IS_ERR(buffer) || !buffer) {
		import_report("framebuffer-create", 0,
		              (u64)(IS_ERR(buffer) ? -PTR_ERR(buffer) : ENOMEM));
		drm_client_release(&b->client);
		return;
	}
	import_report("framebuffer-create", 1, buffer->fb ? buffer->fb->pitches[0] : 0);

	if (paint_pattern(buffer, b->width, b->height) != 0) {
		import_report("render", 0, 0);
		drm_client_framebuffer_delete(buffer);
		drm_client_release(&b->client);
		return;
	}

	/* Point the client's modeset at this framebuffer and commit it. */
	for (usize i = 0; i < (usize)b->client.modesets[0].num_connectors + 1 &&
	                  b->client.modesets;
	     i++) {
		if (!b->client.modesets[i].crtc)
			break;
		b->client.modesets[i].fb = buffer->fb;
	}

	g_record.presented = 0;
	rc = drm_client_modeset_commit(&b->client);
	if (rc != 0) {
		import_report("commit", 0, (u64)(-rc));
		ok = 0;
	} else {
		import_report("commit", 1, 0);
	}

	/* The part that matters: what reached the scanout. */
	int pixels_ok = g_record.presented && g_record.width == b->width &&
	                g_record.height == b->height &&
	                g_record.top_left == PIX_TL &&
	                g_record.top_right == PIX_TR &&
	                g_record.bottom_left == PIX_BL &&
	                g_record.bottom_right == PIX_BR &&
	                g_record.centre == PIX_C;
	import_report("scanout-pixels", pixels_ok && ok, g_record.width);

	drm_client_framebuffer_delete(buffer);
	drm_client_release(&b->client);
	lkpi_printk("M101-KMS: done\n");
}

/* Called once, before any device is brought up: the core's own initcall. */
void drm_core_bringup(void)
{
	static int done;

	if (done)
		return;
	done = 1;
	if (lkpi_initcall_drm_core_init() != 0)
		lkpi_printk("drm: core init failed\n");
}
