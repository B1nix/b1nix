#ifndef B1NIX_U_STDLIB_H
#define B1NIX_U_STDLIB_H

#include <stddef.h>

void  exit(int status) __attribute__((noreturn));
void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
int   atoi(const char *s);
long  strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
double strtod(const char *nptr, char **endptr);
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));
char *getenv(const char *name);
char *realpath(const char *path, char *resolved_path);

#endif
