#include "syscall.h"
#include <ctype.h>
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

int vprintf(const char *fmt, va_list ap) {
  return vfprintf(stdout, fmt, ap);
}

static void _vsnprintf_putc(char *str, size_t size, int *pos, char c) {
  /* C99: count every character (so the return value is the would-be length),
   * but only write into the buffer when there is room (reserving the final
   * byte for the terminating NUL). str may be NULL when size == 0. */
  if (str && (size_t)(*pos + 1) < size)
    str[*pos] = c;
  (*pos)++;
}

static void _vsnprintf_puts(char *str, size_t size, int *pos, const char *s) {
  if (!s) s = "(null)";
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
  /* Format the whole string regardless of buffer size so the return value is
   * the C99 "would-be" length; _vsnprintf_putc only writes what fits. Callers
   * such as GCC's build_attr_access_from_parms rely on snprintf(NULL, 0, ...)
   * returning the full length to size their buffers. */
  for (int i = 0; fmt[i]; i++) {
    if (fmt[i] != '%') {
      _vsnprintf_putc(str, size, &pos, fmt[i]);
      continue;
    }
    i++;

    /* Flags. */
    while (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == ' ' || fmt[i] == '#' ||
           fmt[i] == '0')
      i++;
    /* Field width (parsed but not applied — values are still correct, which is
     * all the assembler output from cc1 needs). */
    while (fmt[i] >= '0' && fmt[i] <= '9')
      i++;
    /* Precision. */
    if (fmt[i] == '.') {
      i++;
      while (fmt[i] >= '0' && fmt[i] <= '9')
        i++;
    }

    /* Length modifiers. On x86_64 long, long long, size_t, intmax_t and
     * ptrdiff_t are all 64-bit, so they collapse to a single "is 64-bit" flag.
     * Handling "ll" (and z/j/t) is what GCC's HOST_WIDE_INT_PRINT ("%lld")
     * needs — without it the native cc1 emitted literal "%ld" into its asm. */
    int is_long = 0;
    while (fmt[i] == 'l') {
      is_long = 1;
      i++;
    }
    if (fmt[i] == 'z' || fmt[i] == 'j' || fmt[i] == 't' || fmt[i] == 'L') {
      is_long = 1;
      i++;
    }
    if (fmt[i] == 'h') { /* h / hh: still passed as int via default promotion */
      i++;
      if (fmt[i] == 'h')
        i++;
    }

    switch (fmt[i]) {
    case 's':
      _vsnprintf_puts(str, size, &pos, va_arg(ap, const char *));
      break;
    case 'd':
    case 'i':
      _vsnprintf_putd(str, size, &pos,
                      is_long ? va_arg(ap, long) : (long)va_arg(ap, int), 10, 1);
      break;
    case 'u':
      _vsnprintf_putd(str, size, &pos,
                      is_long ? (long)va_arg(ap, unsigned long)
                              : (long)va_arg(ap, unsigned int),
                      10, 0);
      break;
    case 'x':
    case 'X':
      _vsnprintf_putd(str, size, &pos,
                      is_long ? (long)va_arg(ap, unsigned long)
                              : (long)va_arg(ap, unsigned int),
                      16, 0);
      break;
    case 'o':
      _vsnprintf_putd(str, size, &pos,
                      is_long ? (long)va_arg(ap, unsigned long)
                              : (long)va_arg(ap, unsigned int),
                      8, 0);
      break;
    case 'p':
      _vsnprintf_puts(str, size, &pos, "0x");
      _vsnprintf_putd(str, size, &pos, (long)(unsigned long)va_arg(ap, void *),
                      16, 0);
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
  /* NUL-terminate within the buffer (truncating if the output didn't fit). */
  if (str && size > 0)
    str[(size_t)pos < size ? pos : (int)size - 1] = '\0';
  return pos;
}

int sprintf(char *str, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(str, 0x7fffffff, fmt, args);
  va_end(args);
  return n;
}

/* ── scanf family ────────────────────────────────────────────────────────────
 * A single character-source-driven engine backs scanf/fscanf/sscanf. The source
 * is either a FILE* (stream) or a NUL-terminated string. It supports the common
 * conversions used by real programs: %d %i %u %o %x %X %c %s %f/%e/%g %p %n %%,
 * with optional assignment-suppression (*), a maximum field width, and length
 * modifiers (l/ll/h/hh/z/j/t/L). Whitespace in the format skips run of input
 * whitespace; a literal format character must match the input. */

struct _scan_src {
  FILE *fp;        /* stream source, or NULL */
  const char *str; /* string source, or NULL */
  int pos;         /* index into str */
  int pushed;      /* one-char pushback, or EOF when empty */
  int nread;       /* total characters consumed from the source */
};

static int _sc_get(struct _scan_src *s) {
  int c;
  if (s->pushed != EOF) {
    c = s->pushed;
    s->pushed = EOF;
  } else if (s->fp) {
    c = fgetc(s->fp);
  } else {
    c = (unsigned char)s->str[s->pos];
    if (c == 0)
      c = EOF;
    else
      s->pos++;
  }
  if (c != EOF)
    s->nread++;
  return c;
}

static void _sc_unget(struct _scan_src *s, int c) {
  if (c == EOF)
    return;
  s->pushed = c;
  s->nread--;
}

static int _sc_digit(int c, int base) {
  int v;
  if (c >= '0' && c <= '9') v = c - '0';
  else if (c >= 'a' && c <= 'z') v = c - 'a' + 10;
  else if (c >= 'A' && c <= 'Z') v = c - 'A' + 10;
  else return -1;
  return v < base ? v : -1;
}

static int _scan_int(struct _scan_src *s, int base, int width, int is_unsigned,
                     long long *out) {
  int c;
  do { c = _sc_get(s); } while (c != EOF && isspace(c));
  int neg = 0, consumed = 0;
  if (c == '+' || c == '-') {
    neg = (c == '-');
    consumed++;
    if (width && consumed >= width) { _sc_unget(s, c); return 0; }
    c = _sc_get(s);
  }
  /* Base autodetect for %i and 0x/0 prefixes. */
  if ((base == 0 || base == 16) && c == '0') {
    int c2 = _sc_get(s);
    if (c2 == 'x' || c2 == 'X') {
      base = 16;
      consumed += 2;
      c = _sc_get(s);
    } else {
      /* leading zero counts as a digit; keep it */
      if (base == 0) base = 8;
      _sc_unget(s, c2);
    }
  }
  if (base == 0) base = 10;
  long long acc = 0;
  int any = 0;
  while (c != EOF) {
    int d = _sc_digit(c, base);
    if (d < 0) break;
    acc = acc * base + d;
    any = 1;
    consumed++;
    if (width && consumed >= width) { c = EOF; break; }
    c = _sc_get(s);
  }
  if (c != EOF) _sc_unget(s, c);
  if (!any) return 0;
  *out = is_unsigned ? (long long)(unsigned long long)acc : (neg ? -acc : acc);
  return 1;
}

static int _scan_float(struct _scan_src *s, int width, double *out) {
  char buf[64];
  int n = 0, c;
  do { c = _sc_get(s); } while (c != EOF && isspace(c));
  while (c != EOF && n < (int)sizeof(buf) - 1 && (width == 0 || n < width)) {
    if (isdigit(c) || c == '.' || c == '+' || c == '-' ||
        c == 'e' || c == 'E') {
      buf[n++] = (char)c;
      c = _sc_get(s);
    } else {
      break;
    }
  }
  if (c != EOF) _sc_unget(s, c);
  buf[n] = '\0';
  if (n == 0) return 0;
  char *end = buf;
  double v = strtod(buf, &end);
  if (end == buf) return 0;
  *out = v;
  return 1;
}

static int _vscan(struct _scan_src *s, const char *fmt, va_list ap) {
  int assigned = 0;
  int saw_input = 0;
  for (const char *f = fmt; *f; f++) {
    if (isspace((unsigned char)*f)) {
      int c;
      do { c = _sc_get(s); } while (c != EOF && isspace(c));
      if (c != EOF) _sc_unget(s, c); else saw_input |= 0;
      continue;
    }
    if (*f != '%') {
      int c = _sc_get(s);
      if (c == EOF) return assigned ? assigned : EOF;
      saw_input = 1;
      if (c != (unsigned char)*f) { _sc_unget(s, c); return assigned; }
      continue;
    }
    /* conversion specifier */
    f++;
    int suppress = 0;
    if (*f == '*') { suppress = 1; f++; }
    int width = 0;
    while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); f++; }
    int lng = 0;
    while (*f == 'l') { lng++; f++; }
    if (*f == 'h') { f++; if (*f == 'h') f++; }
    if (*f == 'z' || *f == 'j' || *f == 't' || *f == 'L') { lng = 1; f++; }
    char conv = *f;
    if (conv == '\0') break;

    if (conv == '%') {
      int c = _sc_get(s);
      if (c == EOF) return assigned ? assigned : EOF;
      if (c != '%') { _sc_unget(s, c); return assigned; }
      continue;
    }
    if (conv == 'n') {
      if (!suppress) { int *p = va_arg(ap, int *); *p = s->nread; }
      continue;
    }
    if (conv == 'c') {
      int w = width ? width : 1;
      char *p = suppress ? 0 : va_arg(ap, char *);
      int got = 0;
      for (int k = 0; k < w; k++) {
        int c = _sc_get(s);
        if (c == EOF) break;
        saw_input = 1;
        if (p) p[k] = (char)c;
        got++;
      }
      if (got == 0) return assigned ? assigned : EOF;
      if (!suppress) assigned++;
      continue;
    }
    if (conv == 's') {
      int c;
      do { c = _sc_get(s); } while (c != EOF && isspace(c));
      if (c == EOF) return assigned ? assigned : EOF;
      saw_input = 1;
      char *p = suppress ? 0 : va_arg(ap, char *);
      int n = 0;
      while (c != EOF && !isspace(c) && (width == 0 || n < width)) {
        if (p) p[n] = (char)c;
        n++;
        c = _sc_get(s);
      }
      if (c != EOF) _sc_unget(s, c);
      if (p) p[n] = '\0';
      if (!suppress) assigned++;
      continue;
    }
    if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'o' ||
        conv == 'x' || conv == 'X' || conv == 'p') {
      int base = (conv == 'd' || conv == 'u') ? 10
               : (conv == 'o') ? 8
               : (conv == 'i') ? 0 : 16;
      int is_uns = (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o' ||
                    conv == 'p');
      long long v = 0;
      int before = s->nread;
      int r = _scan_int(s, base, width, is_uns, &v);
      if (s->nread != before) saw_input = 1;
      if (!r) {
        if (!saw_input) return assigned ? assigned : EOF;
        return assigned;
      }
      if (!suppress) {
        if (conv == 'p') { *va_arg(ap, void **) = (void *)(unsigned long)v; }
        else if (lng >= 2) { *va_arg(ap, long long *) = v; }
        else if (lng == 1) { *va_arg(ap, long *) = (long)v; }
        else if (is_uns) { *va_arg(ap, unsigned int *) = (unsigned int)v; }
        else { *va_arg(ap, int *) = (int)v; }
        assigned++;
      }
      continue;
    }
    if (conv == 'f' || conv == 'e' || conv == 'E' || conv == 'g' ||
        conv == 'G' || conv == 'a' || conv == 'A') {
      double v = 0;
      int before = s->nread;
      int r = _scan_float(s, width, &v);
      if (s->nread != before) saw_input = 1;
      if (!r) {
        if (!saw_input) return assigned ? assigned : EOF;
        return assigned;
      }
      if (!suppress) {
        if (lng) *va_arg(ap, double *) = v;
        else *va_arg(ap, float *) = (float)v;
        assigned++;
      }
      continue;
    }
    /* Unknown conversion: stop. */
    return assigned;
  }
  return assigned;
}

int vsscanf(const char *str, const char *fmt, va_list ap) {
  struct _scan_src s = {0, str, 0, EOF, 0};
  return _vscan(&s, fmt, ap);
}

int vfscanf(FILE *stream, const char *fmt, va_list ap) {
  struct _scan_src s = {stream, 0, 0, EOF, 0};
  return _vscan(&s, fmt, ap);
}

int sscanf(const char *str, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsscanf(str, fmt, ap);
  va_end(ap);
  return r;
}

int fscanf(FILE *stream, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vfscanf(stream, fmt, ap);
  va_end(ap);
  return r;
}

int scanf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vfscanf(stdin, fmt, ap);
  va_end(ap);
  return r;
}

int vscanf(const char *fmt, va_list ap) { return vfscanf(stdin, fmt, ap); }

FILE *tmpfile(void) {
  /* Best-effort: create a uniquely-named regular file under /tmp opened for
   * update. b1nix has no anonymous-inode / O_TMPFILE path, so (unlike POSIX)
   * the file is not auto-removed on close — callers that care unlink it. */
  char name[L_tmpnam];
  if (!tmpnam(name))
    return NULL;
  return fopen(name, "w+");
}
