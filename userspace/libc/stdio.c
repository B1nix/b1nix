#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "syscall.h"
#include <fcntl.h>
#include <unistd.h>

static FILE _stdin =  { 0, 0, 0 };
static FILE _stdout = { 1, 0, 0 };
static FILE _stderr = { 2, 0, 0 };

FILE *stdin = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;
int putchar(int c)
{
	char ch = (char)c;
	syscall(SYS_WRITE, (long)&ch, 1, 1, 0);
	return c;
}

int puts(const char *s)
{
	syscall(SYS_WRITE, (long)s, (long)strlen(s), 1, 0);
	putchar('\n');
	return 0;
}

static void print_dec(unsigned long v, char *buf, int *pos)
{
	if (v >= 10) print_dec(v / 10, buf, pos);
	buf[(*pos)++] = '0' + (v % 10);
}

static void print_hex(unsigned long v, char *buf, int *pos)
{
	const char *hex = "0123456789abcdef";
	if (v >= 16) print_hex(v / 16, buf, pos);
	buf[(*pos)++] = hex[v % 16];
}

int snprintf(char *str, size_t size, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	int pos = 0;
	for (int i = 0; fmt[i] && pos < (int)size - 1; i++) {
		if (fmt[i] != '%') {
			str[pos++] = fmt[i];
			continue;
		}
		i++;
		switch (fmt[i]) {
		case 'd': {
			int v = va_arg(args, int);
			if (v < 0) { str[pos++] = '-'; v = -v; }
			print_dec((unsigned long)v, str, &pos);
			break;
		}
		case 'u': {
			unsigned int v = va_arg(args, unsigned int);
			print_dec(v, str, &pos);
			break;
		}
		case 'x': case 'X': {
			unsigned int v = va_arg(args, unsigned int);
			print_hex(v, str, &pos);
			break;
		}
		case 's': {
			const char *s = va_arg(args, const char *);
			if (!s) s = "(null)";
			while (*s && pos < (int)size - 1) str[pos++] = *s++;
			break;
		}
		case 'c': {
			str[pos++] = (char)va_arg(args, int);
			break;
		}
		case 'l': {
			i++;
			switch (fmt[i]) {
			case 'd': {
				long v = va_arg(args, long);
				if (v < 0) { str[pos++] = '-'; v = -v; }
				print_dec((unsigned long)v, str, &pos);
				break;
			}
			case 'u': print_dec(va_arg(args, unsigned long), str, &pos); break;
			case 'x': case 'X': print_hex(va_arg(args, unsigned long), str, &pos); break;
			default: str[pos++] = '%'; str[pos++] = 'l'; str[pos++] = fmt[i]; break;
			}
			break;
		}
		default:
			str[pos++] = '%';
			str[pos++] = fmt[i];
			break;
		}
	}
	str[pos] = '\0';
	va_end(args);
	return pos;
}

int printf(const char *fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	int n = snprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	syscall(SYS_WRITE, (long)buf, (long)n, 1, 0);
	return n;
}

FILE *fopen(const char *pathname, const char *mode)
{
    int flags = 0;
    if (mode[0] == 'r') {
        flags = O_RDONLY;
        if (mode[1] == '+') flags = O_RDWR;
    } else if (mode[0] == 'w') {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        if (mode[1] == '+') flags = O_RDWR | O_CREAT | O_TRUNC;
    } else if (mode[0] == 'a') {
        flags = O_WRONLY | O_CREAT | O_APPEND;
        if (mode[1] == '+') flags = O_RDWR | O_CREAT | O_APPEND;
    }

    int fd = open(pathname, flags, 0666);
    if (fd < 0) return NULL;

    FILE *f = malloc(sizeof(FILE));
    if (!f) {
        close(fd);
        return NULL;
    }
    f->fd = fd;
    f->eof = 0;
    f->error = 0;
    return f;
}

FILE *freopen(const char *pathname, const char *mode, FILE *stream)
{
    if (stream) fclose(stream);
    FILE *f = fopen(pathname, mode);
    if (!f) return NULL;
    if (stream) {
        stream->fd = f->fd;
        stream->eof = f->eof;
        stream->error = f->error;
        free(f);
        return stream;
    }
    return f;
}

FILE *fdopen(int fd, const char *mode)
{
    (void)mode;
    if (fd < 0) return NULL;
    FILE *f = malloc(sizeof(FILE));
    if (!f) return NULL;
    f->fd = fd;
    f->eof = 0;
    f->error = 0;
    return f;
}

int fclose(FILE *stream)
{
    if (!stream) return -1;
    int res = close(stream->fd);
    if (stream != stdin && stream != stdout && stream != stderr) {
        free(stream);
    }
    return res;
}

int fputs(const char *s, FILE *stream)
{
    if (!stream) return EOF;
    int len = strlen(s);
    if (len == 0) return 0;
    int n = write(stream->fd, s, len);
    if (n < 0) {
        stream->error = 1;
        return EOF;
    }
    return n;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    if (!stream || size == 0 || nmemb == 0) return 0;
    int n = read(stream->fd, ptr, size * nmemb);
    if (n < 0) {
        stream->error = 1;
        return 0;
    }
    if (n == 0) {
        stream->eof = 1;
        return 0;
    }
    return n / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    if (!stream || size == 0 || nmemb == 0) return 0;
    int n = write(stream->fd, ptr, size * nmemb);
    if (n < 0) {
        stream->error = 1;
        return 0;
    }
    return n / size;
}

int fgetc(FILE *stream)
{
    unsigned char c;
    if (fread(&c, 1, 1, stream) != 1) return EOF;
    return c;
}

int fputc(int c, FILE *stream)
{
    unsigned char ch = (unsigned char)c;
    if (fwrite(&ch, 1, 1, stream) != 1) return EOF;
    return c;
}

int ungetc(int c, FILE *stream)
{
    // Extremely basic stub for ungetc. Real one needs a buffer.
    // For now we just seek back by 1 if possible.
    if (c == EOF || !stream) return EOF;
    long pos = syscall(SYS_LSEEK, stream->fd, -1, SEEK_CUR, 0);
    if (pos < 0) return EOF;
    stream->eof = 0;
    return c;
}

int fseek(FILE *stream, long offset, int whence)
{
    if (!stream) return -1;
    long pos = syscall(SYS_LSEEK, stream->fd, offset, whence, 0);
    if (pos < 0) return -1;
    stream->eof = 0;
    return 0;
}

long ftell(FILE *stream)
{
    if (!stream) return -1;
    return syscall(SYS_LSEEK, stream->fd, 0, SEEK_CUR, 0);
}

int fflush(FILE *stream)
{
    // No buffering yet, so fflush is a no-op
    (void)stream;
    return 0;
}

int feof(FILE *stream)
{
    return stream ? stream->eof : 1;
}

int ferror(FILE *stream)
{
    return stream ? stream->error : 1;
}

int fileno(FILE *stream)
{
    return stream ? stream->fd : -1;
}

int remove(const char *pathname)
{
    return syscall(SYS_UNLINK, (long)pathname, 0, 0, 0);
}

int fprintf(FILE *stream, const char *fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (stream && n > 0) {
        write(stream->fd, buf, n);
    }
    return n;
}

int vfprintf(FILE *stream, const char *fmt, va_list ap)
{
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (stream && n > 0) {
        write(stream->fd, buf, n);
    }
    return n;
}

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap)
{
    // Minimal vsnprintf wrapping existing snprintf logic
    // B1NIX minimal libc snprintf already has va_list logic in it, let's reuse it.
    // Wait, snprintf in B1NIX libc uses va_start directly.
    // So we just copy the snprintf logic here.
    
    int pos = 0;
    for (int i = 0; fmt[i] && pos < (int)size - 1; i++) {
        if (fmt[i] != '%') {
            str[pos++] = fmt[i];
            continue;
        }
        i++;
        // ... (we'll implement basic %s %d logic) ...
        // For simplicity, just handling %s and %d in this stub.
        if (fmt[i] == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && pos < (int)size - 1) str[pos++] = *s++;
        } else if (fmt[i] == 'd') {
            int v = va_arg(ap, int);
            char num[32];
            int np = 0;
            if (v < 0) { str[pos++] = '-'; v = -v; }
            do { num[np++] = '0' + (v % 10); v /= 10; } while (v > 0);
            while (np > 0 && pos < (int)size - 1) str[pos++] = num[--np];
        } else if (fmt[i] == 'c') {
            str[pos++] = (char)va_arg(ap, int);
        } else {
            str[pos++] = '%';
            str[pos++] = fmt[i];
        }
    }
    str[pos] = '\0';
    return pos;
}

int sprintf(char *str, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(str, 0x7fffffff, fmt, args);
    va_end(args);
    return n;
}


