#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <unistd.h>

#ifndef ENAMETOOLONG
#define ENAMETOOLONG 36
#endif
#ifndef ELOOP
#define ELOOP 40
#endif

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

static long sys_call(long num, long a0, long a1, long a2) {
  long rc = syscall(num, a0, a1, a2);
  if (rc < 0) {
    errno = (int)(-rc);
    return -1;
  }
  return rc;
}

int main(void) {
  marker("M17-SMOKE: start");

  /* Test 1: ELOOP */
  {
    char from[32];
    char to[32];
    for (int i = 0; i < 17; i++) {
      snprintf(from, sizeof(from), "/tmp/sl%d", i);
      (void)sys_call(SYS_UNLINK, (long)from, 0, 0);
    }
    for (int i = 0; i < 17; i++) {
      snprintf(from, sizeof(from), "/tmp/sl%d", i);
      snprintf(to, sizeof(to), "/tmp/sl%d", (i + 1) % 17);
      errno = 0;
      if (sys_call(SYS_SYMLINK, (long)to, (long)from, 0) < 0 &&
          errno != EEXIST) {
        fail_marker("eloop-setup", errno, 0);
        return 1;
      }
    }
    errno = 0;
    long fd = sys_call(SYS_OPEN, (long)"/tmp/sl0", O_RDONLY, 0);
    if (fd >= 0 || errno != ELOOP) {
      if (fd >= 0)
        (void)sys_call(SYS_CLOSE, fd, 0, 0);
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
    long fd = sys_call(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd >= 0 || errno != ENAMETOOLONG) {
      if (fd >= 0)
        (void)sys_call(SYS_CLOSE, fd, 0, 0);
      fail_marker("enametoolong", errno, ENAMETOOLONG);
      return 1;
    }
    marker("M17-SMOKE: ok enametoolong");
  }

  /* Test 3: ENOTDIR */
  {
    const char *p = "/tmp/m17_file";
    (void)sys_call(SYS_UNLINK, (long)p, 0, 0);
    errno = 0;
    long fd = sys_call(SYS_OPEN, (long)p, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) {
      fail_marker("enotdir-setup", errno, 0);
      return 1;
    }
    (void)sys_call(SYS_CLOSE, fd, 0, 0);
    errno = 0;
    fd = sys_call(SYS_OPEN, (long)"/tmp/m17_file/child", O_RDONLY, 0);
    if (fd >= 0 || errno != ENOTDIR) {
      if (fd >= 0)
        (void)sys_call(SYS_CLOSE, fd, 0, 0);
      fail_marker("enotdir", errno, ENOTDIR);
      return 1;
    }
    marker("M17-SMOKE: ok enotdir");
  }

  /* Test 4: EISDIR */
  {
    errno = 0;
    long fd = sys_call(SYS_OPEN, (long)"/tmp", O_WRONLY, 0);
    if (fd >= 0 || errno != EISDIR) {
      if (fd >= 0)
        (void)sys_call(SYS_CLOSE, fd, 0, 0);
      fail_marker("eisdir", errno, EISDIR);
      return 1;
    }
    marker("M17-SMOKE: ok eisdir");
  }

  /* Test 5: EROFS (conditional) */
  {
    errno = 0;
    long fd = sys_call(SYS_OPEN, (long)"/proc/m17_rofs_test",
                       O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0 && errno == EROFS) {
      marker("M17-SMOKE: ok erofs");
    } else {
      if (fd >= 0) {
        (void)sys_call(SYS_CLOSE, fd, 0, 0);
        (void)sys_call(SYS_UNLINK, (long)"/proc/m17_rofs_test", 0, 0);
      }
      marker("M17-SMOKE: ok erofs-skip");
    }
  }

  /* Test 6: errno isolation */
  {
    errno = 0;
    long fd = sys_call(SYS_OPEN, (long)"/tmp/m17_no_such_file_xyz", O_RDONLY, 0);
    if (fd >= 0 || errno != ENOENT) {
      if (fd >= 0)
        (void)sys_call(SYS_CLOSE, fd, 0, 0);
      fail_marker("errno-isolation-pre", errno, ENOENT);
      return 1;
    }
    int before = errno;
    long pid = sys_call(SYS_GETPID, 0, 0, 0);
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
    long fd = sys_call(SYS_OPEN, (long)"/tmp/m17_dup2_test",
                       O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) {
      fail_marker("dup2-setup", errno, 0);
      return 1;
    }
    long str_len = 13;
    sys_call(SYS_WRITE, fd, (long)"hello b1nix\n", str_len);
    sys_call(SYS_LSEEK, fd, 0, SEEK_SET);

    /* dup2 to fd 100 */
    long dup_fd = 100;
    long rc = sys_call(SYS_DUP2, fd, dup_fd, 0);
    if (rc != dup_fd) {
      fail_marker("dup2", errno, 0);
      return 1;
    }

    char buf[16];
    memset(buf, 0, sizeof(buf));
    long n = sys_call(SYS_READ, dup_fd, (long)buf, sizeof(buf));
    if (n != str_len || strcmp(buf, "hello b1nix\n") != 0) {
      fail_marker("dup2-read", 0, 0);
      return 1;
    }

    sys_call(SYS_CLOSE, fd, 0, 0);
    sys_call(SYS_CLOSE, dup_fd, 0, 0);
    sys_call(SYS_UNLINK, (long)"/tmp/m17_dup2_test", 0, 0);
    marker("M17-SMOKE: ok dup2");
  }

  marker("M17-SMOKE: done");
  return 0;
}
