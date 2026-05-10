#include <stdlib.h>
#include <string.h>
#include <b1nix/syscall.h>
#include <b1nix/mm.h>

void abort(void)
{
	/* Print message then exit */
	const char *msg = "abort() called\n";
	syscall_dispatch(SYS_WRITE, (u64)(usize)msg, strlen(msg), 0, 0, 0, 0);
	syscall_dispatch(SYS_EXIT, 1, 0, 0, 0, 0, 0);
	while (1);
}

void exit(int status)
{
	syscall_dispatch(SYS_EXIT, (u64)status, 0, 0, 0, 0, 0);
	while (1);
}

void *malloc(size_t size)
{
	/* Use kernel heap allocation through mmap-like interface */
	return kmalloc(size);
}

void free(void *ptr)
{
	kfree(ptr);
}

int atoi(const char *s)
{
	int result = 0;
	int sign = 1;
	
	if (*s == '-') {
		sign = -1;
		s++;
	} else if (*s == '+') {
		s++;
	}
	
	while (*s >= '0' && *s <= '9') {
		result = result * 10 + (*s - '0');
		s++;
	}
	
	return result * sign;
}
