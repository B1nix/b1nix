#ifndef B1NIX_U_STDLIB_H
#define B1NIX_U_STDLIB_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

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

void  abort(void) __attribute__((noreturn));
void  exit(int status) __attribute__((noreturn));
void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
int   atoi(const char *s);
long  strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
double strtod(const char *nptr, char **endptr);
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));
char *getenv(const char *name);
int   putenv(char *string);
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);
int   clearenv(void);
char *realpath(const char *path, char *resolved_path);
extern char **environ;

/* Cooperative spin-based semaphores (no kernel futex; shared-memory safe) */
int sem_init(int *sem, int pshared, unsigned int value);
int sem_wait(int *sem);
int sem_post(int *sem);
int sem_destroy(int *sem);

#ifndef B1NIX_WCHAR_T_DEFINED
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
void srand(unsigned int seed);
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
char *mkdtemp(char *tmpl);

const char *getprogname(void);

#ifdef __cplusplus
}
#endif

#endif
