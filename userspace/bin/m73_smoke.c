/* M73 smoke: modern I/O & introspection syscalls.
 *
 * Each marker is emitted only after the operation is performed AND its result
 * verified (data round-trips, sizes/offsets are correct) — no unconditional
 * "ok" prints.
 *
 *   statx            statx() returns the same size/mode/nlink/ino as fstat() on
 *                    the same file, both by path (AT_FDCWD) and by fd
 *                    (AT_EMPTY_PATH).
 *   sendfile         sendfile() copies a file's bytes to another fd; with an
 *                    explicit offset it reads from there, advances the caller's
 *                    offset, and leaves the source fd's own offset untouched.
 *   copy-file-range  copy_file_range() copies a byte range between two files
 *                    using independent explicit offsets.
 *   fallocate        fallocate(mode 0) grows a short file to offset+len and the
 *                    newly-covered bytes read back as zero; KEEP_SIZE does not
 *                    grow it.
 *   splice           splice() moves data file->pipe->file through a pipe and the
 *                    bytes arrive intact.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail;

static void marker(const char *s) {
  write(1, s, strlen(s));
  write(1, "\n", 1);
}
static void ok(const char *name) {
  char buf[128];
  snprintf(buf, sizeof(buf), "M73-SMOKE: ok %s", name);
  marker(buf);
}
static void fail(const char *name, long a, long b) {
  char buf[160];
  snprintf(buf, sizeof(buf), "M73-SMOKE: FAIL %s got=%ld expected=%ld", name, a,
           b);
  marker(buf);
  g_fail = 1;
}

static int write_file(const char *path, const char *data, size_t n) {
  int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (fd < 0)
    return -1;
  if (write(fd, data, n) != (ssize_t)n) {
    close(fd);
    return -1;
  }
  lseek(fd, 0, SEEK_SET);
  return fd;
}

static void test_statx(void) {
  const char *p = "/tmp/m73_statx";
  const char *data = "statx-payload-1234";
  int fd = write_file(p, data, strlen(data));
  if (fd < 0) {
    fail("statx-setup", fd, 0);
    return;
  }
  struct stat st;
  if (fstat(fd, &st) < 0) {
    fail("statx-fstat", -1, 0);
    close(fd);
    return;
  }
  struct statx sx;
  memset(&sx, 0xee, sizeof(sx));
  /* by path */
  if (statx(AT_FDCWD, p, 0, STATX_BASIC_STATS, &sx) < 0) {
    fail("statx-path", -1, 0);
    close(fd);
    return;
  }
  if (sx.stx_size != (unsigned long long)st.st_size) {
    fail("statx-size", (long)sx.stx_size, (long)st.st_size);
    close(fd);
    return;
  }
  if (sx.stx_ino != (unsigned long long)st.st_ino ||
      sx.stx_mode != (unsigned short)st.st_mode ||
      sx.stx_nlink != (unsigned int)st.st_nlink) {
    fail("statx-fields", (long)sx.stx_ino, (long)st.st_ino);
    close(fd);
    return;
  }
  /* by fd with AT_EMPTY_PATH */
  struct statx sx2;
  if (statx(fd, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &sx2) < 0) {
    fail("statx-emptypath", -1, 0);
    close(fd);
    return;
  }
  if (sx2.stx_size != (unsigned long long)st.st_size) {
    fail("statx-emptypath-size", (long)sx2.stx_size, (long)st.st_size);
    close(fd);
    return;
  }
  close(fd);
  ok("statx");
}

static void test_sendfile(void) {
  const char *src = "/tmp/m73_sf_src";
  const char *dst = "/tmp/m73_sf_dst";
  const char *data = "ABCDEFGHIJ"; /* 10 bytes */
  int in = write_file(src, data, 10);
  if (in < 0) {
    fail("sendfile-setup", in, 0);
    return;
  }
  int out = open(dst, O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (out < 0) {
    fail("sendfile-open-out", out, 0);
    close(in);
    return;
  }
  /* explicit offset 3, count 4 -> "DEFG"; source fd offset must stay at 0. */
  off_t off = 3;
  ssize_t n = sendfile(out, in, &off, 4);
  if (n != 4) {
    fail("sendfile-count", (long)n, 4);
    goto out;
  }
  if (off != 7) {
    fail("sendfile-offset", (long)off, 7);
    goto out;
  }
  if (lseek(in, 0, SEEK_CUR) != 0) {
    fail("sendfile-srcpos", (long)lseek(in, 0, SEEK_CUR), 0);
    goto out;
  }
  char buf[8];
  memset(buf, 0, sizeof(buf));
  lseek(out, 0, SEEK_SET);
  if (read(out, buf, 4) != 4 || memcmp(buf, "DEFG", 4) != 0) {
    fail("sendfile-data", 0, 0);
    goto out;
  }
  ok("sendfile");
out:
  close(in);
  close(out);
}

static void test_copy_file_range(void) {
  const char *src = "/tmp/m73_cfr_src";
  const char *dst = "/tmp/m73_cfr_dst";
  const char *data = "0123456789"; /* 10 bytes */
  int in = write_file(src, data, 10);
  if (in < 0) {
    fail("cfr-setup", in, 0);
    return;
  }
  int out = open(dst, O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (out < 0) {
    fail("cfr-open-out", out, 0);
    close(in);
    return;
  }
  off_t off_in = 4, off_out = 2;
  ssize_t n = copy_file_range(in, &off_in, out, &off_out, 5, 0);
  if (n != 5) {
    fail("cfr-count", (long)n, 5);
    goto out;
  }
  if (off_in != 9 || off_out != 7) {
    fail("cfr-offsets", (long)off_in, 9);
    goto out;
  }
  char buf[8];
  memset(buf, 0, sizeof(buf));
  lseek(out, 2, SEEK_SET);
  if (read(out, buf, 5) != 5 || memcmp(buf, "45678", 5) != 0) {
    fail("cfr-data", 0, 0);
    goto out;
  }
  ok("copy-file-range");
out:
  close(in);
  close(out);
}

static void test_fallocate(void) {
  const char *p = "/tmp/m73_falloc";
  const char *data = "hi"; /* 2 bytes */
  int fd = write_file(p, data, 2);
  if (fd < 0) {
    fail("fallocate-setup", fd, 0);
    return;
  }
  /* KEEP_SIZE must NOT grow the file. */
  if (fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, 4096) != 0) {
    fail("fallocate-keepsize", -1, 0);
    close(fd);
    return;
  }
  struct stat st;
  fstat(fd, &st);
  if (st.st_size != 2) {
    fail("fallocate-keepsize-size", (long)st.st_size, 2);
    close(fd);
    return;
  }
  /* mode 0 must grow to offset+len = 4096 and zero-fill the gap. */
  if (fallocate(fd, 0, 0, 4096) != 0) {
    fail("fallocate-grow", -1, 0);
    close(fd);
    return;
  }
  fstat(fd, &st);
  if (st.st_size != 4096) {
    fail("fallocate-grow-size", (long)st.st_size, 4096);
    close(fd);
    return;
  }
  char buf[16];
  lseek(fd, 100, SEEK_SET);
  int z = 1;
  if (read(fd, buf, 16) != 16)
    z = 0;
  for (int i = 0; i < 16; i++)
    if (buf[i] != 0)
      z = 0;
  if (!z) {
    fail("fallocate-zero", 0, 0);
    close(fd);
    return;
  }
  close(fd);
  ok("fallocate");
}

static void test_splice(void) {
  const char *src = "/tmp/m73_spl_src";
  const char *dst = "/tmp/m73_spl_dst";
  const char *data = "splice-me-please"; /* 16 bytes */
  size_t len = strlen(data);
  int in = write_file(src, data, len);
  if (in < 0) {
    fail("splice-setup", in, 0);
    return;
  }
  int pfd[2];
  if (pipe(pfd) < 0) {
    fail("splice-pipe", -1, 0);
    close(in);
    return;
  }
  int out = open(dst, O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (out < 0) {
    fail("splice-open-out", out, 0);
    close(in);
    close(pfd[0]);
    close(pfd[1]);
    return;
  }
  /* file -> pipe (pipe offset must be NULL), then pipe -> file. */
  ssize_t a = splice(in, NULL, pfd[1], NULL, len, 0);
  if (a != (ssize_t)len) {
    fail("splice-to-pipe", (long)a, (long)len);
    goto out;
  }
  ssize_t b = splice(pfd[0], NULL, out, NULL, len, 0);
  if (b != (ssize_t)len) {
    fail("splice-from-pipe", (long)b, (long)len);
    goto out;
  }
  char buf[32];
  memset(buf, 0, sizeof(buf));
  lseek(out, 0, SEEK_SET);
  if (read(out, buf, len) != (ssize_t)len || memcmp(buf, data, len) != 0) {
    fail("splice-data", 0, 0);
    goto out;
  }
  ok("splice");
out:
  close(in);
  close(out);
  close(pfd[0]);
  close(pfd[1]);
}

int main(void) {
  marker("M73-SMOKE: start");
  test_statx();
  test_sendfile();
  test_copy_file_range();
  test_fallocate();
  test_splice();
  marker(g_fail ? "M73-SMOKE: done (with failures)" : "M73-SMOKE: done");
  return g_fail ? 1 : 0;
}
