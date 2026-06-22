#ifndef B1NIX_U_WCHAR_H
#define B1NIX_U_WCHAR_H

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef B1NIX_WINT_T_DEFINED
#define B1NIX_WINT_T_DEFINED
typedef int wint_t;
#endif

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

/* <stdio.h> provides FILE for the wide stdio family below. Guarded so a
 * freestanding consumer that only wants the string ops still compiles. */
#ifndef B1NIX_U_STDIO_H
#include <stdio.h>
#endif

struct tm; /* for wcsftime; full definition in <time.h> */

/* ── string.c ─────────────────────────────────────────────────────────────── */
wchar_t *wmemcpy(wchar_t *dest, const wchar_t *src, size_t n);
wchar_t *wmemmove(wchar_t *dest, const wchar_t *src, size_t n);
wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n);
size_t wcslen(const wchar_t *s);
wchar_t *wcscat(wchar_t *dest, const wchar_t *src);
wchar_t *wcscpy(wchar_t *dest, const wchar_t *src);

/* ── wchar.c — UTF-8 multibyte + wide-string support ──────────────────────── */
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
size_t wcsxfrm(wchar_t *dest, const wchar_t *src, size_t n);
wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
wchar_t *wcsncpy(wchar_t *dest, const wchar_t *src, size_t n);
int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n);
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);
wchar_t *wcsdup(const wchar_t *s);

/* Wide-string operations the C++ <cwchar> wrapper requires once wchar_t is
 * enabled. C-locale (codepoint-order) semantics; honest implementations. */
wchar_t *wcsncat(wchar_t *dest, const wchar_t *src, size_t n);
size_t wcsspn(const wchar_t *s, const wchar_t *accept);
size_t wcscspn(const wchar_t *s, const wchar_t *reject);
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept);
wchar_t *wcsstr(const wchar_t *haystack, const wchar_t *needle);
wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **saveptr);

/* Wide numeric conversion — thin C-locale wrappers over the narrow <stdlib.h>
 * versions (the wide string is transcoded one ASCII char at a time). */
long wcstol(const wchar_t *nptr, wchar_t **endptr, int base);
unsigned long wcstoul(const wchar_t *nptr, wchar_t **endptr, int base);
long long wcstoll(const wchar_t *nptr, wchar_t **endptr, int base);
unsigned long long wcstoull(const wchar_t *nptr, wchar_t **endptr, int base);
double wcstod(const wchar_t *nptr, wchar_t **endptr);
float wcstof(const wchar_t *nptr, wchar_t **endptr);
long double wcstold(const wchar_t *nptr, wchar_t **endptr);

/* wcsftime — formats a struct tm into a wide buffer (delegates to strftime). */
size_t wcsftime(wchar_t *s, size_t maxsize, const wchar_t *format,
                const struct tm *timeptr);

/* ── Wide stdio (UTF-8 byte stream underneath; C-locale conversion) ───────── */
wint_t fgetwc(FILE *stream);
wint_t getwc(FILE *stream);
wint_t getwchar(void);
wint_t fputwc(wchar_t wc, FILE *stream);
wint_t putwc(wchar_t wc, FILE *stream);
wint_t putwchar(wchar_t wc);
wint_t ungetwc(wint_t wc, FILE *stream);
wchar_t *fgetws(wchar_t *ws, int n, FILE *stream);
int fputws(const wchar_t *ws, FILE *stream);
int fwide(FILE *stream, int mode);

int swprintf(wchar_t *ws, size_t n, const wchar_t *format, ...);
int vswprintf(wchar_t *ws, size_t n, const wchar_t *format, va_list arg);
int fwprintf(FILE *stream, const wchar_t *format, ...);
int vfwprintf(FILE *stream, const wchar_t *format, va_list arg);
int wprintf(const wchar_t *format, ...);
int vwprintf(const wchar_t *format, va_list arg);

int swscanf(const wchar_t *ws, const wchar_t *format, ...);
int fwscanf(FILE *stream, const wchar_t *format, ...);
int wscanf(const wchar_t *format, ...);

#ifdef __cplusplus
}

/* glibc's <wchar.h> exposes the standard wide-char functions in BOTH the global
 * namespace and std:: (via __BEGIN_NAMESPACE_STD). C++ code that includes
 * <wchar.h> directly — e.g. Abseil's str_format does `std::wcslen` — then expects
 * them in std. b1nix declares them only globally above, so mirror the ISO C set
 * into std:: here. Redundant with libstdc++'s <cwchar> (duplicate
 * using-declarations are legal). */
namespace std {
  using ::wmemcpy; using ::wmemmove; using ::wmemset; using ::wmemcmp;
  using ::wmemchr; using ::wcslen; using ::wcscat; using ::wcscpy;
  using ::wcsncat; using ::wcsncpy; using ::wcscmp; using ::wcsncmp;
  using ::wcscoll; using ::wcsxfrm; using ::wcschr; using ::wcsrchr;
  using ::wcsspn; using ::wcscspn; using ::wcspbrk; using ::wcsstr;
  using ::wcstok; using ::wcsftime;
  using ::wcstol; using ::wcstoul; using ::wcstoll; using ::wcstoull;
  using ::wcstod; using ::wcstof; using ::wcstold;
  using ::mbrtowc; using ::mbrlen; using ::wcrtomb; using ::mbsinit;
  using ::mbsrtowcs; using ::wcsrtombs; using ::btowc; using ::wctob;
  using ::fgetwc; using ::getwc; using ::getwchar; using ::fputwc;
  using ::putwc; using ::putwchar; using ::ungetwc; using ::fgetws;
  using ::fputws; using ::fwide;
  using ::swprintf; using ::vswprintf; using ::fwprintf; using ::vfwprintf;
  using ::wprintf; using ::vwprintf; using ::swscanf; using ::fwscanf;
  using ::wscanf;
}
#endif

#endif
