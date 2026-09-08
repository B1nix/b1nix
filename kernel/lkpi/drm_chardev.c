/* SPDX-License-Identifier: GPL-2.0-only
 *
 * M101t: the imported DRM core's character device, reached from b1nix's VFS.
 *
 * Everything userspace does with a GPU it does through one file descriptor:
 * open the node, ioctl for resources and buffers, mmap what came back, read the
 * events. The core already implements all of it — drm_open, drm_ioctl,
 * drm_read, drm_poll, drm_release are compiled in and unmodified. What did not
 * exist was the crossing, because b1nix's VFS is a different shape at every one
 * of those points:
 *
 *   - there is no `struct file` and no `struct inode`. Both are synthesised
 *     here; the inode exists only to carry the minor number, which is what
 *     drm_open uses to find the device.
 *   - poll reports rather than sleeps: b1nix's poll asks a handle what is ready
 *     and blocks centrally, so drm_poll's poll_wait has nothing to register and
 *     only its readiness mask is used.
 *   - mmap asks for one page's physical address at a time instead of being
 *     handed a VMA to fill. The fake offset still goes through upstream's
 *     drm_vma_offset_manager, and still through drm_vma_node_is_allowed — that
 *     check is what stops one client mapping another's buffer by guessing an
 *     offset, and dropping it would have been an easy way to make this file
 *     shorter and the system wrong.
 *
 * The driver supplies the last step, resolving an object's page to a frame,
 * because only the driver knows how its objects are backed. i915 will register
 * its own without this file changing.
 */

/* poll(2) constants and poll_table. They used to arrive through
 * <linux/types.h>; that include was removed when fs.h became the full VFS —
 * see the note there — so the users name it themselves now. */
#include <linux/poll.h>
#include <linux/pci.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_vma_manager.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/fcntl.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <lkpi/drm_bridge.h>
#include <lkpi/drmdev.h>
#include <lkpi/env.h>
#include <drm/drm_mode.h>
#include <drm/drm_crtc.h>
#include <drm/drm_plane.h>
#include <drm/drm_framebuffer.h>
#include <linux/kthread.h>
#include <asm/ioctl.h>
#include <linux/atomic.h>
#include <uapi/drm/i915_drm.h>

/* Upstream's file operations, as drm_drv.c builds them for a driver that uses
 * DEFINE_DRM_GEM_FOPS. Called directly rather than through a table: b1nix's VFS
 * dispatches on its own ops, so a file_operations struct here would be one more
 * indirection nothing reads. */
int drm_open(struct inode *inode, struct file *filp);
int drm_release(struct inode *inode, struct file *filp);
ssize_t drm_read(struct file *filp, char __user *buffer, size_t count,
                 loff_t *offset);
__poll_t drm_poll(struct file *filp, struct poll_table_struct *wait);
long drm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

/* ── the registered device ──────────────────────────────────────── */

/*
 * The registered devices, and how each one resolves its pages.
 *
 * A table rather than a single slot: with an imported vendor driver bound to
 * real hardware there is more than one DRM device in the system, and each backs
 * its objects differently — the KMS proof device owns a page array, i915 owns a
 * GTT view. A single slot meant whichever registered last answered for both,
 * and mapping one device's object through another's resolver is a wrong page,
 * not an error.
 *
 * Small and fixed: nothing here creates devices dynamically.
 */
#define LKPI_DRM_MAX_DEVICES 4
static struct {
	struct drm_device *dev;
	lkpi_drm_page_fn resolver;
} g_drm[LKPI_DRM_MAX_DEVICES];
static unsigned g_drm_count;

/* The most recently registered device, which is what the node-level helpers
 * below report. Registration order is boot order, and the last to register is
 * the one a test that just brought a device up means. */
static struct drm_device *g_dev;

extern int drm_console_attach(struct drm_device *dev);
void lkpi_drm_register_device(struct drm_device *dev, lkpi_drm_page_fn resolver)
{
	unsigned i;

	if (!dev)
		return;
	/* The boot log onto this device's display, if nothing else draws one. */
	(void)drm_console_attach(dev);
	for (i = 0; i < g_drm_count; i++) {
		if (g_drm[i].dev == dev) {
			g_drm[i].resolver = resolver;
			g_dev = dev;
			return;
		}
	}
	if (g_drm_count < LKPI_DRM_MAX_DEVICES) {
		g_drm[g_drm_count].dev = dev;
		g_drm[g_drm_count].resolver = resolver;
		g_drm_count++;
		g_dev = dev;
	}
}

/* The resolver registered for a device, or NULL if it is not one of ours. */
static lkpi_drm_page_fn lkpi_drm_resolver_for(struct drm_device *dev)
{
	unsigned i;

	for (i = 0; i < g_drm_count; i++)
		if (g_drm[i].dev == dev)
			return g_drm[i].resolver;
	return 0;
}

int lkpi_drm_have_device(void)
{
	return g_dev && g_dev->primary ? 1 : 0;
}

u32 lkpi_drm_primary_minor(void)
{
	return (g_dev && g_dev->primary) ? (u32)g_dev->primary->index : 0;
}

/* How many devices registered, and the primary minor of each — so userspace
 * gets a node per card rather than one node for whichever registered last. */
unsigned lkpi_drm_device_count(void)
{
	return g_drm_count;
}

/* The PCI identity behind a registered card.
 *
 * Every card used to be published in sysfs under one hardcoded PCI address
 * claiming to be the Intel part, whichever device it actually belonged to. With
 * two cards that makes them indistinguishable: Mesa enumerates EGL devices,
 * matches them to DRM nodes through sysfs, finds two nodes on one device, and
 * initialises the wrong one — "DRI2: failed to create screen" on a GPU that was
 * never the one being driven. Each card is described by its own device now, and
 * this is where the description comes from. */
int lkpi_drm_pci_at(unsigned index, struct lkpi_drm_pci_id *out)
{
	struct drm_device *dev;
	struct pci_dev *pdev;

	if (index >= g_drm_count || !out)
		return -EINVAL;
	dev = g_drm[index].dev;
	if (!dev || !dev->dev)
		return -ENODEV;
	/* Not to_pci_dev(): a container_of cannot tell a PCI function from
	 * anything else that happens to sit at that address, and publishing what it
	 * returns for a non-PCI parent is how every card ended up reporting
	 * 0000:0000. A parent that is not a PCI function has no PCI identity, and
	 * saying so lets the caller fall back instead of inventing one. */
	pdev = lkpi_dev_to_pci(dev->dev);
	if (!pdev)
		return -ENODEV;
	out->bus = pdev->bus_nr;
	out->slot = pdev->slot;
	out->func = pdev->func;
	out->vendor = pdev->vendor;
	out->device = pdev->device;
	out->subsystem_vendor = pdev->subsystem_vendor;
	out->subsystem_device = pdev->subsystem_device;
	out->revision = pdev->revision;
	out->pci_class = pdev->class;
	return 0;
}

int lkpi_drm_minor_at(unsigned index, u32 *out_minor)
{
	if (index >= g_drm_count || !out_minor)
		return -EINVAL;
	if (!g_drm[index].dev || !g_drm[index].dev->primary)
		return -ENODEV;
	*out_minor = (u32)g_drm[index].dev->primary->index;
	return 0;
}

/* ── open and close ─────────────────────────────────────────────── */

int lkpi_drm_open(u32 minor, u32 flags, void **out_file)
{
	struct file *filp;
	struct inode *inode;
	int ret;

	if (!out_file)
		return -EINVAL;
	*out_file = 0;
	if (!lkpi_drm_have_device())
		return -ENODEV;

	filp = kzalloc(sizeof(*filp), GFP_KERNEL);
	if (!filp)
		return -ENOMEM;
	inode = kzalloc(sizeof(*inode), GFP_KERNEL);
	if (!inode) {
		kfree(filp);
		return -ENOMEM;
	}

	/* The whole purpose of this inode: iminor() reads the low twenty bits of
	 * i_ino, and that is how drm_open selects the device. */
	inode->i_ino = minor & 0xfffffu;
	filp->f_inode = inode;
	filp->f_flags = (flags & LKPI_DRM_O_NONBLOCK) ? O_NONBLOCK : 0;
	filp->f_mode = FMODE_READ | ((flags & LKPI_DRM_O_WRITE) ? FMODE_WRITE : 0);
	atomic64_set(&filp->f_count, 1);

	ret = drm_open(inode, filp);
	if (ret == 0 && lkpi_bootflag("b1nix.drm-debug")) {
		struct drm_file *pv = filp->private_data;

		/* Whether this file is a master, and whether the core thinks it is a
		 * lessee. A file wrongly marked as one has every object filtered out of
		 * GETRESOURCES — the card then reports no connectors at all, and a
		 * compositor has nothing to light. */
		pr_info("drm: open minor %u: master %p lessor %p is_master %d\n",
		        minor, (void *)(pv ? pv->master : 0),
		        (void *)(pv && pv->master ? pv->master->lessor : 0),
		        pv ? pv->is_master : -1);
	}
	if (ret) {
		kfree(inode);
		kfree(filp);
		return ret;
	}

	*out_file = filp;
	return 0;
}

void lkpi_drm_close(void *file)
{
	struct file *filp = file;

	if (!filp)
		return;
	drm_release(filp->f_inode, filp);
	kfree(filp->f_inode);
	kfree(filp);
}

/* ── ioctl, read, poll ──────────────────────────────────────────── */

/* Remember which descriptor a DRM file was opened through.
 *
 * Cloning a file (a lease) produces a second descriptor for the same device,
 * and the clone has to look like that device to a program that stats it. The
 * clone gets there through this: fd_install finds the original handle here and
 * copies its node.
 */
/*
 * A second, independent file on the same device — what dentry_open gives the
 * lease path upstream.
 *
 * Handing back the same file instead was wrong in a way that took the kernel
 * down: the caller treats the clone's private_data as the lessee's own state
 * and reassigns its master, so a shared file left the LESSOR pointing at a
 * master that had been put. The next walk up the lease chain dereferenced it.
 */
/* The device minor a DRM file belongs to. 0 and a false return when the file is
 * not one of ours. */
int lkpi_drm_file_minor(void *file, u32 *out)
{
	struct file *f = (struct file *)file;

	if (!out || !lkpi_drm_file_is_drm(f))
		return 0;

	struct drm_file *priv = f->private_data;

	if (!priv->minor)
		return 0;
	*out = (u32)priv->minor->index;
	return 1;
}

/*
 * Is this file one of ours — a file opened on a DRM minor?
 *
 * The test cannot look at private_data, because deciding whether that pointer
 * is a struct drm_file is the entire question: on a dma-buf's file it points at
 * a struct dma_buf, and reading ->minor out of it faults on an address made of
 * the neighbouring bytes. What separates the two without dereferencing anything
 * is f_op. lkpi_drm_open leaves it null — the DRM char device dispatches on its
 * own ops and never needs one — while every anon-inode file, dma-buf above all,
 * is created with the ops that define it. A null f_op is therefore the mark of
 * a file this layer opened itself.
 */
int lkpi_drm_file_is_drm(void *file)
{
	struct file *f = (struct file *)file;

	return f && !f->f_op && f->private_data;
}

void *lkpi_drm_clone_file(void *file)
{
	struct file *f = (struct file *)file;
	void *clone = 0;

	if (!lkpi_drm_file_is_drm(f))
		return 0;

	struct drm_file *priv = f->private_data;

	if (!priv->minor)
		return 0;
	if (lkpi_drm_open((u32)priv->minor->index, 0, &clone) != 0)
		return 0;
	return clone;
}

void lkpi_drm_file_set_handle(void *file, void *handle)
{
	struct file *f = (struct file *)file;

	if (f && !f->f_handle)
		f->f_handle = handle;
}

/* drm_internal.h is not on the include path for this file — it is the core's
 * private header — and this is the one thing needed out of it. */
struct drm_minor *drm_minor_acquire(unsigned int minor_id);

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

/* The connector as userspace receives it.
 *
 * A compositor picks the mode flagged preferred and falls back to the last one
 * in the list when none is; reading 720x400 on a monitor whose EDID asks for
 * 1920x1080 is the shape of a preferred flag that did not survive the trip. The
 * kernel's own list is not the evidence — what the ioctl copied out is. */
static void debug_dump_connector_modes(void *user_arg)
{
	struct drm_mode_get_connector conn;
	struct drm_mode_modeinfo mode;
	u32 i;

	if (lkpi_copy_from_user(&conn, user_arg, sizeof(conn)) != 0)
		return;
	pr_info("drm: connector %u: %u mode(s) copied out (buffer %s)\n",
	        (unsigned)conn.connector_id, (unsigned)conn.count_modes,
	        conn.modes_ptr ? "supplied" : "absent — count only");
	if (!conn.modes_ptr)
		return;
	for (i = 0; i < conn.count_modes && i < 48; i++) {
		const void *src = (const void *)(usize)(conn.modes_ptr +
		                                        (u64)i * sizeof(mode));

		if (lkpi_copy_from_user(&mode, src, sizeof(mode)) != 0)
			return;
		mode.name[sizeof(mode.name) - 1] = 0;
		pr_info("drm:   mode %u %s %ux%u type 0x%x%s\n", (unsigned)i, mode.name,
		        (unsigned)mode.hdisplay, (unsigned)mode.vdisplay,
		        (unsigned)mode.type,
		        (mode.type & (1 << 3)) ? " PREFERRED" : "");
	}
}

/* ── EXECBUFFER2 accounting ─────────────────────────────────────── */

/*
 * What Mesa actually submits, counted at the crossing.
 *
 * "iris submits softpin-only" is a claim about the *guest*, and the only place
 * it can be checked without trusting either side is here: the argument as it
 * arrives from userspace, before the driver has looked at it. Reading it back
 * out of the driver would prove nothing about the ABI, because by then the
 * driver has already reinterpreted whatever we handed it.
 *
 * Four numbers settle the claim, and they have to be separated:
 *
 *   pinned  objects carrying EXEC_OBJECT_PINNED — the address is the client's
 *           choice and the kernel must honour it rather than assign one.
 *   relocs  the total relocation_count over every object. Softpin-only means
 *           this stays zero: one non-zero entry and the kernel would have to
 *           run the relocation path, which is a different code path with
 *           different failure modes, and the claim would be false.
 *   flags   the OR of every execbuffer2 flags word seen. Last-wins would hide
 *           the one submission in a thousand that asks for something else.
 *   fail    calls that returned an error. A run that submits nothing and a run
 *           where every submission is rejected both produce no pixels, and a
 *           call count alone cannot tell them apart.
 *
 * The structures come from the pinned import tree's uapi header, not from a
 * copy kept here: a private copy is how a kernel drifts from the ABI it claims
 * to serve without anything failing to build.
 *
 * All of it is behind b1nix.i915-execbuf, because it copies the whole object
 * array in from userspace a second time on every submission — acceptable for a
 * diagnostic run, not for the ordinary path.
 */
static atomic64_t execbuf_calls;
static atomic64_t execbuf_fail;
static atomic64_t execbuf_objs;
static atomic64_t execbuf_pinned;
static atomic64_t execbuf_relocs;
/* A plain word rather than an atomic64_t: the shim has no atomic64_or, and an
 * OR is the one operation this needs. */
static u64 execbuf_flags_seen;

/* Computed from the pinned uapi header rather than written as a literal — a
 * hand-copied 0x40406469 stays right only until the struct changes size.
 *
 * Matched on type and number alone, deliberately: the same command exists as
 * DRM_IOW and as DRM_IOWR (the _WR form, which returns an out-fence), and both
 * are the submission this is counting. */
#define B1NIX_EXECBUF2_NR (DRM_COMMAND_BASE + DRM_I915_GEM_EXECBUFFER2)

static int execbuf_is_execbuffer2(u64 request)
{
	return _IOC_TYPE((unsigned int)request) == DRM_IOCTL_BASE &&
	       _IOC_NR((unsigned int)request) == B1NIX_EXECBUF2_NR;
}

/* One line per submission while the numbers are small, then one per doubling
 * and one per 1024 after that. A GL probe submits a handful of batches and a
 * compositor submits thousands; any fixed interval either says nothing about
 * the first or drowns the log in the second. */
static int execbuf_should_report(u64 n)
{
	return (n & (n - 1)) == 0 || (n % 1024) == 0;
}

static void execbuf_account(void *user_arg, isize ret)
{
	struct drm_i915_gem_execbuffer2 eb;
	u64 objs = 0, pinned = 0, relocs = 0, i;
	u64 n;

	if (lkpi_copy_from_user(&eb, user_arg, sizeof(eb)) != 0)
		return;

	/* Bounded on purpose. A confused or hostile client can name a buffer
	 * count that describes no memory it owns, and this walk must not be what
	 * turns that into a kernel fault. 4096 is far above anything Mesa
	 * submits, so a run that reaches the cap is itself worth seeing. */
	for (i = 0; i < eb.buffer_count && i < 4096; i++) {
		struct drm_i915_gem_exec_object2 obj;
		const void *src = (const void *)(usize)(eb.buffers_ptr +
		                                        i * sizeof(obj));

		if (lkpi_copy_from_user(&obj, src, sizeof(obj)) != 0)
			break;
		objs++;
		if (obj.flags & EXEC_OBJECT_PINNED)
			pinned++;
		relocs += obj.relocation_count;
		/* Where the client asked for the object, on a submission the kernel
		 * refused.
		 *
		 * A softpin address is not a hint: if it cannot be bound there, the
		 * driver has nowhere else to put the object and answers -ENOSPC after
		 * evicting the entire address space trying. Mesa's iris lays its
		 * buffers out in memzones spread across the full 48-bit range, so an
		 * address space that is narrower than the driver reports would fail
		 * exactly here — on the first batch that reaches a high zone, with
		 * every earlier one succeeding. The offsets are the only thing that
		 * distinguishes that from genuine exhaustion. */
		if (ret < 0)
			pr_info("I915-EXECBUF:   obj[%llu] handle=%u offset=0x%llx "
			        "pad_to_size=0x%llx flags=0x%llx\n",
			        (unsigned long long)i, (unsigned)obj.handle,
			        (unsigned long long)obj.offset,
			        (unsigned long long)obj.pad_to_size,
			        (unsigned long long)obj.flags);
	}

	n = (u64)atomic64_inc_return(&execbuf_calls);
	if (ret < 0)
		atomic64_inc(&execbuf_fail);
	atomic64_add((long)objs, &execbuf_objs);
	atomic64_add((long)pinned, &execbuf_pinned);
	atomic64_add((long)relocs, &execbuf_relocs);
	__atomic_fetch_or(&execbuf_flags_seen, (u64)eb.flags, __ATOMIC_RELAXED);

	/* Every failure is reported, however many there are: a submission the
	 * kernel rejected is the whole reason this counter exists. */
	if (ret >= 0 && !execbuf_should_report(n))
		return;
	pr_info("I915-EXECBUF: call #%llu bufs=%u pinned=%llu relocs=%llu "
	        "flags=0x%llx batch_len=%u -> %d\n",
	        (unsigned long long)n, (unsigned)eb.buffer_count,
	        (unsigned long long)pinned, (unsigned long long)relocs,
	        (unsigned long long)eb.flags, (unsigned)eb.batch_len, (int)ret);
	pr_info("I915-EXECBUF: total calls=%lld fail=%lld objs=%lld pinned=%lld "
	        "relocs=%lld flags=0x%llx\n",
	        (long long)atomic64_read(&execbuf_calls),
	        (long long)atomic64_read(&execbuf_fail),
	        (long long)atomic64_read(&execbuf_objs),
	        (long long)atomic64_read(&execbuf_pinned),
	        (long long)atomic64_read(&execbuf_relocs),
	        (unsigned long long)__atomic_load_n(&execbuf_flags_seen,
	                                            __ATOMIC_RELAXED));
}

static u32 drm_primary_fb_id(void *file);

/* Print every object/property/value triple in an atomic request. */
static void drm_dump_atomic_request(void *file, void *user_arg)
{
	struct file *filp = file;
	struct drm_file *file_priv = filp ? filp->private_data : 0;
	struct drm_device *dev = file_priv ? file_priv->minor->dev : 0;

	struct {
		u32 flags;
		u32 count_objs;
		u64 objs_ptr;
		u64 count_props_ptr;
		u64 props_ptr;
		u64 prop_values_ptr;
		u64 reserved;
		u64 user_data;
	} req;
	u32 objs[64], nprops[64], flat = 0;

	if (!dev || lkpi_copy_from_user(&req, user_arg, sizeof(req)) != 0)
		return;
	if (!req.count_objs || req.count_objs > 64)
		return;
	if (lkpi_copy_from_user(objs, (void *)(usize)req.objs_ptr,
	                        req.count_objs * sizeof(u32)) != 0)
		return;
	if (lkpi_copy_from_user(nprops, (void *)(usize)req.count_props_ptr,
	                        req.count_objs * sizeof(u32)) != 0)
		return;
	for (u32 o = 0; o < req.count_objs; o++) {
		for (u32 k = 0; k < nprops[o] && flat < 64; k++, flat++) {
			u32 pid = 0;
			u64 val = 0;

			if (lkpi_copy_from_user(&pid,
			        (void *)(usize)(req.props_ptr + flat * 4u),
			        sizeof(pid)) != 0)
				return;
			if (lkpi_copy_from_user(&val,
			        (void *)(usize)(req.prop_values_ptr + flat * 8u),
			        sizeof(val)) != 0)
				return;
			{
				/* By name. A refusal reported as "property 18" needs the
				 * property table to read at all, and the table is right
				 * here. */
				struct drm_property *prop = drm_property_find(dev, 0, pid);

				pr_info("drm:   object %u %s = %llu\n", objs[o],
				        prop ? prop->name : "(unknown property)",
				        (unsigned long long)val);
			}
		}
	}
}

/* When the last atomic commit returned, for timing the event that follows. */
static u64 drm_last_commit_ns;

/* The framebuffer the request being handled names for the primary plane. */
static u64 drm_want_fb;

/* Flip completions handed to userspace, against the commits that asked for
 * them. */
unsigned drm_events_delivered;

/* Framebuffer objects created, counted against the frames presented. */
static unsigned drm_addfb_calls;

/* The cookie the last atomic apply passed in. */
static u64 drm_last_user_data;

/* The object id of the first CRTC's primary plane. An atomic request names
 * several planes and only this one carries what the screen shows. */
static u32 drm_primary_plane_id(void *file)
{
	struct file *filp = file;
	struct drm_file *file_priv = filp ? filp->private_data : 0;
	struct drm_device *dev = file_priv ? file_priv->minor->dev : 0;
	struct drm_crtc *crtc;

	if (!dev)
		return 0;
	drm_for_each_crtc(crtc, dev)
		if (crtc->state && crtc->state->active && crtc->primary)
			return crtc->primary->base.id;
	return 0;
}

/* The id of the plane FB_ID property, which is what an atomic request names
 * when it wants a different buffer on screen. */
static u32 drm_fb_id_prop(void *file)
{
	struct file *filp = file;
	struct drm_file *file_priv = filp ? filp->private_data : 0;
	struct drm_device *dev = file_priv ? file_priv->minor->dev : 0;

	if (!dev || !dev->mode_config.prop_fb_id)
		return 0;
	return dev->mode_config.prop_fb_id->base.id;
}

isize lkpi_drm_ioctl(void *file, u64 request, void *user_arg)
{
	/* 0xc05064a7: DRM_IOCTL_MODE_GETCONNECTOR. */
	if (lkpi_bootflag("b1nix.drm-debug") &&
	    (request & 0xffffffffu) == 0xc05064a7u) {
		isize r = drm_ioctl((struct file *)file, (unsigned int)request,
		                    (unsigned long)(usize)user_arg);

		if (r == 0)
			debug_dump_connector_modes(user_arg);
		return r;
	}

	/* How many buffers the client actually got.
	 *
	 * Two framebuffers alive could be a swapchain of two, or one buffer plus
	 * the cursor -- and those mean opposite things when the scanned-out buffer
	 * never changes. Counting the creations, with their size, separates them:
	 * a compositor that asked for two full-screen buffers and got them is
	 * choosing to reuse one, and a compositor that only ever asked for one was
	 * refused somewhere. 0xc02064b2 is DRM_IOCTL_MODE_CREATE_DUMB, whose
	 * request starts with HEIGHT and then width -- in that order, which is
	 * worth spelling out because reading them the other way round makes every
	 * buffer look rotated. */
	if (lkpi_bootflag("b1nix.drm-fps") && (request & 0xffffffffu) == 0xc02064b2u) {
		u32 req[3] = { 0, 0, 0 };
		isize r;

		lkpi_copy_from_user(req, user_arg, sizeof(req));
		r = drm_ioctl((struct file *)file, (unsigned int)request,
		              (unsigned long)(usize)user_arg);
		pr_info("drm: create_dumb %ux%u %ubpp -> %d\n", req[1], req[0], req[2],
		        (int)r);
		return r;
	}

	/* What the atomic request actually asks for.
	 *
	 * The plane's framebuffer does not change from one commit to the next,
	 * and the same compositor against the same driver on Linux does not
	 * behave that way -- so the question is whether the FB_ID the client sets
	 * arrives here at all. The request is four parallel arrays: the objects,
	 * how many properties each carries, then the property ids and their
	 * values, flat. Walking them says what was asked for, before any of this
	 * kernel's own state is involved. */
	if (lkpi_bootflag("b1nix.drm-fps") && (request & 0xffffffffu) == 0xc03864bcu) {
		struct {
			u32 flags;
			u32 count_objs;
			u64 objs_ptr;
			u64 count_props_ptr;
			u64 props_ptr;
			u64 prop_values_ptr;
			u64 reserved;
			u64 user_data;
		} req;
		static unsigned seen_fb_id, seen_commits;
		static u64 prim_a, prim_b;
		static unsigned prim_a_n, prim_b_n, prim_other;
		(void)prim_b_n;

		if (lkpi_copy_from_user(&req, user_arg, sizeof(req)) == 0 &&
		    req.count_objs && req.count_objs < 64) {
			u32 nprops[64];
			u32 total = 0;

			seen_commits++;
			u32 objs[64];

			if (lkpi_copy_from_user(nprops, (void *)(usize)req.count_props_ptr,
			                        req.count_objs * sizeof(u32)) == 0 &&
			    lkpi_copy_from_user(objs, (void *)(usize)req.objs_ptr,
			                        req.count_objs * sizeof(u32)) == 0) {
				u32 flat = 0;

				for (u32 i = 0; i < req.count_objs; i++)
					total += nprops[i];
				/* Walk per object, so a property can be attributed to the
				 * plane that carries it: FB_ID = 0 on the planes a
				 * compositor leaves off says nothing, and printing those
				 * first hides the one value that matters. */
				for (u32 o = 0; o < req.count_objs && flat < 128; o++) {
					for (u32 k = 0; k < nprops[o] && flat < 128; k++, flat++) {
						u32 pid = 0;
						u64 val = 0;

						if (lkpi_copy_from_user(&pid,
						        (void *)(usize)(req.props_ptr + flat * 4u),
						        sizeof(pid)) != 0)
							goto done_props;
						if (lkpi_copy_from_user(&val,
						        (void *)(usize)(req.prop_values_ptr +
						                        flat * 8u),
						        sizeof(val)) != 0)
							goto done_props;
						if (pid != drm_fb_id_prop(file) || !val)
							continue;
						seen_fb_id++;
						if (objs[o] != drm_primary_plane_id(file))
							continue;
						/* Every distinct buffer the primary plane is asked
						 * for, with how often. Printing the first few showed
						 * one id over and over, which is either a compositor
						 * reusing one buffer or a sample taken during
						 * start-up; a tally over the whole run cannot be
						 * mistaken for either. */
						/* Two fixed slots latched onto whatever the first
						 * two ids happened to be -- start-up buffers, in
						 * every run -- and every later frame fell into
						 * "others", so a compositor alternating two buffers
						 * per frame read as one buffer plus noise. What the
						 * question needs is only whether consecutive commits
						 * name the SAME buffer. */
						if (val == prim_a)
							prim_a_n++;
						else
							prim_other++;
						if (val != prim_a) {
							prim_b = prim_a;
							prim_b_n++;
							prim_a = val;
						}
						drm_want_fb = val;
					}
				}
			}
done_props:;
			if ((seen_commits % 120u) == 0)
				pr_info("drm: primary plane: %u commit(s) repeated the buffer, %u changed it (now fb %llu, was fb %llu)\n",
				        prim_a_n, prim_other, (unsigned long long)prim_a,
				        (unsigned long long)prim_b);
		}
	}

	/* Does an atomic commit change the framebuffer the primary plane shows?
	 *
	 * The scanned-out buffer never changes while flips keep completing, and
	 * the compositor keeps two framebuffers alive -- so either its request
	 * names the same one every time, or the commit does not carry it through.
	 * Sampling the plane's own state either side of the ioctl answers that
	 * without parsing the property arrays the request is built from.
	 * 0xc03864bc is DRM_IOCTL_MODE_ATOMIC. */
	if (lkpi_bootflag("b1nix.drm-fps") && (request & 0xffffffffu) == 0xc03864bcu) {
		static unsigned carried, dropped;
		u32 before = drm_primary_fb_id(file);
		isize r = drm_ioctl((struct file *)file, (unsigned int)request,
		                    (unsigned long)(usize)user_arg);
		u32 after = drm_primary_fb_id(file);

		/* A commit that asks for a DIFFERENT buffer than the one on the plane
		 * is the only one that can end the tearing, and there is exactly one
		 * of them in a run. Whether it succeeded, and whether the plane took
		 * it, is the whole question -- a compositor whose swap is refused has
		 * nowhere to go but the buffer it already has. */
		{
			/* Separate the tests from the applies. A compositor validates a
			 * configuration with TEST_ONLY before committing it, and a test
			 * that is refused is a configuration it will never try -- which
			 * looks the same from outside as one it never wanted. */
			static unsigned ok_apply, fail_apply, ok_test, fail_test;
			static u32 last_apply_flags;
			u32 flags = 0;

			lkpi_copy_from_user(&flags, user_arg, sizeof(flags));
			if (flags & 0x100u) {
				if (r < 0)
					fail_test++;
				else
					ok_test++;
			} else {
				if (r < 0)
					fail_apply++;
				else
					ok_apply++;
			}
			/* The whole flags word, not just the test bit. 0x02 is
			 * PAGE_FLIP_ASYNC: a flip that takes effect immediately instead
			 * of at the next vertical blank, which tears by design and is
			 * something a compositor only asks for when it is told the
			 * driver offers it. */
			if (!(flags & 0x100u))
				last_apply_flags = flags;
			if (((ok_apply + fail_apply + ok_test + fail_test) % 120u) == 0)
				pr_info("drm: %u applies submitted, %u completion events delivered\n",
				        ok_apply, drm_events_delivered);
			if (((ok_apply + fail_apply + ok_test + fail_test) % 120u) == 0)
				pr_info("drm: atomic applies %u ok %u failed, tests %u ok %u failed, apply flags 0x%x%s\n",
				        ok_apply, fail_apply, ok_test, fail_test,
				        last_apply_flags,
				        (last_apply_flags & 0x2u) ? " (ASYNC/tearing)" : "");
			/* One accepted request, printed the same way as a refused one.
			 * The two differ in exactly the property this side would not
			 * take, and that difference is easier to read than any amount of
			 * reasoning about which check might have run. */
			if (r >= 0 && (flags & 0x100u) && ok_test == 1) {
				pr_info("drm: atomic test ACCEPTED, flags 0x%x, it asked:\n",
				        flags);
				drm_dump_atomic_request(file, user_arg);
			}
			if (r < 0 && (fail_apply + fail_test) <= 4) {
				/* Everything the refused request asked for. EINVAL from an
				 * atomic commit names nothing by itself, and a compositor
				 * whose commit is refused carries the divergence forward --
				 * so the properties it set are the only way to see which one
				 * this side would not take. */
				pr_info("drm: atomic %s REFUSED (%d), flags 0x%x, it asked:\n",
				        (flags & 0x100u) ? "test" : "apply", (int)r, flags);
				drm_dump_atomic_request(file, user_arg);
			}
		}
		drm_want_fb = 0;

		/* When the commit returned, so the event that follows can be timed
		 * against it. A page flip completes at a vertical blank -- up to a
		 * frame away, never immediately. A completion delivered at once is a
		 * compositor told its buffer is free while the display is still
		 * reading it, which is precisely the shape of the tearing here. */
		drm_last_commit_ns = lkpi_monotonic_ns();
		{
			/* The cookie the request carries. A compositor matches a
			 * completion to the flip it belongs to by this value and releases
			 * the framebuffer that flip replaced; one that never matches
			 * keeps every buffer but the newest marked as still in flight,
			 * and has nowhere to draw but the one on screen. */
			struct {
				u32 flags;
				u32 count_objs;
				u64 objs_ptr;
				u64 count_props_ptr;
				u64 props_ptr;
				u64 prop_values_ptr;
				u64 reserved;
				u64 user_data;
			} rq;

			if (lkpi_copy_from_user(&rq, user_arg, sizeof(rq)) == 0)
				drm_last_user_data = rq.user_data;
		}

		if (before && after) {
			if (before == after)
				dropped++;
			else
				carried++;
			if (((carried + dropped) % 120u) == 0)
				pr_info("drm: atomic commits: %u changed the plane's framebuffer, %u left it\n",
				        carried, dropped);
		}
		return r;
	}

	/* Framebuffers created, per second, against the frames drawn.
	 *
	 * A compositor normally makes one of these per buffer and keeps it. One
	 * per FRAME means it is rebuilding the object every time it draws, and
	 * because ids are handed back out again the same numbers keep appearing --
	 * which is enough to make a sampler believe the screen never changed.
	 * 0xc06464b8 is DRM_IOCTL_MODE_ADDFB2. */
	if (lkpi_bootflag("b1nix.drm-fps") && (request & 0xffffffffu) == 0xc06464b8u) {
		isize r = drm_ioctl((struct file *)file, (unsigned int)request,
		                    (unsigned long)(usize)user_arg);

		drm_addfb_calls++;
		return r;
	}

	/* Mapping a dumb buffer.
	 *
	 * A compositor's swapchain is built by allocating buffers and mapping
	 * each one; the first that cannot be mapped ends the loop, and a
	 * swapchain of one buffer has nowhere to draw but the buffer on screen.
	 * 0xc01064b3 is DRM_IOCTL_MODE_MAP_DUMB: handle in, offset out. */
	if (lkpi_bootflag("b1nix.drm-fps") && (request & 0xffffffffu) == 0xc01064b3u) {
		u64 arg[2] = { 0, 0 };
		isize r;

		lkpi_copy_from_user(arg, user_arg, sizeof(arg));
		r = drm_ioctl((struct file *)file, (unsigned int)request,
		              (unsigned long)(usize)user_arg);
		lkpi_copy_from_user(arg, user_arg, sizeof(arg));
		pr_info("drm: map_dumb handle %u -> offset 0x%llx (ret %d)\n",
		        (u32)arg[0], (unsigned long long)arg[1], (int)r);
		return r;
	}

	/* Buffers and framebuffers the client throws away.
	 *
	 * A swapchain that loses a slot has nothing left to rotate to. 0xc00464af
	 * is DRM_IOCTL_MODE_RMFB and 0xc00464b4 DESTROY_DUMB; both take a single
	 * id, which is enough to say which buffer went. */
	if (lkpi_bootflag("b1nix.drm-fps") &&
	    ((request & 0xffffffffu) == 0xc00464afu ||
	     (request & 0xffffffffu) == 0xc00464b4u)) {
		u32 id = 0;
		isize r;

		lkpi_copy_from_user(&id, user_arg, sizeof(id));
		r = drm_ioctl((struct file *)file, (unsigned int)request,
		              (unsigned long)(usize)user_arg);
		pr_info("drm: %s %u -> %d\n",
		        (request & 0xffffffffu) == 0xc00464afu ? "rmfb" : "destroy_dumb",
		        id, (int)r);
		return r;
	}

	/* Every capability the client asks about, with the answer.
	 *
	 * A compositor decides whether it can use the hardware cursor from these,
	 * and it disables the cursor plane outright here -- while the same
	 * compositor on virtio-gpu keeps a framebuffer for it. A cursor drawn into
	 * the frame instead means a full repaint on every mouse movement, which is
	 * what a person sitting in front of the panel would call glitching.
	 * 0xc010640c is DRM_IOCTL_GET_CAP: capability in, value out. */
	if (lkpi_bootflag("b1nix.drm-fps") && (request & 0xffffffffu) == 0xc010640cu) {
		u64 cap[2] = { 0, 0 };
		isize r;

		lkpi_copy_from_user(cap, user_arg, sizeof(cap));
		r = drm_ioctl((struct file *)file, (unsigned int)request,
		              (unsigned long)(usize)user_arg);
		lkpi_copy_from_user(cap, user_arg, sizeof(cap));
		pr_info("drm: get_cap %llu -> %llu (ret %d)\n",
		        (unsigned long long)cap[0], (unsigned long long)cap[1], (int)r);
		return r;
	}

	/* What the compositor asks a legacy page flip to put on screen.
	 *
	 * The scanned-out framebuffer never changes while flips keep completing,
	 * which is either the client naming the same buffer every time or this
	 * side ignoring the one it named. The request carries the id, so the
	 * question is answered by reading it. struct drm_mode_crtc_page_flip is
	 * crtc_id, fb_id, flags, reserved, user_data -- the id is the second u32.
	 * 0xc01864b0 is DRM_IOCTL_MODE_PAGE_FLIP. */
	if (lkpi_bootflag("b1nix.drm-fps") && (request & 0xffffffffu) == 0xc01864b0u) {
		static u32 last_fb;
		static unsigned same, changed;
		u32 hdr[2] = { 0, 0 };

		if (lkpi_copy_from_user(hdr, user_arg, sizeof(hdr)) == 0) {
			if (hdr[1] == last_fb)
				same++;
			else
				changed++;
			last_fb = hdr[1];
			if (((same + changed) % 120u) == 0)
				pr_info("drm: page flips requested: %u to a new framebuffer, %u to the same one\n",
				        changed, same);
		}
	}

	/* Atomic commits, with the flag that decides whether they touch hardware.
	 *
	 * A compositor validates a configuration with TEST_ONLY before applying it,
	 * and both calls look identical from outside: same command, same success.
	 * If only the tests ever arrive, the screen stays dark while every ioctl
	 * reports success — which is exactly the shape of the problem here. */
	if (lkpi_bootflag("b1nix.drm-debug") && (request & 0xffffffffu) == 0xc03864bcu) {
		u32 flags = 0;

		if (lkpi_copy_from_user(&flags, user_arg, sizeof(flags)) == 0)
			pr_info("drm: atomic ioctl flags %x%s\n", (unsigned)flags,
			        (flags & 0x100) ? " (test only)" : " (apply)");
		if (!(flags & 0x100)) {
			/* The registers immediately after a commit that was applied, not
			 * fifteen seconds later: a pipe that is enabled and then torn down
			 * again looks identical to one that was never enabled if the only
			 * evidence is a periodic dump. */
			extern void lkpi_i915_dump_port_state_pub(void) __attribute__((weak));

			if (lkpi_i915_dump_port_state_pub) {
				isize r = drm_ioctl((struct file *)file, (unsigned int)request,
				                    (unsigned long)(usize)user_arg);

				pr_info("drm: applied commit -> %d, port state:\n", (int)r);
				lkpi_i915_dump_port_state_pub();
				return r;
			}
		}
	}


	struct file *filp = file;

	if (!filp)
		return -EBADF;

	isize ret = (isize)drm_ioctl(filp, (unsigned int)request,
	                             (unsigned long)(usize)user_arg);

	/* After the call, not before: the argument is counted either way, but the
	 * only way to know a submission was *accepted* is its return value. */
	if (execbuf_is_execbuffer2(request) && user_arg &&
	    lkpi_bootflag("b1nix.i915-execbuf"))
		execbuf_account(user_arg, ret);
	/* A refused ioctl, named.
	 *
	 * Userspace usually reports only that something did not work — "failed to
	 * open drm device" covers an open that succeeded and a follow-up call that
	 * did not — and the number of the call that was actually refused is the
	 * difference between a guess and a fix. Behind the debug flag, because a
	 * working session issues thousands of these per second. */
	if (ret < 0 && lkpi_bootflag("b1nix.drm-debug")) {
		pr_info("drm: ioctl 0x%08x -> %d\n", (unsigned)request, (int)ret);
		/* SET_CLIENT_CAP names the capability in its argument, and which one
		 * was refused is the whole content of the failure: a compositor probes
		 * several, and a refusal of UNIVERSAL_PLANES costs it every plane
		 * while a refusal of one it merely asked about costs nothing. The
		 * number alone cannot tell those apart. */
		if ((request & 0xffffffffu) == 0x4010640du && user_arg) {
			struct {
				u64 capability;
				u64 value;
			} cap;

			if (lkpi_copy_from_user(&cap, user_arg, sizeof(cap)) == 0)
				pr_info("drm:   set_client_cap %llu = %llu refused\n",
				        (unsigned long long)cap.capability,
				        (unsigned long long)cap.value);
		}
	}
	return ret;
}

/*
 * How the output actually behaves, once a second.
 *
 * "Torn" and "it froze" are the only descriptions available from the far side
 * of a monitor, and neither says how often a frame landed or how long the
 * longest stall was. Every completed page flip reaches the compositor as an
 * event read from this fd, so counting the reads counts presented frames, and
 * the largest gap between two of them is the freeze the eye saw.
 */
/* The framebuffer id the CRTC is scanning out, sampled when a flip completes.
 * Reported as the number of DISTINCT ids seen in the second: one means every
 * flip went to the same buffer, which is front-buffer rendering and tears by
 * construction; two or three is the double or triple buffering that does not. */
static u32 drm_fps_fb_last;
static unsigned drm_fps_fb_changes;

static unsigned drm_fps_fb_alive;
static unsigned drm_fps_fb_fullscreen;

/* The framebuffer id on the first CRTC's primary plane, or 0 if there is none
 * to read. Shared by the flip sampler and the commit sampler. */
static u32 drm_primary_fb_id(void *file)
{
	struct file *filp = file;
	struct drm_file *file_priv = filp ? filp->private_data : 0;
	struct drm_device *dev = file_priv ? file_priv->minor->dev : 0;
	struct drm_crtc *crtc;

	if (!dev)
		return 0;
	/* The CRTC that is actually on. i915 has several pipes and the one in use
	 * is not the first: taking the first read an unused plane, whose fb is
	 * NULL, and made every commit look as though it left the framebuffer
	 * alone. Every conclusion drawn from that was about the wrong plane. */
	drm_for_each_crtc(crtc, dev) {
		if (!crtc->state || !crtc->state->active)
			continue;
		if (crtc->primary && crtc->primary->state &&
		    crtc->primary->state->fb)
			return crtc->primary->state->fb->base.id;
	}
	return 0;
}

/* Every plane the device offers, once: id, type, and the pixel formats it
 * accepts. A compositor picks the cursor plane by these, and one that finds no
 * format it can use falls back to drawing the cursor into the frame. */
static void drm_dump_planes_once(struct drm_device *dev)
{
	static int done;
	struct drm_plane *plane;

	if (done || !dev)
		return;
	done = 1;
	drm_for_each_plane(plane, dev) {
		static const char *const kind[] = { "overlay", "primary", "cursor" };

		pr_info("drm: plane %u is %s, %u format(s): %.4s %.4s %.4s\n",
		        plane->base.id,
		        plane->type <= 2 ? kind[plane->type] : "?",
		        plane->format_count,
		        plane->format_count > 0 ? (const char *)&plane->format_types[0]
		                                : "----",
		        plane->format_count > 1 ? (const char *)&plane->format_types[1]
		                                : "----",
		        plane->format_count > 2 ? (const char *)&plane->format_types[2]
		                                : "----");
	}
}

static void drm_fps_note_flip(struct file *filp)
{
	struct drm_file *file_priv = filp ? filp->private_data : 0;
	struct drm_device *dev = file_priv ? file_priv->minor->dev : 0;
	struct drm_crtc *crtc;
	struct drm_framebuffer *fb;

	if (!dev)
		return;
	drm_dump_planes_once(dev);
	{
		/* How many pipes the compositor turned on. Two swapchain buffers and
		 * one framebuffer per output reads the same as one output with a
		 * single-buffer swapchain, and only this tells them apart. */
		static unsigned last_active = 999;
		unsigned active = 0;
		struct drm_crtc *c;

		drm_for_each_crtc(c, dev)
			if (c->state && c->state->active)
				active++;
		if (active != last_active) {
			last_active = active;
			pr_info("drm: %u crtc(s) active\n", active);
		}
	}
	/* How many framebuffer objects the client is keeping. Sampling which one
	 * a flip landed on says what is on screen; this says whether there was
	 * ever anything else to put there. One live framebuffer is a compositor
	 * drawing into the buffer being scanned out. */
	drm_fps_fb_alive = 0;
	drm_fps_fb_fullscreen = 0;
	mutex_lock(&dev->mode_config.fb_lock);
	{
		/* Do the two full-screen buffers actually hold different memory?
		 *
		 * Everything else about the flip path checks out, and drawing into a
		 * "back" buffer that shares pages with the front tears on every
		 * change while looking, from the outside, exactly like a swapchain
		 * that alternates. The GEM handle behind each framebuffer says it in
		 * one line. Printed once. */
		static int aliased_checked;

		if (!aliased_checked && lkpi_bootflag("b1nix.drm-tearwatch")) {
			struct drm_framebuffer *a = 0, *b = 0;
			struct drm_framebuffer *f;

			list_for_each_entry(f, &dev->mode_config.fb_list, head) {
				if (f->width < 1024)
					continue;
				if (!a)
					a = f;
				else if (!b)
					b = f;
			}
			if (a && b) {
				aliased_checked = 1;
				pr_info("drm: fb %u obj %p vs fb %u obj %p: %s\n",
				        a->base.id, (void *)a->obj[0], b->base.id,
				        (void *)b->obj[0],
				        a->obj[0] == b->obj[0] ? "SAME OBJECT"
				                               : "different objects");
			}
		}
	}
	list_for_each_entry(fb, &dev->mode_config.fb_list, head) {
		drm_fps_fb_alive++;
		/* Full-screen ones only are the swapchain. Counting every framebuffer
		 * together with the cursor's made a single-buffer swapchain look like
		 * a double-buffered one. */
		if (fb->width >= 1024)
			drm_fps_fb_fullscreen++;
	}
	mutex_unlock(&dev->mode_config.fb_lock);
	drm_for_each_crtc(crtc, dev) {
		u32 id;

		if (!crtc->state || !crtc->state->active)
			continue;
		if (!crtc->primary || !crtc->primary->state ||
		    !crtc->primary->state->fb)
			continue;
		id = crtc->primary->state->fb->base.id;
		/* Transitions, not distinct ids: what matters is whether the buffer
		 * on screen CHANGES from one completed flip to the next. A swapchain
		 * that alternates gives one transition per flip; a plane left on the
		 * same framebuffer gives none, and the compositor is then drawing
		 * into what the display is reading. */
		if (id != drm_fps_fb_last) {
			drm_fps_fb_last = id;
			drm_fps_fb_changes++;
		}
		return;
	}
}

/* What a completion event actually carries.
 *
 * A compositor frees the framebuffer a flip replaced when the event for that
 * flip arrives, and it matches the event to the flip by the user_data it
 * passed in. An event with the wrong user_data, or a sequence that never
 * advances, is one it cannot account for -- and a swapchain whose buffers are
 * never released has nothing to rotate to, which is what the plane shows. */
/*
 * Where the beam is when the compositor is handed a flip completion.
 *
 * A completion says "the buffer you flipped away from is yours again". If it
 * arrives before the hardware has actually latched the new surface, the
 * compositor starts repainting the buffer the display is still reading, and
 * the picture tears however well the flip itself was timed. Delivered right
 * after the vertical blank, the beam is at the top of the frame; spread over
 * the frame, the events are not vblank-locked at all.
 */
extern u32 lkpi_i915_scanline(void);

static void drm_event_scanline_note(void)
{
	static u64 events, early, late;
	static u32 lo = 0xffffffff, hi;
	u32 line;

	if (!lkpi_bootflag("b1nix.drm-tearwatch") &&
	    !lkpi_bootflag("b1nix.drm-eventwatch"))
		return;
	/*
	 * The completion the compositor is reading right now: has the flip it
	 * reports actually landed?
	 *
	 * PLANE_SURF holds the address the driver armed, PLANE_SURFLIVE the one
	 * the display engine is reading. While a flip is pending they differ, so
	 * an event handed over in that state tells the compositor a frame is on
	 * screen that is not, and the buffer it then considers free is the one
	 * still being scanned.
	 */
	{
		extern u32 lkpi_i915_live_surface(void);
		extern u32 lkpi_i915_armed_surface(void);
		static u64 seen, pending;
		u32 live = lkpi_i915_live_surface();
		u32 armed = lkpi_i915_armed_surface();

		if (live || armed) {
			seen++;
			/*
			 * Page granularity, because the two registers do not agree in
			 * their low bits: PLANE_SURFLIVE reads back the address with the
			 * plane's own offset folded in (a constant 0x20 here), so a raw
			 * comparison calls every completion late and proves nothing.
			 * What matters is whether the display engine is fetching the
			 * page the flip armed.
			 */
			if ((live >> 12) != (armed >> 12))
				pending++;
			if ((seen % 60) == 0)
				pr_info("drm: flip completions: %llu handed to the "
				        "compositor, %llu of them while the hardware had "
				        "NOT yet latched the new address\n",
				        (unsigned long long)seen,
				        (unsigned long long)pending);
		}
	}
	line = lkpi_i915_scanline();
	if (line == 0xffffffff)
		return;
	events++;
	if (line < lo)
		lo = line;
	if (line > hi)
		hi = line;
	if (line < 100 || line >= 1080)
		early++; /* at the top of the frame or in blanking: vblank-locked */
	else
		late++;
	if ((events % 60) == 0) {
		pr_info("drm: flip events: %llu at the top or in blanking, %llu in "
		        "mid-frame (lines %u..%u)\n", (unsigned long long)early,
		        (unsigned long long)late, lo, hi);
		early = late = 0;
		lo = 0xffffffff;
		hi = 0;
	}
}

static void drm_fps_note_event(const void *user_buf, isize len)
{
	struct {
		u32 type;
		u32 length;
		u64 user_data;
		u32 tv_sec;
		u32 tv_usec;
		u32 sequence;
		u32 crtc_id;
	} ev;
	static unsigned reported;

	if (reported >= 6 || (usize)len < sizeof(ev))
		return;
	if (lkpi_copy_from_user(&ev, user_buf, sizeof(ev)) != 0)
		return;
	reported++;
	{
		u64 now = lkpi_monotonic_ns();

		if (drm_last_commit_ns && now > drm_last_commit_ns)
			pr_info("drm: flip event arrived %u us after the commit returned\n",
			        (unsigned)((now - drm_last_commit_ns) / 1000ull));
		pr_info("drm: request carried user_data 0x%llx, event carries 0x%llx%s\n",
		        (unsigned long long)drm_last_user_data,
		        (unsigned long long)ev.user_data,
		        drm_last_user_data == ev.user_data ? "" : "  <-- MISMATCH");
	}
	pr_info("drm: event type %u len %u user_data 0x%llx seq %u crtc %u at %u.%06u\n",
	        ev.type, ev.length, (unsigned long long)ev.user_data,
	        ev.sequence, ev.crtc_id, ev.tv_sec, ev.tv_usec);
}

static void drm_fps_note(void)
{
	static u64 window_start_ns;
	static u64 last_ns;
	static u32 frames;
	static u64 max_gap_ns;
	u64 now = lkpi_monotonic_ns();

	if (!window_start_ns) {
		window_start_ns = now;
		last_ns = now;
		return;
	}
	if (last_ns && now - last_ns > max_gap_ns)
		max_gap_ns = now - last_ns;
	/* A gap this long is a stall, not a still picture. Print who was asleep
	 * and on what while it lasted -- the wait channel and the site that
	 * parked each task is the only thing that names the waiter; the counters
	 * above can only say that nobody was running. */
	if (last_ns && now - last_ns > 1500000000ull) {
		extern void scheduler_dump_tasks(void);
		static unsigned dumped;

		if (dumped < 3) {
			dumped++;
			pr_info("drm: %u ms without a frame — tasks:\n",
			        (unsigned)((now - last_ns) / 1000000ull));
			scheduler_dump_tasks();
		}
	}
	last_ns = now;
	frames++;

	if (now - window_start_ns >= 1000000000ull) {
		/* What the gap was made of. A second with a long gap and a high
		 * user count is a compositor that was busy drawing; the same gap
		 * against an idle count is something the system was waiting for. */
		extern void kprof_tick_totals(u64 *user, u64 *kernel, u64 *idle);
		extern void input_event_counts(u64 *pushed, u64 *delivered,
		                               u64 *dropped);
		static u64 last_u, last_k, last_i, last_in, last_drop;
		u64 u = 0, k = 0, i = 0, in = 0, deliv = 0, drop = 0;

		kprof_tick_totals(&u, &k, &i);
		input_event_counts(&in, &deliv, &drop);
		{
			extern unsigned long lkpi_mmio_reads, lkpi_mmio_writes;
			static unsigned long last_r, last_w;
			unsigned long r = lkpi_mmio_reads, w = lkpi_mmio_writes;

			pr_info("drm: %u framebuffer(s) alive (%u full-screen), scanout changed %u times, %u created\n",
		        drm_fps_fb_alive, drm_fps_fb_fullscreen, drm_fps_fb_changes,
		        drm_addfb_calls);
		drm_fps_fb_changes = 0;
		drm_addfb_calls = 0;
		pr_info("drm: mmio %lu reads %lu writes in the last second\n",
			        r - last_r, w - last_w);
			last_r = r;
			last_w = w;
		}
		pr_info("drm: fps %u, longest gap %u ms, ticks user %u kernel %u idle %u, input %u (dropped %u)\n",
		        frames, (unsigned)(max_gap_ns / 1000000ull),
		        (unsigned)(u - last_u), (unsigned)(k - last_k),
		        (unsigned)(i - last_i), (unsigned)(in - last_in),
		        (unsigned)(drop - last_drop));
		last_u = u;
		last_k = k;
		last_i = i;
		last_in = in;
		last_drop = drop;
		window_start_ns = now;
		frames = 0;
		max_gap_ns = 0;
	}
}

isize lkpi_drm_read(void *file, void *user_buf, usize len)
{
	struct file *filp = file;
	loff_t pos = 0;
	isize ret;

	if (!filp)
		return -EBADF;
	ret = (isize)drm_read(filp, (char __user *)user_buf, (size_t)len, &pos);
	/* The completion check stands on its own.
	 *
	 * It used to sit inside the frame-rate block, so asking only for it got
	 * a silent run: the question "was the flip finished when the compositor
	 * was told" has nothing to do with counting frames per second. */
	if (ret > 0 && lkpi_bootflag("b1nix.drm-eventwatch"))
		drm_event_scanline_note();
	/* The per-commit note is about which memory a framebuffer is made of,
	 * which the binding watch needs and the frame-rate counter does not. */
	if (ret > 0 && lkpi_bootflag("b1nix.drm-bindwatch")) {
		extern void lkpi_i915_note_commit(void);

		lkpi_i915_note_commit();
	}
	if (ret > 0 && lkpi_bootflag("b1nix.drm-fps")) {
		/* Events, counted by the bytes they occupy rather than by reads: one
		 * read can carry several. A compositor that submits more flips than
		 * it is told completed has one buffer permanently in flight as far as
		 * it knows, and nothing to draw into but the one on screen. */
		extern unsigned drm_events_delivered;

		drm_events_delivered += (unsigned)(ret / 32);
		/* Which framebuffer each completed flip actually put on the screen.
		 *
		 * A compositor that draws into the buffer being scanned out tears no
		 * matter how well the timing works, and every counter here would
		 * still look healthy -- frames land, nothing is late, no commit
		 * overruns. The one thing that tells the two apart is whether the
		 * flips alternate between framebuffers or keep naming the same one. */
		drm_fps_note_flip(filp);
		drm_fps_note_event(user_buf, ret);
		drm_event_scanline_note();
		{
			extern void lkpi_i915_note_commit(void);

			lkpi_i915_note_commit();
		}
		drm_fps_note();
	}
	/* What a compositor's event loop actually receives. A page-flip completion
	 * that is queued but never read leaves it waiting for a frame that, as far
	 * as it can tell, never landed — and it tears the output down again. */
	if (lkpi_bootflag("b1nix.drm-debug"))
		pr_info("drm: read %d bytes\n", (int)ret);
	return ret;
}

int lkpi_drm_readable(void *file)
{
	struct file *filp = file;
	static unsigned reported;

	if (!filp)
		return 0;
	if (lkpi_bootflag("b1nix.drm-debug") && reported < 40) {
		reported++;
		pr_info("drm: poll -> %s\n",
		        (drm_poll(filp, 0) & (EPOLLIN | EPOLLRDNORM)) ? "readable"
		                                                      : "empty");
	}
	/* poll_wait is a no-op here — b1nix parks the caller on its own poll
	 * channel — so what comes back is purely the readiness mask. */
	return (drm_poll(filp, 0) & (EPOLLIN | EPOLLRDNORM)) ? 1 : 0;
}

/* ── mmap ───────────────────────────────────────────────────────── */

int lkpi_drm_mmap_page_phys(void *file, u64 offset, u64 *out_phys)
{
	struct file *filp = file;
	struct drm_file *priv;
	struct drm_vma_offset_node *node;
	unsigned long pgoff = (unsigned long)(offset / PAGE_SIZE);
	u64 index;
	int ret;

	if (!filp || !out_phys)
		return -EINVAL;
	/* Only a DRM file has a private_data to read here. A dma-buf's file is
	 * mapped through its own ops, and reinterpreting it would walk a struct
	 * dma_buf as a struct drm_file. */
	if (!lkpi_drm_file_is_drm(filp))
		return -EBADF;
	priv = filp->private_data;

	/*
	 * The device comes from the open file, not from a global: the offset being
	 * mapped is meaningful only within the device whose node userspace opened,
	 * and the same offset names a different object on another device.
	 */
	struct drm_device *dev = (priv->minor && priv->minor->dev) ? priv->minor->dev
	                                                           : g_dev;
	lkpi_drm_page_fn resolver = lkpi_drm_resolver_for(dev);

	if (!dev || !resolver) {
		pr_info("drm: mmap offset %llx: no %s for this device\n",
		        (unsigned long long)offset, dev ? "resolver" : "device");
		return -EINVAL;
	}

	drm_vma_offset_lock_lookup(dev->vma_offset_manager);
	node = drm_vma_offset_lookup_locked(dev->vma_offset_manager, pgoff, 1);
	if (node && !drm_vma_node_is_allowed(node, priv))
		node = 0;
	/* The reference across the resolve is the driver's to take: it is the one
	 * that knows what this node is part of. The bridge only finds it. */
	drm_vma_offset_unlock_lookup(dev->vma_offset_manager);

	if (!node) {
		pr_info("drm: mmap offset %llx (page %lx): nothing mapped there\n",
		        (unsigned long long)offset, pgoff);
		return -EACCES;
	}

	index = (u64)(pgoff - node->vm_node.start);
	ret = resolver(node, index, out_phys);
	if (ret)
		pr_info("drm: mmap offset %llx page %llu: resolver says %d\n",
		        (unsigned long long)offset, (unsigned long long)index, ret);
	return ret;
	/* Say so when a page of a mapped buffer cannot be resolved. A compositor
	 * that cannot map its second buffer builds a swapchain of one and draws
	 * into the buffer being displayed; from outside that looks like tearing
	 * and nothing else. */
	{
		static unsigned mmap_fail_reported;

		if (mmap_fail_reported < 8) {
			mmap_fail_reported++;
			pr_info("drm: mmap of offset 0x%llx page failed\n",
			        (unsigned long long)offset);
		}
	}

}
