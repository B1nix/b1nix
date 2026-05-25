#include <stdlib.h>
#include <string.h>
#include "syscall.h"
#include <errno.h>

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

double strtod(const char *nptr, char **endptr)
{
	const char *s = nptr;
	while (*s == ' ' || *s == '\t') s++;
	if (*s == '-' || *s == '+') s++;
	int any = 0;
	while (*s >= '0' && *s <= '9') { s++; any = 1; }
	if (*s == '.') {
		s++;
		while (*s >= '0' && *s <= '9') { s++; any = 1; }
	}
	if (any && (*s == 'e' || *s == 'E' || *s == 'p' || *s == 'P')) {
		s++;
		if (*s == '-' || *s == '+') s++;
		while (*s >= '0' && *s <= '9') s++;
	}
	if (endptr) *endptr = (char *)(any ? s : nptr);
	return 0.0;
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

void *dlopen(const char *filename, int flag) { (void)filename; (void)flag; return NULL; }
char *dlerror(void) { return "Dynamic loading not supported"; }
void *dlsym(void *handle, const char *symbol) { (void)handle; (void)symbol; return NULL; }
int dlclose(void *handle) { (void)handle; return -1; }

double ldexp(double x, int exp) { (void)x; (void)exp; return 0.0; }
long double ldexpl(long double x, int exp) { (void)x; (void)exp; return 0.0; }
float strtof(const char *nptr, char **endptr) { if (endptr) *endptr = (char *)nptr; return 0.0f; }
long double strtold(const char *nptr, char **endptr) { if (endptr) *endptr = (char *)nptr; return 0.0; }

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

int sigemptyset(sigset_t *set) { if (set) { *set = 0; return 0; } return -1; }
int sigaddset(sigset_t *set, int signum) { if (set) { *set |= (1UL << (signum % 64)); return 0; } return -1; }
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) { (void)how; (void)set; (void)oldset; return 0; }

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
		act = &kernel_act;
	}
	int rc = (int)syscall(SYS_SIGNAL, signum, (long)act, (long)oldact, 0);
	if (rc < 0) {
		errno = -rc;
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
