#ifndef B1NIX_U_STDIO_H
#define B1NIX_U_STDIO_H

#include <stddef.h>
#include <stdarg.h>

#define EOF (-1)

typedef struct {
    int fd;
    int eof;
    int error;
    int unget_buf;
    int has_unget;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int printf(const char *fmt, ...);
int sprintf(char *str, const char *fmt, ...);
int putchar(int c);
int puts(const char *s);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int fprintf(FILE *stream, const char *fmt, ...);
int vfprintf(FILE *stream, const char *fmt, va_list ap);
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);

FILE *fopen(const char *pathname, const char *mode);
FILE *freopen(const char *pathname, const char *mode, FILE *stream);
FILE *fdopen(int fd, const char *mode);
int fclose(FILE *stream);
int fputs(const char *s, FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fgetc(FILE *stream);
int fputc(int c, FILE *stream);
int ungetc(int c, FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int fflush(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
int fileno(FILE *stream);
int remove(const char *pathname);

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#endif
