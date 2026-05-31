#ifndef STDIO_H
#define STDIO_H

#include <stddef.h>
#include <stdarg.h>

int printf(const char *fmt, ...);
int putchar(int c);
int puts(const char *s);
int snprintf(char *str, size_t size, const char *fmt, ...);
int vsnprintf(char *str, size_t size, const char *fmt, va_list args);

#endif
