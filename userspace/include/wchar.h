#ifndef B1NIX_U_WCHAR_H
#define B1NIX_U_WCHAR_H

#include <stddef.h>

typedef int wint_t;

#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

#ifndef B1NIX_WCHAR_T_DEFINED
#define B1NIX_WCHAR_T_DEFINED
typedef int wchar_t;
#endif

wchar_t *wmemcpy(wchar_t *dest, const wchar_t *src, size_t n);
wchar_t *wmemmove(wchar_t *dest, const wchar_t *src, size_t n);
wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n);
size_t wcslen(const wchar_t *s);
wchar_t *wcscat(wchar_t *dest, const wchar_t *src);
wchar_t *wcscpy(wchar_t *dest, const wchar_t *src);

#endif
