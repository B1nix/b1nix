/*
 * SPDX-License-Identifier: MIT
 *
 * M101 linuxkpi: the remaining leaf functions the DRM core links against.
 *
 * Each is here rather than as a header inline because it has a body worth
 * reading — a sort that has to be stable, a formatter that has to report what
 * it truncated to, a quirk table that deliberately finds nothing.
 */

#include <linux/dma-fence.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/list_sort.h>
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

int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	(void)adap;
	(void)msgs;
	/*
	 * DDC — reading an EDID off a monitor — goes through here. b1nix's i2c
	 * driver talks to a controller this adapter is not yet bound to, so the
	 * transfer is refused rather than answered with zeros: a driver that reads
	 * an all-zero EDID would conclude the monitor reported no modes, which is
	 * a worse failure than being told the bus is not there.
	 */
	return num > 0 ? -ENODEV : 0;
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
