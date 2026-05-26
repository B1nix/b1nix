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
void perror(const char *s);
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

static inline void clearerr(FILE *stream) {
    if (stream) {
        stream->eof = 0;
        stream->error = 0;
    }
}

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef long fpos_t;

static inline int getc(FILE *stream) {
    return fgetc(stream);
}

static inline int putc(int c, FILE *stream) {
    return fputc(c, stream);
}

static inline int fgetpos(FILE *stream, fpos_t *pos) {
    long p = ftell(stream);
    if (p < 0) return -1;
    *pos = p;
    return 0;
}

static inline int fsetpos(FILE *stream, const fpos_t *pos) {
    return fseek(stream, *pos, SEEK_SET);
}

#include <syscall.h>
#include <errno.h>
int normalize_errno(long rc);

static inline int rename(const char *oldpath, const char *newpath) {
    int rc = (int)syscall(SYS_RENAME, oldpath, newpath);
    if (rc < 0) {
        errno = normalize_errno(rc);
        return -1;
    }
    return 0;
}

static inline int scanf(const char *format, ...) {
    (void)format;
    return 0;
}

static inline void setbuf(FILE *stream, char *buf) {
    (void)stream;
    (void)buf;
}

static inline int setvbuf(FILE *stream, char *buf, int mode, size_t size) {
    (void)stream;
    (void)buf;
    (void)mode;
    (void)size;
    return 0;
}

static inline int vprintf(const char *format, va_list ap) {
    return vfprintf(stdout, format, ap);
}

static inline char *fgets(char *s, int size, FILE *stream) {
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(stream);
        if (c == EOF) {
            if (i == 0) return NULL;
            break;
        }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

static inline int vsprintf(char *str, const char *format, va_list ap) {
    return vsnprintf(str, 0x7fffffff, format, ap);
}

// Simple sscanf wrapper suited for parsing integers/strings
static inline int sscanf(const char *str, const char *format, ...) {
    (void)format;
    va_list ap;
    va_start(ap, format);
    unsigned long long *val = va_arg(ap, unsigned long long *);
    *val = 0;
    // Skip leading spaces
    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r') str++;
    // Basic base-10 parser
    while (*str >= '0' && *str <= '9') {
        *val = (*val * 10) + (*str - '0');
        str++;
    }
    va_end(ap);
    return 1;
}

#define BUFSIZ 1024

static inline FILE *tmpfile(void) {
    return NULL;
}

static inline void rewind(FILE *stream) {
    fseek(stream, 0L, SEEK_SET);
}

static inline int fscanf(FILE *stream, const char *format, ...) {
    (void)format;
    va_list ap;
    va_start(ap, format);
    char *dummy = va_arg(ap, char *);
    int i = 0;
    while (1) {
        int c = fgetc(stream);
        if (c == EOF) {
            if (i == 0) {
                va_end(ap);
                return EOF;
            }
            break;
        }
        if (c == '\n' || c == ':') {
            ungetc(c, stream);
            break;
        }
        dummy[i++] = (char)c;
    }
    dummy[i] = '\0';
    va_end(ap);
    return 1;
}

static inline int getchar(void) {
    return fgetc(stdin);
}

#endif
