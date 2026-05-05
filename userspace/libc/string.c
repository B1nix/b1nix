#include <string.h>
#include <stdlib.h>
#include "syscall.h"

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
