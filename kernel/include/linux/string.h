/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_STRING_H
#define LKPI_LINUX_STRING_H
#include <b1nix/types.h>
#include <string.h>
/* b1nix's kernel string.h already provides memcpy/memset/strlen/strcmp and
 * friends with the same signatures; this only adds the spellings Linux has
 * that it does not. */
static inline char *strim(char *s) { return s; }

/*
 * Bounded copy that always terminates and reports truncation, which is what
 * makes it safer than strncpy: strncpy leaves the destination unterminated when
 * the source fills it exactly, and callers then read past the end.
 * Returns the length copied, or -E2BIG on truncation.
 */
static inline isize strscpy(char *dst, const char *src, usize size)
{
	if (size == 0)
		return -7 /* -E2BIG */;
	usize i = 0;
	while (i + 1 < size && src[i]) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = 0;
	return src[i] ? -7 : (isize)i;
}

/* As above, then zero-fills the rest of the buffer — for a field that is
 * compared or hashed whole. */
static inline isize strscpy_pad(char *dst, const char *src, usize size)
{
	isize r = strscpy(dst, src, size);
	usize len = (r < 0) ? size - 1 : (usize)r;
	for (usize i = len; i < size; i++)
		dst[i] = 0;
	return r;
}

/* First byte in the buffer that is NOT `c`, or NULL if all of them are. Used to
 * check that a block is entirely padding. */
/* Find a character within the first n bytes, or NULL. */
/* Length of the prefix if `str` starts with it, else 0 — so the result doubles
 * as "how far to skip". */
/* Compare a sysfs-style input against a constant, ignoring one trailing
 * newline — which is what a shell's echo leaves behind. */
static inline int sysfs_streq(const char *a, const char *b)
{
	while (*a && *b && *a == *b) {
		a++;
		b++;
	}
	if (*a == '\n' && !a[1])
		a++;
	if (*b == '\n' && !b[1])
		b++;
	return *a == *b;
}

static inline usize str_has_prefix(const char *str, const char *prefix)
{
	usize i = 0;
	while (prefix[i] && str[i] == prefix[i])
		i++;
	return prefix[i] ? 0 : i;
}

static inline char *strnchr(const char *s, usize n, int c)
{
	for (usize i = 0; i < n && s[i]; i++)
		if (s[i] == (char)c)
			return (char *)(s + i);
	return 0;
}

static inline void *memchr_inv(const void *p, int c, usize n)
{
	const unsigned char *s = (const unsigned char *)p;
	for (usize i = 0; i < n; i++)
		if (s[i] != (unsigned char)c)
			return (void *)(s + i);
	return 0;
}

/* b1nix's kernel stdio provides these with the same signatures. */
int snprintf(char *buf, usize size, const char *fmt, ...);
int scnprintf(char *buf, usize size, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int vsnprintf(char *buf, usize size, const char *fmt, __builtin_va_list ap);
#endif
