#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>

void abort(void);
void exit(int status);
void *malloc(size_t size);
void free(void *ptr);

int atoi(const char *s);

#endif
