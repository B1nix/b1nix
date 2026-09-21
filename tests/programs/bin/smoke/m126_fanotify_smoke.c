/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * m126_fanotify_smoke — fanotify(7): watching a whole mount, and refusing an
 * open before it happens.
 *
 *   init          fanotify_init(2) needs privilege and rejects nonsense flags
 *   mark-file     a mark on one file reports the open and the read of THAT
 *                 file, with a descriptor that reads the right contents, and
 *                 says nothing about a file beside it
 *   mark-mount    a mark on a mount reports a file created under it afterwards
 *                 -- no walk, no watch per directory, which is the whole point
 *   perm-allow    a permission event stops the open until the monitor answers
 *                 FAN_ALLOW, and the open then succeeds
 *   perm-deny     FAN_DENY makes that same open fail with EPERM
 *   close-write   closing a writable descriptor is reported as FAN_CLOSE_WRITE
 *                 and a read-only one as FAN_CLOSE_NOWRITE
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <linux/fanotify.h>

#ifndef __NR_fanotify_init
#define __NR_fanotify_init 300
#endif
#ifndef __NR_fanotify_mark
#define __NR_fanotify_mark 301
#endif

#define DIR "/tmp/m126fan"

static int fails;

static void ok(const char *what) {
  printf("M126-FAN: ok %s\n", what);
  fflush(stdout);
}

static void bad(const char *what, const char *why, long v) {
  printf("M126-FAN: FAIL %s — %s (%ld, errno=%d)\n", what, why, v, errno);
  fflush(stdout);
  fails++;
}

static void judge(const char *what, int good, const char *why, long v) {
  if (good)
    ok(what);
  else
    bad(what, why, v);
}

static void note(const char *fmt, ...) {
  char b[300];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  printf("M126-FAN:   %s\n", b);
  fflush(stdout);
}

static int fan_init(unsigned flags, unsigned ev_flags) {
  return (int)syscall(__NR_fanotify_init, flags, ev_flags);
}

static int fan_mark(int fd, unsigned flags, uint64_t mask, int dirfd,
                    const char *path) {
  return (int)syscall(__NR_fanotify_mark, fd, flags, mask, dirfd, path);
}

static int write_file(const char *path, const char *text) {
  int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);

  if (fd < 0)
    return -1;
  ssize_t n = write(fd, text, strlen(text));

  close(fd);
  return n == (ssize_t)strlen(text) ? 0 : -1;
}

/* Read one event, waiting up to `ms`. */
static int next_event(int fd, struct fanotify_event_metadata *md, int ms) {
  struct pollfd pfd = {.fd = fd, .events = POLLIN};

  if (poll(&pfd, 1, ms) <= 0)
    return -1;
  if (!(pfd.revents & POLLIN))
    return -1;
  ssize_t n = read(fd, md, sizeof(*md));

  return n == (ssize_t)sizeof(*md) ? 0 : -1;
}

/* ── init ────────────────────────────────────────────────────────────────── */

static void check_init(void) {
  int fd = fan_init(FAN_CLASS_NOTIF | FAN_NONBLOCK, O_RDONLY);

  if (fd < 0) {
    bad("init", "fanotify_init(2) failed", (long)fd);
    return;
  }
  int junk = fan_init(0x40000000u, O_RDONLY);

  judge("init", junk < 0 && errno == EINVAL,
        "an undefined init flag was accepted", (long)junk);
  if (junk >= 0)
    close(junk);
  close(fd);
}

/* ── a mark on one file ──────────────────────────────────────────────────── */

static void check_mark_file(void) {
  int fd = fan_init(FAN_CLASS_NOTIF | FAN_NONBLOCK, O_RDONLY);
  char watched[128], other[128];

  snprintf(watched, sizeof(watched), DIR "/watched");
  snprintf(other, sizeof(other), DIR "/other");
  if (fd < 0 || write_file(watched, "watched-content") != 0 ||
      write_file(other, "other") != 0) {
    bad("mark-file", "setup", (long)fd);
    return;
  }
  if (fan_mark(fd, FAN_MARK_ADD, FAN_OPEN | FAN_ACCESS, AT_FDCWD, watched) !=
      0) {
    bad("mark-file", "fanotify_mark", -1);
    close(fd);
    return;
  }

  /* Touch the file that is NOT watched first: it must produce nothing. */
  int of = open(other, O_RDONLY);
  char junk[8];

  if (of >= 0) {
    (void)!read(of, junk, sizeof(junk));
    close(of);
  }

  int wf = open(watched, O_RDONLY);
  char buf[32] = {0};

  if (wf >= 0)
    (void)!read(wf, buf, sizeof(buf) - 1);

  struct fanotify_event_metadata md;
  int got_open = 0, got_access = 0, fd_reads_right = 0;

  while (next_event(fd, &md, 200) == 0) {
    if (md.mask & FAN_OPEN)
      got_open++;
    if (md.mask & FAN_ACCESS)
      got_access++;
    if (md.fd >= 0) {
      char c[32] = {0};

      if (pread(md.fd, c, sizeof(c) - 1, 0) > 0 &&
          strcmp(c, "watched-content") == 0)
        fd_reads_right = 1;
      close(md.fd);
    }
  }
  if (wf >= 0)
    close(wf);
  close(fd);

  note("%d open, %d access events; the event's own fd read the file: %s",
       got_open, got_access, fd_reads_right ? "yes" : "no");
  judge("mark-file",
        got_open == 1 && got_access >= 1 && fd_reads_right,
        "the marked file's open and read were not reported exactly once each",
        (long)got_open);
}

/* ── a mark on a mount ───────────────────────────────────────────────────── */

static void check_mark_mount(void) {
  int fd = fan_init(FAN_CLASS_NOTIF | FAN_NONBLOCK, O_RDONLY);
  char fresh[128];

  if (fd < 0) {
    bad("mark-mount", "fanotify_init", (long)fd);
    return;
  }
  /* The mark goes on the directory as a MOUNT mark: everything below it is
   * watched, including files that do not exist yet. */
  if (fan_mark(fd, FAN_MARK_ADD | FAN_MARK_MOUNT, FAN_OPEN, AT_FDCWD, DIR) !=
      0) {
    bad("mark-mount", "fanotify_mark MOUNT", -1);
    close(fd);
    return;
  }
  snprintf(fresh, sizeof(fresh), DIR "/made-after-the-mark");
  if (write_file(fresh, "new") != 0) {
    bad("mark-mount", "creating a file under the mark", -1);
    close(fd);
    return;
  }
  int f = open(fresh, O_RDONLY);
  struct fanotify_event_metadata md;
  int opens = 0;

  while (next_event(fd, &md, 200) == 0) {
    if (md.mask & FAN_OPEN)
      opens++;
    if (md.fd >= 0)
      close(md.fd);
  }
  if (f >= 0)
    close(f);
  close(fd);

  note("%d opens seen under the mount mark", opens);
  judge("mark-mount", opens >= 1,
        "a file created after the mark was not watched", (long)opens);
}

/* ── permission events ───────────────────────────────────────────────────── */

struct verdict_arg {
  int fan_fd;
  int response;
  volatile int events;
  volatile int stop;
};

static void *verdict_thread(void *arg) {
  struct verdict_arg *v = arg;

  while (!v->stop) {
    struct fanotify_event_metadata md;

    if (next_event(v->fan_fd, &md, 100) != 0)
      continue;
    if (md.mask & (FAN_OPEN_PERM | FAN_ACCESS_PERM)) {
      struct fanotify_response r;

      r.fd = md.fd;
      r.response = (unsigned)v->response;
      (void)!write(v->fan_fd, &r, sizeof(r));
      v->events++;
    }
    if (md.fd >= 0)
      close(md.fd);
  }
  return NULL;
}

static void check_perm(int response, const char *name, int expect_open) {
  struct verdict_arg v;
  pthread_t th;
  char path[128];

  snprintf(path, sizeof(path), DIR "/perm-%s", name);
  if (write_file(path, "guarded") != 0) {
    bad(name, "setup", -1);
    return;
  }
  memset(&v, 0, sizeof(v));
  v.response = response;
  v.fan_fd = fan_init(FAN_CLASS_CONTENT, O_RDONLY);
  if (v.fan_fd < 0) {
    bad(name, "fanotify_init for permission events", (long)v.fan_fd);
    return;
  }
  if (fan_mark(v.fan_fd, FAN_MARK_ADD, FAN_OPEN_PERM, AT_FDCWD, path) != 0) {
    bad(name, "fanotify_mark FAN_OPEN_PERM", -1);
    close(v.fan_fd);
    return;
  }
  if (pthread_create(&th, NULL, verdict_thread, &v) != 0) {
    bad(name, "pthread_create", -1);
    close(v.fan_fd);
    return;
  }

  errno = 0;
  int fd = open(path, O_RDONLY);
  int saved = errno;

  v.stop = 1;
  pthread_join(th, NULL);

  note("%s: open returned %d (errno %d) after %d permission events", name, fd,
       saved, v.events);
  if (expect_open)
    judge(name, fd >= 0 && v.events >= 1,
          "an allowed open did not go through", (long)fd);
  else
    judge(name, fd < 0 && saved == EPERM && v.events >= 1,
          "a denied open was not refused with EPERM", (long)fd);
  if (fd >= 0)
    close(fd);
  close(v.fan_fd);
}

/* ── close events ────────────────────────────────────────────────────────── */

static void check_close(void) {
  int fd = fan_init(FAN_CLASS_NOTIF | FAN_NONBLOCK, O_RDONLY);
  char path[128];

  snprintf(path, sizeof(path), DIR "/closed");
  if (fd < 0 || write_file(path, "x") != 0) {
    bad("close-write", "setup", (long)fd);
    return;
  }
  if (fan_mark(fd, FAN_MARK_ADD, FAN_CLOSE_WRITE | FAN_CLOSE_NOWRITE, AT_FDCWD,
               path) != 0) {
    bad("close-write", "fanotify_mark", -1);
    close(fd);
    return;
  }

  int w = open(path, O_WRONLY);

  if (w >= 0)
    close(w);
  int r = open(path, O_RDONLY);

  if (r >= 0)
    close(r);

  struct fanotify_event_metadata md;
  int cw = 0, cn = 0;

  while (next_event(fd, &md, 200) == 0) {
    if (md.mask & FAN_CLOSE_WRITE)
      cw++;
    if (md.mask & FAN_CLOSE_NOWRITE)
      cn++;
    if (md.fd >= 0)
      close(md.fd);
  }
  close(fd);

  note("%d close-write, %d close-nowrite", cw, cn);
  judge("close-write", cw >= 1 && cn >= 1,
        "the two kinds of close were not told apart", (long)cw);
}

int main(void) {
  printf("M126-FAN: start\n");
  fflush(stdout);

  mkdir(DIR, 0755);

  int probe = fan_init(FAN_CLASS_NOTIF | FAN_NONBLOCK, O_RDONLY);

  if (probe < 0 && errno == ENOSYS) {
    printf("M126-FAN: FAIL init — fanotify_init(2) is ENOSYS\n");
    printf("M126-FAN: done\n");
    fflush(stdout);
    return 1;
  }
  if (probe >= 0)
    close(probe);

  check_init();
  check_mark_file();
  check_mark_mount();
  check_perm(FAN_ALLOW, "perm-allow", 1);
  check_perm(FAN_DENY, "perm-deny", 0);
  check_close();

  printf("M126-FAN: done\n");
  fflush(stdout);
  return fails ? 1 : 0;
}
