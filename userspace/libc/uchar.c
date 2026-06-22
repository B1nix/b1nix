/* C11 <uchar.h> conversions for b1nix (UTF-8 / C locale).
 *
 * char32_t is UCS-4, identical to b1nix's 32-bit wchar_t, so the c32 functions
 * delegate straight to mbrtowc/wcrtomb. char16_t is UTF-16: code points above
 * the BMP (> 0xFFFF) become a surrogate pair, so mbrtoc16 emits the high
 * surrogate and parks the low surrogate in the mbstate_t (__value, with
 * __count flagged) to return on the next call (the standard (size_t)-3 path);
 * c16rtomb buffers a high surrogate until its matching low surrogate arrives.
 */
#include <uchar.h>
#include <wchar.h>
#include <errno.h>
#include <stddef.h>

size_t mbrtoc32(char32_t *pc32, const char *s, size_t n, mbstate_t *ps) {
	wchar_t wc = 0;
	size_t r = mbrtowc(&wc, s, n, ps);
	if (pc32 && (r != (size_t)-1) && (r != (size_t)-2))
		*pc32 = (char32_t)wc;
	return r;
}

size_t c32rtomb(char *s, char32_t c32, mbstate_t *ps) {
	return wcrtomb(s, (wchar_t)c32, ps);
}

size_t mbrtoc16(char16_t *pc16, const char *s, size_t n, mbstate_t *ps) {
	static mbstate_t internal;
	if (!ps) ps = &internal;

	/* A low surrogate was parked by the previous call: return it now,
	 * consuming no further input. */
	if (ps->__count == (unsigned)-1) {
		if (pc16) *pc16 = (char16_t)ps->__value;
		ps->__count = 0;
		ps->__value = 0;
		return (size_t)-3;
	}

	wchar_t wc = 0;
	size_t r = mbrtowc(&wc, s, n, ps);
	if (r == (size_t)-1 || r == (size_t)-2)
		return r;

	if ((char32_t)wc > 0xFFFFu) {
		char32_t v = (char32_t)wc - 0x10000u;
		char16_t hi = (char16_t)(0xD800u + (v >> 10));
		char16_t lo = (char16_t)(0xDC00u + (v & 0x3FFu));
		ps->__count = (unsigned)-1;   /* flag: low surrogate pending */
		ps->__value = lo;
		if (pc16) *pc16 = hi;
		return r;
	}

	if (pc16) *pc16 = (char16_t)wc;
	return r;
}

size_t c16rtomb(char *s, char16_t c16, mbstate_t *ps) {
	static mbstate_t internal;
	if (!ps) ps = &internal;

	/* High surrogate parked: this call must supply the matching low one. */
	if (ps->__count == (unsigned)-1) {
		if (c16 < 0xDC00u || c16 > 0xDFFFu) {
			ps->__count = 0;
			ps->__value = 0;
			errno = EILSEQ;
			return (size_t)-1;
		}
		char32_t hi = ps->__value;
		char32_t cp = 0x10000u + (((hi - 0xD800u) << 10) | (c16 - 0xDC00u));
		ps->__count = 0;
		ps->__value = 0;
		return wcrtomb(s, (wchar_t)cp, NULL);
	}

	if (c16 >= 0xD800u && c16 <= 0xDBFFu) {
		/* High surrogate: park it, emit nothing yet. */
		ps->__count = (unsigned)-1;
		ps->__value = c16;
		return 0;
	}

	if (c16 >= 0xDC00u && c16 <= 0xDFFFu) {
		errno = EILSEQ;
		return (size_t)-1;
	}

	return wcrtomb(s, (wchar_t)c16, NULL);
}
