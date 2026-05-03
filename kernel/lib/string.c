#include <string.h>

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
