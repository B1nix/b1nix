/* SPDX-License-Identifier: GPL-2.0-only
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
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <uapi/drm/virtgpu_drm.h>
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
#include <linux/pci.h>
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
	/* The virgl resource this object backs, or 0 for a plain dumb buffer.
	 * A GEM handle is what Mesa passes around; the host knows only resource
	 * ids, and this is where one becomes the other. */
	u32 res_id;
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
	/* Tell the host before the pages go: the resource points at them, and
	 * freeing the memory a live resource is attached to leaves the host
	 * writing into whatever takes their place. */
	if (bo->res_id)
		lkpi_virgl_unref(bo->res_id);
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
	/* The parent is a REAL pci_dev, not a bare device.
	 *
	 * The core hands drm_device->dev straight to sysfs and to anything that
	 * wants the hardware's identity, and every such reader gets there with
	 * to_pci_dev() -- a container_of, which cannot fail and cannot check. A
	 * parent that was only a struct device therefore had its neighbouring
	 * members read as vendor, device and slot, and b1nix published the result
	 * as fact: 0000:0000, which is precisely the id Mesa refuses to match a
	 * driver against. The fix is not to guess better at the far end; it is for
	 * the device to be what the far end is entitled to assume it is. */
	struct pci_dev pdev;
	struct pci_bus pci_bus;
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

	if (!fb)
		return;

	obj = fb->obj[0];
	if (!obj)
		return;
	if (drm_gem_vmap_unlocked(obj, &map) != 0)
		return;

	const u32 *src = map.vaddr;
	u32 w = fb->width, h = fb->height;
	struct drm_rect damage;
	u32 dx = 0, dy = 0, dw = w, dh = h;

	/*
	 * What actually changed. A compositor that redraws a cursor and one
	 * widget attaches damage clips to the plane; forwarding them turns a
	 * frame from a whole-screen transfer into a few kilobytes. A commit that
	 * carries no clips is reported by the helper as the whole plane, so the
	 * conservative answer is what arrives here by itself — and the full frame
	 * stays the fallback for a state the helper declines to iterate, because
	 * presenting too much is a cost and presenting too little is a bug.
	 */
	if (drm_atomic_helper_damage_merged(old_state, state, &damage)) {
		if (damage.x1 < 0)
			damage.x1 = 0;
		if (damage.y1 < 0)
			damage.y1 = 0;
		if (damage.x2 > (int)w)
			damage.x2 = (int)w;
		if (damage.y2 > (int)h)
			damage.y2 = (int)h;
		if (damage.x2 > damage.x1 && damage.y2 > damage.y1) {
			dx = (u32)damage.x1;
			dy = (u32)damage.y1;
			dw = (u32)(damage.x2 - damage.x1);
			dh = (u32)(damage.y2 - damage.y1);
		}
	}

	g_record.presented = 1;
	g_record.width = w;
	g_record.height = h;
	g_record.top_left = src[0];
	g_record.top_right = src[w - 1];
	g_record.bottom_left = src[(usize)(h - 1) * (fb->pitches[0] / 4)];
	g_record.bottom_right =
		src[(usize)(h - 1) * (fb->pitches[0] / 4) + (w - 1)];
	g_record.centre = src[(usize)(h / 2) * (fb->pitches[0] / 4) + (w / 2)];

	lkpi_scanout_present(src, w, h, dx, dy, dw, dh);
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

/* ── the virtgpu ioctls Mesa speaks ──────────────────────────────────────
 *
 * The generic DRM ioctls get a client as far as opening the node. Rendering
 * needs the driver's own: Mesa's virgl winsys asks GETPARAM whether 3D is
 * there, reads the capset, creates resources, maps them, submits virgl command
 * streams and transfers pixels in both directions. Those are these.
 *
 * Every resource is a GEM object, because that is what a `bo_handle` means to
 * the caller: the handle is per-file and refcounted by the DRM core, and the
 * resource id the host knows is carried inside the object. Nothing here hands
 * a raw resource id to userspace.
 *
 * One virgl context serves the node. Per-file contexts are what CONTEXT_INIT
 * exists for and are not implemented; a client that asks for one is told so
 * rather than being given the shared one under a different name.
 */
#define B1NIX_VIRGL_CTX 3

/*
 * What Mesa asked for and what it was told.
 *
 * A client that cannot create a screen reports one line -- "failed to create
 * dri2 screen" -- for a sequence of half a dozen ioctls, so from the outside
 * every failure looks the same. b1nix.virgl-trace prints each call and its
 * answer, which is the difference between knowing WHICH call refused and
 * guessing.
 */
static int virgl_trace(void)
{
	static int on = -1;

	if (on < 0)
		on = lkpi_bootflag("b1nix.virgl-trace");
	return on;
}

#define VIRGL_TRACE(fmt, ...)                                                  \
	do {                                                                   \
		if (virgl_trace())                                             \
			pr_info("virgl: " fmt "\n", ##__VA_ARGS__);            \
	} while (0)

/*
 * Is the accelerated path offered at all?
 *
 * It is complete up to one defect that is NOT in this file: the first real use
 * of upstream's drm_mm range allocator -- inserting a SECOND mmap offset --
 * faults inside drm_mm_insert_node_in_range, in the shim's augmented rbtree.
 * Until that is fixed, a client that takes this path renders one object and
 * then takes the machine down with it.
 *
 * So the offer is behind b1nix.virgl-drm. Off, GETPARAM answers "no 3D" and
 * Mesa falls back to software cleanly, which is the designed behaviour and is
 * safe. On, the path runs for whoever is working on it. Answering "yes" by
 * default while knowing it crashes would be the worst of the three.
 */
static int b1nix_virgl_offered(void)
{
	static int on = -1;

	if (on < 0)
		on = lkpi_bootflag("b1nix.virgl-drm");
	return on && lkpi_virgl_available();
}

static int b1nix_virgl_ctx_ready(void)
{
	static int made;

	if (!b1nix_virgl_offered())
		return 0;
	if (!made) {
		if (lkpi_virgl_ctx_create(B1NIX_VIRGL_CTX) < 0)
			return 0;
		made = 1;
	}
	return 1;
}

static struct b1nix_gem *b1nix_gem_lookup(struct drm_file *file, u32 handle)
{
	struct drm_gem_object *obj = drm_gem_object_lookup(file, handle);

	return obj ? to_b1nix_gem(obj) : 0;
}

/*
 * GETPARAM writes THROUGH the pointer in `value`, it does not fill the field.
 *
 * This is the part of the ABI that is easy to get backwards and impossible to
 * notice from the kernel side: `struct drm_virtgpu_getparam.value` is a user
 * pointer, and upstream copies a 4-byte int to it. Filling the field instead
 * makes every query "succeed" while the caller reads whatever was on its own
 * stack -- so Mesa was told 3D was unavailable by its own zeroed variable,
 * gave up before asking anything else, and reported only "failed to create
 * dri2 screen". The size matters as much as the direction: an int, not a u64.
 */
static int b1nix_ioctl_getparam(struct drm_device *dev, void *data,
				struct drm_file *file)
{
	struct drm_virtgpu_getparam *args = data;
	int value = 0;

	(void)file;
	switch (args->param) {
	case VIRTGPU_PARAM_3D_FEATURES:
		VIRGL_TRACE("driver %s %d.%d.%d", dev->driver->name,
			    dev->driver->major, dev->driver->minor,
			    dev->driver->patchlevel);
		/* Answer with what is true, not with what makes Mesa proceed:
		 * claiming 3D on a device that cannot render turns a clean
		 * fallback to software into an outright failure. */
		value = b1nix_virgl_offered() ? 1 : 0;
		break;
	case VIRTGPU_PARAM_CAPSET_QUERY_FIX:
		value = 1;
		break;
	case VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs: {
		/* The end of the conversation, if it is zero: told there is no
		 * capset, Mesa concludes there is no context it could create
		 * and stops without asking what a capset contains. The ids come
		 * from the host's own replies, never from a guess here. */
		u64 mask = 0;

		if (lkpi_virgl_capset_ids(&mask) < 0)
			mask = 0;
		value = (int)mask;
		break;
	}
	default:
		/* Blob resources, cross-device sharing, context init, fenced
		 * rings: not implemented, and said so. */
		value = 0;
		break;
	}

	VIRGL_TRACE("getparam %llu -> %d", (unsigned long long)args->param,
		    value);
	if (lkpi_copy_to_user((void *)(usize)args->value, &value,
			      sizeof(value)) != 0)
		return -EFAULT;
	return 0;
}

static int b1nix_ioctl_get_caps(struct drm_device *dev, void *data,
				struct drm_file *file)
{
	struct drm_virtgpu_get_caps *args = data;

	(void)dev;
	(void)file;
	/* The capset is what tells Mesa which virgl features the host offers.
	 * It comes from the host, never from here: a fabricated one would make
	 * Mesa emit commands the host rejects, which is worse than no
	 * acceleration at all. */
	void *blob;
	u32 len = args->size;
	int ret;

	VIRGL_TRACE("get_caps id=%u ver=%u size=%u", args->cap_set_id,
		    args->cap_set_ver, args->size);
	if (args->size == 0 || args->size > 64u * 1024u)
		return -EINVAL;
	if (!b1nix_virgl_offered())
		return -ENODEV;

	blob = kzalloc(args->size, GFP_KERNEL);
	if (!blob)
		return -ENOMEM;
	if (lkpi_virgl_capset(0, args->cap_set_id, args->cap_set_ver, blob,
			      &len) < 0) {
		VIRGL_TRACE("get_caps: host refused");
		kfree(blob);
		return -EIO;
	}
	VIRGL_TRACE("get_caps -> %u bytes", len);
	ret = lkpi_copy_to_user((void *)(usize)args->addr, blob, len) == 0 ? 0
									  : -EFAULT;
	kfree(blob);
	return ret;
}

static int b1nix_ioctl_resource_create(struct drm_device *dev, void *data,
				       struct drm_file *file)
{
	struct drm_virtgpu_resource_create *args = data;
	struct lkpi_virgl_res_desc d;
	struct b1nix_gem *bo;
	u64 *phys = 0;
	u32 handle = 0;
	u32 res_id;
	usize size;
	int ret;

	VIRGL_TRACE("res_create %ux%u fmt=%u bind=0x%x target=%u", args->width,
		    args->height, args->format, args->bind, args->target);
	if (!b1nix_virgl_ctx_ready())
		return -ENODEV;
	if (args->width == 0)
		return -EINVAL;

	/* The host validates transfers against this size, so it has to be the
	 * size of the memory actually attached, rounded to whole pages -- a
	 * mapping is only ever handed out in pages. */
	size = args->size ? args->size
			  : (usize)args->width * (args->height ? args->height : 1) * 4u;
	size = (size + PAGE_SIZE - 1) & ~(usize)(PAGE_SIZE - 1);
	if (size == 0)
		return -EINVAL;

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return -ENOMEM;
	bo->npages = size / PAGE_SIZE;
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
	memset(bo->vaddr, 0, size);

	res_id = lkpi_virgl_res_alloc();
	if (!res_id) {
		ret = -ENOSPC;
		goto out_free;
	}

	phys = kzalloc(sizeof(u64) * bo->npages, GFP_KERNEL);
	if (!phys) {
		ret = -ENOMEM;
		goto out_res;
	}
	for (usize i = 0; i < bo->npages; i++)
		phys[i] = page_to_phys(bo->pages[i]);

	d.target = args->target;
	d.format = args->format;
	d.bind = args->bind;
	d.width = args->width;
	d.height = args->height;
	d.depth = args->depth;
	d.array_size = args->array_size;
	d.last_level = args->last_level;
	d.nr_samples = args->nr_samples;
	d.flags = args->flags;

	if (lkpi_virgl_res_create(B1NIX_VIRGL_CTX, &d, phys, (u32)bo->npages,
				  res_id) < 0) {
		VIRGL_TRACE("res_create res=%u FAILED", res_id);
		ret = -EIO;
		goto out_phys;
	}
	VIRGL_TRACE("res_create res=%u ok (%u pages)", res_id, (u32)bo->npages);
	kfree(phys);
	phys = 0;
	bo->res_id = res_id;

	drm_gem_private_object_init(dev, &bo->base, size);
	bo->base.funcs = &b1nix_gem_funcs;
	ret = drm_gem_create_mmap_offset(&bo->base);
	if (ret)
		goto out_obj;
	ret = drm_gem_handle_create(file, &bo->base, &handle);
	/* The handle owns the object now; this reference was only for us. */
	drm_gem_object_put(&bo->base);
	if (ret)
		return ret;

	args->bo_handle = handle;
	args->res_handle = res_id;
	args->size = (u32)size;
	if (args->stride == 0)
		args->stride = args->width * 4u;
	return 0;

out_obj:
	lkpi_virgl_unref(res_id);
	bo->res_id = 0;
	drm_gem_object_put(&bo->base);
	return ret;
out_phys:
	kfree(phys);
out_res:
	lkpi_virgl_unref(res_id);
out_free:
	lkpi_vunmap(bo->vaddr);
	shmem_free_pages(bo->pages, bo->npages);
	kfree(bo);
	return ret;
}

static int b1nix_ioctl_map(struct drm_device *dev, void *data,
			   struct drm_file *file)
{
	struct drm_virtgpu_map *args = data;
	struct b1nix_gem *bo = b1nix_gem_lookup(file, args->handle);

	(void)dev;
	if (!bo)
		return -ENOENT;
	/* The offset is a key into the node's mmap space, not an address. */
	args->offset = drm_vma_node_offset_addr(&bo->base.vma_node);
	drm_gem_object_put(&bo->base);
	return 0;
}

static int b1nix_ioctl_resource_info(struct drm_device *dev, void *data,
				     struct drm_file *file)
{
	struct drm_virtgpu_resource_info *args = data;
	struct b1nix_gem *bo = b1nix_gem_lookup(file, args->bo_handle);

	(void)dev;
	if (!bo)
		return -ENOENT;
	args->res_handle = bo->res_id;
	args->size = (u32)bo->base.size;
	args->blob_mem = 0;
	drm_gem_object_put(&bo->base);
	return 0;
}

static int b1nix_ioctl_execbuffer(struct drm_device *dev, void *data,
				  struct drm_file *file)
{
	struct drm_virtgpu_execbuffer *args = data;
	u32 *cmd;
	int ret;

	(void)dev;
	(void)file;
	if (!b1nix_virgl_ctx_ready())
		return -ENODEV;
	if (args->size == 0 || (args->size & 3))
		return -EINVAL;
	if (args->size > 512u * 1024u)
		return -EINVAL;

	cmd = kzalloc(args->size, GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;
	if (lkpi_copy_from_user(cmd, (const void *)(usize)args->command,
				args->size) != 0) {
		kfree(cmd);
		return -EFAULT;
	}
	VIRGL_TRACE("execbuffer %u bytes, %u bo handles", args->size,
		    args->num_bo_handles);
	ret = lkpi_virgl_submit(B1NIX_VIRGL_CTX, cmd, args->size) < 0 ? -EIO : 0;
	VIRGL_TRACE("execbuffer -> %d", ret);
	kfree(cmd);
	return ret;
}

static int b1nix_transfer(struct drm_file *file, u32 bo_handle,
			  const struct drm_virtgpu_3d_box *box, u32 level,
			  u32 offset, int to_host)
{
	struct b1nix_gem *bo = b1nix_gem_lookup(file, bo_handle);
	u32 box6[6];
	int ret;

	if (!bo)
		return -ENOENT;
	if (!bo->res_id) {
		/* A dumb buffer has no host-side resource: there is nothing to
		 * transfer to or from, and saying so beats pretending. */
		drm_gem_object_put(&bo->base);
		return -EINVAL;
	}
	box6[0] = box->x;
	box6[1] = box->y;
	box6[2] = box->z;
	box6[3] = box->w;
	box6[4] = box->h;
	box6[5] = box->d;
	ret = lkpi_virgl_transfer(to_host, B1NIX_VIRGL_CTX, bo->res_id, level,
				  box6, offset) < 0
		      ? -EIO
		      : 0;
	VIRGL_TRACE("transfer %s res=%u %ux%u -> %d", to_host ? "to" : "from",
		    bo->res_id, box->w, box->h, ret);
	drm_gem_object_put(&bo->base);
	return ret;
}

static int b1nix_ioctl_transfer_to_host(struct drm_device *dev, void *data,
					struct drm_file *file)
{
	struct drm_virtgpu_3d_transfer_to_host *args = data;

	(void)dev;
	return b1nix_transfer(file, args->bo_handle, &args->box, args->level,
			      args->offset, 1);
}

static int b1nix_ioctl_transfer_from_host(struct drm_device *dev, void *data,
					  struct drm_file *file)
{
	struct drm_virtgpu_3d_transfer_from_host *args = data;

	(void)dev;
	return b1nix_transfer(file, args->bo_handle, &args->box, args->level,
			      args->offset, 0);
}

static int b1nix_ioctl_wait(struct drm_device *dev, void *data,
			    struct drm_file *file)
{
	struct drm_virtgpu_3d_wait *args = data;
	struct b1nix_gem *bo = b1nix_gem_lookup(file, args->handle);

	(void)dev;
	if (!bo)
		return -ENOENT;
	/* Submissions on this path complete before the transport returns -- the
	 * command is fenced and waited on inside vgpu_submit_stream -- so by the
	 * time a client asks, the work it is asking about is done. When
	 * submission becomes asynchronous this has to grow a real wait, and the
	 * comment is here so that is not forgotten. */
	VIRGL_TRACE("wait handle=%u", args->handle);
	drm_gem_object_put(&bo->base);
	return 0;
}

static const struct drm_ioctl_desc b1nix_drm_ioctls[] = {
	DRM_IOCTL_DEF_DRV(VIRTGPU_GETPARAM, b1nix_ioctl_getparam,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VIRTGPU_GET_CAPS, b1nix_ioctl_get_caps,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VIRTGPU_RESOURCE_CREATE, b1nix_ioctl_resource_create,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VIRTGPU_RESOURCE_INFO, b1nix_ioctl_resource_info,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VIRTGPU_MAP, b1nix_ioctl_map, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VIRTGPU_EXECBUFFER, b1nix_ioctl_execbuffer,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VIRTGPU_TRANSFER_TO_HOST,
			  b1nix_ioctl_transfer_to_host, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VIRTGPU_TRANSFER_FROM_HOST,
			  b1nix_ioctl_transfer_from_host, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VIRTGPU_WAIT, b1nix_ioctl_wait, DRM_RENDER_ALLOW),
};

static const struct drm_driver b1nix_drm_driver = {
	/* DRIVER_RENDER: the driver ioctls below are served on the render node,
	 * which is what a client that only wants to draw opens -- it needs no
	 * master lease and no modeset rights. */
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC |
			   DRIVER_RENDER,
	/* The name is not decoration: it is the FIRST thing Mesa's loader matches,
	 * ahead of the PCI id, and it looks for "<name>_dri.so". Called "b1nix" it
	 * sent every client hunting for a b1nix_dri.so that does not exist and
	 * never will, while the driver for the hardware actually behind this device
	 * -- virtio-gpu -- sat unused in the image. The device is a virtio GPU; the
	 * honest name is the one its driver is called by everywhere else. */
	.name = "virtio_gpu",
	.desc = "virtio GPU",
	.date = "20260808",
	/* 0.1, which is what upstream's virtio_gpu reports. A driver that takes
	 * another driver's name should answer its version too, and some versions
	 * of Mesa's virgl winsys refuse a major other than 0 outright. It was
	 * NOT what stopped this client -- changing it alone changed nothing --
	 * so it is here for correctness rather than as the fix. */
	.major = 0,
	.minor = 1,
	.dumb_create = b1nix_dumb_create,
	.ioctls = b1nix_drm_ioctls,
	.num_ioctls = (int)(sizeof(b1nix_drm_ioctls) / sizeof(b1nix_drm_ioctls[0])),
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
	 * b1nix's virtio-gpu is not modelled as a `struct pci_dev`, so this driver
	 * owns one and fills it from the enumerator: the real bus address and the
	 * real vendor/device ids of the function the scanout is driving. Anything
	 * downstream that reaches for the hardware's identity then finds the
	 * hardware's identity. */
	{
		u16 vendor = 0, device = 0;
		u8 bus = 0, slot = 0, func = 0;

		if (lkpi_scanout_pci_id(&vendor, &device, &bus, &slot, &func) != 0)
			return -ENODEV;
		b->pdev.bus_nr = bus;
		b->pdev.slot = slot;
		b->pdev.func = func;
		b->pdev.devfn = (unsigned)((slot << 3) | func);
		b->pdev.vendor = vendor;
		b->pdev.device = device;
		/* virtio's subsystem ids carry the device type; 0x0010 is the GPU,
		 * which is what a virtio driver matches on for the legacy id. */
		b->pdev.subsystem_vendor = vendor;
		b->pdev.subsystem_device = 0x0010;
		pci_read_config_byte(&b->pdev, 0x08, &b->pdev.revision);
		b->pdev.class = 0x030000; /* display / VGA-compatible */
		/* Imported code reads pdev->bus->number rather than bus_nr; a null
		 * bus pointer is a fault waiting for the first reader. */
		b->pci_bus.number = bus;
		b->pdev.bus = &b->pci_bus;
		b->pdev.lkpi_is_pci = LKPI_PCI_DEV_MAGIC;
		device_initialize(&b->pdev.dev);
		dev_set_name(&b->pdev.dev, "0000:%02x:%02x.%u", bus, slot, func);
	}

	drm = drm_dev_alloc(&b1nix_drm_driver, &b->pdev.dev);
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

	/* Advertise FB_DAMAGE_CLIPS, so a compositor can say which part of the
	 * frame it redrew instead of every commit meaning "all of it". */
	drm_plane_enable_fb_damage_clips(&b->pipe.plane);

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
