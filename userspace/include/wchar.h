#ifndef B1NIX_U_WCHAR_H
#define B1NIX_U_WCHAR_H

#include <stddef.h>

typedef int wint_t;

#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

#if !defined(__cplusplus) && !defined(B1NIX_WCHAR_T_DEFINED)
#define B1NIX_WCHAR_T_DEFINED
typedef int wchar_t;
#endif

#ifndef WCHAR_MIN
#define WCHAR_MIN ((wchar_t)-2147483647 - 1)
#define WCHAR_MAX ((wchar_t)2147483647)
#endif

/* UTF-8 conversion is stateless here, but POSIX requires a real object so the
 * restartable conversion functions take a pointer. The fields are unused. */
#ifndef B1NIX_MBSTATE_T_DEFINED
#define B1NIX_MBSTATE_T_DEFINED
typedef struct __mbstate_t {
	unsigned int __count;
	unsigned int __value;
} mbstate_t;
#endif

/* string.c */
wchar_t *wmemcpy(wchar_t *dest, const wchar_t *src, size_t n);
wchar_t *wmemmove(wchar_t *dest, const wchar_t *src, size_t n);
wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n);
size_t wcslen(const wchar_t *s);
wchar_t *wcscat(wchar_t *dest, const wchar_t *src);
wchar_t *wcscpy(wchar_t *dest, const wchar_t *src);

/* wchar.c — UTF-8 multibyte + wide-string support */
size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps);
size_t mbrlen(const char *s, size_t n, mbstate_t *ps);
size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps);
int mbsinit(const mbstate_t *ps);
int wcwidth(wchar_t wc);
int wcswidth(const wchar_t *s, size_t n);
size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *ps);
size_t wcsrtombs(char *dst, const wchar_t **src, size_t len, mbstate_t *ps);
wint_t btowc(int c);
int wctob(wint_t c);
int wcscmp(const wchar_t *a, const wchar_t *b);
int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
int wcscoll(const wchar_t *a, const wchar_t *b);
wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
wchar_t *wcsncpy(wchar_t *dest, const wchar_t *src, size_t n);
int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n);
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);
wchar_t *wcsdup(const wchar_t *s);

#endif
