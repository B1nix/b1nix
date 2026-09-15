/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * M101 linuxkpi: the remaining leaf functions the DRM core links against.
 *
 * Each is here rather than as a header inline because it has a body worth
 * reading — a sort that has to be stable, a formatter that has to report what
 * it truncated to, a quirk table that deliberately finds nothing.
 */

#include <b1nix/arch.h>
#include <linux/delay.h>
#include <linux/dma-fence.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/ktime.h>
#include <linux/list_sort.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/string.h>
#include <linux/sync_file.h>
#include <lkpi/env.h>
#include <stdarg.h>
#include <stdio.h>

/* ── formatting ─────────────────────────────────────────────────── */

int sprintf(char *buf, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	/* Unbounded by definition: the caller promised the buffer is big enough.
	 * A bound is imposed anyway, large enough not to change any correct
	 * caller's result but small enough that an incorrect one corrupts a known
	 * amount rather than the whole heap. */
	int n = vsnprintf(buf, 4096, fmt, ap);
	va_end(ap);
	return n;
}

int scnprintf(char *buf, usize size, const char *fmt, ...)
{
	if (!buf || size == 0)
		return 0;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	/*
	 * The difference from snprintf, and the whole reason this exists: snprintf
	 * returns what it *would* have written, so a caller advancing a cursor by
	 * it walks past the end of its own buffer on truncation. This returns what
	 * was actually written.
	 */
	if (n < 0)
		return 0;
	return ((usize)n >= size) ? (int)(size - 1) : n;
}

/* ── sorting ────────────────────────────────────────────────────── */

static void sort_swap_bytes(char *a, char *b, usize size)
{
	for (usize i = 0; i < size; i++) {
		char t = a[i];
		a[i] = b[i];
		b[i] = t;
	}
}

void sort(void *base, usize num, usize size,
          int (*cmp)(const void *, const void *),
          void (*swap_fn)(void *, void *, int))
{
	if (!base || !cmp || num < 2 || size == 0)
		return;

	/* Insertion sort. The arrays here are mode lists and format lists — tens
	 * of entries — where the constant factor beats an asymptotically better
	 * algorithm, and where being stable matters more than being fast. */
	char *arr = (char *)base;
	for (usize i = 1; i < num; i++) {
		for (usize j = i; j > 0; j--) {
			char *cur = arr + j * size;
			char *prev = cur - size;
			if (cmp(prev, cur) <= 0)
				break;
			if (swap_fn)
				swap_fn(prev, cur, (int)size);
			else
				sort_swap_bytes(prev, cur, size);
		}
	}
}

/* sort() with a context pointer handed to the comparison. Same stable
 * insertion sort. */
void sort_r(void *base, usize num, usize size,
            int (*cmp)(const void *, const void *, const void *),
            void (*swap_fn)(void *, void *, int, const void *),
            const void *priv)
{
	if (!base || !cmp || num < 2 || size == 0)
		return;

	char *arr = (char *)base;
	for (usize i = 1; i < num; i++) {
		for (usize j = i; j > 0; j--) {
			char *cur = arr + j * size;
			char *prev = cur - size;
			if (cmp(prev, cur, priv) <= 0)
				break;
			if (swap_fn)
				swap_fn(prev, cur, (int)size, priv);
			else
				sort_swap_bytes(prev, cur, size);
		}
	}
}

void list_sort(void *priv, struct list_head *head,
               int (*cmp)(void *priv, const struct list_head *a,
                          const struct list_head *b))
{
	if (!head || !cmp || list_empty(head))
		return;

	/*
	 * Insertion sort over the list, taking each node out and walking back to
	 * its place. Stability is a property callers depend on — the DRM core
	 * sorts modes and expects equal entries to keep the order the driver added
	 * them in — and inserting *after* the last node that compares less-or-equal
	 * is what preserves it.
	 */
	struct list_head sorted;
	INIT_LIST_HEAD(&sorted);

	while (!list_empty(head)) {
		struct list_head *node = head->next;
		list_del(node);

		struct list_head *at = sorted.prev;
		while (at != &sorted && cmp(priv, at, node) > 0)
			at = at->prev;

		node->next = at->next;
		node->prev = at;
		at->next->prev = node;
		at->next = node;
	}

	/* Splice the sorted list back into the caller's head. */
	head->next = sorted.next;
	head->prev = sorted.prev;
	head->next->prev = head;
	head->prev->next = head;
}

/* ── i2c ────────────────────────────────────────────────────────── */

/*
 * DDC — reading an EDID off a monitor — goes through here.
 *
 * The adapter is not b1nix's. A display driver registers its own: i915's is
 * GMBUS, the controller inside the GPU, and its algorithm drives the GPU's own
 * registers. So the core's whole job is to call that algorithm, which is what
 * i2c_add_adapter() already promises when it refuses an adapter without one.
 *
 * This used to refuse every transfer, on the reasoning that b1nix's i2c driver
 * was not bound to the controller. That reasoning was about the wrong bus: no
 * b1nix driver is involved at any point. The cost was not a missing extra —
 * every connector reported disconnected, because a display is detected by
 * reading its EDID, so a passed-through GPU with a monitor physically attached
 * enumerated no modes and no modeset could run.
 *
 * Retries follow the adapter's own count: GMBUS reports -EAGAIN when the bus is
 * busy, and one retry is normal rather than a fault.
 */
int __i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	int tries, ret;

	if (!adap || !adap->algo || !adap->algo->master_xfer)
		return -ENODEV;
	if (num <= 0)
		return 0;

	ret = -EAGAIN;
	for (tries = 0; tries <= adap->retries && ret == -EAGAIN; tries++)
		ret = adap->algo->master_xfer(adap, msgs, num);

	return ret;
}

/*
 * The last EDID read from each adapter, kept so a re-probe costs nothing.
 *
 * On this hardware GMBUS times out and the driver falls back to bit-banging the
 * I2C lines, which takes minutes for one 128-byte block. Userspace re-probes a
 * connector whenever it enumerates it, so a compositor's startup spent those
 * minutes before it had a mode list — and used a fallback list in the meantime,
 * committing 720x400 to a monitor offering 1920x1080. The bytes do not change
 * while the cable stays in; a fresh read still happens whenever the cached one
 * fails to answer, which is what a hotplug looks like from here.
 */
struct lkpi_edid_cache {
	struct i2c_adapter *adap;
	u8 addr;
	u8 data[256];
	unsigned len;
	u64 fail_until;
};

static struct lkpi_edid_cache g_edid_cache[8];

/* An EDID read is a one-byte offset write followed by a read from 0x50. */
static int i2c_edid_shape(struct i2c_msg *msgs, int num, u8 *offset)
{
	if (num != 2 || msgs[0].addr != 0x50 || msgs[1].addr != 0x50)
		return 0;
	if ((msgs[0].flags & I2C_M_RD) || !(msgs[1].flags & I2C_M_RD))
		return 0;
	if (msgs[0].len != 1 || !msgs[0].buf || !msgs[1].buf)
		return 0;
	*offset = msgs[0].buf[0];
	return 1;
}

int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	int ret;
	u8 offset = 0;
	int is_edid = msgs && i2c_edid_shape(msgs, num, &offset);
	struct lkpi_edid_cache *slot = 0;

	if (!adap)
		return -ENODEV;

	if (is_edid) {
		for (unsigned i = 0; i < 8; i++) {
			if (g_edid_cache[i].adap == adap) {
				slot = &g_edid_cache[i];
				break;
			}
			if (!g_edid_cache[i].adap && !slot)
				slot = &g_edid_cache[i];
		}
		if (slot && slot->adap == adap && slot->len &&
		    (unsigned)offset + msgs[1].len <= slot->len) {
			memcpy(msgs[1].buf, slot->data + offset, msgs[1].len);
			return num;
		}

		/* Negative cache: a controller that just failed an EDID read fails the
		 * same way for the next couple of seconds, each failure a multi-segment
		 * GMBUS timeout of tens of ms. Under legacy IGD passthrough GMBUS has no
		 * reference clock and never works, so a compositor polling the connector
		 * turned every frame into hundreds of ms of futile i2c -- a desktop at a
		 * tenth of a frame a second. The cooldown still lets a hotplug re-probe. */
		if (slot && slot->adap == adap && slot->fail_until &&
		    lkpi_monotonic_ns() < slot->fail_until)
			return -ENODEV;
	}

	/*
	 * The adapter's own bus lock when it has one, and nothing when it does
	 * not.
	 *
	 * i915 publishes lock_ops for GMBUS so a full EDID read holds the bus
	 * across its segments; taking a lock of b1nix's own instead would not
	 * serialise against the driver's other users of the same controller,
	 * which is the thing that actually needs excluding.
	 */
	if (adap->lock_ops && adap->lock_ops->lock_bus)
		adap->lock_ops->lock_bus(adap, I2C_LOCK_SEGMENT);

	{
		u64 t0 = lkpi_monotonic_ns();

		ret = __i2c_transfer(adap, msgs, num);
		/* Anything on this bus that takes longer than a frame is worth naming:
		 * an EDID read is milliseconds when the controller works and minutes
		 * when it does not, and the difference decides whether a compositor
		 * sees a display's real modes or a fallback list. */
		{
			u64 ms = (lkpi_monotonic_ns() - t0) / 1000000ull;

			if (ms >= 20)
				pr_info("lkpi: i2c transfer %d msgs took %llu ms (ret %d)\n",
				        num, (unsigned long long)ms, ret);
		}
	}

	if (adap->lock_ops && adap->lock_ops->unlock_bus)
		adap->lock_ops->unlock_bus(adap, I2C_LOCK_SEGMENT);

	if (is_edid && slot && ret == num &&
	    (unsigned)offset + msgs[1].len <= sizeof(slot->data)) {
		slot->adap = adap;
		slot->addr = 0x50;
		memcpy(slot->data + offset, msgs[1].buf, msgs[1].len);
		if ((unsigned)offset + msgs[1].len > slot->len)
			slot->len = (unsigned)offset + msgs[1].len;
		slot->fail_until = 0;
	} else if (is_edid && slot && ret < 0 && slot->len == 0) {
		/* Never read successfully -- a cached EDID is never dropped by a blip. */
		slot->adap = adap;
		slot->fail_until = lkpi_monotonic_ns() + 2000000000ull;
	}

	return ret;
}

/* ── quirks ─────────────────────────────────────────────────────── */

int drm_get_panel_orientation_quirk(int width, int height)
{
	(void)width;
	(void)height;
	/*
	 * Upstream matches a DMI table to learn that a particular tablet's panel is
	 * mounted rotated. b1nix does not parse DMI (see <linux/dmi.h>), so no
	 * quirk applies — and applying one to the wrong machine turns the display
	 * upside down, which is worse than not applying it at all.
	 *
	 * The value is upstream's DRM_MODE_PANEL_ORIENTATION_UNKNOWN.
	 */
	return -1;
}

/* ── sync_file ──────────────────────────────────────────────────── */

/*
 * A fence wrapped in a descriptor so userspace can wait on GPU work.
 *
 * Mesa asks i915 for an out-fence on every batch a compositor wants to know
 * the end of (EGL_ANDROID_native_fence_sync), and KWin polls that descriptor
 * before it presents. With no sync_file every such submission failed, and the
 * compositor fell back to waiting out its own timeouts -- a desktop on the GPU
 * drawing one frame a second while the CPUs sat idle.
 *
 * The uapi is <uapi/linux/sync_file.h>, reproduced here field for field.
 */
#include <linux/anon_inodes.h>
#include <linux/bitops.h>
#include <linux/file.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <asm/ioctl.h>

struct sync_merge_data {
	char name[32];
	s32 fd2;
	s32 fence;
	u32 flags;
	u32 pad;
};

struct sync_fence_info {
	char obj_name[32];
	char driver_name[32];
	s32 status;
	u32 flags;
	u64 timestamp_ns;
};

struct sync_file_info {
	char name[32];
	s32 status;
	u32 flags;
	u32 num_fences;
	u32 pad;
	u64 sync_fence_info;
};

#define SYNC_IOC_MAGIC		'>'
#define SYNC_IOC_MERGE		_IOWR(SYNC_IOC_MAGIC, 3, struct sync_merge_data)
#define SYNC_IOC_FILE_INFO	_IOWR(SYNC_IOC_MAGIC, 4, struct sync_file_info)
#define SYNC_IOC_SET_DEADLINE	_IOW(SYNC_IOC_MAGIC, 5, u64[2])

#define SYNC_POLL_ENABLED 0

static void sync_file_fence_done(struct dma_fence *f, struct dma_fence_cb *cb)
{
	struct sync_file *sf = container_of(cb, struct sync_file, cb);

	(void)f;
	wake_up_all(&sf->wq);
}

/* -errno if a fence failed, 1 when every one is done, 0 while any is pending. */
static int sync_fence_status(struct dma_fence *f)
{
	if (!dma_fence_is_signaled(f))
		return 0;
	return f->error ? f->error : 1;
}

static int sync_file_release(struct inode *inode, struct file *file)
{
	struct sync_file *sf = file->private_data;

	(void)inode;
	if (!sf)
		return 0;
	if (test_bit(SYNC_POLL_ENABLED, &sf->flags))
		dma_fence_remove_callback(sf->fence, &sf->cb);
	dma_fence_put(sf->fence);
	lkpi_kfree(sf);
	file->private_data = 0;
	return 0;
}

static __poll_t sync_file_poll(struct file *file, poll_table *wait)
{
	struct sync_file *sf = file->private_data;

	poll_wait(file, &sf->wq, wait);
	/* Armed on the first poll, as upstream: a callback that is refused means
	 * the fence is already done, and the wake stands in for it. */
	if (!test_and_set_bit(SYNC_POLL_ENABLED, &sf->flags) &&
	    dma_fence_add_callback(sf->fence, &sf->cb, sync_file_fence_done) < 0)
		wake_up_all(&sf->wq);
	return dma_fence_is_signaled(sf->fence) ? EPOLLIN : 0;
}

static struct sync_file *sync_file_alloc(void)
{
	struct sync_file *sf = lkpi_kmalloc(sizeof(*sf), GFP_KERNEL | __GFP_ZERO);

	if (!sf)
		return 0;
	sf->file = anon_inode_getfile("sync_file", &sync_file_fops, sf, 0);
	if (!sf->file) {
		lkpi_kfree(sf);
		return 0;
	}
	init_waitqueue_head(&sf->wq);
	INIT_LIST_HEAD(&sf->cb.node);
	return sf;
}

static long sync_file_ioctl_merge(struct sync_file *a, unsigned long arg)
{
	struct sync_merge_data data;
	struct dma_fence *b, **pair;
	struct dma_fence_array *array;
	struct sync_file *merged;
	int fd;

	if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
		return -EFAULT;
	if (data.flags || data.pad)
		return -EINVAL;
	b = sync_file_get_fence(data.fd2);
	if (!b)
		return -ENOENT;

	pair = lkpi_kmalloc(2 * sizeof(*pair), GFP_KERNEL);
	if (!pair) {
		dma_fence_put(b);
		return -ENOMEM;
	}
	pair[0] = dma_fence_get(a->fence);
	pair[1] = b;
	array = dma_fence_array_create(2, pair, dma_fence_context_alloc(1), 1, false);
	if (!array) {
		dma_fence_put(pair[0]);
		dma_fence_put(pair[1]);
		lkpi_kfree(pair);
		return -ENOMEM;
	}
	merged = sync_file_create(&array->base);
	dma_fence_put(&array->base);
	if (!merged)
		return -ENOMEM;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		fput(merged->file);
		return fd;
	}
	data.fence = fd;
	if (copy_to_user((void __user *)arg, &data, sizeof(data))) {
		put_unused_fd((unsigned int)fd);
		fput(merged->file);
		return -EFAULT;
	}
	fd_install((unsigned int)fd, merged->file);
	return 0;
}

static void sync_fill_fence_info(struct dma_fence *f, struct sync_fence_info *info)
{
	memset(info, 0, sizeof(*info));
	strscpy(info->obj_name, dma_fence_timeline_name(f), sizeof(info->obj_name));
	strscpy(info->driver_name, dma_fence_driver_name(f), sizeof(info->driver_name));
	info->status = sync_fence_status(f);
	if (info->status == 1)
		info->timestamp_ns = (u64)ktime_to_ns(f->timestamp);
}

static long sync_file_ioctl_info(struct sync_file *sf, unsigned long arg)
{
	struct sync_file_info info;
	struct dma_fence_array *array = to_dma_fence_array(sf->fence);
	u32 count = array ? array->num_fences : 1;

	if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
		return -EFAULT;
	if (info.flags || info.pad)
		return -EINVAL;

	strscpy(info.name, sf->user_name[0] ? sf->user_name
	                                    : dma_fence_driver_name(sf->fence),
	        sizeof(info.name));
	info.status = sync_fence_status(sf->fence);

	/* Zero entries asks how many there are; otherwise the table must fit. */
	if (info.num_fences) {
		struct sync_fence_info fi;

		if (info.num_fences < count)
			return -EINVAL;
		for (u32 i = 0; i < count; i++) {
			sync_fill_fence_info(array ? array->fences[i] : sf->fence, &fi);
			if (copy_to_user((void __user *)(unsigned long)
			                 (info.sync_fence_info + i * sizeof(fi)),
			                 &fi, sizeof(fi)))
				return -EFAULT;
		}
	}
	info.num_fences = count;
	if (copy_to_user((void __user *)arg, &info, sizeof(info)))
		return -EFAULT;
	return 0;
}

static long sync_file_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct sync_file *sf = file->private_data;

	switch (cmd) {
	case SYNC_IOC_MERGE:
		return sync_file_ioctl_merge(sf, arg);
	case SYNC_IOC_FILE_INFO:
		return sync_file_ioctl_info(sf, arg);
	case SYNC_IOC_SET_DEADLINE:
		/* A hint for drivers that can boost; i915 has no deadline op. */
		return 0;
	default:
		return -ENOTTY;
	}
}

const struct file_operations sync_file_fops = {
	.release = sync_file_release,
	.poll = sync_file_poll,
	.unlocked_ioctl = sync_file_ioctl,
};

struct sync_file *sync_file_create(struct dma_fence *fence)
{
	struct sync_file *sf;

	if (!fence)
		return 0;
	sf = sync_file_alloc();
	if (!sf)
		return 0;
	sf->fence = dma_fence_get(fence);
	return sf;
}

struct dma_fence *sync_file_get_fence(int fd)
{
	struct file *f = lkpi_fd_owned_file(fd);
	struct sync_file *sf;

	if (!f || f->f_op != &sync_file_fops)
		return 0;
	sf = f->private_data;
	return sf ? dma_fence_get(sf->fence) : 0;
}

/* ── eventfd ────────────────────────────────────────────────────── */

struct eventfd_ctx *eventfd_ctx_fdget(int fd)
{
	(void)fd;
	/*
	 * b1nix has eventfd (VFS_HANDLE_EVENTFD), but reaching it means calling
	 * into the VFS from a translation unit that cannot include its headers —
	 * the same boundary the descriptor calls in <lkpi/env.h> cross. Wiring it
	 * belongs with the first caller that needs a driver to signal userspace,
	 * and until then this reports absence rather than accepting a descriptor
	 * it would never signal.
	 */
	return ERR_PTR(-ENOSYS);
}

void eventfd_ctx_put(struct eventfd_ctx *ctx)
{
	(void)ctx;
}

void eventfd_signal(struct eventfd_ctx *ctx)
{
	(void)ctx;
}
