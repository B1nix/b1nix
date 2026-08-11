#ifndef B1NIX_STRING_H
#define B1NIX_STRING_H

#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t count);
void *memset(void *dest, int value, size_t count);
void *memmove(void *dest, const void *src, size_t count);
int memcmp(const void *ptr1, const void *ptr2, size_t count);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t n);
size_t strlen(const char *text);

/* Additional string functions */
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strdup(const char *s);
char *strtok(char *str, const char *delim);
char *strstr(const char *haystack, const char *needle);


size_t strnlen(const char *s, size_t maxlen);
size_t strlcat(char *dst, const char *src, size_t size);

#endif
