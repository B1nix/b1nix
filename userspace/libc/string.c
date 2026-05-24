#include <string.h>
#include <stdlib.h>
#include "syscall.h"
#include <errno.h>

void *memcpy(void *dest, const void *src, size_t n)
{
	unsigned char *d = (unsigned char *)dest;
	const unsigned char *s = (const unsigned char *)src;
	for (size_t i = 0; i < n; i++) d[i] = s[i];
	return dest;
}

void *memset(void *dest, int v, size_t n)
{
	unsigned char *d = (unsigned char *)dest;
	for (size_t i = 0; i < n; i++) d[i] = (unsigned char)v;
	return dest;
}

void *memmove(void *dest, const void *src, size_t n)
{
	unsigned char *d = (unsigned char *)dest;
	const unsigned char *s = (const unsigned char *)src;
	if (d < s) {
		for (size_t i = 0; i < n; i++) d[i] = s[i];
	} else {
		for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
	}
	return dest;
}

int memcmp(const void *p1, const void *p2, size_t n)
{
	const unsigned char *a = (const unsigned char *)p1;
	const unsigned char *b = (const unsigned char *)p2;
	for (size_t i = 0; i < n; i++) {
		if (a[i] != b[i]) return (int)a[i] - (int)b[i];
	}
	return 0;
}

size_t strlen(const char *s)
{
	size_t n = 0;
	while (s[n]) n++;
	return n;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
		if (a[i] == '\0') return 0;
	}
	return 0;
}

char *strcpy(char *dest, const char *src)
{
	char *d = dest;
	while ((*d++ = *src++));
	return dest;
}

char *strncpy(char *dest, const char *src, size_t n)
{
	size_t i;
	for (i = 0; i < n && src[i]; i++) dest[i] = src[i];
	for (; i < n; i++) dest[i] = '\0';
	return dest;
}

char *strchr(const char *s, int c)
{
	while (*s) {
		if (*s == (char)c) return (char *)s;
		s++;
	}
	return (c == '\0') ? (char *)s : 0;
}

char *strstr(const char *haystack, const char *needle)
{
	if (!*needle) return (char *)haystack;
	size_t nl = strlen(needle);
	while (*haystack) {
		if (strncmp(haystack, needle, nl) == 0) return (char *)haystack;
		haystack++;
	}
	return 0;
}

char *strdup(const char *s)
{
	size_t len = strlen(s) + 1;
	char *p = malloc(len);
	if (p) memcpy(p, s, len);
	return p;
}

char *strcat(char *dest, const char *src)
{
	char *d = dest;
	while (*d) d++;
	while ((*d++ = *src++));
	return dest;
}

char *strrchr(const char *s, int c)
{
	const char *last = NULL;
	while (*s) {
		if (*s == (char)c) last = s;
		s++;
	}
	if (c == '\0') return (char *)s;
	return (char *)last;
}

char *strerror(int errnum)
{
	switch (errnum) {
		case 0: return "Success";
		case EPERM: return "Operation not permitted";
		case ENOENT: return "No such file or directory";
		case ESRCH: return "No such process";
		case EINTR: return "Interrupted system call";
		case EIO: return "I/O error";
		case ENXIO: return "No such device or address";
		case E2BIG: return "Argument list too long";
		case ENOEXEC: return "Exec format error";
		case EBADF: return "Bad file number";
		case ECHILD: return "No child processes";
		case EAGAIN: return "Try again";
		case ENOMEM: return "Out of memory";
		case EACCES: return "Permission denied";
		case EFAULT: return "Bad address";
		case ENOTBLK: return "Block device required";
		case EBUSY: return "Device or resource busy";
		case EEXIST: return "File exists";
		case EXDEV: return "Cross-device link";
		case ENODEV: return "No such device";
		case ENOTDIR: return "Not a directory";
		case EISDIR: return "Is a directory";
		case EINVAL: return "Invalid argument";
		case ENFILE: return "File table overflow";
		case EMFILE: return "Too many open files";
		case ENOTTY: return "Not a typewriter";
		case ETXTBSY: return "Text file busy";
		case EFBIG: return "File too large";
		case ENOSPC: return "No space left on device";
		case ESPIPE: return "Illegal seek";
		case EROFS: return "Read-only file system";
		case EMLINK: return "Too many links";
		case EPIPE: return "Broken pipe";
		case EDOM: return "Math argument out of domain of func";
		case ERANGE: return "Math result not representable";
		default: return "Unknown error";
	}
}

char *strpbrk(const char *s, const char *accept)
{
	while (*s) {
		const char *a = accept;
		while (*a) {
			if (*a == *s) return (char *)s;
			a++;
		}
		s++;
	}
	return NULL;
}



