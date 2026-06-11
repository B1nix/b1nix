/* UTF-8 multibyte / wide-character support.
 *
 * Implemented to let ports (notably GNU bash, via HANDLE_MULTIBYTE) handle
 * UTF-8 text and do correct multibyte line editing. The encoding is UTF-8 only;
 * the conversion functions treat each call's input as UTF-8 and produce/consume
 * Unicode code points in wchar_t (a 32-bit int here).
 *
 * mbstate_t is effectively unused: callers reset it before a buffer and process
 * the buffer to completion, and an incomplete trailing sequence is reported as
 * (size_t)-2 rather than carried across calls — sufficient for bash/readline.
 */
#include <wchar.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* Decode one UTF-8 code point from s[0..n). Returns bytes consumed (>0), 0 for
 * the NUL, (size_t)-2 if the buffer holds only an incomplete prefix, or
 * (size_t)-1 (EILSEQ) on an invalid sequence. */
size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps) {
	(void)ps;
	if (s == NULL)
		return 0; /* return to initial state; UTF-8 is stateless */
	if (n == 0)
		return (size_t)-2;

	unsigned char c0 = (unsigned char)s[0];
	int need;
	unsigned int v;

	if (c0 < 0x80) {
		if (pwc)
			*pwc = (wchar_t)c0;
		return c0 ? 1 : 0;
	} else if ((c0 & 0xE0) == 0xC0) {
		need = 2;
		v = c0 & 0x1F;
	} else if ((c0 & 0xF0) == 0xE0) {
		need = 3;
		v = c0 & 0x0F;
	} else if ((c0 & 0xF8) == 0xF0) {
		need = 4;
		v = c0 & 0x07;
	} else {
		errno = EILSEQ;
		return (size_t)-1;
	}

	if (n < (size_t)need)
		return (size_t)-2; /* incomplete */

	for (int i = 1; i < need; i++) {
		unsigned char cc = (unsigned char)s[i];
		if ((cc & 0xC0) != 0x80) {
			errno = EILSEQ;
			return (size_t)-1;
		}
		v = (v << 6) | (cc & 0x3F);
	}

	/* Reject overlong encodings and out-of-range code points. */
	static const unsigned int mins[5] = {0, 0, 0x80, 0x800, 0x10000};
	if (v < mins[need] || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {
		errno = EILSEQ;
		return (size_t)-1;
	}

	if (pwc)
		*pwc = (wchar_t)v;
	return (size_t)need;
}

size_t mbrlen(const char *s, size_t n, mbstate_t *ps) {
	return mbrtowc(NULL, s, n, ps);
}

/* Non-restartable byte length of the next multibyte character (UTF-8). Returns
 * 0 for the NUL, -1 on an invalid/incomplete sequence. */
int mblen(const char *s, size_t n) {
	if (s == NULL)
		return 0; /* UTF-8 has no shift state */
	size_t r = mbrtowc(NULL, s, n, NULL);
	if (r == (size_t)-1 || r == (size_t)-2)
		return -1;
	return (int)r;
}

/* Encode wc as UTF-8 into s (must have room for up to MB_CUR_MAX=4 bytes).
 * Returns the number of bytes written, or (size_t)-1 (EILSEQ) on a bad value. */
size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps) {
	(void)ps;
	char buf[4];
	if (s == NULL) {
		s = buf;
		wc = 0; /* "return to initial state" — write a NUL, report 1 */
	}
	unsigned int v = (unsigned int)wc;
	if (v < 0x80) {
		s[0] = (char)v;
		return 1;
	} else if (v < 0x800) {
		s[0] = (char)(0xC0 | (v >> 6));
		s[1] = (char)(0x80 | (v & 0x3F));
		return 2;
	} else if (v < 0x10000) {
		if (v >= 0xD800 && v <= 0xDFFF) {
			errno = EILSEQ;
			return (size_t)-1;
		}
		s[0] = (char)(0xE0 | (v >> 12));
		s[1] = (char)(0x80 | ((v >> 6) & 0x3F));
		s[2] = (char)(0x80 | (v & 0x3F));
		return 3;
	} else if (v <= 0x10FFFF) {
		s[0] = (char)(0xF0 | (v >> 18));
		s[1] = (char)(0x80 | ((v >> 12) & 0x3F));
		s[2] = (char)(0x80 | ((v >> 6) & 0x3F));
		s[3] = (char)(0x80 | (v & 0x3F));
		return 4;
	}
	errno = EILSEQ;
	return (size_t)-1;
}

int mbsinit(const mbstate_t *ps) {
	(void)ps;
	return 1; /* always the initial (stateless) conversion state */
}

/* Display width of a code point: -1 for non-printable, 0 for combining marks,
 * 2 for East-Asian-wide, 1 otherwise. Ranges are the common subset. */
int wcwidth(wchar_t wc) {
	unsigned int c = (unsigned int)wc;
	if (c == 0)
		return 0;
	if (c < 0x20 || (c >= 0x7F && c < 0xA0))
		return -1;
	if ((c >= 0x0300 && c <= 0x036F) || (c >= 0x0483 && c <= 0x0489) ||
	    (c >= 0x0591 && c <= 0x05BD) || (c >= 0x1AB0 && c <= 0x1AFF) ||
	    (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x20D0 && c <= 0x20FF) ||
	    (c >= 0xFE20 && c <= 0xFE2F) || c == 0x200B)
		return 0;
	if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) ||
	    (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
	    (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF60) ||
	    (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1FAFF) ||
	    (c >= 0x20000 && c <= 0x3FFFD))
		return 2;
	return 1;
}

int wcswidth(const wchar_t *s, size_t n) {
	int w = 0;
	for (size_t i = 0; i < n && s[i]; i++) {
		int cw = wcwidth(s[i]);
		if (cw < 0)
			return -1;
		w += cw;
	}
	return w;
}

size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *ps) {
	const char *p = *src;
	size_t count = 0;
	for (;;) {
		if (dst && count >= len)
			break;
		wchar_t wc;
		size_t r = mbrtowc(&wc, p, 4, ps);
		if (r == (size_t)-1)
			return (size_t)-1;
		if (r == (size_t)-2)
			return (size_t)-1; /* incomplete treated as error here */
		if (dst)
			dst[count] = wc;
		count++;
		if (r == 0) { /* hit NUL */
			if (dst)
				*src = NULL;
			return count - 1;
		}
		p += r;
	}
	if (dst)
		*src = p;
	return count;
}

size_t wcsrtombs(char *dst, const wchar_t **src, size_t len, mbstate_t *ps) {
	const wchar_t *p = *src;
	size_t count = 0;
	char tmp[4];
	while (*p) {
		size_t r = wcrtomb(tmp, *p, ps);
		if (r == (size_t)-1)
			return (size_t)-1;
		if (dst) {
			if (count + r > len)
				break;
			for (size_t i = 0; i < r; i++)
				dst[count + i] = tmp[i];
		}
		count += r;
		p++;
	}
	if (dst) {
		if (count < len) {
			dst[count] = '\0';
			*src = NULL;
		} else {
			*src = p;
		}
	}
	return count;
}

wint_t btowc(int c) {
	if (c == EOF || (unsigned)c >= 0x80)
		return WEOF;
	return (wint_t)c;
}

int wctob(wint_t c) {
	if (c < 0x80)
		return (int)c;
	return EOF;
}

/* Wide-string helpers (locale-independent; collation == code-point order).
 * wcslen/wcscat/wcscpy/wmem* live in string.c; these are the additions. */
int wcscmp(const wchar_t *a, const wchar_t *b) {
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (int)(*a - *b);
}

int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n) {
	for (size_t i = 0; i < n; i++) {
		if (a[i] != b[i])
			return (int)(a[i] - b[i]);
		if (a[i] == 0)
			break;
	}
	return 0;
}

int wcscoll(const wchar_t *a, const wchar_t *b) {
	return wcscmp(a, b);
}

wchar_t *wcschr(const wchar_t *s, wchar_t c) {
	for (; *s; s++)
		if (*s == c)
			return (wchar_t *)s;
	return c == 0 ? (wchar_t *)s : NULL;
}

wchar_t *wcsrchr(const wchar_t *s, wchar_t c) {
	const wchar_t *last = NULL;
	for (; *s; s++)
		if (*s == c)
			last = s;
	if (c == 0)
		return (wchar_t *)s;
	return (wchar_t *)last;
}

wchar_t *wcsncpy(wchar_t *dest, const wchar_t *src, size_t n) {
	size_t i = 0;
	for (; i < n && src[i]; i++)
		dest[i] = src[i];
	for (; i < n; i++)
		dest[i] = 0;
	return dest;
}

int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n) {
	for (size_t i = 0; i < n; i++)
		if (a[i] != b[i])
			return (int)(a[i] - b[i]);
	return 0;
}

wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n) {
	for (size_t i = 0; i < n; i++)
		if (s[i] == c)
			return (wchar_t *)(s + i);
	return NULL;
}

wchar_t *wcsdup(const wchar_t *s) {
	size_t n = wcslen(s) + 1;
	wchar_t *d = (wchar_t *)malloc(n * sizeof(wchar_t));
	if (!d)
		return NULL;
	for (size_t i = 0; i < n; i++)
		d[i] = s[i];
	return d;
}
