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
#include <time.h>

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

/* Bounded variants for libc++'s locale fallbacks (bsd_locale_fallbacks.h):
 * consume at most nmc source bytes / nwc source wide chars. */
size_t mbsnrtowcs(wchar_t *dst, const char **src, size_t nmc, size_t len, mbstate_t *ps) {
	const char *p = *src;
	size_t count = 0, consumed = 0;
	for (;;) {
		if (dst && count >= len)
			break;
		if (consumed >= nmc)
			break;
		wchar_t wc;
		size_t avail = nmc - consumed;
		size_t r = mbrtowc(&wc, p, avail < 4 ? avail : 4, ps);
		if (r == (size_t)-1)
			return (size_t)-1;
		if (r == (size_t)-2)
			break; /* multibyte char straddles the nmc bound — stop */
		if (dst)
			dst[count] = wc;
		count++;
		if (r == 0) { /* source NUL */
			if (dst)
				*src = NULL;
			return count - 1;
		}
		p += r;
		consumed += r;
	}
	if (dst)
		*src = p;
	return count;
}

size_t wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t len, mbstate_t *ps) {
	const wchar_t *p = *src;
	size_t count = 0, consumed = 0;
	char tmp[4];
	while (consumed < nwc && *p) {
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
		consumed++;
	}
	if (dst) {
		if (consumed < nwc && *p == 0 && count < len) {
			dst[count] = '\0'; /* reached source NUL within bounds */
			*src = NULL;
		} else {
			*src = p;
		}
	}
	return count;
}

/* Non-restartable string conversions (UTF-8). */
size_t mbstowcs(wchar_t *dest, const char *src, size_t n) {
	const char *p = src;
	mbstate_t st = {0, 0};
	if (!dest) {
		/* count characters */
		size_t count = 0;
		while (*p) {
			wchar_t wc;
			size_t r = mbrtowc(&wc, p, 4, &st);
			if (r == (size_t)-1 || r == (size_t)-2)
				return (size_t)-1;
			if (r == 0)
				break;
			p += r;
			count++;
		}
		return count;
	}
	size_t i = 0;
	while (i < n) {
		wchar_t wc;
		size_t r = mbrtowc(&wc, p, 4, &st);
		if (r == (size_t)-1)
			return (size_t)-1;
		dest[i] = wc;
		if (r == 0)
			return i; /* not counting the terminating NUL */
		p += r;
		i++;
	}
	return i;
}

size_t wcstombs(char *dest, const wchar_t *src, size_t n) {
	const wchar_t *p = src;
	mbstate_t st = {0, 0};
	char tmp[4];
	size_t count = 0;
	while (*p) {
		size_t r = wcrtomb(tmp, *p, &st);
		if (r == (size_t)-1)
			return (size_t)-1;
		if (dest) {
			if (count + r > n)
				break;
			for (size_t i = 0; i < r; i++)
				dest[count + i] = tmp[i];
		}
		count += r;
		p++;
	}
	if (dest && count < n)
		dest[count] = '\0';
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

/* ── More wide-string operations (C-locale) ───────────────────────────────── */

wchar_t *wcsncat(wchar_t *dest, const wchar_t *src, size_t n) {
	wchar_t *d = dest;
	while (*d)
		d++;
	size_t i = 0;
	for (; i < n && src[i]; i++)
		d[i] = src[i];
	d[i] = 0;
	return dest;
}

size_t wcsspn(const wchar_t *s, const wchar_t *accept) {
	size_t count = 0;
	for (; s[count]; count++) {
		const wchar_t *a = accept;
		for (; *a && *a != s[count]; a++)
			;
		if (!*a)
			break;
	}
	return count;
}

size_t wcscspn(const wchar_t *s, const wchar_t *reject) {
	size_t count = 0;
	for (; s[count]; count++) {
		const wchar_t *r = reject;
		for (; *r && *r != s[count]; r++)
			;
		if (*r)
			break;
	}
	return count;
}

wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept) {
	for (; *s; s++) {
		const wchar_t *a = accept;
		for (; *a; a++)
			if (*a == *s)
				return (wchar_t *)s;
	}
	return NULL;
}

wchar_t *wcsstr(const wchar_t *haystack, const wchar_t *needle) {
	if (!*needle)
		return (wchar_t *)haystack;
	for (; *haystack; haystack++) {
		const wchar_t *h = haystack, *n = needle;
		while (*h && *n && *h == *n) {
			h++;
			n++;
		}
		if (!*n)
			return (wchar_t *)haystack;
	}
	return NULL;
}

wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **saveptr) {
	if (s == NULL)
		s = *saveptr;
	if (s == NULL)
		return NULL;
	/* skip leading delimiters */
	s += wcsspn(s, delim);
	if (*s == 0) {
		*saveptr = NULL;
		return NULL;
	}
	/* find end of token */
	wchar_t *end = s + wcscspn(s, delim);
	if (*end) {
		*end = 0;
		*saveptr = end + 1;
	} else {
		*saveptr = NULL;
	}
	return s;
}

/* ── Wide numeric conversion (C-locale; transcode to a narrow ASCII buffer) ──
 * A numeric literal is ASCII-only in the C locale, so it is safe to copy the
 * leading run of characters that could form a number into a narrow buffer,
 * delegate to the narrow strto* function, then map the narrow endptr back to
 * the wide string by character count. */

#define WSTRTO_BUF 128

static size_t __wide_to_narrow_numeric(const wchar_t *nptr, char *buf,
                                       size_t bufsz) {
	size_t i = 0;
	for (; nptr[i] && i + 1 < bufsz; i++) {
		wchar_t c = nptr[i];
		if (c < 0 || c > 0x7F)
			break; /* non-ASCII can't be part of a C-locale number */
		buf[i] = (char)c;
	}
	buf[i] = 0;
	return i;
}

#define DEFINE_WCSTO_INT(name, narrow, rettype)                                \
	rettype name(const wchar_t *nptr, wchar_t **endptr, int base) {         \
		char buf[WSTRTO_BUF];                                           \
		size_t copied = __wide_to_narrow_numeric(nptr, buf, sizeof buf); \
		char *nend = NULL;                                             \
		rettype r = narrow(buf, &nend, base);                          \
		if (endptr) {                                                  \
			size_t off = (size_t)(nend - buf);                    \
			if (off > copied)                                     \
				off = copied;                                 \
			*endptr = (wchar_t *)(nptr + off);                    \
		}                                                             \
		return r;                                                      \
	}

DEFINE_WCSTO_INT(wcstol, strtol, long)
DEFINE_WCSTO_INT(wcstoul, strtoul, unsigned long)
DEFINE_WCSTO_INT(wcstoll, strtoll, long long)
DEFINE_WCSTO_INT(wcstoull, strtoull, unsigned long long)

#define DEFINE_WCSTO_FLT(name, narrow, rettype)                                \
	rettype name(const wchar_t *nptr, wchar_t **endptr) {                  \
		char buf[WSTRTO_BUF];                                           \
		size_t copied = __wide_to_narrow_numeric(nptr, buf, sizeof buf); \
		char *nend = NULL;                                             \
		rettype r = narrow(buf, &nend);                               \
		if (endptr) {                                                  \
			size_t off = (size_t)(nend - buf);                    \
			if (off > copied)                                     \
				off = copied;                                 \
			*endptr = (wchar_t *)(nptr + off);                    \
		}                                                             \
		return r;                                                      \
	}

DEFINE_WCSTO_FLT(wcstod, strtod, double)
DEFINE_WCSTO_FLT(wcstof, strtof, float)
DEFINE_WCSTO_FLT(wcstold, strtold, long double)

/* ── Collation transform (C locale: identity copy, like strxfrm) ──────────── */
size_t wcsxfrm(wchar_t *dest, const wchar_t *src, size_t n) {
	size_t len = wcslen(src);
	if (n) {
		size_t copy = len < n - 1 ? len : n - 1;
		for (size_t i = 0; i < copy; i++)
			dest[i] = src[i];
		dest[copy] = 0;
	}
	return len;
}

/* ── wcsftime — wide wrapper over strftime ────────────────────────────────── */
size_t wcsftime(wchar_t *s, size_t maxsize, const wchar_t *format,
                const struct tm *timeptr) {
	if (maxsize == 0)
		return 0;
	/* Transcode the (ASCII/UTF-8-representable) wide format to narrow. */
	char *nfmt = (char *)malloc(maxsize * 4 + 1);
	if (!nfmt)
		return 0;
	size_t fi = 0;
	char tmp[4];
	for (const wchar_t *p = format; *p; p++) {
		size_t r = wcrtomb(tmp, *p, NULL);
		if (r == (size_t)-1) {
			free(nfmt);
			return 0;
		}
		for (size_t k = 0; k < r; k++)
			nfmt[fi++] = tmp[k];
	}
	nfmt[fi] = 0;

	char *nbuf = (char *)malloc(maxsize * 4 + 1);
	if (!nbuf) {
		free(nfmt);
		return 0;
	}
	size_t nlen = strftime(nbuf, maxsize * 4 + 1, nfmt, timeptr);
	free(nfmt);
	if (nlen == 0) {
		free(nbuf);
		return 0;
	}
	/* Transcode the narrow result back to wide, bounded by maxsize. */
	const char *q = nbuf;
	size_t wi = 0;
	mbstate_t st = {0, 0};
	while (*q && wi + 1 < maxsize) {
		wchar_t wc;
		size_t r = mbrtowc(&wc, q, 4, &st);
		if (r == (size_t)-1 || r == (size_t)-2)
			break;
		if (r == 0)
			break;
		s[wi++] = wc;
		q += r;
	}
	s[wi] = 0;
	free(nbuf);
	/* POSIX: 0 if the result (incl. NUL) did not fit. */
	if (*q != 0)
		return 0;
	return wi;
}

/* ── Wide stdio: UTF-8 byte stream beneath; C-locale conversion ───────────── */

wint_t fgetwc(FILE *stream) {
	char buf[4];
	int c = fgetc(stream);
	if (c == EOF)
		return WEOF;
	buf[0] = (char)c;
	unsigned char c0 = (unsigned char)c;
	int need = 1;
	if (c0 >= 0x80) {
		if ((c0 & 0xE0) == 0xC0)
			need = 2;
		else if ((c0 & 0xF0) == 0xE0)
			need = 3;
		else if ((c0 & 0xF8) == 0xF0)
			need = 4;
		else {
			errno = EILSEQ;
			return WEOF;
		}
	}
	for (int i = 1; i < need; i++) {
		int cc = fgetc(stream);
		if (cc == EOF) {
			errno = EILSEQ;
			return WEOF;
		}
		buf[i] = (char)cc;
	}
	wchar_t wc;
	size_t r = mbrtowc(&wc, buf, (size_t)need, NULL);
	if (r == (size_t)-1 || r == (size_t)-2)
		return WEOF;
	return (wint_t)wc;
}

wint_t getwc(FILE *stream) {
	return fgetwc(stream);
}

wint_t getwchar(void) {
	return fgetwc(stdin);
}

wint_t fputwc(wchar_t wc, FILE *stream) {
	char buf[4];
	size_t r = wcrtomb(buf, wc, NULL);
	if (r == (size_t)-1)
		return WEOF;
	for (size_t i = 0; i < r; i++)
		if (fputc((unsigned char)buf[i], stream) == EOF)
			return WEOF;
	return (wint_t)wc;
}

wint_t putwc(wchar_t wc, FILE *stream) {
	return fputwc(wc, stream);
}

wint_t putwchar(wchar_t wc) {
	return fputwc(wc, stdout);
}

wint_t ungetwc(wint_t wc, FILE *stream) {
	if (wc == WEOF)
		return WEOF;
	/* Only single-byte (ASCII) pushback is supported by the narrow ungetc
	 * one-char buffer; multibyte pushback is not needed by current ports. */
	if ((unsigned)wc < 0x80) {
		if (ungetc((int)wc, stream) == EOF)
			return WEOF;
		return wc;
	}
	return WEOF;
}

wchar_t *fgetws(wchar_t *ws, int n, FILE *stream) {
	if (n <= 0)
		return NULL;
	int i = 0;
	while (i < n - 1) {
		wint_t wc = fgetwc(stream);
		if (wc == WEOF) {
			if (i == 0)
				return NULL;
			break;
		}
		ws[i++] = (wchar_t)wc;
		if (wc == L'\n')
			break;
	}
	ws[i] = 0;
	return ws;
}

int fputws(const wchar_t *ws, FILE *stream) {
	for (; *ws; ws++)
		if (fputwc(*ws, stream) == WEOF)
			return -1;
	return 0;
}

/* b1nix streams are byte-oriented; report/keep wide orientation as requested
 * without enforcing exclusivity (the underlying I/O is UTF-8 bytes). */
int fwide(FILE *stream, int mode) {
	(void)stream;
	return mode; /* return the requested (or, for 0, "no orientation") mode */
}

/* Wide formatted output: transcode the wide format to a narrow UTF-8 format and
 * delegate to the narrow vsnprintf/vfprintf. Wide-specific conversions (%ls/%lc)
 * are rewritten to their narrow equivalents so a single va_list walk works.
 * This is C-locale, UTF-8 correct for the conversions ports actually use. */
static char *__wfmt_to_narrow(const wchar_t *format) {
	size_t cap = wcslen(format) * 4 + 1;
	char *nf = (char *)malloc(cap);
	if (!nf)
		return NULL;
	size_t fi = 0;
	char tmp[4];
	for (const wchar_t *p = format; *p; p++) {
		/* The narrow vsnprintf already understands the %ls and %lc wide
		 * conversions, so the format only needs UTF-8 transcoding here;
		 * each format byte is emitted unchanged below. */
		size_t r = wcrtomb(tmp, *p, NULL);
		if (r == (size_t)-1) {
			free(nf);
			return NULL;
		}
		for (size_t k = 0; k < r; k++)
			nf[fi++] = tmp[k];
	}
	nf[fi] = 0;
	return nf;
}

int vswprintf(wchar_t *ws, size_t n, const wchar_t *format, va_list arg) {
	char *nf = __wfmt_to_narrow(format);
	if (!nf)
		return -1;
	size_t nbufsz = n * 4 + 1;
	char *nbuf = (char *)malloc(nbufsz);
	if (!nbuf) {
		free(nf);
		return -1;
	}
	int rc = vsnprintf(nbuf, nbufsz, nf, arg);
	free(nf);
	if (rc < 0) {
		free(nbuf);
		return -1;
	}
	/* Transcode the narrow result back to wide, bounded by n. */
	const char *q = nbuf;
	size_t wi = 0;
	mbstate_t st = {0, 0};
	while (*q && wi + 1 < n) {
		wchar_t wc;
		size_t r = mbrtowc(&wc, q, 4, &st);
		if (r == (size_t)-1 || r == (size_t)-2)
			break;
		if (r == 0)
			break;
		ws[wi++] = wc;
		q += r;
	}
	if (n)
		ws[wi] = 0;
	free(nbuf);
	if (*q != 0)
		return -1; /* truncated: POSIX returns negative */
	return (int)wi;
}

int swprintf(wchar_t *ws, size_t n, const wchar_t *format, ...) {
	va_list ap;
	va_start(ap, format);
	int rc = vswprintf(ws, n, format, ap);
	va_end(ap);
	return rc;
}

int vfwprintf(FILE *stream, const wchar_t *format, va_list arg) {
	char *nf = __wfmt_to_narrow(format);
	if (!nf)
		return -1;
	int rc = vfprintf(stream, nf, arg);
	free(nf);
	return rc;
}

int fwprintf(FILE *stream, const wchar_t *format, ...) {
	va_list ap;
	va_start(ap, format);
	int rc = vfwprintf(stream, format, ap);
	va_end(ap);
	return rc;
}

int vwprintf(const wchar_t *format, va_list arg) {
	return vfwprintf(stdout, format, arg);
}

int wprintf(const wchar_t *format, ...) {
	va_list ap;
	va_start(ap, format);
	int rc = vfwprintf(stdout, format, ap);
	va_end(ap);
	return rc;
}

/* Wide formatted input is not used by the current ports; provide honest
 * implementations that report no conversions (EOF) rather than faking matches.
 * They exist so the libstdc++ wchar_t configuration links. */
int swscanf(const wchar_t *ws, const wchar_t *format, ...) {
	(void)ws;
	(void)format;
	return EOF;
}

int fwscanf(FILE *stream, const wchar_t *format, ...) {
	(void)stream;
	(void)format;
	return EOF;
}

int wscanf(const wchar_t *format, ...) {
	(void)format;
	return EOF;
}
