#include "syscall.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static FILE _stdin = {.fd = 0};
static FILE _stdout = {.fd = 1};
static FILE _stderr = {.fd = 2};

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

  FILE *f = calloc(1, sizeof(FILE)); /* zero ms_* and all fields */
  if (!f) {
    close(fd);
    return NULL;
  }
  f->fd = fd;
  f->eof = 0;
  f->error = 0;
  f->unget_buf = 0;
  f->has_unget = 0;
  f->pipe_pid = 0;
  return f;
}

FILE *freopen(const char *pathname, const char *mode, FILE *stream) {
  if (!stream)
    return fopen(pathname, mode);
  close(stream->fd);
  FILE *f = fopen(pathname, mode);
  if (!f) {
    stream->fd = -1;
    stream->eof = 0;
    stream->error = 1;
    stream->has_unget = 0;
    stream->pipe_pid = 0;
    return NULL;
  }
  *stream = *f;
  free(f);
  return stream;
}

FILE *fdopen(int fd, const char *mode) {
  (void)mode;
  if (fd < 0)
    return NULL;
  FILE *f = calloc(1, sizeof(FILE)); /* zero ms_* and all fields */
  if (!f)
    return NULL;
  f->fd = fd;
  f->eof = 0;
  f->error = 0;
  f->unget_buf = 0;
  f->has_unget = 0;
  f->pipe_pid = 0;
  return f;
}

FILE *popen(const char *command, const char *mode) {
  if (!command || !mode || (mode[0] != 'r' && mode[0] != 'w')) {
    errno = EINVAL;
    return NULL;
  }
  int fds[2];
  if (pipe(fds) < 0)
    return NULL;
  int pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return NULL;
  }
  if (pid == 0) {
    if (mode[0] == 'r') {
      close(fds[0]);
      dup2(fds[1], STDOUT_FILENO);
      close(fds[1]);
    } else {
      close(fds[1]);
      dup2(fds[0], STDIN_FILENO);
      close(fds[0]);
    }
    char *argv[] = {"/bin/sh", "-c", (char *)command, NULL};
    execv("/bin/sh", argv);
    _exit(127);
  }
  int fd = mode[0] == 'r' ? fds[0] : fds[1];
  close(mode[0] == 'r' ? fds[1] : fds[0]);
  FILE *stream = fdopen(fd, mode);
  if (!stream) {
    close(fd);
    waitpid(pid, NULL, 0);
    return NULL;
  }
  stream->pipe_pid = pid;
  return stream;
}

int pclose(FILE *stream) {
  if (!stream || stream->pipe_pid <= 0) {
    errno = EINVAL;
    return -1;
  }
  int pid = stream->pipe_pid;
  fclose(stream);
  int status = 0;
  return waitpid(pid, &status, 0) < 0 ? -1 : status;
}

int fclose(FILE *stream) {
  if (!stream)
    return -1;
  if (stream->ms_active) {
    /* Finalize the caller-owned buffer; do NOT free ms_buf (the caller owns it
     * after close per open_memstream(3)). */
    if (stream->ms_userbuf)
      *stream->ms_userbuf = stream->ms_buf;
    if (stream->ms_usersize)
      *stream->ms_usersize = stream->ms_size;
    free(stream);
    return 0;
  }
  int res = close(stream->fd);
  if (stream != stdin && stream != stdout && stream != stderr) {
    free(stream);
  }
  return res;
}

/* Single write funnel: routes to the open_memstream heap buffer when active,
 * otherwise to the fd. Returns bytes written, or -1 on error. */
long _file_write(FILE *f, const void *buf, size_t n) {
  if (f->ms_active) {
    if (f->ms_size + n + 1 > f->ms_cap) {
      size_t cap = f->ms_cap ? f->ms_cap : 64;
      while (f->ms_size + n + 1 > cap)
        cap *= 2;
      char *nb = (char *)realloc(f->ms_buf, cap);
      if (!nb) {
        f->error = 1;
        return -1;
      }
      f->ms_buf = nb;
      f->ms_cap = cap;
    }
    memcpy(f->ms_buf + f->ms_size, buf, n);
    f->ms_size += n;
    f->ms_buf[f->ms_size] = '\0';
    if (f->ms_userbuf)
      *f->ms_userbuf = f->ms_buf;
    if (f->ms_usersize)
      *f->ms_usersize = f->ms_size;
    return (long)n;
  }
  return write(f->fd, buf, n);
}

FILE *open_memstream(char **bufp, size_t *sizep) {
  if (!bufp || !sizep)
    return NULL;
  FILE *f = (FILE *)calloc(1, sizeof(FILE));
  if (!f)
    return NULL;
  f->fd = -1;
  f->ms_active = 1;
  f->ms_cap = 64;
  f->ms_buf = (char *)malloc(f->ms_cap);
  if (!f->ms_buf) {
    free(f);
    return NULL;
  }
  f->ms_buf[0] = '\0';
  f->ms_userbuf = bufp;
  f->ms_usersize = sizep;
  *bufp = f->ms_buf;
  *sizep = 0;
  return f;
}

int fputs(const char *s, FILE *stream) {
  if (!stream)
    return EOF;
  int len = strlen(s);
  if (len == 0)
    return 0;
  long n = _file_write(stream, s, len);
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
  long n = _file_write(stream, ptr, size * nmemb);
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

int fseeko(FILE *stream, off_t offset, int whence) {
  if (!stream)
    return -1;
  off_t pos = (off_t)lseek(stream->fd, (long)offset, whence);
  if (pos < 0)
    return -1;
  stream->eof = 0;
  return 0;
}

off_t ftello(FILE *stream) {
  if (!stream)
    return (off_t)-1;
  return (off_t)lseek(stream->fd, 0, SEEK_CUR);
}

int fflush(FILE *stream) {
  /* Memstreams publish the current buffer/size to the caller's pointers;
   * fd streams are unbuffered so flush is a no-op. */
  if (stream && stream->ms_active) {
    if (stream->ms_userbuf)
      *stream->ms_userbuf = stream->ms_buf;
    if (stream->ms_usersize)
      *stream->ms_usersize = stream->ms_size;
  }
  return 0;
}

int feof(FILE *stream) { return stream ? stream->eof : 1; }

int ferror(FILE *stream) { return stream ? stream->error : 1; }

int fileno(FILE *stream) { return stream ? stream->fd : -1; }

int remove(const char *pathname) {
  /* POSIX remove(3): unlink(2) for files, rmdir(2) for directories. Try the
   * file path first; if the target is a directory the kernel reports EISDIR
   * (or EPERM on some POSIX systems), so fall back to rmdir. */
  int r = unlink(pathname);
  if (r != 0 && (errno == EISDIR || errno == EPERM))
    r = rmdir(pathname);
  return r;
}

int fprintf(FILE *stream, const char *fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  if (stream && n > 0) {
    _file_write(stream, buf, n);
  }
  return n;
}

int vfprintf(FILE *stream, const char *fmt, va_list ap) {
  char buf[512];
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  if (stream && n > 0) {
    _file_write(stream, buf, n);
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

static void _vsnprintf_putsn(char *str, size_t size, int *pos, const char *s,
                             int length) {
  if (!s)
    s = "(null)";
  while (*s && length-- > 0)
    _vsnprintf_putc(str, size, pos, *s++);
}

static void _vsnprintf_putd(char *str, size_t size, int *pos, long long v,
                            int base, int signed_val, int width, int pad_zero,
                            int left_align, int uppercase) {
  char buf[32];
  int p = 0;
  int neg = 0;
  unsigned long long uv = (unsigned long long)v;
  if (signed_val && v < 0) {
    neg = 1;
    uv = -(unsigned long long)v;
  }
  const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
  do {
    buf[p++] = digits[uv % base];
    uv /= base;
  } while (uv > 0);

  int total = p + (neg ? 1 : 0);
  int pad = width > total ? width - total : 0;

  /* '0' padding is ignored for left-justified conversions (POSIX). */
  if (pad_zero && !left_align) {
    if (neg)
      _vsnprintf_putc(str, size, pos, '-');
    for (int k = 0; k < pad; k++)
      _vsnprintf_putc(str, size, pos, '0');
    while (p > 0)
      _vsnprintf_putc(str, size, pos, buf[--p]);
  } else {
    if (!left_align)
      for (int k = 0; k < pad; k++)
        _vsnprintf_putc(str, size, pos, ' ');
    if (neg)
      _vsnprintf_putc(str, size, pos, '-');
    while (p > 0)
      _vsnprintf_putc(str, size, pos, buf[--p]);
    if (left_align)
      for (int k = 0; k < pad; k++)
        _vsnprintf_putc(str, size, pos, ' ');
  }
}

static void _vsnprintf_putf(char *str, size_t size, int *pos, double value,
                            int precision) {
  if (precision < 0)
    precision = 6;
  if (precision > 9)
    precision = 9;

  if (value < 0) {
    _vsnprintf_putc(str, size, pos, '-');
    value = -value;
  }

  unsigned long scale = 1;
  for (int i = 0; i < precision; i++)
    scale *= 10;
  unsigned long rounded = (unsigned long)(value * scale + 0.5);
  unsigned long whole = scale ? rounded / scale : rounded;
  unsigned long fraction = scale ? rounded % scale : 0;
  _vsnprintf_putd(str, size, pos, (long)whole, 10, 0, 0, 0, 0, 0);

  if (precision > 0) {
    _vsnprintf_putc(str, size, pos, '.');
    unsigned long divisor = scale / 10;
    while (divisor > 0) {
      _vsnprintf_putc(str, size, pos,
                      (char)('0' + (fraction / divisor) % 10));
      divisor /= 10;
    }
  }
}

/* printf %g: shortest of %e/%f for the value, trailing zeros stripped. Needed
 * by e.g. BusyBox awk (CONVFMT/OFMT default "%.6g"); without it the format was
 * copied through literally ("%g"). */
static void _vsnprintf_putg(char *str, size_t size, int *pos, double value,
                            int precision) {
  char buf[72];
  int n = 0;

  if (precision < 0)
    precision = 6;
  if (precision == 0)
    precision = 1;
  if (precision > 17)
    precision = 17;

  if (value < 0) {
    buf[n++] = '-';
    value = -value;
  }

  /* Decimal exponent X with value ~ m * 10^X, 1 <= m < 10 (0 for value 0). */
  int X = 0;
  double m = value;
  if (m != 0.0) {
    while (m >= 10.0) { m /= 10.0; X++; }
    while (m < 1.0)  { m *= 10.0; X--; }
  }

  if (X < -4 || X >= precision) {
    /* %e form: one mantissa digit + (precision-1) fraction digits. */
    int fprec = precision - 1;
    unsigned long scale = 1;
    for (int i = 0; i < fprec; i++) scale *= 10;
    unsigned long r = (unsigned long)(m * scale + 0.5);
    if (scale && r >= 10UL * scale) { r /= 10; X++; } /* rounding carried to 10.x */
    unsigned long whole = scale ? r / scale : r;
    unsigned long frac = scale ? r % scale : 0;
    buf[n++] = (char)('0' + (whole % 10));
    char fb[20]; int fn = 0; unsigned long div = scale / 10;
    for (int i = 0; i < fprec; i++) { fb[fn++] = (char)('0' + (frac / (div ? div : 1)) % 10); if (div) div /= 10; }
    while (fn > 0 && fb[fn - 1] == '0') fn--;
    if (fn > 0) { buf[n++] = '.'; for (int i = 0; i < fn; i++) buf[n++] = fb[i]; }
    buf[n++] = 'e';
    buf[n++] = X < 0 ? '-' : '+';
    int ax = X < 0 ? -X : X;
    buf[n++] = (char)('0' + (ax / 10) % 10);
    buf[n++] = (char)('0' + ax % 10);
  } else {
    /* %f form with precision (precision-1-X), trailing zeros stripped. */
    int fprec = precision - 1 - X;
    if (fprec < 0) fprec = 0;
    if (fprec > 17) fprec = 17;
    unsigned long scale = 1;
    for (int i = 0; i < fprec; i++) scale *= 10;
    unsigned long rounded = (unsigned long)(value * scale + 0.5);
    unsigned long whole = scale ? rounded / scale : rounded;
    unsigned long frac = scale ? rounded % scale : 0;
    char wb[24]; int wn = 0;
    if (whole == 0) { wb[wn++] = '0'; }
    else { char t[24]; int tn = 0; unsigned long w = whole; while (w) { t[tn++] = (char)('0' + w % 10); w /= 10; } while (tn) wb[wn++] = t[--tn]; }
    for (int i = 0; i < wn; i++) buf[n++] = wb[i];
    if (fprec > 0) {
      char fb[20]; int fn = 0; unsigned long div = scale / 10;
      for (int i = 0; i < fprec; i++) { fb[fn++] = (char)('0' + (frac / (div ? div : 1)) % 10); if (div) div /= 10; }
      while (fn > 0 && fb[fn - 1] == '0') fn--;
      if (fn > 0) { buf[n++] = '.'; for (int i = 0; i < fn; i++) buf[n++] = fb[i]; }
    }
  }
  buf[n] = '\0';
  for (int i = 0; i < n; i++) _vsnprintf_putc(str, size, pos, buf[i]);
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
    int left_align = 0;
    int zero_pad = 0;
    while (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == ' ' || fmt[i] == '#' ||
           fmt[i] == '0') {
      if (fmt[i] == '-')
        left_align = 1;
      else if (fmt[i] == '0')
        zero_pad = 1;
      i++;
    }
    int width = 0;
    if (fmt[i] == '*') {
      width = va_arg(ap, int);
      i++;
      if (width < 0) {
        left_align = 1;
        width = -width;
      }
    } else {
      while (fmt[i] >= '0' && fmt[i] <= '9') {
        width = width * 10 + fmt[i] - '0';
        i++;
      }
    }
    int precision = -1;
    if (fmt[i] == '.') {
      i++;
      precision = 0;
      if (fmt[i] == '*') {
        precision = va_arg(ap, int);
        i++;
      } else {
        while (fmt[i] >= '0' && fmt[i] <= '9') {
          precision = precision * 10 + fmt[i] - '0';
          i++;
        }
      }
    }

    /* Length modifiers. `ll`/`j`/`L` are 64-bit on every target; `l`/`z`/`t`
     * are word-width (64-bit on x86_64, 32-bit on i686). Distinguishing them
     * matters on i686, where reading a 64-bit `%llu`/`%llo` arg as a 32-bit
     * `long` (the old behaviour) consumed only half the value and misaligned
     * every following argument — BusyBox `cksum` (`%llu`) hung and `tar`'s
     * `putOctal` (`%llo`) wrote garbage. */
    int lcount = 0;
    while (fmt[i] == 'l') {
      lcount++;
      i++;
    }
    int is_ll = (lcount >= 2);
    int is_l = (lcount == 1);
    if (fmt[i] == 'j' || fmt[i] == 'L') {
      is_ll = 1;
      i++;
    } else if (fmt[i] == 'z' || fmt[i] == 't') {
      is_l = 1;
      i++;
    }
    if (fmt[i] == 'h') { /* h / hh: still passed as int via default promotion */
      i++;
      if (fmt[i] == 'h')
        i++;
    }

    switch (fmt[i]) {
    case 's': {
      const char *value = va_arg(ap, const char *);
      if (!value)
        value = "(null)";
      int length = (int)strlen(value);
      if (precision >= 0 && length > precision)
        length = precision;
      if (!left_align)
        for (int pad = width - length; pad > 0; pad--)
          _vsnprintf_putc(str, size, &pos, ' ');
      _vsnprintf_putsn(str, size, &pos, value, length);
      if (left_align)
        for (int pad = width - length; pad > 0; pad--)
          _vsnprintf_putc(str, size, &pos, ' ');
      break;
    }
    case 'd':
    case 'i':
      _vsnprintf_putd(str, size, &pos,
                      is_ll ? va_arg(ap, long long)
                            : is_l ? (long long)va_arg(ap, long)
                                   : (long long)va_arg(ap, int),
                      10, 1, width, zero_pad, left_align, 0);
      break;
    case 'u':
      _vsnprintf_putd(str, size, &pos,
                      is_ll ? (long long)va_arg(ap, unsigned long long)
                            : is_l ? (long long)va_arg(ap, unsigned long)
                                   : (long long)va_arg(ap, unsigned int),
                      10, 0, width, zero_pad, left_align, 0);
      break;
    case 'x':
    case 'X':
      _vsnprintf_putd(str, size, &pos,
                      is_ll ? (long long)va_arg(ap, unsigned long long)
                            : is_l ? (long long)va_arg(ap, unsigned long)
                                   : (long long)va_arg(ap, unsigned int),
                      16, 0, width, zero_pad, left_align, fmt[i] == 'X');
      break;
    case 'o':
      _vsnprintf_putd(str, size, &pos,
                      is_ll ? (long long)va_arg(ap, unsigned long long)
                            : is_l ? (long long)va_arg(ap, unsigned long)
                                   : (long long)va_arg(ap, unsigned int),
                      8, 0, width, zero_pad, left_align, 0);
      break;
    case 'p':
      _vsnprintf_puts(str, size, &pos, "0x");
      _vsnprintf_putd(str, size, &pos,
                      (long long)(unsigned long)va_arg(ap, void *), 16, 0, 0, 0,
                      0, 0);
      break;
    case 'c':
      _vsnprintf_putc(str, size, &pos, (char)va_arg(ap, int));
      break;
    case 'f':
    case 'F':
      _vsnprintf_putf(str, size, &pos, va_arg(ap, double), precision);
      break;
    case 'g':
    case 'G':
      _vsnprintf_putg(str, size, &pos, va_arg(ap, double), precision);
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

int vsprintf(char *str, const char *fmt, va_list ap) {
  return vsnprintf(str, 0x7fffffff, fmt, ap);
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
    if (conv == '[') {
      /* Scanset %[...] / %[^...]: read characters that are in (or, with a
       * leading ^, not in) the bracket set, up to the field width. Unlike %s
       * it does NOT skip leading whitespace. A ']' right after '[' or '[^' is
       * a literal member; "a-z" denotes an inclusive range. */
      const char *setp = f + 1;
      int negate = 0;
      if (*setp == '^') { negate = 1; setp++; }
      unsigned char inset[256];
      memset(inset, 0, sizeof(inset));
      if (*setp == ']') { inset[(unsigned char)']'] = 1; setp++; }
      while (*setp && *setp != ']') {
        if (setp[1] == '-' && setp[2] && setp[2] != ']') {
          unsigned char lo = (unsigned char)setp[0], hi = (unsigned char)setp[2];
          if (lo <= hi)
            for (int ch = lo; ch <= hi; ch++) inset[ch] = 1;
          setp += 3;
        } else {
          inset[(unsigned char)*setp] = 1;
          setp++;
        }
      }
      /* Leave f on the closing ']' so the loop's f++ steps past it. */
      f = (*setp == ']') ? setp : setp - 1;
      char *p = suppress ? 0 : va_arg(ap, char *);
      int n = 0;
      int w = width ? width : 0x7fffffff;
      int c;
      while (n < w) {
        c = _sc_get(s);
        if (c == EOF) break;
        int member = inset[(unsigned char)c] ? 1 : 0;
        if (negate) member = !member;
        if (!member) { _sc_unget(s, c); break; }
        saw_input = 1;
        if (p) p[n] = (char)c;
        n++;
      }
      if (n == 0)
        return saw_input ? assigned : (assigned ? assigned : EOF);
      if (p) p[n] = '\0';
      if (!suppress) assigned++;
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

ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
  if (!lineptr || !n || !stream) {
    errno = EINVAL;
    return -1;
  }

  if (!*lineptr) {
    *n = 128;
    *lineptr = malloc(*n);
    if (!*lineptr) {
      return -1;
    }
  }

  size_t pos = 0;
  while (1) {
    int c = fgetc(stream);
    if (c == EOF) {
      if (pos == 0) {
        return -1;
      }
      break;
    }

    if (pos + 1 >= *n) {
      size_t new_n = *n * 2;
      char *new_ptr = realloc(*lineptr, new_n);
      if (!new_ptr) {
        return -1;
      }
      *lineptr = new_ptr;
      *n = new_n;
    }

    (*lineptr)[pos++] = (char)c;
    if (c == '\n') {
      break;
    }
  }

  (*lineptr)[pos] = '\0';
  return (ssize_t)pos;
}

int vasprintf(char **strp, const char *fmt, va_list ap) {
  va_list ap_copy;
  va_copy(ap_copy, ap);
  int len = vsnprintf(NULL, 0, fmt, ap_copy);
  va_end(ap_copy);
  if (len < 0) return -1;

  char *buf = malloc((size_t)len + 1);
  if (!buf) return -1;

  int r = vsnprintf(buf, (size_t)len + 1, fmt, ap);
  if (r < 0) {
    free(buf);
    return -1;
  }
  *strp = buf;
  return r;
}

int asprintf(char **strp, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vasprintf(strp, fmt, ap);
  va_end(ap);
  return r;
}

#include <errno.h>

int vdprintf(int fd, const char *format, va_list ap) {
  char *buf = NULL;
  int r = vasprintf(&buf, format, ap);
  if (r < 0) return -1;
  int written = 0;
  while (written < r) {
    ssize_t nw = write(fd, buf + written, (size_t)(r - written));
    if (nw < 0) {
      if (errno == EINTR) continue;
      free(buf);
      return -1;
    }
    if (nw == 0) break;
    written += (int)nw;
  }
  free(buf);
  return written;
}

int dprintf(int fd, const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  int r = vdprintf(fd, format, ap);
  va_end(ap);
  return r;
}
