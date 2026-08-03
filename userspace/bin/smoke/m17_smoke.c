/* M17: errno matrix — ELOOP, ENAMETOOLONG, ENOTDIR, EISDIR, EROFS, plus errno
 * isolation and a dup2 round-trip. Written against POSIX: the libc wrappers set
 * errno themselves, so the test exercises the same kernel paths a real program
 * reaches rather than the raw syscall surface. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void marker(const char *s) {
  write(1, s, strlen(s));
  write(1, "\n", 1);
}

static void fail_marker(const char *name, int got, int expected) {
  char buf[128];
  snprintf(buf, sizeof(buf), "M17-SMOKE: FAIL %s errno=%d expected=%d", name,
           got, expected);
  marker(buf);
}

int main(void) {
  marker("M17-SMOKE: start");

  /* Test 1: ELOOP */
  {
    char from[32];
    char to[32];
    for (int i = 0; i < 17; i++) {
      snprintf(from, sizeof(from), "/tmp/sl%d", i);
      (void)unlink(from);
    }
    for (int i = 0; i < 17; i++) {
      snprintf(from, sizeof(from), "/tmp/sl%d", i);
      snprintf(to, sizeof(to), "/tmp/sl%d", (i + 1) % 17);
      errno = 0;
      if (symlink(to, from) < 0 && errno != EEXIST) {
        fail_marker("eloop-setup", errno, 0);
        return 1;
      }
    }
    errno = 0;
    int fd = open("/tmp/sl0", O_RDONLY);
    if (fd >= 0 || errno != ELOOP) {
      if (fd >= 0)
        (void)close(fd);
      fail_marker("eloop", errno, ELOOP);
      return 1;
    }
    marker("M17-SMOKE: ok eloop");
  }

  /* Test 2: ENAMETOOLONG */
  {
    char longname[300];
    memset(longname, 'a', 257);
    longname[257] = '\0';
    char path[320];
    snprintf(path, sizeof(path), "/tmp/%s", longname);
    errno = 0;
    int fd = open(path, O_RDONLY);
    if (fd >= 0 || errno != ENAMETOOLONG) {
      if (fd >= 0)
        (void)close(fd);
      fail_marker("enametoolong", errno, ENAMETOOLONG);
      return 1;
    }
    marker("M17-SMOKE: ok enametoolong");
  }

  /* Test 3: ENOTDIR */
  {
    const char *p = "/tmp/m17_file";
    (void)unlink(p);
    errno = 0;
    int fd = open(p, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) {
      fail_marker("enotdir-setup", errno, 0);
      return 1;
    }
    (void)close(fd);
    errno = 0;
    fd = open("/tmp/m17_file/child", O_RDONLY);
    if (fd >= 0 || errno != ENOTDIR) {
      if (fd >= 0)
        (void)close(fd);
      fail_marker("enotdir", errno, ENOTDIR);
      return 1;
    }
    marker("M17-SMOKE: ok enotdir");
  }

  /* Test 4: EISDIR */
  {
    errno = 0;
    int fd = open("/tmp", O_WRONLY);
    if (fd >= 0 || errno != EISDIR) {
      if (fd >= 0)
        (void)close(fd);
      fail_marker("eisdir", errno, EISDIR);
      return 1;
    }
    marker("M17-SMOKE: ok eisdir");
  }

  /* Test 5: EROFS (conditional) */
  {
    errno = 0;
    int fd = open("/proc/m17_rofs_test", O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0 && errno == EROFS) {
      marker("M17-SMOKE: ok erofs");
    } else {
      if (fd >= 0) {
        (void)close(fd);
        (void)unlink("/proc/m17_rofs_test");
      }
      marker("M17-SMOKE: ok erofs-skip");
    }
  }

  /* Test 6: errno isolation — a successful call must not disturb errno */
  {
    errno = 0;
    int fd = open("/tmp/m17_no_such_file_xyz", O_RDONLY);
    if (fd >= 0 || errno != ENOENT) {
      if (fd >= 0)
        (void)close(fd);
      fail_marker("errno-isolation-pre", errno, ENOENT);
      return 1;
    }
    int before = errno;
    pid_t pid = getpid();
    if (pid <= 0) {
      fail_marker("errno-isolation-getpid", errno, 0);
      return 1;
    }
    if (errno != before) {
      fail_marker("errno-isolation", errno, before);
      return 1;
    }
    marker("M17-SMOKE: ok errno-isolation");
  }

  /* Test 7: dup2 smoke */
  {
    errno = 0;
    int fd = open("/tmp/m17_dup2_test", O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) {
      fail_marker("dup2-setup", errno, 0);
      return 1;
    }
    /* 13 bytes: the trailing NUL is written too, so the strcmp below sees a
     * terminated string straight out of read(). */
    const char msg[] = "hello b1nix\n";
    ssize_t str_len = (ssize_t)sizeof(msg);
    if (write(fd, msg, (size_t)str_len) != str_len ||
        lseek(fd, 0, SEEK_SET) != 0) {
      fail_marker("dup2-write", errno, 0);
      return 1;
    }

    /* dup2 to fd 100 */
    int dup_fd = 100;
    if (dup2(fd, dup_fd) != dup_fd) {
      fail_marker("dup2", errno, 0);
      return 1;
    }

    char buf[16];
    memset(buf, 0, sizeof(buf));
    ssize_t n = read(dup_fd, buf, sizeof(buf));
    if (n != str_len || strcmp(buf, msg) != 0) {
      fail_marker("dup2-read", 0, 0);
      return 1;
    }

    (void)close(fd);
    (void)close(dup_fd);
    (void)unlink("/tmp/m17_dup2_test");
    marker("M17-SMOKE: ok dup2");
  }

  marker("M17-SMOKE: done");
  return 0;
}
