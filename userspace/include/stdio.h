#ifndef B1NIX_U_STDIO_H
#define B1NIX_U_STDIO_H

#include <stddef.h>
#include <stdarg.h>

int printf(const char *fmt, ...);
int putchar(int c);
int puts(const char *s);
int snprintf(char *buf, size_t size, const char *fmt, ...);

#endif
