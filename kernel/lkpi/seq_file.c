/* SPDX-License-Identifier: GPL-2.0-only
 *
 * M101 linuxkpi: seq_file, the buffered text file behind debugfs dumps.
 *
 * A driver's debugfs file is almost never a plain read(). It is an open() that
 * calls single_open() with a show() function, and fops.read = seq_read. This is
 * that machinery: the show() runs once into a buffer, and reads are served out
 * of the buffer at the reader's offset.
 *
 * Two properties are what make it worth having rather than approximating:
 *
 *   - A dump longer than one read works. Without the buffer, each read re-runs
 *     the show() and returns its beginning, so everything past the first buffer
 *     is unreachable — and a caller cannot tell that from a short file.
 *   - The answer does not change under the reader. The show() runs to
 *     completion before the first byte is served, so a driver whose state moves
 *     between reads cannot hand back a spliced-together answer.
 *
 * A show() that does not fit is not truncated: the buffer doubles and the
 * show() runs again from scratch. That is Linux's rule too, and it is why a
 * show() must be free of side effects.
 */

#include <linux/errno.h>
#include <linux/stdarg.h>
#include <linux/fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <lkpi/env.h>

/* Where rendering starts, and where it gives up. A debugfs dump that needs more
 * than this is not a dump any more. */
#define SEQ_BUF_MIN (4u * 1024u)
#define SEQ_BUF_MAX (1u * 1024u * 1024u)

/* ── printing into the buffer ───────────────────────────────────── */

int seq_printf(struct seq_file *m, const char *fmt, ...)
{
	if (!m || !m->buf)
		return 0;
	if (m->count >= m->size) {
		m->overflow = 1;
		return 0;
	}

	va_list ap;
	va_start(ap, fmt);
	int n = lkpi_vsnprintf(m->buf + m->count, m->size - m->count, fmt, ap);
	va_end(ap);
	if (n < 0)
		return 0;

	/* Filling the buffer exactly is indistinguishable from being cut short,
	 * so it counts as an overflow: rendering again in a bigger buffer costs
	 * one pass, and serving a truncated dump costs a wrong answer. */
	if ((usize)n + 1 >= m->size - m->count) {
		m->overflow = 1;
		m->count = m->size;
		return 0;
	}
	m->count += (usize)n;
	return n;
}

int seq_puts(struct seq_file *m, const char *s)
{
	if (!m || !m->buf || !s)
		return 0;
	usize len = strlen(s);
	if (m->count + len + 1 > m->size) {
		m->overflow = 1;
		m->count = m->size;
		return 0;
	}
	memcpy(m->buf + m->count, s, len);
	m->count += len;
	m->buf[m->count] = 0;
	return (int)len;
}

int seq_putc(struct seq_file *m, char c)
{
	char s[2] = { c, 0 };

	return seq_puts(m, s);
}

int seq_write(struct seq_file *m, const void *data, usize len)
{
	if (!m || !m->buf || !data)
		return 0;
	if (m->count + len > m->size) {
		m->overflow = 1;
		m->count = m->size;
		return -1;
	}
	memcpy(m->buf + m->count, data, len);
	m->count += len;
	return 0;
}

/* ── open ───────────────────────────────────────────────────────── */

static struct seq_file *seq_alloc(void)
{
	struct seq_file *m = kzalloc(sizeof(*m), GFP_KERNEL);

	if (!m)
		return 0;
	m->owns_buf = 1;
	return m;
}

int single_open(struct file *file, int (*show)(struct seq_file *, void *),
                void *data)
{
	struct seq_file *m;

	if (!file || !show)
		return -EINVAL;
	m = seq_alloc();
	if (!m)
		return -ENOMEM;
	m->single_show = show;
	m->private = data;
	file->private_data = m;
	return 0;
}

int seq_open(struct file *file, const struct seq_operations *op)
{
	struct seq_file *m;

	if (!file || !op)
		return -EINVAL;
	m = seq_alloc();
	if (!m)
		return -ENOMEM;
	m->op = op;
	file->private_data = m;
	return 0;
}

static void seq_free(struct file *file)
{
	struct seq_file *m = file ? file->private_data : 0;

	if (!m)
		return;
	if (m->owns_buf)
		kfree(m->buf);
	kfree(m);
	file->private_data = 0;
}

int single_release(struct inode *inode, struct file *file)
{
	(void)inode;
	seq_free(file);
	return 0;
}

int seq_release(struct inode *inode, struct file *file)
{
	(void)inode;
	seq_free(file);
	return 0;
}

/* ── rendering and reading ──────────────────────────────────────── */

/* One pass over the file's contents into the current buffer. Returns 0, or
 * -EOVERFLOW when the buffer was too small — the caller grows and retries. */
static int seq_render_once(struct seq_file *m)
{
	m->count = 0;
	m->overflow = 0;
	m->buf[0] = 0;

	if (m->single_show) {
		m->single_show(m, 0);
	} else if (m->op) {
		loff_t pos = 0;
		void *v = m->op->start ? m->op->start(m, &pos) : 0;

		while (v) {
			if (m->op->show)
				m->op->show(m, v);
			if (m->overflow)
				break;
			v = m->op->next ? m->op->next(m, v, &pos) : 0;
		}
		if (m->op->stop)
			m->op->stop(m, v);
	}
	return m->overflow ? -EOVERFLOW : 0;
}

/* Render into a buffer that is grown until the whole thing fits. */
static int seq_render(struct seq_file *m)
{
	usize size = m->size ? m->size : SEQ_BUF_MIN;

	if (m->rendered)
		return 0;

	for (;;) {
		if (!m->buf || m->size < size) {
			kfree(m->buf);
			m->buf = kmalloc(size, GFP_KERNEL);
			if (!m->buf) {
				m->size = 0;
				return -ENOMEM;
			}
			m->size = size;
		}
		if (seq_render_once(m) == 0)
			break;
		if (size >= SEQ_BUF_MAX)
			break; /* keep what fits rather than failing the read outright */
		size *= 2;
	}
	m->rendered = 1;
	return 0;
}

ssize_t seq_read(struct file *file, char __user *buf, size_t size, loff_t *ppos)
{
	struct seq_file *m = file ? file->private_data : 0;

	if (!m || !buf || !ppos)
		return -EINVAL;

	int rc = seq_render(m);
	if (rc)
		return rc;

	if (*ppos < 0 || (usize)*ppos >= m->count)
		return 0; /* past the end: a read there is EOF, not an error */

	usize avail = m->count - (usize)*ppos;
	usize n = avail < size ? avail : size;
	memcpy(buf, m->buf + (usize)*ppos, n);
	*ppos += (loff_t)n;
	return (ssize_t)n;
}

loff_t seq_lseek(struct file *file, loff_t offset, int whence)
{
	struct seq_file *m = file ? file->private_data : 0;
	loff_t base = 0;

	if (!m)
		return -EINVAL;
	/* SEEK_END needs the size, and the only way to know it is to render. */
	if (whence == 2) {
		if (seq_render(m) != 0)
			return -ENOMEM;
		base = (loff_t)m->count;
	} else if (whence == 1) {
		base = file->f_pos;
	}
	if (base + offset < 0)
		return -EINVAL;
	file->f_pos = base + offset;
	return file->f_pos;
}
