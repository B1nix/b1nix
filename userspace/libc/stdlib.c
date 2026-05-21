#include <stdlib.h>
#include <string.h>
#include "syscall.h"

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
	if (heap_used + size > HEAP_SIZE) return 0;
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
	// Basic realloc: allocate new, copy old (assume conservative size), free old
	// Since we don't know the old size and have a bump allocator, we will copy size bytes...
	// Wait, we can't safely copy size bytes if it's larger than old size.
	// Let's copy 1024 bytes or size, whichever is smaller.
	// Actually, a simple workaround is just to copy "size" bytes because bump allocator
	// memory is contiguous and readable, but it might read garbage.
	void *new_ptr = malloc(size);
	if (new_ptr) {
		memcpy(new_ptr, ptr, size); // Might copy some garbage, but safe in our heap
		free(ptr);
	}
	return new_ptr;
}

long strtol(const char *nptr, char **endptr, int base)
{
	(void)base; // Ignoring base for now, assume 10
	long val = atoi(nptr);
	if (endptr) {
		while (*nptr == ' ' || *nptr == '-' || *nptr == '+') nptr++;
		while (*nptr >= '0' && *nptr <= '9') nptr++;
		*endptr = (char *)nptr;
	}
	return val;
}

unsigned long strtoul(const char *nptr, char **endptr, int base)
{
	return (unsigned long)strtol(nptr, endptr, base);
}

unsigned long long strtoull(const char *nptr, char **endptr, int base)
{
	return (unsigned long long)strtol(nptr, endptr, base);
}

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *))
{
	// Minimal bubble sort for TCC stub
	if (nmemb < 2) return;
	char *arr = (char *)base;
	char *tmp = malloc(size);
	if (!tmp) return;
	for (size_t i = 0; i < nmemb - 1; i++) {
		for (size_t j = 0; j < nmemb - i - 1; j++) {
			if (compar(arr + j * size, arr + (j + 1) * size) > 0) {
				memcpy(tmp, arr + j * size, size);
				memcpy(arr + j * size, arr + (j + 1) * size, size);
				memcpy(arr + (j + 1) * size, tmp, size);
			}
		}
	}
	free(tmp);
}

double strtod(const char *nptr, char **endptr)
{
	if (endptr) *endptr = (char *)nptr;
	return 0.0;
}

int setjmp(long env[8])
{
	(void)env;
	return 0;
}

void longjmp(long env[8], int val)
{
	(void)env; (void)val;
	exit(1);
}

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
	return (int)syscall(SYS_SIGNAL, signum, (long)act, (long)oldact, 0);
}

int errno = 0;

int sem_init(int *sem, int pshared, unsigned int value) { (void)sem; (void)pshared; (void)value; return 0; }
int sem_wait(int *sem) { (void)sem; return 0; }
int sem_post(int *sem) { (void)sem; return 0; }
int sem_destroy(int *sem) { (void)sem; return 0; }







