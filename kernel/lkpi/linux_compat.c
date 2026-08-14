/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * M101 linuxkpi: the small out-of-line pieces the linux headers declare.
 *
 * printk and seq_printf live here rather than as inlines because both take a
 * format string and have to reach b1nix's own vsnprintf, which the headers
 * should not drag into every translation unit that includes them.
 */

#include <b1nix/console.h>
#include <linux/printk.h>
#include <linux/seq_file.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/ioport.h>
#include <linux/errno.h>
#include <linux/kstrtox.h>
#include <linux/err.h>
#include <lkpi/env.h>
#include <linux/ww_mutex.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int lkpi_printk(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	/* One console_write for the whole line. Building a line from several
	 * writes lets another CPU interleave into the middle of it, which is how
	 * SMP log interleaving ate smoke-test markers before (M32B). */
	console_write(buf);
	return n;
}

int lkpi_vprintk(const char *fmt, va_list args)
{
	char buf[512];
	int n = vsnprintf(buf, sizeof(buf), fmt, args);
	console_write(buf);
	return n;
}

/* Referenced by imported code; see the headers that declare them for why each
 * is what it is. */
int oops_in_progress = 0;
struct resource iomem_resource = { 0, ~0ull, "iomem", 0, 0, 0, 0 };

/* One acquire class for every reservation lock: ordering comes from the stamp,
 * so a second class would distinguish nothing. */
struct ww_class reservation_ww_class = { "reservation" };

char *kvasprintf(gfp_t flags, const char *fmt, va_list ap)
{
	/*
	 * Grow until the result fits, rather than asking how long it would be.
	 * C99's vsnprintf returns the length it *would* have written; b1nix's
	 * stops counting at truncation and returns what it did write. Measuring
	 * with a one-byte buffer therefore reported 0 and this returned an empty
	 * string — which reached the DRM core as an unnamed sysfs link and failed
	 * a device registration two layers up, with nothing in the error to say a
	 * string had gone missing.
	 *
	 * A caller here cannot assume either behaviour, so it assumes neither: a
	 * result that exactly fills the buffer might have been truncated, so try
	 * again with twice the room.
	 */
	usize cap = 128;
	for (;;) {
		char *buf = (char *)lkpi_kmalloc(cap, flags);
		if (!buf)
			return 0;

		/* A copy per attempt: vsnprintf consumes the list, and reusing the
		 * original for a second pass reads garbage. */
		va_list attempt;
		va_copy(attempt, ap);
		int n = vsnprintf(buf, cap, fmt, attempt);
		va_end(attempt);

		if (n < 0) {
			lkpi_kfree(buf);
			return 0;
		}
		if ((usize)n + 1 < cap)
			return buf; /* room to spare: nothing was cut */

		lkpi_kfree(buf);
		if (cap > (usize)1 << 20)
			return 0; /* a format this large is a bug in the caller */
		cap *= 2;
	}
}

char *kasprintf(gfp_t flags, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char *s = kvasprintf(flags, fmt, ap);
	va_end(ap);
	return s;
}

char *kstrdup(const char *s, gfp_t flags)
{
	if (!s)
		return 0;
	usize len = strlen(s) + 1;
	char *copy = (char *)lkpi_kmalloc(len, flags);
	if (copy)
		memcpy(copy, s, len);
	return copy;
}

void *memdup_user(const void *user_src, usize len)
{
	if (len == 0)
		return ERR_PTR(-EINVAL);
	void *dst = lkpi_kmalloc(len, GFP_KERNEL);
	if (!dst)
		return ERR_PTR(-ENOMEM);
	if (lkpi_copy_from_user(dst, user_src, len) != 0) {
		lkpi_kfree(dst);
		/* The caller must not see a half-copied buffer: returning it would
		 * hand a driver userspace-shaped garbage it would then trust. */
		return ERR_PTR(-EFAULT);
	}
	return dst;
}

void print_hex_dump(const char *level, const char *prefix, int prefix_type,
                    int rowsize, int groupsize, const void *buf, usize len,
                    _Bool ascii)
{
	(void)level;
	(void)prefix_type;
	(void)groupsize;
	(void)ascii;
	const unsigned char *p = (const unsigned char *)buf;
	if (rowsize <= 0)
		rowsize = 16;

	/* One console_write per line: building a line from several writes lets
	 * another CPU interleave into the middle of it. */
	for (usize off = 0; off < len; off += (usize)rowsize) {
		char line[128];
		int n = snprintf(line, sizeof(line), "%s%04lx:", prefix ? prefix : "",
		                 (unsigned long)off);
		for (int i = 0; i < rowsize && off + (usize)i < len; i++) {
			if (n < 0 || (usize)n + 4 >= sizeof(line))
				break;
			n += snprintf(line + n, sizeof(line) - (usize)n, " %02x",
			              p[off + (usize)i]);
		}
		if (n > 0 && (usize)n + 2 < sizeof(line)) {
			line[n] = '\n';
			line[n + 1] = 0;
			console_write(line);
		}
	}
}

void *kmemdup(const void *src, usize len, gfp_t flags)
{
	if (!src || len == 0)
		return 0;
	void *dst = lkpi_kmalloc(len, flags);
	if (dst)
		memcpy(dst, src, len);
	return dst;
}

void *krealloc(const void *p, usize new_size, gfp_t flags)
{
	/* kheap cannot resize in place, so this is allocate-copy-free. The old
	 * size is not knowable from the pointer here, so the copy is bounded by
	 * the new size — safe when growing, and callers that shrink get a
	 * truncated copy, which is what shrinking means. */
	if (new_size == 0) {
		lkpi_kfree((void *)p);
		return 0;
	}
	void *n = lkpi_kmalloc(new_size, flags);
	if (!n)
		return 0;
	if (p) {
		memcpy(n, p, new_size);
		lkpi_kfree((void *)p);
	}
	return n;
}

long simple_strtol(const char *cp, char **endp, unsigned int base)
{
	int neg = 0;
	if (cp && *cp == '-') {
		neg = 1;
		cp++;
	}
	unsigned long v = simple_strtoul(cp, endp, base);
	return neg ? -(long)v : (long)v;
}

unsigned long simple_strtoul(const char *cp, char **endp, unsigned int base)
{
	unsigned long v = 0;
	if (!cp)
		goto out;
	if (base == 0) {
		if (cp[0] == '0' && (cp[1] == 'x' || cp[1] == 'X')) {
			base = 16;
			cp += 2;
		} else if (cp[0] == '0') {
			base = 8;
		} else {
			base = 10;
		}
	}
	for (;;) {
		unsigned int d;
		if (*cp >= '0' && *cp <= '9')
			d = (unsigned int)(*cp - '0');
		else if (*cp >= 'a' && *cp <= 'z')
			d = (unsigned int)(*cp - 'a') + 10;
		else if (*cp >= 'A' && *cp <= 'Z')
			d = (unsigned int)(*cp - 'A') + 10;
		else
			break;
		if (d >= base)
			break;
		v = v * base + d;
		cp++;
	}
out:
	/* The caller decides whether stopping here was an error, which is the whole
	 * reason this form still exists. */
	if (endp)
		*endp = (char *)cp;
	return v;
}

int kstrtoint(const char *s, unsigned int base, int *res)
{
	char *end;
	long v = simple_strtol(s, &end, base);
	if (end == s)
		return -EINVAL;
	if (res)
		*res = (int)v;
	return 0;
}

int kstrtouint(const char *s, unsigned int base, unsigned int *res)
{
	char *end;
	unsigned long v = simple_strtoul(s, &end, base);
	if (end == s)
		return -EINVAL;
	if (res)
		*res = (unsigned int)v;
	return 0;
}

int sysfs_emit(char *buf, const char *fmt, ...)
{
	if (!buf)
		return 0;
	va_list ap;
	va_start(ap, fmt);
	/* One page, the size sysfs gives a show() method. */
	int n = vsnprintf(buf, 4096, fmt, ap);
	va_end(ap);
	return n < 0 ? 0 : n;
}

int sysfs_emit_at(char *buf, int at, const char *fmt, ...)
{
	if (!buf || at < 0 || at >= 4096)
		return 0;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf + at, 4096 - (usize)at, fmt, ap);
	va_end(ap);
	return n < 0 ? 0 : n;
}

int dev_set_name(struct device *dev, const char *fmt, ...)
{
	if (!dev)
		return -EINVAL;
	/* The name outlives the call, so it cannot point at a caller's stack
	 * buffer; it is duplicated onto the heap and owned by the device. */
	char buf[64];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	char *copy = kstrdup(buf, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	dev->init_name = copy;
	return 0;
}
