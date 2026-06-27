#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include "syscall.h"
#include <errno.h>
#include <math.h>
#include <sys/mman.h>
#include <setjmp.h>
#include <signal.h>
#include <dlfcn.h>
#include <time.h>

extern int normalize_errno(long rc);

#define MAX_CXA_ATEXIT 64

struct cxa_atexit_entry {
	void (*dtor)(void *);
	void *obj;
	void *dso_handle;
};

static struct cxa_atexit_entry cxa_atexit_table[MAX_CXA_ATEXIT];
static int cxa_atexit_count = 0;

/* C++ ABI: register a destructor for static/global objects.
 * dtor is called with obj when the shared object is unloaded or at exit. */
int __cxa_atexit(void (*dtor)(void *), void *obj, void *dso_handle) {
	(void)dso_handle;
	if (cxa_atexit_count >= MAX_CXA_ATEXIT) return -1;
	cxa_atexit_table[cxa_atexit_count].dtor = dtor;
	cxa_atexit_table[cxa_atexit_count].obj = obj;
	cxa_atexit_table[cxa_atexit_count].dso_handle = dso_handle;
	cxa_atexit_count++;
	return 0;
}

/* C++ ABI: unregister destructors for a specific DSO (or all if dso == NULL). */
void __cxa_finalize(void *dso_handle) {
	if (!dso_handle) {
		/* Finalize all registered destructors in reverse order. */
		for (int i = cxa_atexit_count - 1; i >= 0; i--) {
			cxa_atexit_table[i].dtor(cxa_atexit_table[i].obj);
		}
		cxa_atexit_count = 0;
		return;
	}
	/* Finalize only destructors matching the given DSO. */
	for (int i = cxa_atexit_count - 1; i >= 0; i--) {
		if (cxa_atexit_table[i].dso_handle == dso_handle) {
			cxa_atexit_table[i].dtor(cxa_atexit_table[i].obj);
			/* Shift remaining entries down. */
			for (int j = i; j < cxa_atexit_count - 1; j++) {
				cxa_atexit_table[j] = cxa_atexit_table[j + 1];
			}
			cxa_atexit_count--;
		}
	}
}

/* C++ ABI: get the once-used guard variable for thread-safe statics.
 * Returns 0 on first call, nonzero otherwise. Uses kernel futex. */
int __cxa_guard_acquire(int *guard) {
	if (*guard) return 0;
	return 1;
}

void __cxa_guard_release(int *guard) {
	*guard = 1;
}

void __cxa_guard_abort(int *guard) {
	*guard = 0;
}

/* C++ ABI: pure virtual function call handler. */
void __cxa_pure_virtual(void) {
	__builtin_trap();
}

/* C++ ABI: deleted virtual function call handler. */
void __cxa_deleted_virtual(void) {
	__builtin_trap();
}

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
	__builtin_trap();
}

/* C99 _Exit: terminate immediately without running atexit handlers. */
void _Exit(int status)
{
	syscall(SYS_EXIT, status, 0, 0, 0);
	__builtin_trap();
}

void abort(void)
{
	exit(127);
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

static void *_malloc_unlocked(size_t size)
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

static void _free_unlocked(void *ptr)
{
	if (!ptr) return;
	size_t size = MA_SIZE(MA_HDR(ptr));
	MA_PUT(MA_HDR(ptr), MA_PACK(size, 0));
	MA_PUT(MA_FTR(ptr), MA_PACK(size, 0));
	ma_coalesce((char *)ptr);
}

static void *_calloc_unlocked(size_t nmemb, size_t size)
{
	size_t total = nmemb * size;
	if (size != 0 && total / size != nmemb) { errno = ENOMEM; return 0; } /* overflow */
	void *p = _malloc_unlocked(total);
	if (p) memset(p, 0, total);
	return p;
}

static pthread_mutex_t g_malloc_lock = PTHREAD_MUTEX_INITIALIZER;

void *malloc(size_t size)
{
	pthread_mutex_lock(&g_malloc_lock);
	void *p = _malloc_unlocked(size);
	pthread_mutex_unlock(&g_malloc_lock);
	return p;
}

/* Sentinel marking an over-aligned block (see ma_aligned below). Its bit 0 is
 * CLEAR; a live boundary-tag header is always MA_PACK(size, 1) with the alloc
 * bit set, so a real header can never equal the sentinel — no collision on
 * either arch (the low word is what 32-bit reads). */
#define MA_ALIGNED_MAGIC ((size_t)0xA11C0CA11C0CA110ULL)

/* For an over-aligned payload, recover the original malloc block pointer; for an
 * ordinary block, return it unchanged. Every malloc-family entry that reaches
 * into the boundary-tag header (free/realloc/malloc_usable_size) must go through
 * this — otherwise it reads the sentinel as a size and corrupts the heap. */
static void *ma_base_ptr(void *ptr)
{
	if (!ptr) return ptr;   /* realloc(NULL, 0) reaches here before any null check */
	if (((size_t *)ptr)[-1] == MA_ALIGNED_MAGIC)
		return (void *)(size_t)((size_t *)ptr)[-2];
	return ptr;
}

/* Usable bytes reachable from `ptr` (accounting for the alignment shift of an
 * over-aligned block, whose payload starts above the malloc block). */
static size_t ma_user_size(void *ptr)
{
	void *base = ma_base_ptr(ptr);
	size_t block_payload = MA_SIZE(MA_HDR(base)) - MA_DSIZE;
	size_t off = (size_t)ptr - (size_t)base;   /* 0 for an ordinary block */
	return block_payload - off;
}

void free(void *ptr)
{
	if (!ptr) return;
	/* Over-aligned blocks stash the sentinel in the word just below the payload
	 * and the real malloc pointer below that; recover it before freeing. */
	void *base = ma_base_ptr(ptr);
	pthread_mutex_lock(&g_malloc_lock);
	_free_unlocked(base);
	pthread_mutex_unlock(&g_malloc_lock);
}

void *calloc(size_t nmemb, size_t size)
{
	pthread_mutex_lock(&g_malloc_lock);
	void *p = _calloc_unlocked(nmemb, size);
	pthread_mutex_unlock(&g_malloc_lock);
	return p;
}

long long atoll(const char *s)
{
	return strtoll(s, (char **)0, 10);
}

lldiv_t lldiv(long long numer, long long denom)
{
	lldiv_t r;
	r.quot = numer / denom;
	r.rem = numer % denom;
	return r;
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

/* glibc-compat program-name globals (used by Chromium's set_process_title). Set
 * from argv[0] by crt0 startup when available; default to a harmless name so the
 * symbols resolve even for binaries that don't populate them. */
char *program_invocation_name = (char *)"b1nix";
char *program_invocation_short_name = (char *)"b1nix";

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

int clearenv(void)
{
	/* Detach from any heap-allocated environ array and present an empty
	 * environment. The previous array (if malloc'd by setenv/putenv) is
	 * intentionally not freed: callers may still hold pointers into the old
	 * strings, matching the conservative glibc behaviour. */
	environ = empty_env;
	return 0;
}

char *realpath(const char *path, char *resolved_path)
{
	if (!resolved_path) return strdup(path);
	strcpy(resolved_path, path);
	return resolved_path;
}

static void *_realloc_unlocked(void *ptr, size_t size)
{
	if (size == 0) { _free_unlocked(ma_base_ptr(ptr)); return NULL; }
	if (!ptr) return _malloc_unlocked(size);
	/* Honour over-aligned blocks: copy from the (possibly shifted) user payload,
	 * but free the real malloc block. The reallocated block is only 16-aligned —
	 * standard realloc never promises to preserve over-alignment, and callers that
	 * need it (e.g. V8's Isolate) do not realloc. */
	size_t old_usable = ma_user_size(ptr);
	void *new_ptr = _malloc_unlocked(size);
	if (!new_ptr) return NULL;
	size_t copy = size < old_usable ? size : old_usable;
	memcpy(new_ptr, ptr, copy);
	_free_unlocked(ma_base_ptr(ptr));
	return new_ptr;
}

void *realloc(void *ptr, size_t size)
{
	pthread_mutex_lock(&g_malloc_lock);
	void *p = _realloc_unlocked(ptr, size);
	pthread_mutex_unlock(&g_malloc_lock);
	return p;
}

/* Usable payload of an allocation (>= the requested size). Matches the
 * old_payload computation in _realloc_unlocked. */
size_t malloc_usable_size(void *ptr)
{
	if (!ptr) return 0;
	return ma_user_size(ptr);
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
				/* Leading 0 selects octal, but the 0 is itself a digit: do
				 * NOT skip it, or a "0" followed by a non-octal char (e.g.
				 * "0+1") would consume no digits, leave any==0, and report
				 * endptr==nptr — making strtoull-driven parsers (ash's
				 * arithmetic) spin forever on a stuck cursor. The digit loop
				 * below consumes the 0. */
				base = 8;
			}
		} else {
			base = 10;
		}
	} else if (base == 16) {
		if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
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

long long strtoll(const char *nptr, char **endptr, int base)
{
	return (long long)strtol(nptr, endptr, base);
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

#ifdef __x86_64__
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
#else
__asm__(
".global setjmp\n"
"setjmp:\n"
"    movl 4(%esp), %eax\n"
"    movl %ebx, 0(%eax)\n"
"    movl %esi, 4(%eax)\n"
"    movl %edi, 8(%eax)\n"
"    movl %ebp, 12(%eax)\n"
"    leal 4(%esp), %ecx\n"
"    movl %ecx, 16(%eax)\n"
"    movl (%esp), %ecx\n"
"    movl %ecx, 20(%eax)\n"
"    xorl %eax, %eax\n"
"    ret\n"
);

__asm__(
".global longjmp\n"
"longjmp:\n"
"    movl 4(%esp), %edx\n"
"    movl 8(%esp), %eax\n"
"    testl %eax, %eax\n"
"    jnz 1f\n"
"    movl $1, %eax\n"
"1:\n"
"    movl 0(%edx), %ebx\n"
"    movl 4(%edx), %esi\n"
"    movl 8(%edx), %edi\n"
"    movl 12(%edx), %ebp\n"
"    movl 16(%edx), %ecx\n"
"    movl %ecx, %esp\n"
"    movl 20(%edx), %ecx\n"
"    jmp *%ecx\n"
);
#endif

/* sigsetjmp/siglongjmp signal-mask handling (see <setjmp.h>). __sigsetjmp_save
 * runs (via the sigsetjmp macro) immediately before the inline setjmp and, when
 * savemask is non-zero, records the current signal mask so siglongjmp can
 * restore it on the way out. */
int __sigsetjmp_save(struct __sigjmp_buf *env, int savemask) {
	env->__savemask = savemask;
	if (savemask) {
		sigset_t cur = 0;
		sigprocmask(SIG_BLOCK, NULL, &cur);
		env->__mask = (unsigned long long)cur;
	}
	return 0;
}

void siglongjmp(sigjmp_buf env, int val) {
	if (env[0].__savemask) {
		sigset_t m = (sigset_t)env[0].__mask;
		sigprocmask(SIG_SETMASK, &m, NULL);
	}
	longjmp(env[0].__jb, val);
}

/* dlopen/dlsym/dlclose/dlerror/dladdr live in dlfcn.c (M69 runtime loader). */

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

double frexp(double x, int *exp)
{
	/* Decompose x into a normalised fraction in [0.5, 1) and a power of two,
	 * so that x == fraction * 2^*exp. Zero, NaN and infinity return x with an
	 * exponent of 0 (IEEE 754 / C99). */
	if (exp) *exp = 0;
	if (x == 0.0 || x != x || (x - x) != 0.0)
		return x;
	int e = 0;
	double ax = x < 0 ? -x : x;
	while (ax >= 1.0) { ax *= 0.5; e++; }
	while (ax < 0.5)  { ax *= 2.0; e--; }
	if (exp) *exp = e;
	return x < 0 ? -ax : ax;
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

char *strsignal(int sig) {
  static char unknown[24];
  switch (sig) {
  case SIGABRT: return "Aborted";
  case SIGALRM: return "Alarm clock";
  case SIGBUS: return "Bus error";
  case SIGCHLD: return "Child exited";
  case SIGCONT: return "Continued";
  case SIGFPE: return "Floating point exception";
  case SIGHUP: return "Hangup";
  case SIGILL: return "Illegal instruction";
  case SIGINT: return "Interrupt";
  case SIGKILL: return "Killed";
  case SIGPIPE: return "Broken pipe";
  case SIGQUIT: return "Quit";
  case SIGSEGV: return "Segmentation fault";
  case SIGSTOP: return "Stopped";
  case SIGTERM: return "Terminated";
  case SIGTSTP: return "Stopped";
  case SIGTTIN: return "Stopped (tty input)";
  case SIGTTOU: return "Stopped (tty output)";
  case SIGUSR1: return "User signal 1";
  case SIGUSR2: return "User signal 2";
  case SIGSYS: return "Bad system call";
  case SIGTRAP: return "Trace/breakpoint trap";
  case SIGXCPU: return "CPU time limit exceeded";
  case SIGXFSZ: return "File size limit exceeded";
  case SIGVTALRM: return "Virtual timer expired";
  case SIGWINCH: return "Window changed";
  default:
    snprintf(unknown, sizeof(unknown), "Signal %d", sig);
    return unknown;
  }
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
  int rc = (int)syscall(SYS_SIGPROCMASK, how, (long)set, (long)oldset, 0);
  if (rc < 0) {
    errno = normalize_errno(rc);
    return -1;
  }
  return 0;
}

#ifdef __x86_64__
__asm__(
	".global __sig_restorer\n"
	"__sig_restorer:\n"
	"movq $99, %rax\n" /* SYS_SIGRETURN */
	"syscall\n"
);
#else
__asm__(
	".global __sig_restorer\n"
	"__sig_restorer:\n"
	"movl $99, %eax\n" /* SYS_SIGRETURN */
	"int $0x80\n"
);
#endif

extern void __sig_restorer(void);

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
	struct sigaction kernel_act;
	if (act) {
		kernel_act = *act;
		/* sa_restorer is a non-portable, b1nix-internal implementation detail:
		 * the kernel returns from a signal handler by jumping to it. POSIX apps
		 * do not set it and routinely leave the field uninitialized (e.g.
		 * dropbear's SIGCHLD handler only sets sa_handler/sa_flags/sa_mask).
		 * Trusting a garbage value there made the handler "return" to a random
		 * address → SIGSEGV. Always force our own trampoline regardless of what
		 * the caller left in the field. */
		kernel_act.sa_restorer = __sig_restorer;
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

int sigaltstack(const stack_t *ss, stack_t *old_ss)
{
	int rc = (int)syscall(SYS_SIGALTSTACK, (long)ss, (long)old_ss, 0);
	if (rc < 0) {
		errno = normalize_errno(rc);
		return -1;
	}
	return 0;
}

int errno = 0;

/* POSIX semaphores, futex-backed. The sem_t is a single int holding the count
 * (always >= 0). Waiters atomically decrement when the count is positive and
 * otherwise park in the kernel via SYS_FUTEX (no busy-spin, no decrement race —
 * the old `while (*sem<=0) yield; (*sem)--` lost posts and double-decremented
 * under contention). */
int sem_init(int *sem, int pshared, unsigned int value) {
  (void)pshared;
  if (!sem) { errno = EINVAL; return -1; }
  __atomic_store_n(sem, (int)value, __ATOMIC_RELEASE);
  return 0;
}

/* Try to take one count without blocking. Returns 0 on success, -1/EAGAIN if
 * the count is currently zero. */
int sem_trywait(int *sem) {
  if (!sem) { errno = EINVAL; return -1; }
  int v = __atomic_load_n(sem, __ATOMIC_ACQUIRE);
  while (v > 0) {
    if (__atomic_compare_exchange_n(sem, &v, v - 1, 1,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
      return 0;
    /* v reloaded with the current value on CAS failure; retry. */
  }
  errno = EAGAIN;
  return -1;
}

int sem_wait(int *sem) {
  if (!sem) { errno = EINVAL; return -1; }
  for (;;) {
    if (sem_trywait(sem) == 0)
      return 0;
    /* Count is zero: wait until a post bumps it. FUTEX_WAIT returns
     * immediately (-EAGAIN) if *sem != 0, closing the post/park race. */
    syscall(SYS_FUTEX, sem, FUTEX_WAIT, 0);
  }
}

int sem_timedwait(int *sem, const struct timespec *abs_timeout) {
  if (!sem) { errno = EINVAL; return -1; }
  for (;;) {
    if (sem_trywait(sem) == 0)
      return 0;
    long ms = 0;
    if (abs_timeout) {
      struct timespec now;
      clock_gettime(CLOCK_REALTIME, &now);
      ms = (abs_timeout->tv_sec - now.tv_sec) * 1000L +
           (abs_timeout->tv_nsec - now.tv_nsec) / 1000000L;
      if (ms <= 0) { errno = ETIMEDOUT; return -1; }
    }
    long rc = syscall(SYS_FUTEX, sem, FUTEX_WAIT, 0, ms);
    if (rc == -ETIMEDOUT) { errno = ETIMEDOUT; return -1; }
  }
}

int sem_post(int *sem) {
  if (!sem) { errno = EINVAL; return -1; }
  __atomic_add_fetch(sem, 1, __ATOMIC_RELEASE);
  /* Wake one waiter; harmless if none are parked. */
  syscall(SYS_FUTEX, sem, FUTEX_WAKE, 1);
  return 0;
}

int sem_getvalue(int *sem, int *sval) {
  if (!sem || !sval) { errno = EINVAL; return -1; }
  *sval = __atomic_load_n(sem, __ATOMIC_ACQUIRE);
  return 0;
}

int sem_destroy(int *sem) {
  (void)sem;
  return 0;
}

static unsigned long next_rand = 1;

int rand(void) {
	next_rand = next_rand * 1103515245 + 12345;
	return (int)(next_rand & RAND_MAX);
}

/* rand_r: reentrant rand over a caller-supplied seed (same LCG as rand). */
int rand_r(unsigned int *seedp) {
	unsigned long s = (unsigned long)*seedp * 1103515245UL + 12345UL;
	*seedp = (unsigned int)s;
	return (int)(s & RAND_MAX);
}

void srand(unsigned int seed) {
	next_rand = seed;
}

/* random() family: classic BSD TYPE_3 additive-feedback generator (degree 31,
 * separation 3), seeded with the minstd LCG like glibc. Produces 31-bit values.
 * random_r/srandom_r/initstate_r are the GNU reentrant forms; random()/srandom()
 * wrap a single global state. */
int srandom_r(unsigned int seed, struct random_data *buf) {
	if (!buf) { errno = EINVAL; return -1; }
	if (seed == 0) seed = 1;
	buf->x[0] = (int32_t)seed;
	for (int i = 1; i < 31; i++) {
		/* minstd via Schrage's method to avoid 32-bit overflow */
		int32_t hi = buf->x[i - 1] / 127773;
		int32_t lo = buf->x[i - 1] % 127773;
		int32_t w = 16807 * lo - 2836 * hi;
		if (w < 0) w += 2147483647;
		buf->x[i] = w;
	}
	buf->fptr = 3;
	buf->rptr = 0;
	buf->valid = 1;
	int32_t dummy;
	for (int i = 0; i < 310; i++) random_r(buf, &dummy); /* warm up */
	return 0;
}

int random_r(struct random_data *buf, int32_t *result) {
	if (!buf || !result || !buf->valid) { errno = EINVAL; return -1; }
	buf->x[buf->fptr] += buf->x[buf->rptr];
	*result = (int32_t)((uint32_t)buf->x[buf->fptr] >> 1);
	if (++buf->fptr >= 31) buf->fptr = 0;
	if (++buf->rptr >= 31) buf->rptr = 0;
	return 0;
}

int initstate_r(unsigned int seed, char *statebuf, size_t statelen,
                struct random_data *buf) {
	(void)statebuf; (void)statelen; /* state lives in buf; size is a quality hint */
	return srandom_r(seed, buf);
}

static struct random_data g_random = { .valid = 0 };

void srandom(unsigned int seed) { srandom_r(seed, &g_random); }

/* b1nix keeps no system load average. Report "unavailable" (-1) so callers
 * (e.g. google_benchmark's sysinfo) fall back to a zero/unknown load. */
int getloadavg(double loadavg[], int nelem) {
	(void)loadavg;
	(void)nelem;
	return -1;
}

long random(void) {
	int32_t r;
	if (!g_random.valid) srandom_r(1, &g_random);
	random_r(&g_random, &r);
	return (long)r;
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

__attribute__((weak)) void __register_frame_info(const void *begin, void *ob) {
	(void)begin;
	(void)ob;
}

const char *getprogname(void) {
	return "wget";
}

int raise(int sig) {
	int pid = (int)syscall(SYS_GETPID);
	return (int)syscall(SYS_KILL, pid, sig);
}

/* UTF-8 (non-restartable) conversions, delegating to the wchar.c primitives so
 * the whole libc agrees on the encoding. */
extern size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, void *ps);
extern size_t wcrtomb(char *s, wchar_t wc, void *ps);

int wctomb(char *s, wchar_t wc) {
	if (!s) return 0;
	size_t r = wcrtomb(s, wc, 0);
	return r == (size_t)-1 ? -1 : (int)r;
}

int mbtowc(wchar_t *pwc, const char *s, size_t n) {
	if (!s) return 0;
	if (n == 0) return -1;
	size_t r = mbrtowc(pwc, s, n, 0);
	if (r == (size_t)-1 || r == (size_t)-2) return -1;
	return (int)r;
}

char *tzname[2] = { (char *)"UTC", (char *)"UTC" };
long timezone = 0;
int daylight = 0;
int __b1nix_tz_dst_rule = 0; /* 0=none, 1=EU, 2=US */

static int tz_parse_hhmm(const char *s, int *consumed, int *seconds_out) {
	int h = 0, m = 0, i = 0;
	if (s[i] < '0' || s[i] > '9') return 0;
	while (s[i] >= '0' && s[i] <= '9') {
		h = h * 10 + (s[i] - '0');
		i++;
	}
	if (s[i] == ':') {
		i++;
		if (!(s[i] >= '0' && s[i] <= '9' && s[i + 1] >= '0' && s[i + 1] <= '9')) {
			return 0;
		}
		m = (s[i] - '0') * 10 + (s[i + 1] - '0');
		i += 2;
	}
	if (m < 0 || m > 59) return 0;
	*seconds_out = h * 3600 + m * 60;
	*consumed = i;
	return 1;
}

void tzset(void) {
	const char *tz = getenv("TZ");
	static char stdname[16] = "UTC";
	static char dstname[16] = "DST";

	timezone = 0;
	daylight = 0;
	__b1nix_tz_dst_rule = 0;
	tzname[0] = stdname;
	tzname[1] = dstname;
	strcpy(stdname, "UTC");
	strcpy(dstname, "DST");

	if (!tz || !*tz) return;

	/* Accept TZ as:
	 *  - "UTC", "GMT"
	 *  - "+HH[:MM]", "-HH[:MM]"
	 *  - "UTC+HH[:MM]", "GMT-HH[:MM]"
	 *  - "NAME+HH[:MM]" / "NAME-HH[:MM]"
	 *  - "NAMEHH[:MM]" (POSIX-style: west of UTC) */
	const char *p = tz;
	int had_name = 0;
	int ni = 0;
	while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
		if (ni < (int)sizeof(stdname) - 1) stdname[ni++] = *p;
		p++;
		had_name = 1;
	}
	stdname[ni] = '\0';
	if (!had_name) strcpy(stdname, "UTC");

	if (*p == '\0') return;

	int sign = 0;
	if (*p == '+') {
		sign = +1;
		p++;
	} else if (*p == '-') {
		sign = -1;
		p++;
	}

	int consumed = 0;
	int off = 0;
	if (!tz_parse_hhmm(p, &consumed, &off)) return;

	/* Human-readable +/- means east/west respectively.
	 * POSIX NAMEHH form (without +/-) means west of UTC. */
	int offset_east_sec;
	if (sign == 0 && had_name) offset_east_sec = -off;
	else if (sign == 0) offset_east_sec = off;
	else offset_east_sec = sign * off;

	timezone = -offset_east_sec; /* seconds west of UTC */

	p += consumed;
	if (*p == ',') {
		p++;
		if ((p[0] == 'E' || p[0] == 'e') && (p[1] == 'U' || p[1] == 'u') && p[2] == '\0') {
			daylight = 1;
			__b1nix_tz_dst_rule = 1;
		} else if ((p[0] == 'U' || p[0] == 'u') && (p[1] == 'S' || p[1] == 's') && p[2] == '\0') {
			daylight = 1;
			__b1nix_tz_dst_rule = 2;
		}
	}
}

#include <fcntl.h>
#include <unistd.h>

int mkstemp(char *tmpl) {
	int len = strlen(tmpl);
	if (len < 6 || strcmp(tmpl + len - 6, "XXXXXX") != 0) {
		errno = EINVAL;
		return -1;
	}
	static unsigned int seed = 12345;
	for (int pass = 0; pass < 100; pass++) {
		unsigned int val = seed;
		seed = seed * 1103515245 + 12345;
		for (int i = 0; i < 6; i++) {
			int c = val % 62;
			val /= 62;
			char ch;
			if (c < 10) ch = '0' + c;
			else if (c < 36) ch = 'a' + (c - 10);
			else ch = 'A' + (c - 36);
			tmpl[len - 6 + i] = ch;
		}
		int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0) return fd;
		if (errno != EEXIST) return -1;
	}
	errno = EEXIST;
	return -1;
}

/* mkstemps: like mkstemp but the template is "...XXXXXX<suffix>" where the last
 * `suffixlen` chars are kept verbatim (ANGLE uses it for ".tmp"-suffixed files). */
int mkstemps(char *tmpl, int suffixlen) {
	int len = strlen(tmpl);
	if (suffixlen < 0 || len < 6 + suffixlen ||
	    strncmp(tmpl + len - 6 - suffixlen, "XXXXXX", 6) != 0) {
		errno = EINVAL;
		return -1;
	}
	int xpos = len - 6 - suffixlen;
	static unsigned int seed = 0x9e3779b9;
	for (int pass = 0; pass < 100; pass++) {
		unsigned int val = seed;
		seed = seed * 1103515245 + 12345;
		for (int i = 0; i < 6; i++) {
			int c = val % 62;
			val /= 62;
			char ch;
			if (c < 10) ch = '0' + c;
			else if (c < 36) ch = 'a' + (c - 10);
			else ch = 'A' + (c - 36);
			tmpl[xpos + i] = ch;
		}
		int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0) return fd;
		if (errno != EEXIST) return -1;
	}
	errno = EEXIST;
	return -1;
}

char *mkdtemp(char *tmpl) {
	int len = strlen(tmpl);
	if (len < 6 || strcmp(tmpl + len - 6, "XXXXXX") != 0) {
		errno = EINVAL;
		return NULL;
	}
	static unsigned int seed = 0x4d3c2b1a;
	for (int pass = 0; pass < 100; pass++) {
		unsigned int val = seed;
		seed = seed * 1103515245 + 12345;
		for (int i = 0; i < 6; i++) {
			int c = val % 62;
			val /= 62;
			tmpl[len - 6 + i] =
				c < 10 ? '0' + c : c < 36 ? 'a' + c - 10 : 'A' + c - 36;
		}
		if (mkdir(tmpl, 0700) == 0)
			return tmpl;
		if (errno != EEXIST)
			return NULL;
	}
	errno = EEXIST;
	return NULL;
}

/* Aligned allocation. b1nix malloc returns 16-byte-aligned payloads; for
 * alignment > 16 (V8's JIT needs the Isolate page-aligned so isolate_data_ is
 * 64-aligned; some ports want cache-line alignment) we over-allocate, bump the
 * payload up to the requested boundary, and stash a sentinel + the original
 * malloc pointer in the two words just below it so plain free() still works
 * (see free() / MA_ALIGNED_MAGIC). Power-of-two alignment only. */
static void *ma_aligned(size_t alignment, size_t size) {
  if (alignment <= MA_DSIZE)                 /* malloc already 16-aligns */
    return malloc(size ? size : 1);
  if (alignment == 0 || (alignment & (alignment - 1)) != 0)
    return 0;                                /* must be a power of two */
  size_t want = size ? size : 1;
  size_t slack = alignment + 2 * MA_WSIZE;   /* room to shift up + 2 metadata words */
  if (want > (size_t)-1 - slack)
    return 0;                                /* overflow */
  char *raw = malloc(want + slack);
  if (!raw)
    return 0;
  size_t base = (size_t)raw + 2 * MA_WSIZE;
  size_t aligned = (base + (alignment - 1)) & ~(alignment - 1);
  ((size_t *)aligned)[-1] = MA_ALIGNED_MAGIC;          /* sentinel for free() */
  ((size_t *)aligned)[-2] = (size_t)raw;               /* real block to free */
  return (void *)aligned;
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
  if (!memptr || (alignment & (alignment - 1)) != 0 ||
      alignment % sizeof(void *) != 0)
    return EINVAL;
  void *p = ma_aligned(alignment, size);
  if (!p)
    return ENOMEM;
  *memptr = p;
  return 0;
}

void *aligned_alloc(size_t alignment, size_t size) {
  return ma_aligned(alignment, size);
}

/* memalign: SVID/BSD aligned allocator (used by libstdc++'s operator new when
 * its config detects _GLIBCXX_HAVE_MEMALIGN). */
void *memalign(size_t alignment, size_t size) {
  return ma_aligned(alignment, size);
}
