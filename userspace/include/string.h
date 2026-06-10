#ifndef B1NIX_U_STRING_H
#define B1NIX_U_STRING_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t count);
void *mempcpy(void *dest, const void *src, size_t count);
void *memset(void *dest, int value, size_t count);
void *memmove(void *dest, const void *src, size_t count);
int   memcmp(const void *p1, const void *p2, size_t count);
int   strcmp(const char *a, const char *b);
int   strcoll(const char *a, const char *b);
int   strverscmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, size_t n);
int   strcasecmp(const char *a, const char *b);
int   strncasecmp(const char *a, const char *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);
char *strcpy(char *dest, const char *src);
char *stpcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
char *stpncpy(char *dest, const char *src, size_t n);
char *strchr(const char *s, int c);
char *strchrnul(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);
char *strrchr(const char *s, int c);
char *strpbrk(const char *s, const char *accept);
char *strerror(int errnum);

void *memchr(const void *s, int c, size_t n);
char *strtok(char *str, const char *delim);
char *strtok_r(char *str, const char *delim, char **saveptr);
char *strsep(char **stringp, const char *delim);
char *strcasestr(const char *haystack, const char *needle);
size_t strxfrm(char *dest, const char *src, size_t n);
size_t strcspn(const char *s, const char *reject);
size_t strspn(const char *s, const char *accept);

#ifdef __cplusplus
}
#endif

#endif
