#include <string.h>
#include <b1nix/mm.h>

void *memcpy(void *dest, const void *src, size_t count)
{
	unsigned char *d = dest;
	const unsigned char *s = src;

	for (size_t i = 0; i < count; i++) {
		d[i] = s[i];
	}

	return dest;
}

void *memset(void *dest, int value, size_t count)
{
	unsigned char *d = dest;

	for (size_t i = 0; i < count; i++) {
		d[i] = (unsigned char)value;
	}

	return dest;
}

void *memmove(void *dest, const void *src, size_t count)
{
	unsigned char *d = dest;
	const unsigned char *s = src;

	if (d < s) {
		for (size_t i = 0; i < count; i++) {
			d[i] = s[i];
		}
	} else if (d > s) {
		for (size_t i = count; i > 0; i--) {
			d[i - 1] = s[i - 1];
		}
	}

	return dest;
}

int memcmp(const void *ptr1, const void *ptr2, size_t count)
{
	const unsigned char *p1 = ptr1;
	const unsigned char *p2 = ptr2;
	for (size_t i = 0; i < count; i++) {
		if (p1[i] < p2[i]) return -1;
		if (p1[i] > p2[i]) return 1;
	}
	return 0;
}

int strcmp(const char *left, const char *right)
{
	while (*left != '\0' && *left == *right) {
		left++;
		right++;
	}

	return (unsigned char)*left - (unsigned char)*right;
}

int strncmp(const char *left, const char *right, size_t n)
{
	while (n > 0 && *left != '\0' && *left == *right) {
		left++;
		right++;
		n--;
	}

	if (n == 0) {
		return 0;
	}

	return (unsigned char)*left - (unsigned char)*right;
}

size_t strlen(const char *text)
{
	size_t length = 0;

	while (text[length] != '\0') {
		length++;
	}

	return length;
}

char *strcpy(char *dest, const char *src)
{
	char *d = dest;
	while ((*d++ = *src++) != '\0');
	return dest;
}

char *strncpy(char *dest, const char *src, size_t n)
{
	size_t i;
	for (i = 0; i < n && src[i] != '\0'; i++) {
		dest[i] = src[i];
	}
	for (; i < n; i++) {
		dest[i] = '\0';
	}
	return dest;
}

char *strcat(char *dest, const char *src)
{
	char *d = dest;
	while (*d) d++;
	while ((*d++ = *src++) != '\0');
	return dest;
}

char *strchr(const char *s, int c)
{
	while (*s != '\0') {
		if (*s == (char)c) return (char *)s;
		s++;
	}
	return (c == '\0') ? (char *)s : 0;
}

char *strrchr(const char *s, int c)
{
	const char *last = 0;
	while (*s != '\0') {
		if (*s == (char)c) last = s;
		s++;
	}
	if (c == '\0') return (char *)s;
	return (char *)last;
}

char *strdup(const char *s)
{
	size_t len = strlen(s) + 1;
	char *new_s = kmalloc(len);
	if (new_s) {
		memcpy(new_s, s, len);
	}
	return new_s;
}

char *strtok(char *str, const char *delim)
{
	static char *last = 0;
	
	if (str) last = str;
	if (!last || *last == '\0') return 0;
	
	/* Skip leading delimiters */
	while (*last && strchr(delim, *last)) last++;
	if (*last == '\0') return 0;
	
	char *token_start = last;
	
	/* Find end of token */
	while (*last && !strchr(delim, *last)) last++;
	
	if (*last) {
		*last = '\0';
		last++;
	} else {
		last = 0;
	}
	
	return token_start;
}
