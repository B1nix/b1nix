#ifndef B1NIX_U_STDLIB_H
#define B1NIX_U_STDLIB_H

#include <stddef.h>

void  exit(int status) __attribute__((noreturn));
void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t nmemb, size_t size);
int   atoi(const char *s);

#endif
