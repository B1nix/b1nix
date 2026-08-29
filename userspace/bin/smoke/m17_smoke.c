/* M17: errno matrix — ELOOP, ENAMETOOLONG, ENOTDIR, EISDIR, EROFS, plus errno
 * isolation and a dup2 round-trip. Written against POSIX: the libc wrappers set
 * errno themselves, so the test exercises the same kernel paths a real program
 * reaches rather than the raw syscall surface. */

#include <errno.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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

  /* Test 8: O_NOFOLLOW and O_PATH.
   *
   * A path walker done by hand -- systemd's chase(), and so every sd-device
   * lookup and so logind -- opens each component O_PATH|O_NOFOLLOW and asks
   * fstat what it got: a symlink is read with readlink and spliced into the
   * walk, anything else is descended into. Both flags used to be dropped, so
   * the open followed the link and fstat described the target; a walk of
   * /sys/dev/char/226:1 therefore ended at the link's own path and the device
   * it named was never reached. These four checks are that behaviour, stated
   * the way the standard does. */
  {
    const char *target = "/tmp/m17_nofollow_target";
    const char *link = "/tmp/m17_nofollow_link";

    (void)unlink(target);
    (void)unlink(link);
    int fd = open(target, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
      fail_marker("nofollow-setup", errno, 0);
      return 1;
    }
    (void)write(fd, "x", 1);
    (void)close(fd);
    if (symlink(target, link) != 0) {
      fail_marker("nofollow-symlink", errno, 0);
      return 1;
    }

    /* Without the flag the link is followed and the target opens. */
    fd = open(link, O_RDONLY);
    if (fd < 0) {
      fail_marker("nofollow-plain-open", errno, 0);
      return 1;
    }
    (void)close(fd);

    /* With it, and nothing else, there is no file to open: ELOOP. */
    fd = open(link, O_RDONLY | O_NOFOLLOW);
    if (fd >= 0 || errno != ELOOP) {
      fail_marker("nofollow-eloop", fd >= 0 ? 0 : errno, ELOOP);
      (void)close(fd);
      return 1;
    }
    marker("M17-SMOKE: ok o-nofollow-eloop");

    /* With O_PATH as well, the descriptor refers to the LINK itself -- which
     * is what fstat has to say, or a walker cannot tell a link from what it
     * points at. */
    fd = open(link, O_RDONLY | O_NOFOLLOW | O_PATH);
    if (fd < 0) {
      fail_marker("opath-open", errno, 0);
      return 1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISLNK(st.st_mode)) {
      fail_marker("opath-fstat-islnk", errno, 0);
      (void)close(fd);
      return 1;
    }
    marker("M17-SMOKE: ok o-path-symlink");

    /* An O_PATH descriptor names a file without opening its contents, so a
     * read of one is EBADF. A descriptor that reads happily is not O_PATH. */
    char one[4];
    ssize_t rn = read(fd, one, sizeof(one));
    if (rn >= 0 || errno != EBADF) {
      fail_marker("opath-read-ebadf", rn >= 0 ? 0 : errno, EBADF);
      (void)close(fd);
      return 1;
    }
    (void)close(fd);
    marker("M17-SMOKE: ok o-path-read-ebadf");

    /* O_PATH on a directory still yields a usable *at() anchor: the walker
     * descends through exactly such descriptors. */
    int dfd = open("/tmp", O_RDONLY | O_PATH | O_DIRECTORY);
    if (dfd < 0) {
      fail_marker("opath-dir", errno, 0);
      return 1;
    }
    int leaf = openat(dfd, "m17_nofollow_target", O_RDONLY);
    if (leaf < 0) {
      fail_marker("opath-dirfd-openat", errno, 0);
      (void)close(dfd);
      return 1;
    }
    (void)close(leaf);
    (void)close(dfd);
    marker("M17-SMOKE: ok o-path-dirfd");

    (void)unlink(link);
    (void)unlink(target);
  }

  /* renameat2 flags. The kernel used to accept the flag word and ignore it,
   * running a plain rename: a caller asking NOT to clobber the destination was
   * told the rename succeeded, having just destroyed the file it was
   * protecting. NOREPLACE must refuse with EEXIST, and a flag this kernel
   * cannot honour atomically must say so with EINVAL rather than pretend. */
  {
    const char *src = "/tmp/m17_r2_src";
    const char *dst = "/tmp/m17_r2_dst";
    int fd = open(src, O_CREAT | O_WRONLY | O_TRUNC, 0644);

    if (fd < 0) {
      fail_marker("renameat2-setup", errno, 0);
      return 1;
    }
    (void)write(fd, "src", 3);
    (void)close(fd);

    fd = open(dst, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
      fail_marker("renameat2-setup-dst", errno, 0);
      return 1;
    }
    (void)write(fd, "dst", 3);
    (void)close(fd);

    /* 1 = RENAME_NOREPLACE: destination exists, so this must fail with EEXIST
     * and leave both files alone. */
    errno = 0;
    long rc = syscall(SYS_renameat2, AT_FDCWD, src, AT_FDCWD, dst, 1u);
    if (rc != -1 || errno != EEXIST) {
      fail_marker("renameat2-noreplace", errno, EEXIST);
      return 1;
    }

    /* The refusal must have been a refusal: the source is still there and the
     * destination still holds its own bytes. */
    char buf[8];
    fd = open(dst, O_RDONLY);
    if (fd < 0) {
      fail_marker("renameat2-dst-gone", errno, 0);
      return 1;
    }
    ssize_t n = read(fd, buf, sizeof(buf));
    (void)close(fd);
    if (n != 3 || memcmp(buf, "dst", 3) != 0) {
      fail_marker("renameat2-dst-clobbered", (int)n, 3);
      return 1;
    }
    if (access(src, F_OK) != 0) {
      fail_marker("renameat2-src-gone", errno, 0);
      return 1;
    }
    marker("M17-SMOKE: ok renameat2-noreplace");

    /* 2 = RENAME_EXCHANGE: not implementable atomically here, so EINVAL —
     * which is what makes a caller fall back instead of trusting a swap that
     * never happened. */
    errno = 0;
    rc = syscall(SYS_renameat2, AT_FDCWD, src, AT_FDCWD, dst, 2u);
    if (rc != -1 || errno != EINVAL) {
      fail_marker("renameat2-exchange", errno, EINVAL);
      return 1;
    }
    /* An unknown flag is EINVAL too, not a silent plain rename. */
    errno = 0;
    rc = syscall(SYS_renameat2, AT_FDCWD, src, AT_FDCWD, dst, 0x40u);
    if (rc != -1 || errno != EINVAL) {
      fail_marker("renameat2-unknown-flag", errno, EINVAL);
      return 1;
    }
    marker("M17-SMOKE: ok renameat2-einval");

    /* With no flags it is an ordinary rename, and must still work. */
    if (syscall(SYS_renameat2, AT_FDCWD, src, AT_FDCWD, dst, 0u) != 0) {
      fail_marker("renameat2-plain", errno, 0);
      return 1;
    }
    fd = open(dst, O_RDONLY);
    if (fd < 0) {
      fail_marker("renameat2-plain-open", errno, 0);
      return 1;
    }
    n = read(fd, buf, sizeof(buf));
    (void)close(fd);
    if (n != 3 || memcmp(buf, "src", 3) != 0) {
      fail_marker("renameat2-plain-content", (int)n, 3);
      return 1;
    }
    marker("M17-SMOKE: ok renameat2-plain");
    (void)unlink(dst);
  }

  marker("M17-SMOKE: done");
  return 0;
}
