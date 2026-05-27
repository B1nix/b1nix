#include <stdlib.h>
#include <string.h>
#include "syscall.h"
#include <errno.h>
#include <math.h>
#include <sys/mman.h>

extern int normalize_errno(long rc);

static void (*atexit_funcs[32])(void);
static int atexit_count = 0;

int atexit(void (*function)(void)) {
	if (atexit_count >= 32) return -1;
	atexit_funcs[atexit_count++] = function;
	return 0;
}

void exit(int status)
{
	while (atexit_count > 0) {
		atexit_funcs[--atexit_count]();
	}
	syscall(SYS_EXIT, status, 0, 0, 0);
	while (1);
}

/* ── Dynamic memory allocator ───────────────────────────────────────────────
 * Explicit free list with boundary tags, backed by anonymous mmap. This
 * replaces an earlier 16 MB static bump pool whose free() was a no-op: large
 * programs such as the native GCC's cc1 exhausted the pool, malloc() returned
 * NULL, and cc1 crashed dereferencing it. Payloads are 16-byte aligned
 * (x86-64 MALLOC_ABI_ALIGNMENT). Coalescing is bounded per mmap region by
 * allocated prologue/epilogue sentinels so it never crosses a region edge. */

#define MA_WSIZE   ((size_t)8)                  /* header / footer size */
#define MA_DSIZE   ((size_t)16)                 /* alignment, hdr+ftr */
#define MA_CHUNK   ((size_t)(4 * 1024 * 1024))  /* heap growth granularity */
#define MA_MINBLK  ((size_t)32)                 /* hdr+ftr + 16B free links */

#define MA_PACK(sz, a)  ((size_t)(sz) | (size_t)(a))
#define MA_GET(p)       (*(volatile size_t *)(p))
#define MA_PUT(p, v)    (*(volatile size_t *)(p) = (size_t)(v))
#define MA_SIZE(p)      (MA_GET(p) & ~(size_t)0xF)
#define MA_ALLOC(p)     (MA_GET(p) & (size_t)0x1)
#define MA_HDR(bp)      ((char *)(bp) - MA_WSIZE)
#define MA_FTR(bp)      ((char *)(bp) + MA_SIZE(MA_HDR(bp)) - MA_DSIZE)
#define MA_NEXT(bp)     ((char *)(bp) + MA_SIZE((char *)(bp) - MA_WSIZE))
#define MA_PREV(bp)     ((char *)(bp) - MA_SIZE((char *)(bp) - MA_DSIZE))
#define MA_FLINK(bp)    (*(char **)(bp))                       /* free-list next */
#define MA_BLINK(bp)    (*(char **)((char *)(bp) + MA_WSIZE))  /* free-list prev */

static char *ma_free_list = 0;

static void ma_fl_insert(char *bp) {
	MA_FLINK(bp) = ma_free_list;
	MA_BLINK(bp) = 0;
	if (ma_free_list) MA_BLINK(ma_free_list) = bp;
	ma_free_list = bp;
}

static void ma_fl_remove(char *bp) {
	char *prev = MA_BLINK(bp), *next = MA_FLINK(bp);
	if (prev) MA_FLINK(prev) = next; else ma_free_list = next;
	if (next) MA_BLINK(next) = prev;
}

static char *ma_coalesce(char *bp) {
	size_t prev_alloc = MA_ALLOC((char *)bp - MA_DSIZE); /* previous footer */
	size_t next_alloc = MA_ALLOC(MA_HDR(MA_NEXT(bp)));   /* next header */
	size_t size = MA_SIZE(MA_HDR(bp));
	if (prev_alloc && next_alloc) {
		/* isolated block */
	} else if (prev_alloc && !next_alloc) {
		char *nx = MA_NEXT(bp);
		ma_fl_remove(nx);
		size += MA_SIZE(MA_HDR(nx));
		MA_PUT(MA_HDR(bp), MA_PACK(size, 0));
		MA_PUT(MA_FTR(bp), MA_PACK(size, 0));
	} else if (!prev_alloc && next_alloc) {
		char *pv = MA_PREV(bp);
		ma_fl_remove(pv);
		size += MA_SIZE(MA_HDR(pv));
		MA_PUT(MA_FTR(bp), MA_PACK(size, 0));
		MA_PUT(MA_HDR(pv), MA_PACK(size, 0));
		bp = pv;
	} else {
		char *pv = MA_PREV(bp), *nx = MA_NEXT(bp);
		ma_fl_remove(pv); ma_fl_remove(nx);
		size += MA_SIZE(MA_HDR(pv)) + MA_SIZE(MA_HDR(nx));
		MA_PUT(MA_HDR(pv), MA_PACK(size, 0));
		MA_PUT(MA_FTR(nx), MA_PACK(size, 0));
		bp = pv;
	}
	ma_fl_insert(bp);
	return bp;
}

static char *ma_extend(size_t need) {
	size_t overhead = MA_WSIZE + MA_DSIZE + MA_WSIZE; /* pad + prologue + epilogue */
	size_t region = overhead + need;
	if (region < MA_CHUNK) region = MA_CHUNK;
	region = (region + (MA_DSIZE - 1)) & ~(MA_DSIZE - 1);
	char *r = (char *)mmap(0, region, PROT_READ | PROT_WRITE,
	                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (r == (char *)MAP_FAILED || r == 0) return 0;
	MA_PUT(r + MA_WSIZE, MA_PACK(MA_DSIZE, 1)); /* prologue header */
	MA_PUT(r + MA_DSIZE, MA_PACK(MA_DSIZE, 1)); /* prologue footer */
	char *bp = r + MA_WSIZE + MA_DSIZE + MA_WSIZE; /* payload of first block (16-aligned) */
	size_t fsize = region - overhead;
	MA_PUT(MA_HDR(bp), MA_PACK(fsize, 0));
	MA_PUT(MA_FTR(bp), MA_PACK(fsize, 0));
	MA_PUT(MA_HDR(MA_NEXT(bp)), MA_PACK(0, 1)); /* epilogue header */
	ma_fl_insert(bp);
	return bp;
}

static char *ma_find_fit(size_t asize) {
	for (char *bp = ma_free_list; bp; bp = MA_FLINK(bp))
		if (MA_SIZE(MA_HDR(bp)) >= asize) return bp;
	return 0;
}

static void ma_place(char *bp, size_t asize) {
	size_t csize = MA_SIZE(MA_HDR(bp));
	ma_fl_remove(bp);
	if (csize - asize >= MA_MINBLK) {
		MA_PUT(MA_HDR(bp), MA_PACK(asize, 1));
		MA_PUT(MA_FTR(bp), MA_PACK(asize, 1));
		char *nb = MA_NEXT(bp);
		MA_PUT(MA_HDR(nb), MA_PACK(csize - asize, 0));
		MA_PUT(MA_FTR(nb), MA_PACK(csize - asize, 0));
		ma_fl_insert(nb);
	} else {
		MA_PUT(MA_HDR(bp), MA_PACK(csize, 1));
		MA_PUT(MA_FTR(bp), MA_PACK(csize, 1));
	}
}

void *malloc(size_t size)
{
	if (size == 0) return 0;
	size_t payload = (size + (MA_DSIZE - 1)) & ~(MA_DSIZE - 1);
	size_t asize = payload + MA_DSIZE;
	if (asize < MA_MINBLK) asize = MA_MINBLK;
	char *bp = ma_find_fit(asize);
	if (!bp) {
		bp = ma_extend(asize);
		if (!bp) { errno = ENOMEM; return 0; }
	}
	ma_place(bp, asize);
	return bp;
}

void free(void *ptr)
{
	if (!ptr) return;
	size_t size = MA_SIZE(MA_HDR(ptr));
	MA_PUT(MA_HDR(ptr), MA_PACK(size, 0));
	MA_PUT(MA_FTR(ptr), MA_PACK(size, 0));
	ma_coalesce((char *)ptr);
}

void *calloc(size_t nmemb, size_t size)
{
	size_t total = nmemb * size;
	if (size != 0 && total / size != nmemb) { errno = ENOMEM; return 0; } /* overflow */
	void *p = malloc(total);
	if (p) memset(p, 0, total);
	return p;
}

int atoi(const char *s)
{
	int n = 0;
	int sign = 1;
	while (*s == ' ') s++;
	if (*s == '-') { sign = -1; s++; }
	else if (*s == '+') s++;
	while (*s >= '0' && *s <= '9') {
		n = n * 10 + (*s - '0');
		s++;
	}
	return n * sign;
}

static char *empty_env[] = { NULL };
char **environ = empty_env;

char *getenv(const char *name)
{
	if (!name || !environ) return NULL;
	size_t len = strlen(name);
	for (char **env = environ; *env; env++) {
		if (strncmp(*env, name, len) == 0 && (*env)[len] == '=') {
			return *env + len + 1;
		}
	}
	return NULL;
}

int putenv(char *string)
{
	if (!string) {
		errno = EINVAL;
		return -1;
	}
	char *equals = strchr(string, '=');
	if (!equals || equals == string) {
		errno = EINVAL;
		return -1;
	}
	
	size_t name_len = equals - string;
	
	/* Count existing environment variables */
	size_t count = 0;
	int found_idx = -1;
	if (environ) {
		while (environ[count]) {
			if (strncmp(environ[count], string, name_len) == 0 && environ[count][name_len] == '=') {
				found_idx = (int)count;
			}
			count++;
		}
	}
	
	if (found_idx != -1) {
		/* Overwrite existing entry */
		environ[found_idx] = string;
	} else {
		/* Allocate a new environ array */
		char **new_environ = malloc((count + 2) * sizeof(char *));
		if (!new_environ) {
			errno = ENOMEM;
			return -1;
		}
		if (environ && environ != empty_env) {
			memcpy(new_environ, environ, count * sizeof(char *));
		}
		new_environ[count] = string;
		new_environ[count + 1] = NULL;
		environ = new_environ;
	}
	return 0;
}

int setenv(const char *name, const char *value, int overwrite)
{
	if (!name || name[0] == '\0' || strchr(name, '=')) {
		errno = EINVAL;
		return -1;
	}
	
	char *existing = getenv(name);
	if (existing && !overwrite) {
		return 0;
	}
	
	size_t name_len = strlen(name);
	size_t val_len = value ? strlen(value) : 0;
	
	/* Allocate string "name=value" */
	char *buf = malloc(name_len + val_len + 2);
	if (!buf) {
		errno = ENOMEM;
		return -1;
	}
	memcpy(buf, name, name_len);
	buf[name_len] = '=';
	if (value) {
		memcpy(buf + name_len + 1, value, val_len);
	}
	buf[name_len + 1 + val_len] = '\0';
	
	return putenv(buf);
}

int unsetenv(const char *name)
{
	if (!name || name[0] == '\0' || strchr(name, '=')) {
		errno = EINVAL;
		return -1;
	}
	
	if (!environ || environ == empty_env) return 0;
	
	size_t len = strlen(name);
	size_t i = 0, j = 0;
	while (environ[i]) {
		if (strncmp(environ[i], name, len) == 0 && environ[i][len] == '=') {
			/* Skip copying this element */
			i++;
		} else {
			environ[j++] = environ[i++];
		}
	}
	environ[j] = NULL;
	return 0;
}

char *realpath(const char *path, char *resolved_path)
{
	if (!resolved_path) return strdup(path);
	strcpy(resolved_path, path);
	return resolved_path;
}

void *realloc(void *ptr, size_t size)
{
	if (size == 0) { free(ptr); return NULL; }
	if (!ptr) return malloc(size);
	size_t old_payload = MA_SIZE(MA_HDR(ptr)) - MA_DSIZE; /* usable bytes in old block */
	void *new_ptr = malloc(size);
	if (!new_ptr) return NULL;
	size_t copy = size < old_payload ? size : old_payload;
	memcpy(new_ptr, ptr, copy);
	free(ptr);
	return new_ptr;
}

long strtol(const char *nptr, char **endptr, int base)
{
	const char *s = nptr;
	long acc = 0;
	int sign = 1;
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
	if (*s == '-') { sign = -1; s++; }
	else if (*s == '+') s++;

	if (base == 0) {
		if (*s == '0') {
			if (s[1] == 'x' || s[1] == 'X') {
				base = 16;
				s += 2;
			} else {
				base = 8;
				s++;
			}
		} else {
			base = 10;
		}
	} else if (base == 16) {
		if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
	} else if (base == 8) {
		if (s[0] == '0') s++;
	}

	if (base == 0) base = 10;

	int any = 0;
	while (*s) {
		int v;
		if (*s >= '0' && *s <= '9') v = *s - '0';
		else if (*s >= 'a' && *s <= 'z') v = *s - 'a' + 10;
		else if (*s >= 'A' && *s <= 'Z') v = *s - 'A' + 10;
		else break;

		if (v >= base) break;
		acc = acc * base + v;
		any = 1;
		s++;
	}

	if (endptr) *endptr = (char *)(any ? s : nptr);
	return acc * sign;
}

unsigned long strtoul(const char *nptr, char **endptr, int base)
{
	return (unsigned long)strtol(nptr, endptr, base);
}

unsigned long long strtoull(const char *nptr, char **endptr, int base)
{
	return (unsigned long long)strtol(nptr, endptr, base);
}

static void swap(char *a, char *b, size_t size) {
    char tmp;
    for (size_t i = 0; i < size; i++) {
        tmp = a[i];
        a[i] = b[i];
        b[i] = tmp;
    }
}

static void quicksort(char *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    if (nmemb < 2) return;
    
    char *pivot = base + (nmemb / 2) * size;
    swap(pivot, base + (nmemb - 1) * size, size);
    
    size_t i = 0;
    for (size_t j = 0; j < nmemb - 1; j++) {
        if (compar(base + j * size, base + (nmemb - 1) * size) < 0) {
            if (i != j) swap(base + i * size, base + j * size, size);
            i++;
        }
    }
    swap(base + i * size, base + (nmemb - 1) * size, size);
    
    if (i > 0) quicksort(base, i, size, compar);
    if (i + 1 < nmemb) quicksort(base + (i + 1) * size, nmemb - i - 1, size, compar);
}

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *))
{
    if (nmemb < 2 || size == 0) return;
    quicksort((char *)base, nmemb, size, compar);
}

static double pow10_helper(int n)
{
	double res = 1.0;
	double base = 10.0;
	while (n > 0) {
		if (n & 1) res *= base;
		base *= base;
		n >>= 1;
	}
	return res;
}

double strtod(const char *nptr, char **endptr)
{
	const char *s = nptr;
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\v' || *s == '\f') {
		s++;
	}

	int sign = 1;
	if (*s == '-') {
		sign = -1;
		s++;
	} else if (*s == '+') {
		s++;
	}

	/* Case-insensitive check for inf/infinity/nan */
	int is_inf = 0;
	int is_nan = 0;
	if ((*s == 'i' || *s == 'I') &&
	    (s[1] == 'n' || s[1] == 'N') &&
	    (s[2] == 'f' || s[2] == 'F')) {
		s += 3;
		is_inf = 1;
		if ((*s == 'i' || *s == 'I') &&
		    (s[1] == 'n' || s[1] == 'N') &&
		    (s[2] == 'i' || s[2] == 'I') &&
		    (s[3] == 't' || s[3] == 'T') &&
		    (s[4] == 'y' || s[4] == 'Y')) {
			s += 5;
		}
	} else if ((*s == 'n' || *s == 'N') &&
	           (s[1] == 'a' || s[1] == 'A') &&
	           (s[2] == 'n' || s[2] == 'N')) {
		s += 3;
		is_nan = 1;
		if (*s == '(') {
			const char *p = s + 1;
			while (*p && *p != ')') p++;
			if (*p == ')') {
				s = p + 1;
			}
		}
	}

	if (is_inf) {
		if (endptr) *endptr = (char *)s;
		return sign * (1.0 / 0.0);
	}
	if (is_nan) {
		if (endptr) *endptr = (char *)s;
		return 0.0 / 0.0;
	}

	/* Check for hexadecimal float */
	int is_hex = 0;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		const char *hex_check = s + 2;
		int has_hex_digit = 0;
		while ((*hex_check >= '0' && *hex_check <= '9') ||
		       (*hex_check >= 'a' && *hex_check <= 'f') ||
		       (*hex_check >= 'A' && *hex_check <= 'F')) {
			has_hex_digit = 1;
			hex_check++;
		}
		if (*hex_check == '.') {
			hex_check++;
			while ((*hex_check >= '0' && *hex_check <= '9') ||
			       (*hex_check >= 'a' && *hex_check <= 'f') ||
			       (*hex_check >= 'A' && *hex_check <= 'F')) {
				has_hex_digit = 1;
				hex_check++;
			}
		}
		if (has_hex_digit) {
			is_hex = 1;
			s += 2;
		}
	}

	double val = 0.0;
	int any = 0;

	if (is_hex) {
		/* Parse hexadecimal float */
		while (1) {
			int digit;
			if (*s >= '0' && *s <= '9') digit = *s - '0';
			else if (*s >= 'a' && *s <= 'f') digit = *s - 'a' + 10;
			else if (*s >= 'A' && *s <= 'F') digit = *s - 'A' + 10;
			else break;
			val = val * 16.0 + digit;
			any = 1;
			s++;
		}
		if (*s == '.') {
			s++;
			double frac_mult = 1.0 / 16.0;
			while (1) {
				int digit;
				if (*s >= '0' && *s <= '9') digit = *s - '0';
				else if (*s >= 'a' && *s <= 'f') digit = *s - 'a' + 10;
				else if (*s >= 'A' && *s <= 'F') digit = *s - 'A' + 10;
				else break;
				val += digit * frac_mult;
				frac_mult /= 16.0;
				any = 1;
				s++;
			}
		}
		int bin_exp = 0;
		if (*s == 'p' || *s == 'P') {
			const char *exp_start = s;
			s++;
			int exp_sign = 1;
			if (*s == '-') {
				exp_sign = -1;
				s++;
			} else if (*s == '+') {
				s++;
			}
			int has_exp_digits = 0;
			int exp_val = 0;
			while (*s >= '0' && *s <= '9') {
				if (exp_val < 100000) {
					exp_val = exp_val * 10 + (*s - '0');
				}
				has_exp_digits = 1;
				s++;
			}
			if (has_exp_digits) {
				bin_exp = exp_sign * exp_val;
			} else {
				s = exp_start;
			}
		}
		if (any) {
			val = ldexp(val, bin_exp);
		}
	} else {
		/* Parse decimal float */
		int decimals = 0;
		int has_dot = 0;
		int sig_digits = 0;
		int exponent_adjustment = 0;

		while (1) {
			if (*s >= '0' && *s <= '9') {
				any = 1;
				if (sig_digits < 17) {
					val = val * 10.0 + (*s - '0');
					if (val > 0.0) sig_digits++;
					if (has_dot) decimals++;
				} else {
					if (!has_dot) exponent_adjustment++;
				}
				s++;
			} else if (*s == '.' && !has_dot) {
				has_dot = 1;
				s++;
			} else {
				break;
			}
		}

		int dec_exp = 0;
		if (any && (*s == 'e' || *s == 'E')) {
			const char *exp_start = s;
			s++;
			int exp_sign = 1;
			if (*s == '-') {
				exp_sign = -1;
				s++;
			} else if (*s == '+') {
				s++;
			}
			int has_exp_digits = 0;
			int exp_val = 0;
			while (*s >= '0' && *s <= '9') {
				if (exp_val < 100000) {
					exp_val = exp_val * 10 + (*s - '0');
				}
				has_exp_digits = 1;
				s++;
			}
			if (has_exp_digits) {
				dec_exp = exp_sign * exp_val;
			} else {
				s = exp_start;
			}
		}

		if (any) {
			int total_exp = dec_exp + exponent_adjustment - decimals;
			if (total_exp > 0) {
				val *= pow10_helper(total_exp);
			} else if (total_exp < 0) {
				val /= pow10_helper(-total_exp);
			}
		}
	}

	if (endptr) {
		*endptr = (char *)(any ? s : nptr);
	}
	return any ? (val * sign) : 0.0;
}

__asm__(
".global setjmp\n"
"setjmp:\n"
"    movq %rbx, 0(%rdi)\n"
"    movq %rsp, 8(%rdi)\n"
"    movq %rbp, 16(%rdi)\n"
"    movq %r12, 24(%rdi)\n"
"    movq %r13, 32(%rdi)\n"
"    movq %r14, 40(%rdi)\n"
"    movq %r15, 48(%rdi)\n"
"    movq (%rsp), %rax\n"
"    movq %rax, 56(%rdi)\n"
"    xorq %rax, %rax\n"
"    ret\n"
);

__asm__(
".global longjmp\n"
"longjmp:\n"
"    movq 0(%rdi), %rbx\n"
"    movq 16(%rdi), %rbp\n"
"    movq 24(%rdi), %r12\n"
"    movq 32(%rdi), %r13\n"
"    movq 40(%rdi), %r14\n"
"    movq 48(%rdi), %r15\n"
"    movq 56(%rdi), %rdx\n"
"    movq 8(%rdi), %rsp\n"
"    movq %rsi, %rax\n"
"    testq %rax, %rax\n"
"    jnz 1f\n"
"    movq $1, %rax\n"
"1:\n"
"    movq %rdx, (%rsp)\n"
"    ret\n"
);

/* -----------------------------------------------------------------------
 * dlfcn stubs — B1NIX supports static linking only; dynamic loading is
 * not available.  These stubs follow the POSIX error-reporting contract:
 *   • dlopen()  always fails → returns NULL, sets dlerror buffer.
 *   • dlerror() returns the last error string and clears the buffer.
 *   • dlsym()   always fails → returns NULL, sets dlerror buffer.
 *   • dlclose() always fails → returns -1, sets dlerror buffer.
 * A program that checks dlerror() after each call will behave correctly.
 * ----------------------------------------------------------------------- */
static const char *_dl_errmsg;

void *dlopen(const char *filename, int flag)
{
    (void)flag;
    if (filename == NULL) {
        /* RTLD_DEFAULT / self-handle: return a non-NULL sentinel so that
         * dlsym(RTLD_DEFAULT, ...) callers get a consistent NULL back from
         * dlsym rather than a misleading dlerror from dlopen itself. */
        return (void *)(unsigned long)1;
    }
    _dl_errmsg = "dlopen: dynamic loading not supported on b1nix";
    return NULL;
}

char *dlerror(void)
{
    /* POSIX: each successful call to dlerror() resets the error indicator. */
    const char *msg = _dl_errmsg;
    _dl_errmsg = NULL;
    return (char *)msg;
}

void *dlsym(void *handle, const char *symbol)
{
    (void)handle;
    (void)symbol;
    _dl_errmsg = "dlsym: dynamic symbol lookup not supported on b1nix";
    return NULL;
}

int dlclose(void *handle)
{
    (void)handle;
    _dl_errmsg = "dlclose: dynamic loading not supported on b1nix";
    return -1;
}

double ldexp(double x, int exp)
{
	if (x == 0.0 || exp == 0) return x;
	while (exp > 100) {
		x *= 1.2676506002282294e+30; /* 2^100 */
		exp -= 100;
	}
	while (exp < -100) {
		x *= 7.888609052210118e-31; /* 2^-100 */
		exp += 100;
	}
	if (exp > 0) {
		double base = 2.0;
		while (exp > 0) {
			if (exp & 1) x *= base;
			base *= base;
			exp >>= 1;
		}
	} else if (exp < 0) {
		exp = -exp;
		double base = 0.5;
		while (exp > 0) {
			if (exp & 1) x *= base;
			base *= base;
			exp >>= 1;
		}
	}
	return x;
}

long double ldexpl(long double x, int exp)
{
	if (x == 0.0 || exp == 0) return x;
	while (exp > 100) {
		x *= 1.2676506002282294e+30L; /* 2^100 */
		exp -= 100;
	}
	while (exp < -100) {
		x *= 7.888609052210118e-31L; /* 2^-100 */
		exp += 100;
	}
	if (exp > 0) {
		long double base = 2.0L;
		while (exp > 0) {
			if (exp & 1) x *= base;
			base *= base;
			exp >>= 1;
		}
	} else if (exp < 0) {
		exp = -exp;
		long double base = 0.5L;
		while (exp > 0) {
			if (exp & 1) x *= base;
			base *= base;
			exp >>= 1;
		}
	}
	return x;
}

float strtof(const char *nptr, char **endptr)
{
	return (float)strtod(nptr, endptr);
}

long double strtold(const char *nptr, char **endptr)
{
	return (long double)strtod(nptr, endptr);
}

#include <signal.h>

sighandler_t signal(int signum, sighandler_t handler)
{
	struct sigaction act, old;
	act.sa_handler = handler;
	act.sa_flags = 0;
	act.sa_restorer = NULL;
	act.sa_mask = 0;
	if (sigaction(signum, &act, &old) < 0) {
		return SIG_ERR;
	}
	return old.sa_handler;
}

int sigemptyset(sigset_t *set) {
  if (!set) {
    errno = EINVAL;
    return -1;
  }
  *set = 0;
  return 0;
}
int sigfillset(sigset_t *set) {
  if (!set) {
    errno = EINVAL;
    return -1;
  }
  *set = ~0UL;
  return 0;
}
int sigaddset(sigset_t *set, int signum) {
  if (!set || signum <= 0 || signum >= 64) {
    errno = EINVAL;
    return -1;
  }
  *set |= (1UL << (signum - 1));
  return 0;
}
int sigdelset(sigset_t *set, int signum) {
  if (!set || signum <= 0 || signum >= 64) {
    errno = EINVAL;
    return -1;
  }
  *set &= ~(1UL << (signum - 1));
  return 0;
}
int sigismember(const sigset_t *set, int signum) {
  if (!set || signum <= 0 || signum >= 64) {
    errno = EINVAL;
    return -1;
  }
  return (*set & (1UL << (signum - 1))) ? 1 : 0;
}
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
  int rc = (int)syscall(SYS_SIGPROCMASK, how, (long)set, (long)oldset, 0);
  if (rc < 0) {
    errno = normalize_errno(rc);
    return -1;
  }
  return 0;
}

__asm__(
	".global __sig_restorer\n"
	"__sig_restorer:\n"
	"movq $99, %rax\n" /* SYS_SIGRETURN */
	"syscall\n"
);

extern void __sig_restorer(void);

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
	struct sigaction kernel_act;
	if (act) {
		kernel_act = *act;
		if (!kernel_act.sa_restorer) {
			kernel_act.sa_restorer = __sig_restorer;
		}
		kernel_act.sa_flags |= SA_RESTORER;
		act = &kernel_act;
	}
	int rc = (int)syscall(SYS_SIGNAL, signum, (long)act, (long)oldact, 0);
	if (rc < 0) {
		errno = normalize_errno(rc);
		return -1;
	}
	return rc;
}

int errno = 0;

int sem_init(int *sem, int pshared, unsigned int value) {
  (void)pshared;
  if (!sem) return -1;
  *sem = (int)value;
  return 0;
}

int sem_wait(int *sem) {
  if (!sem) return -1;
  while (*sem <= 0) {
    syscall(SYS_YIELD);
  }
  (*sem)--;
  return 0;
}

int sem_post(int *sem) {
  if (!sem) return -1;
  (*sem)++;
  return 0;
}

int sem_destroy(int *sem) {
  (void)sem;
  return 0;
}

static unsigned long next_rand = 1;

int rand(void) {
	next_rand = next_rand * 1103515245 + 12345;
	return (unsigned int)(next_rand / 65536) % 32768;
}

void srand(unsigned int seed) {
	next_rand = seed;
}

int fork(void);
int execv(const char *pathname, char *const argv[]);
typedef int pid_t;
pid_t waitpid(pid_t pid, int *status, int options);

int system(const char *command) {
	if (!command) return 1;
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		char *args[] = { "/bin/sh", "-c", (char *)command, NULL };
		execv("/bin/sh", args);
		exit(127);
	}
	int status;
	if (waitpid(pid, &status, 0) < 0) return -1;
	return status;
}
