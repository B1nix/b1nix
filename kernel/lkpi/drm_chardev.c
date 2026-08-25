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

void lkpi_drm_register_device(struct drm_device *dev, lkpi_drm_page_fn resolver)
{
	unsigned i;

	if (!dev)
		return;
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
	if (ret < 0 && lkpi_bootflag("b1nix.drm-debug"))
		pr_info("drm: ioctl 0x%08x -> %d\n", (unsigned)request, (int)ret);
	return ret;
}

isize lkpi_drm_read(void *file, void *user_buf, usize len)
{
	struct file *filp = file;
	loff_t pos = 0;
	isize ret;

	if (!filp)
		return -EBADF;
	ret = (isize)drm_read(filp, (char __user *)user_buf, (size_t)len, &pos);
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
}
