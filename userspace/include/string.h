#ifndef B1NIX_U_STRING_H
#define B1NIX_U_STRING_H

#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t count);
void *memset(void *dest, int value, size_t count);
void *memmove(void *dest, const void *src, size_t count);
int   memcmp(const void *p1, const void *p2, size_t count);
int   strcmp(const char *a, const char *b);
int   strcoll(const char *a, const char *b);
int   strncmp(const char *a, const char *b, size_t n);
size_t strlen(const char *s);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
char *strchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
char *strdup(const char *s);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);
char *strrchr(const char *s, int c);
char *strpbrk(const char *s, const char *accept);
char *strerror(int errnum);

static inline size_t strcspn(const char *s, const char *reject) {
    const char *p = s;
    while (*p) {
        const char *r = reject;
        while (*r) {
            if (*p == *r) return p - s;
            r++;
        }
        p++;
    }
    return p - s;
}

static inline size_t strspn(const char *s, const char *accept) {
    const char *p = s;
    while (*p) {
        const char *a = accept;
        while (*a) {
            if (*p == *a) break;
            a++;
        }
        if (!*a) return p - s;
        p++;
    }
    return p - s;
}

#endif
