#include <stdlib.h>
#include <string.h>
#include "syscall.h"
#include <errno.h>
#include <math.h>

extern int normalize_errno(long rc);

void exit(int status)
{
	syscall(SYS_EXIT, status, 0, 0, 0);
	while (1);
}

/* Simple bump-allocator for userspace malloc.
   In a real system, this would use sbrk/mmap.
   For M25, we use a static pool. */

#define HEAP_SIZE (16 * 1024 * 1024)
static char heap[HEAP_SIZE];
static size_t heap_used;

void *malloc(size_t size)
{
	if (size == 0) return 0;
	/* Align to 8 bytes */
	size = (size + 7) & ~(size_t)7;
	if (heap_used + size > HEAP_SIZE) {
		errno = ENOMEM;
		return 0;
	}
	void *p = heap + heap_used;
	heap_used += size;
	return p;
}

void free(void *ptr)
{
	/* No-op bump allocator — memory is never freed */
	(void)ptr;
}

void *calloc(size_t nmemb, size_t size)
{
	size_t total = nmemb * size;
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
	(void)name;
	return NULL;
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
	void *new_ptr = malloc(size);
	if (new_ptr) {
		size_t safe_copy = size;
		if ((char *)ptr + safe_copy > heap + HEAP_SIZE) {
			safe_copy = (heap + HEAP_SIZE) - (char *)ptr;
		}
		memcpy(new_ptr, ptr, safe_copy);
	}
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
