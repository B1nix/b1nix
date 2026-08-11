/* SPDX-License-Identifier: MIT
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

isize lkpi_drm_ioctl(void *file, u64 request, void *user_arg)
{
	struct file *filp = file;

	if (!filp)
		return -EBADF;
	return (isize)drm_ioctl(filp, (unsigned int)request,
	                        (unsigned long)(usize)user_arg);
}

isize lkpi_drm_read(void *file, void *user_buf, usize len)
{
	struct file *filp = file;
	loff_t pos = 0;

	if (!filp)
		return -EBADF;
	return (isize)drm_read(filp, (char __user *)user_buf, (size_t)len, &pos);
}

int lkpi_drm_readable(void *file)
{
	struct file *filp = file;

	if (!filp)
		return 0;
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
	struct drm_gem_object *obj;
	unsigned long pgoff = (unsigned long)(offset / PAGE_SIZE);
	u64 index;
	int ret;

	if (!filp || !out_phys)
		return -EINVAL;
	priv = filp->private_data;
	if (!priv)
		return -EBADF;

	/*
	 * The device comes from the open file, not from a global: the offset being
	 * mapped is meaningful only within the device whose node userspace opened,
	 * and the same offset names a different object on another device.
	 */
	struct drm_device *dev = (priv->minor && priv->minor->dev) ? priv->minor->dev
	                                                           : g_dev;
	lkpi_drm_page_fn resolver = lkpi_drm_resolver_for(dev);

	if (!dev || !resolver)
		return -EINVAL;

	drm_vma_offset_lock_lookup(dev->vma_offset_manager);
	node = drm_vma_offset_lookup_locked(dev->vma_offset_manager, pgoff, 1);
	if (node && !drm_vma_node_is_allowed(node, priv))
		node = 0;
	if (node) {
		obj = container_of(node, struct drm_gem_object, vma_node);
		/* Taken under the lookup lock: the object could otherwise be freed
		 * between finding it and using it. */
		drm_gem_object_get(obj);
	} else {
		obj = 0;
	}
	drm_vma_offset_unlock_lookup(dev->vma_offset_manager);

	if (!obj)
		return -EACCES;

	index = (u64)(pgoff - node->vm_node.start);
	ret = resolver(obj, index, out_phys);
	drm_gem_object_put(obj);
	return ret;
}
