#ifndef B1NIX_U_STDIO_H
#define B1NIX_U_STDIO_H
#define _STDIO_H 1

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/types.h>
#include <stdarg.h>

#define EOF (-1)

typedef struct {
    int fd;
    int eof;
    int error;
    int unget_buf;
    int has_unget;
    int pipe_pid;
    /* open_memstream(3): when ms_active, writes append to a grown heap buffer
     * instead of fd, and the caller's buf/size pointers track it. Zero on
     * normal streams (partial {0,...} init leaves these clear). */
    int ms_active;
    char *ms_buf;
    size_t ms_size;
    size_t ms_cap;
    char **ms_userbuf;
    size_t *ms_usersize;
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
int asprintf(char **strp, const char *fmt, ...);
int vasprintf(char **strp, const char *fmt, va_list ap);
int dprintf(int fd, const char *format, ...);
int vdprintf(int fd, const char *format, va_list ap);
int vfprintf(FILE *stream, const char *fmt, va_list ap);
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);

FILE *open_memstream(char **bufp, size_t *sizep);
FILE *fopen(const char *pathname, const char *mode);
FILE *freopen(const char *pathname, const char *mode, FILE *stream);
FILE *fdopen(int fd, const char *mode);
FILE *popen(const char *command, const char *mode);
int pclose(FILE *stream);
int fclose(FILE *stream);
int fputs(const char *s, FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fgetc(FILE *stream);
int fputc(int c, FILE *stream);
int ungetc(int c, FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int fseeko(FILE *stream, off_t offset, int whence);
off_t ftello(FILE *stream);
/* LFS *64 names: off_t is already 64-bit on b1nix, so these are plain aliases
 * (zlib/minizip and other ports call fopen64/fseeko64/...). */
static inline FILE *fopen64(const char *pathname, const char *mode) { return fopen(pathname, mode); }
static inline FILE *freopen64(const char *pathname, const char *mode, FILE *stream) { return freopen(pathname, mode, stream); }
static inline int fseeko64(FILE *stream, off_t offset, int whence) { return fseeko(stream, offset, whence); }
static inline off_t ftello64(FILE *stream) { return ftello(stream); }
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

/* Real exported symbol (not a static inline) so foreign-function callers — e.g.
 * Rust std's `libc` FFI — can resolve it at link time. Defined in posix_compat.c. */
int rename(const char *oldpath, const char *newpath);

int scanf(const char *format, ...);
int fscanf(FILE *stream, const char *format, ...);
int sscanf(const char *str, const char *format, ...);
int vscanf(const char *format, va_list ap);
int vfscanf(FILE *stream, const char *format, va_list ap);
int vsscanf(const char *str, const char *format, va_list ap);

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

static inline void setlinebuf(FILE *stream) {
    (void)setvbuf(stream, (char *)0, 1 /* _IOLBF */, 0);
}

int vprintf(const char *format, va_list ap);

static inline char *fgets(char *s, int size, FILE *stream) {
    if (size <= 0)
        return NULL;
    /* size == 1 stores only the NUL, but must still report EOF (return NULL)
     * rather than a non-NULL empty string — callers that loop `while (fgets())`
     * over a shrinking buffer (e.g. dropbear's /etc/shells parser) would
     * otherwise spin forever once the buffer is exhausted. */
    if (size == 1) {
        int c = fgetc(stream);
        if (c == EOF)
            return NULL;
        ungetc(c, stream);
        s[0] = '\0';
        return s;
    }
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

/* Real exported symbols (not header-only inlines): libstdc++'s locale code
 * links against vsprintf/sprintf as external symbols. */
int vsprintf(char *str, const char *format, va_list ap);

#define BUFSIZ 1024
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

#define L_tmpnam 20
#define FILENAME_MAX 4096
#define FOPEN_MAX 16
#define TMP_MAX 238328

FILE *tmpfile(void);

/* Best-effort unique temp name under /tmp. b1nix has no mkstemp, so this backs
 * the L_tmpnam fallback in programs like GNU Make's open_tmpfile(). Not a
 * security boundary; uniqueness is a per-process counter. */
static inline char *tmpnam(char *s) {
    static unsigned int __tmpnam_seq = 0;
    static char __tmpnam_buf[L_tmpnam];
    char *out = s ? s : __tmpnam_buf;
    snprintf(out, L_tmpnam, "/tmp/tmp%u", ++__tmpnam_seq);
    return out;
}

static inline void rewind(FILE *stream) {
    fseek(stream, 0L, SEEK_SET);
}

static inline int getchar(void) {
    return fgetc(stdin);
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream);

static inline int getc_unlocked(FILE *stream) { return getc(stream); }
static inline int getchar_unlocked(void) { return getchar(); }
static inline int putc_unlocked(int c, FILE *stream) { return putc(c, stream); }
static inline int putchar_unlocked(int c) { return putchar(c); }
static inline int fgetc_unlocked(FILE *stream) { return fgetc(stream); }
static inline int fputc_unlocked(int c, FILE *stream) { return fputc(c, stream); }
static inline char *fgets_unlocked(char *s, int n, FILE *stream) { return fgets(s, n, stream); }
static inline int fputs_unlocked(const char *s, FILE *stream) { return fputs(s, stream); }
static inline int ferror_unlocked(FILE *stream) { return ferror(stream); }
static inline int feof_unlocked(FILE *stream) { return feof(stream); }
static inline int fflush_unlocked(FILE *stream) { return fflush(stream); }
static inline void clearerr_unlocked(FILE *stream) { clearerr(stream); }
static inline int fileno_unlocked(FILE *stream) { return fileno(stream); }

#ifdef __cplusplus
}
#endif

#endif
