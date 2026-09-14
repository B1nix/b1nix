/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SEQ_BUF_H
#define LKPI_LINUX_SEQ_BUF_H
#include <linux/kernel.h>
#include <linux/bug.h>
#include <linux/stdarg.h>
/* Upstream's EDID code reaches seq_write() through the headers around this one. */
#include <linux/seq_file.h>

/*
 * A sequence buffer: printf into a fixed buffer, remembering whether it
 * overflowed. Linux's own (include/linux/seq_buf.h, lib/seq_buf.c); only the
 * parts the DRM import uses. Overflow is recorded as len > size.
 */
struct seq_buf {
	char *buffer;
	size_t size;
	size_t len;
};

#define DECLARE_SEQ_BUF(NAME, SIZE)            \
	struct seq_buf NAME = {                \
		.buffer = (char[SIZE]) { 0 },  \
		.size = SIZE,                  \
	}

static inline void seq_buf_clear(struct seq_buf *s)
{
	s->len = 0;
	if (s->size)
		s->buffer[0] = '\0';
}

static inline void seq_buf_init(struct seq_buf *s, char *buf, unsigned int size)
{
	s->buffer = buf;
	s->size = size;
	seq_buf_clear(s);
}

static inline bool seq_buf_has_overflowed(struct seq_buf *s)
{ return s->len > s->size; }

static inline void seq_buf_set_overflow(struct seq_buf *s)
{ s->len = s->size + 1; }

static inline unsigned int seq_buf_buffer_left(struct seq_buf *s)
{
	if (seq_buf_has_overflowed(s))
		return 0;
	return (unsigned int)(s->size - s->len);
}

static inline const char *seq_buf_str(struct seq_buf *s)
{
	if (WARN_ON(s->size == 0))
		return "";
	if (seq_buf_buffer_left(s))
		s->buffer[s->len] = 0;
	else
		s->buffer[s->size - 1] = 0;
	return s->buffer;
}

static inline int seq_buf_vprintf(struct seq_buf *s, const char *fmt, va_list args)
{
	int len;

	WARN_ON(s->size == 0);
	if (s->len < s->size) {
		len = vsnprintf(s->buffer + s->len, s->size - s->len, fmt, args);
		if (s->len + len < s->size) {
			s->len += len;
			return 0;
		}
	}
	seq_buf_set_overflow(s);
	return -1;
}

static inline int seq_buf_printf(struct seq_buf *s, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = seq_buf_vprintf(s, fmt, ap);
	va_end(ap);
	return ret;
}

#endif
