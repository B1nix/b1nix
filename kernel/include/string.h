#ifndef TINYUNIX_STRING_H
#define TINYUNIX_STRING_H

#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t count);
void *memset(void *dest, int value, size_t count);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t n);
size_t strlen(const char *text);

#endif
