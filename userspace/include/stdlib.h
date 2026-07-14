#ifndef B1NIX_U_STDLIB_H
#define B1NIX_U_STDLIB_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/cdefs.h>  /* __THROW (noexcept in C++) — glibc-compatible specs */

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
/* Bytes in the longest multibyte character of the current locale. The b1nix
 * libc is UTF-8, so the multibyte conversion functions (mbtowc/mbrtowc/...) and
 * MB_CUR_MAX advertise up to 4 bytes per character; ports key their multibyte
 * code paths off MB_CUR_MAX > 1. */
#ifndef MB_CUR_MAX
#define MB_CUR_MAX 4
#endif
#define RAND_MAX 2147483647

#ifndef alloca
#define alloca(size) __builtin_alloca(size)
#endif

typedef struct {
    int quot;
    int rem;
} div_t;

typedef struct {
    long quot;
    long rem;
} ldiv_t;

typedef struct {
    long long quot;
    long long rem;
} lldiv_t;

void  abort(void) __attribute__((noreturn));
void  exit(int status) __attribute__((noreturn));
void  _Exit(int status) __attribute__((noreturn));
void *malloc(size_t size) __THROW;
int posix_memalign(void **memptr, size_t alignment, size_t size) __THROW;
void *aligned_alloc(size_t alignment, size_t size) __THROW;
void *memalign(size_t alignment, size_t size) __THROW;
void  free(void *ptr) __THROW;
void *calloc(size_t nmemb, size_t size) __THROW;
void *realloc(void *ptr, size_t size) __THROW;
int   atoi(const char *s);
long long atoll(const char *s);
lldiv_t lldiv(long long numer, long long denom);
long double strtold(const char *nptr, char **endptr);
long  strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
double strtod(const char *nptr, char **endptr);
float strtof(const char *nptr, char **endptr);

/* Per-locale string-to-number variants (added for the Chromium port, M60-62).
 * b1nix is C-locale ONLY, so the locale handle is ignored and these delegate to
 * the standard conversions (which already use the C locale). Correct, not faked.
 * locale_t comes from <locale.h>. */
#ifndef _LOCALE_T_DEFINED_FOR_STRTO_L
#define _LOCALE_T_DEFINED_FOR_STRTO_L
typedef void *locale_t;  /* matches <locale.h>; harmless duplicate typedef in C11/C++ */
#endif
static inline double strtod_l(const char *nptr, char **endptr, locale_t loc) {
  (void)loc; return strtod(nptr, endptr);
}
static inline float strtof_l(const char *nptr, char **endptr, locale_t loc) {
  (void)loc; return strtof(nptr, endptr);
}
static inline long double strtold_l(const char *nptr, char **endptr, locale_t loc) {
  (void)loc; return strtold(nptr, endptr);
}
static inline long strtol_l(const char *nptr, char **endptr, int base, locale_t loc) {
  (void)loc; return strtol(nptr, endptr, base);
}
static inline unsigned long strtoul_l(const char *nptr, char **endptr, int base, locale_t loc) {
  (void)loc; return strtoul(nptr, endptr, base);
}
/* strtoll_l / strtoull_l are also supplied by LLVM libc++'s musl support shim
 * (<__support/musl/xlocale.h>) when libc++ is built with _LIBCPP_HAS_MUSL_LIBC —
 * which is how b1nix builds it, since b1nix's libc is musl-like and C-locale
 * only. Defining them here too would be a redefinition in any libc++ TU that
 * pulls <locale>. So in a libc++ build let libc++ provide them; the GCC
 * libstdc++ path (where _LIBCPP_HAS_MUSL_LIBC is never defined) still gets the
 * b1nix definitions it expects. The other *_l variants above are NOT in libc++'s
 * shim, so they stay unconditionally provided by the libc. */
#ifndef _LIBCPP_HAS_MUSL_LIBC
static inline long long strtoll_l(const char *nptr, char **endptr, int base, locale_t loc) {
  (void)loc; return strtoll(nptr, endptr, base);
}
static inline unsigned long long strtoull_l(const char *nptr, char **endptr, int base, locale_t loc) {
  (void)loc; return strtoull(nptr, endptr, base);
}
#endif
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));
/* C11 Annex K: qsort_s — sorting with context parameter.
 * Linux/glibc signature: context arg is last to qsort_s, first to comparator. */
void qsort_s(void *base, size_t nmemb, size_t size,
             int (*compar)(const void *, const void *, void *), void *arg);
char *getenv(const char *name);
int   putenv(char *string);
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);
int   clearenv(void);
char *realpath(const char *path, char *resolved_path);
extern char **environ;

/* POSIX semaphores live in <semaphore.h> (sem_t-typed). They were previously
 * declared here too with raw `int *` args, which clashes with <semaphore.h>'s
 * sem_t* declarations under C++ (the Chromium port pulls both via abseil).
 * Removed from <stdlib.h> — include <semaphore.h> for the semaphore API. */

#if !defined(__cplusplus) && !defined(B1NIX_WCHAR_T_DEFINED)
#define B1NIX_WCHAR_T_DEFINED
typedef int wchar_t;
#endif

int wctomb(char *s, wchar_t wc);
int mbtowc(wchar_t *pwc, const char *s, size_t n);
int mblen(const char *s, size_t n);

/* UTF-8 string conversions (implemented in wchar.c). */
size_t mbstowcs(wchar_t *dest, const char *src, size_t n);
size_t wcstombs(char *dest, const wchar_t *src, size_t n);

static inline long labs(long x) {
    return x < 0 ? -x : x;
}
static inline long long llabs(long long x) {
    return x < 0 ? -x : x;
}

static inline int abs(int x) {
    return x < 0 ? -x : x;
}

static inline div_t div(int numer, int denom) {
    div_t r;
    r.quot = numer / denom;
    r.rem = numer % denom;
    return r;
}

static inline ldiv_t ldiv(long numer, long denom) {
    ldiv_t r;
    r.quot = numer / denom;
    r.rem = numer % denom;
    return r;
}

int atexit(void (*function)(void));
int rand(void);
int rand_r(unsigned int *seedp);
void srand(unsigned int seed);

/* BSD/POSIX random() family. A classic TYPE_3 additive-feedback generator
 * (better quality than rand()'s LCG). random_r/initstate_r are the GNU
 * reentrant forms (used by e.g. fontconfig). */
#include <stdint.h>
struct random_data {
  int32_t x[31];   /* additive-feedback state (degree 31) */
  int fptr, rptr;  /* feedback/return indices */
  int valid;
};
long random(void);
void srandom(unsigned int seed);
/* BSD/glibc load-average query. b1nix tracks no load average; returns -1 so
 * callers fall back to a zero/unknown load. */
int getloadavg(double loadavg[], int nelem);
int random_r(struct random_data *buf, int32_t *result);
int srandom_r(unsigned int seed, struct random_data *buf);
int initstate_r(unsigned int seed, char *statebuf, size_t statelen,
                struct random_data *buf);

int system(const char *command);

static inline void *bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    size_t l = 0, r = nmemb;
    while (l < r) {
        size_t mid = l + (r - l) / 2;
        const void *item = (const char *)base + mid * size;
        int cmp = compar(key, item);
        if (cmp == 0) return (void *)item;
        if (cmp < 0) {
            r = mid;
        } else {
            l = mid + 1;
        }
    }
    return NULL;
}

static inline double atof(const char *nptr) {
    return strtod(nptr, NULL);
}

static inline long atol(const char *nptr) {
    return strtol(nptr, NULL, 10);
}

static inline char *mktemp(char *tmpl) {
    char *p = tmpl;
    while (*p) p++;
    int count = 0;
    while (p > tmpl && *(p - 1) == 'X' && count < 6) {
        p--;
        count++;
    }
    static int counter = 100000;
    int temp = counter++;
    for (int i = 0; i < count; i++) {
        p[i] = '0' + (temp % 10);
        temp /= 10;
    }
    return tmpl;
}

int mkstemp(char *tmpl);
int mkstemps(char *tmpl, int suffixlen);
char *mkdtemp(char *tmpl);

const char *getprogname(void);

#ifdef __cplusplus
}
#endif

#endif
#include <stdint.h>
uint32_t arc4random(void);
void arc4random_buf(void *buf, size_t nbytes);
