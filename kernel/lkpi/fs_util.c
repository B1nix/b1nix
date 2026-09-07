/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * linuxkpi: the small services the imported filesystems stand on.
 *
 * Strings, number parsing, mount-option matching, time, identity, and the
 * handful of allocator and bit helpers that have no home of their own. Nothing
 * here is filesystem-specific; it is the layer a filesystem assumes exists.
 *
 * Kept on the b1nix side of the boundary — b1nix and lkpi headers only, never a
 * linux/ one — for the reason <lkpi/env.h> gives: b1nix and Linux spell
 * `spinlock_t`, `kmalloc` and `current` differently, and a translation unit that
 * sees both has to be rescued by include order.
 */

#include <lkpi/env.h>
#include <lkpi/types.h>
#include <b1nix/arch.h>
#include <b1nix/klog.h>
#include <b1nix/ktime.h>
#include <b1nix/rtc.h>
#include <b1nix/sched.h>
#include <string.h>

/* ── strings ────────────────────────────────────────────────────── */

static int lkpi_isspace(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
	       c == '\r';
}

/*
 * Advance past leading whitespace, returning a pointer into the SAME string.
 *
 * Not a copy — callers walk a mount-option list they are also writing NULs
 * into, and a copy would leave them editing the wrong buffer.
 */
char *skip_spaces(const char *str)
{
	while (lkpi_isspace(*str))
		++str;
	return (char *)str;
}

/* Trim both ends in place, returning where the trimmed string starts. The
 * trailing NUL is written into the caller's buffer, which is why it takes a
 * mutable pointer where skip_spaces does not. */
char *strim(char *s)
{
	size_t size;
	char *end;

	size = strlen(s);
	if (!size)
		return s;
	end = s + size - 1;
	while (end >= s && lkpi_isspace(*end))
		end--;
	*(end + 1) = '\0';
	return skip_spaces(s);
}

char *strreplace(char *str, char old, char new)
{
	char *s = str;

	for (; *s; ++s)
		if (*s == old)
			*s = new;
	return str;
}

size_t strcspn(const char *s, const char *reject)
{
	const char *p;

	for (p = s; *p; p++) {
		const char *r;

		for (r = reject; *r; r++)
			if (*p == *r)
				return (size_t)(p - s);
	}
	return (size_t)(p - s);
}

size_t strspn(const char *s, const char *accept)
{
	const char *p;

	for (p = s; *p; p++) {
		const char *a;

		for (a = accept; *a; a++)
			if (*p == *a)
				break;
		if (!*a)
			break;
	}
	return (size_t)(p - s);
}

char *strpbrk(const char *cs, const char *ct)
{
	const char *s;

	for (s = cs; *s; s++) {
		const char *c;

		for (c = ct; *c; c++)
			if (*s == *c)
				return (char *)s;
	}
	return NULL;
}

/*
 * Split at the first character in `ct`, advancing the caller's pointer past it.
 *
 * The separator is overwritten with a NUL, so the returned token is a substring
 * of the original — which is what makes it usable on a mount-option string and
 * unusable on a string literal.
 */
char *strsep(char **s, const char *ct)
{
	char *sbegin = *s;
	char *end;

	if (!sbegin)
		return NULL;
	end = strpbrk(sbegin, ct);
	if (end)
		*end++ = '\0';
	*s = end;
	return sbegin;
}

/* The base name of a path, without copying. A trailing slash is not stripped:
 * upstream's does not either, and a caller that cares checks. */
const char *kbasename(const char *path)
{
	const char *tail = strrchr(path, '/');

	return tail ? tail + 1 : path;
}

size_t strlcpy(char *dest, const char *src, size_t size)
{
	size_t ret = strlen(src);

	if (size) {
		size_t len = (ret >= size) ? size - 1 : ret;

		memcpy(dest, src, len);
		dest[len] = '\0';
	}
	/* The SOURCE length, so a caller can detect truncation. Returning the
	 * copied length instead makes truncation invisible. */
	return ret;
}

/* `isize` rather than ssize_t: this file is on the b1nix side, where that is
 * the spelling. The Linux declaration in <linux/string.h> says ssize_t, and the
 * two are the same type. */
isize strscpy(char *dest, const char *src, size_t count)
{
	size_t len = strlen(src);

	if (!count)
		return -7; /* -E2BIG */
	if (len >= count) {
		memcpy(dest, src, count - 1);
		dest[count - 1] = '\0';
		return -7;
	}
	memcpy(dest, src, len + 1);
	return (isize)len;
}

int match_string(const char * const *array, size_t n, const char *string)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (!array[i])
			break;
		if (!strcmp(array[i], string))
			return (int)i;
	}
	return -22; /* -EINVAL */
}

char *kstrndup(const char *s, size_t max, u32 gfp)
{
	size_t len;
	char *buf;

	if (!s)
		return NULL;
	for (len = 0; len < max && s[len]; len++)
		;
	buf = lkpi_kmalloc(len + 1, gfp);
	if (buf) {
		memcpy(buf, s, len);
		buf[len] = '\0';
	}
	return buf;
}

/*
 * Duplicate at most `len` bytes and ALWAYS terminate.
 *
 * That is the difference from kmemdup and the reason it exists: an on-disk name
 * is not NUL-terminated, and treating it as if it were reads past the record
 * into whatever follows it in the metadata block.
 */
char *kmemdup_nul(const char *s, size_t len, u32 gfp)
{
	char *buf = lkpi_kmalloc(len + 1, gfp);

	if (buf) {
		memcpy(buf, s, len);
		buf[len] = '\0';
	}
	return buf;
}

/* ── numbers ────────────────────────────────────────────────────── */

/* Implemented in kernel/lkpi/linux_compat.c, declared here because this file
 * calls them and is on the b1nix side, where <linux/kstrtox.h> is out of
 * bounds. */
int kstrtoll(const char *s, unsigned int base, long long *res);
int kstrtouint(const char *s, unsigned int base, unsigned int *res);

/*
 * Only what kernel/lkpi/linux_compat.c does not already provide.
 *
 * That file arrived with the DRM import and implements kstrtoint, kstrtoul and
 * the simple_strto* family; the filesystems need a few more of the same shape,
 * and the ones that already exist are deliberately not repeated here — two
 * definitions of the same name is a link failure, and the second one silently
 * winning would be worse.
 */

/*
 * The core of the kstrto* family.
 *
 * Returns 0 and the value, or -EINVAL. The distinction that matters is between
 * "not a number" and "the number zero": a mount option parser that cannot tell
 * them apart accepts `commit=banana` as `commit=0`.
 *
 * base 0 infers from the prefix — 0x hex, 0 octal, otherwise decimal.
 */
static int lkpi_strtoull(const char *s, unsigned int base, unsigned long long *res)
{
	unsigned long long acc = 0;
	int any = 0;

	if (!s || !*s)
		return -22;
	while (lkpi_isspace(*s))
		s++;
	if (base == 0) {
		if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
			base = 16;
			s += 2;
		} else if (s[0] == '0' && s[1]) {
			base = 8;
			s++;
		} else {
			base = 10;
		}
	} else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
	}
	for (; *s; s++) {
		unsigned int d;

		if (*s >= '0' && *s <= '9')
			d = (unsigned int)(*s - '0');
		else if (*s >= 'a' && *s <= 'f')
			d = (unsigned int)(*s - 'a' + 10);
		else if (*s >= 'A' && *s <= 'F')
			d = (unsigned int)(*s - 'A' + 10);
		else if (*s == '\n' && s[1] == '\0')
			break;   /* a trailing newline is accepted, as upstream does */
		else
			return -22;
		if (d >= base)
			return -22;
		acc = acc * base + d;
		any = 1;
	}
	if (!any)
		return -22;
	*res = acc;
	return 0;
}

int kstrtoul(const char *s, unsigned int base, unsigned long *res)
{
	unsigned long long v;
	int ret = lkpi_strtoull(s, base, &v);

	if (ret)
		return ret;
	*res = (unsigned long)v;
	return 0;
}

int kstrtol(const char *s, unsigned int base, long *res)
{
	long long v;
	int ret = kstrtoll(s, base, &v);

	if (ret)
		return ret;
	*res = (long)v;
	return 0;
}

int kstrtou64(const char *s, unsigned int base, u64 *res)
{
	unsigned long long v;
	int ret = lkpi_strtoull(s, base, &v);

	if (ret)
		return ret;
	*res = (u64)v;
	return 0;
}

int kstrtos64(const char *s, unsigned int base, i64 *res)
{
	long long v;
	int ret = kstrtoll(s, base, &v);

	if (ret)
		return ret;
	*res = (i64)v;
	return 0;
}

unsigned long long simple_strtoull(const char *cp, char **endp, unsigned int base)
{
	unsigned long long result = 0;

	if (base == 0) {
		if (cp[0] == '0' && (cp[1] == 'x' || cp[1] == 'X')) {
			base = 16;
			cp += 2;
		} else if (cp[0] == '0') {
			base = 8;
			cp++;
		} else {
			base = 10;
		}
	}
	for (;;) {
		unsigned int d;

		if (*cp >= '0' && *cp <= '9')
			d = (unsigned int)(*cp - '0');
		else if (*cp >= 'a' && *cp <= 'f')
			d = (unsigned int)(*cp - 'a' + 10);
		else if (*cp >= 'A' && *cp <= 'F')
			d = (unsigned int)(*cp - 'A' + 10);
		else
			break;
		if (d >= base)
			break;
		result = result * base + d;
		cp++;
	}
	if (endp)
		*endp = (char *)cp;
	return result;
}

/*
 * A size with an optional K/M/G/T suffix.
 *
 * `retptr` is where it stopped, and a caller checks it to reject trailing
 * junk — the difference between accepting "16M" and accepting "16Mb-please".
 */
unsigned long long memparse(const char *ptr, char **retptr)
{
	char *endptr;
	unsigned long long ret = simple_strtoull(ptr, &endptr, 0);

	switch (*endptr) {
	case 'E': case 'e':
		ret <<= 10; /* fall through */
		__attribute__((fallthrough));
	case 'P': case 'p':
		ret <<= 10;
		__attribute__((fallthrough));
	case 'T': case 't':
		ret <<= 10;
		__attribute__((fallthrough));
	case 'G': case 'g':
		ret <<= 10;
		__attribute__((fallthrough));
	case 'M': case 'm':
		ret <<= 10;
		__attribute__((fallthrough));
	case 'K': case 'k':
		ret <<= 10;
		endptr++;
		break;
	default:
		break;
	}
	if (retptr)
		*retptr = endptr;
	return ret;
}

/* ── time ───────────────────────────────────────────────────────── */

/* Mirrors struct timespec64 from <linux/time64.h>; this file is on the b1nix
 * side and cannot include it. Checked against the real one by
 * kernel/lkpi/fs_abi_check.c. */
struct lkpi_timespec64 {
	long long tv_sec;
	long tv_nsec;
};

/*
 * Wall clock, in seconds and nanoseconds.
 *
 * It goes into an inode timestamp and onto the disk, so it must be the real
 * time and not the monotonic one — a filesystem timestamped from a counter that
 * restarts every boot is one every tool reports as being from 1970.
 */
long long ktime_get_real_seconds(void)
{
	return (long long)rtc_now_unix_seconds();
}

u64 ktime_get_real_ns(void)
{
	return rtc_now_unix_nanos();
}

/* Monotonic, for measuring intervals. Using the wall clock here would make a
 * commit interval jump when somebody sets the clock. */
u64 ktime_get_ns(void)
{
	return ktime_monotonic_ns();
}

u64 ktime_get_seconds(void)
{
	return ktime_monotonic_ns() / 1000000000ull;
}

/* A raw counter with no fixed unit. btrfs uses it only as a source of entropy
 * for hashing, never as a time. */
u64 get_cycles(void)
{
	return ktime_monotonic_ns();
}

/* Wall clock as a timespec, which is what an inode timestamp is set from. */
void ktime_get_real_ts64(struct lkpi_timespec64 *ts)
{
	u64 ns = rtc_now_unix_nanos();

	ts->tv_sec = (long long)(ns / 1000000000ull);
	ts->tv_nsec = (long)(ns % 1000000000ull);
}

void ktime_get_coarse_real_ts64(struct lkpi_timespec64 *ts)
{
	/* "Coarse" upstream means the cached tick time rather than a hardware
	 * read. b1nix's clock read is cheap enough that the distinction would be
	 * a second code path for no measurable gain. */
	ktime_get_real_ts64(ts);
}
