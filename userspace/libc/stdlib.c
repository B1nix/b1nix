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

#define HEAP_SIZE (64 * 1024)
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
