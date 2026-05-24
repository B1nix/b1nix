#include "syscall.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static FILE _stdin = {0, 0, 0, 0, 0};
static FILE _stdout = {1, 0, 0, 0, 0};
static FILE _stderr = {2, 0, 0, 0, 0};

FILE *stdin = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;
int putchar(int c) {
  char ch = (char)c;
  write(stdout->fd, &ch, 1);
  return c;
}

int puts(const char *s) {
  write(stdout->fd, s, strlen(s));
  putchar('\n');
  return 0;
}

void perror(const char *s) {
  if (s && *s) {
    printf("%s: error\n", s);
  } else {
    printf("error\n");
  }
}

static void print_dec(unsigned long v, char *buf, int *pos) {
  if (v >= 10)
    print_dec(v / 10, buf, pos);
  buf[(*pos)++] = '0' + (v % 10);
}

static void print_hex(unsigned long v, char *buf, int *pos) {
  const char *hex = "0123456789abcdef";
  if (v >= 16)
    print_hex(v / 16, buf, pos);
  buf[(*pos)++] = hex[v % 16];
}

int snprintf(char *str, size_t size, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(str, size, fmt, args);
  va_end(args);
  return n;
}

int printf(const char *fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  write(stdout->fd, buf, n);
  return n;
}

FILE *fopen(const char *pathname, const char *mode) {
  int flags = 0;
  if (mode[0] == 'r') {
    flags = O_RDONLY;
    if (mode[1] == '+')
      flags = O_RDWR;
  } else if (mode[0] == 'w') {
    flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (mode[1] == '+')
      flags = O_RDWR | O_CREAT | O_TRUNC;
  } else if (mode[0] == 'a') {
    flags = O_WRONLY | O_CREAT | O_APPEND;
    if (mode[1] == '+')
      flags = O_RDWR | O_CREAT | O_APPEND;
  }

  int fd = open(pathname, flags, 0666);
  if (fd < 0)
    return NULL;

  FILE *f = malloc(sizeof(FILE));
  if (!f) {
    close(fd);
    return NULL;
  }
  f->fd = fd;
  f->eof = 0;
  f->error = 0;
  f->unget_buf = 0;
  f->has_unget = 0;
  return f;
}

FILE *freopen(const char *pathname, const char *mode, FILE *stream) {
  if (stream)
    fclose(stream);
  FILE *f = fopen(pathname, mode);
  if (!f)
    return NULL;
  if (stream) {
    stream->fd = f->fd;
    stream->eof = f->eof;
    stream->error = f->error;
    free(f);
    return stream;
  }
  return f;
}

FILE *fdopen(int fd, const char *mode) {
  (void)mode;
  if (fd < 0)
    return NULL;
  FILE *f = malloc(sizeof(FILE));
  if (!f)
    return NULL;
  f->fd = fd;
  f->eof = 0;
  f->error = 0;
  return f;
}

int fclose(FILE *stream) {
  if (!stream)
    return -1;
  int res = close(stream->fd);
  if (stream != stdin && stream != stdout && stream != stderr) {
    free(stream);
  }
  return res;
}

int fputs(const char *s, FILE *stream) {
  if (!stream)
    return EOF;
  int len = strlen(s);
  if (len == 0)
    return 0;
  int n = write(stream->fd, s, len);
  if (n < 0) {
    stream->error = 1;
    return EOF;
  }
  return n;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
  if (!stream || size == 0 || nmemb == 0)
    return 0;

  size_t total_requested = size * nmemb;
  size_t done = 0;
  unsigned char *p = (unsigned char *)ptr;

  if (stream->has_unget) {
    *p++ = (unsigned char)stream->unget_buf;
    stream->has_unget = 0;
    done++;
  }

  if (done < total_requested) {
    int n = read(stream->fd, p, total_requested - done);
    if (n < 0) {
      stream->error = 1;
      return done / size;
    }
    if (n == 0) {
      stream->eof = 1;
      return done / size;
    }
    done += n;
  }

  return done / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
  if (!stream || size == 0 || nmemb == 0)
    return 0;
  int n = write(stream->fd, ptr, size * nmemb);
  if (n < 0) {
    stream->error = 1;
    return 0;
  }
  return n / size;
}

int fgetc(FILE *stream) {
  unsigned char c;
  if (fread(&c, 1, 1, stream) != 1)
    return EOF;
  return c;
}

int fputc(int c, FILE *stream) {
  unsigned char ch = (unsigned char)c;
  if (fwrite(&ch, 1, 1, stream) != 1)
    return EOF;
  return c;
}

int ungetc(int c, FILE *stream) {
  if (c == EOF || !stream)
    return EOF;
  stream->unget_buf = c;
  stream->has_unget = 1;
  stream->eof = 0;
  return c;
}

int fseek(FILE *stream, long offset, int whence) {
  if (!stream)
    return -1;
  long pos = lseek(stream->fd, offset, whence);
  if (pos < 0)
    return -1;
  stream->eof = 0;
  return 0;
}

long ftell(FILE *stream) {
  if (!stream)
    return -1;
  return lseek(stream->fd, 0, SEEK_CUR);
}

int fflush(FILE *stream) {
  // No buffering yet, so fflush is a no-op
  (void)stream;
  return 0;
}

int feof(FILE *stream) { return stream ? stream->eof : 1; }

int ferror(FILE *stream) { return stream ? stream->error : 1; }

int fileno(FILE *stream) { return stream ? stream->fd : -1; }

int remove(const char *pathname) {
  return unlink(pathname);
}

int fprintf(FILE *stream, const char *fmt, ...) {
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

int vfprintf(FILE *stream, const char *fmt, va_list ap) {
  char buf[512];
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  if (stream && n > 0) {
    write(stream->fd, buf, n);
  }
  return n;
}

static void _vsnprintf_putc(char *str, size_t size, int *pos, char c) {
  if (*pos < (int)size - 1) {
    str[(*pos)++] = c;
  }
}

static void _vsnprintf_puts(char *str, size_t size, int *pos, const char *s) {
  while (*s)
    _vsnprintf_putc(str, size, pos, *s++);
}

static void _vsnprintf_putd(char *str, size_t size, int *pos, long v, int base,
                            int signed_val) {
  char buf[32];
  int p = 0;
  unsigned long uv = (unsigned long)v;
  if (signed_val && v < 0) {
    _vsnprintf_putc(str, size, pos, '-');
    uv = (unsigned long)-v;
  }
  const char *digits = "0123456789abcdef";
  do {
    buf[p++] = digits[uv % base];
    uv /= base;
  } while (uv > 0);
  while (p > 0)
    _vsnprintf_putc(str, size, pos, buf[--p]);
}

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap) {
  int pos = 0;
  if (size == 0)
    return 0;

  for (int i = 0; fmt[i] && pos < (int)size - 1; i++) {
    if (fmt[i] != '%') {
      str[pos++] = fmt[i];
      continue;
    }
    i++;
    if (fmt[i] == '0')
      i++; // Ignore padding for now

    int is_long = 0;
    if (fmt[i] == 'l') {
      is_long = 1;
      i++;
    }

    switch (fmt[i]) {
    case 's':
      _vsnprintf_puts(str, size, &pos, va_arg(ap, const char *));
      break;
    case 'd':
      _vsnprintf_putd(str, size, &pos,
                      is_long ? va_arg(ap, long) : va_arg(ap, int), 10, 1);
      break;
    case 'u':
      _vsnprintf_putd(str, size, &pos,
                      is_long ? va_arg(ap, unsigned long)
                              : va_arg(ap, unsigned int),
                      10, 0);
      break;
    case 'x':
    case 'X':
      _vsnprintf_putd(str, size, &pos,
                      is_long ? va_arg(ap, unsigned long)
                              : va_arg(ap, unsigned int),
                      16, 0);
      break;
    case 'p':
      _vsnprintf_puts(str, size, &pos, "0x");
      _vsnprintf_putd(str, size, &pos, (unsigned long)va_arg(ap, void *), 16,
                      0);
      break;
    case 'c':
      _vsnprintf_putc(str, size, &pos, (char)va_arg(ap, int));
      break;
    case '%':
      _vsnprintf_putc(str, size, &pos, '%');
      break;
    default:
      _vsnprintf_putc(str, size, &pos, '%');
      _vsnprintf_putc(str, size, &pos, fmt[i]);
      break;
    }
  }
  str[pos] = '\0';
  return pos;
}

int sprintf(char *str, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(str, 0x7fffffff, fmt, args);
  va_end(args);
  return n;
}
