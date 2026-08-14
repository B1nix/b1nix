/*
 * SPDX-License-Identifier: MIT
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

struct sync_file *sync_file_create(struct dma_fence *fence)
{
	(void)fence;
	/*
	 * A fence wrapped in a descriptor so userspace can wait on GPU work.
	 * Everything it needs is here — anonymous inodes and the fd table — but
	 * the file operations it must carry (poll, ioctl) have no implementation
	 * yet, and a descriptor whose poll never fires is worse than none: a
	 * caller would wait forever instead of getting an error.
	 */
	return 0;
}

struct dma_fence *sync_file_get_fence(int fd)
{
	(void)fd;
	/* Nothing creates one, so nothing can be recognised as one. */
	return 0;
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

void eventfd_signal(struct eventfd_ctx *ctx, u64 n)
{
	(void)ctx;
	(void)n;
}
