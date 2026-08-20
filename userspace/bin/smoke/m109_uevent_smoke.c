#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* M109 uevent smoke: the hot-plug channel BusyBox mdev is written against.
 *
 * Every marker is emitted only after the operation ran AND its effect was
 * verified from something this test did not itself write.
 *
 *   sysfs-dev-tree       /sys/dev/block/<major:minor>/dev parses as a device
 *                        number that matches the directory it is in, and the
 *                        `uevent` file beside it names a DEVNAME that exists
 *                        in /dev as a block special file with that very
 *                        st_rdev. This is what `mdev -s` reads, field for
 *                        field.
 *   uevent-hotplug-add   with a NETLINK_KOBJECT_UEVENT socket bound, adding a
 *                        loop device through LOOP_CTL_ADD delivers
 *                        "add@/block/loop8" with ACTION, SUBSYSTEM, DEVNAME,
 *                        MAJOR, MINOR and SEQNUM — and the device really is
 *                        in /sys afterwards.
 *   mdev-scan            with the new device present in /sys and NO node in
 *                        /dev, `mdev -s` creates one, as a block special file
 *                        whose major:minor is the pair /sys published.
 *   uevent-hotplug-remove LOOP_CTL_REMOVE delivers "remove@/block/loop8" with
 *                        a strictly greater SEQNUM, and the device is gone
 *                        from /sys.
 *   mdev-daemon          `mdev -d` running as a daemon creates the node when
 *                        the device is added and unlinks it when the device is
 *                        removed — driven by the netlink broadcast alone, with
 *                        nothing in this test touching /dev.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_NETLINK
#define AF_NETLINK 16
#endif
#define NETLINK_KOBJECT_UEVENT 15

/* Linux's struct sockaddr_nl, byte for byte. */
struct nlsa {
  unsigned short nl_family;
  unsigned short nl_pad;
  unsigned int nl_pid;
  unsigned int nl_groups;
};

/* linux/loop.h — the ioctls on /dev/loop-control. The argument is the loop
 * number itself, not a pointer to it. */
#define LOOP_CTL_ADD 0x4C80
#define LOOP_CTL_REMOVE 0x4C81
#define LOOP_CTL_GET_FREE 0x4C82

/* The loop device this test creates and destroys. Chosen above the eight that
 * exist from boot, so it is unambiguously a device that appeared afterwards
 * and nothing else in the suite can be using it. */
#define HOT_LOOP 8
#define HOT_NAME "loop8"
#define HOT_NODE "/dev/loop8"
#define HOT_SYS "/sys/block/loop8"

#define BLK_MAJOR 8

static int g_fail;

static void marker(const char *s) {
  write(1, s, strlen(s));
  write(1, "\n", 1);
}

static void ok(const char *name) {
  char line[128];
  snprintf(line, sizeof(line), "M109-UEVENT: ok %s", name);
  marker(line);
}

static void failm(const char *name, const char *why) {
  char line[256];
  snprintf(line, sizeof(line), "M109-UEVENT: FAIL %s (%s, errno=%d)", name, why,
           errno);
  marker(line);
  g_fail = 1;
}

static void note(const char *fmt, ...) {
  char line[224];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  char out[256];
  snprintf(out, sizeof(out), "M109-UEVENT: note %s", line);
  marker(out);
}

/* ── small helpers ───────────────────────────────────────────────────────── */

/* Whole contents of a small file, NUL-terminated. Bytes read, or -1. */
static int read_file(const char *path, char *buf, size_t cap) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  ssize_t n = read(fd, buf, cap - 1);
  close(fd);
  if (n < 0)
    return -1;
  buf[n] = '\0';
  return (int)n;
}

/* "<major>:<minor>" -> the pair. 0 on success. */
static int parse_devno(const char *s, int *maj, int *min) {
  char *end = 0;
  long a = strtol(s, &end, 10);
  if (!end || *end != ':')
    return -1;
  char *end2 = 0;
  long b = strtol(end + 1, &end2, 10);
  if (end2 == end + 1)
    return -1;
  *maj = (int)a;
  *min = (int)b;
  return 0;
}

/* The DEVNAME= line of a sysfs `uevent` file. mdev looks for it preceded by a
 * newline, so this does too — a DEVNAME on the very first line is one mdev
 * would not find, and this test must fail in that case rather than pass. */
static int uevent_devname(const char *uevent, char *out, size_t cap) {
  const char *p = strstr(uevent, "\nDEVNAME=");
  if (!p)
    return -1;
  p += sizeof("\nDEVNAME=") - 1;
  size_t i = 0;
  while (p[i] && p[i] != '\n' && i + 1 < cap) {
    out[i] = p[i];
    i++;
  }
  out[i] = '\0';
  return i ? 0 : -1;
}

static void sleep_ms(int ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep(&ts, 0);
}

/* Wait for a path to exist (want=1) or to be gone (want=0). 1 if it happened
 * within the budget. */
static int wait_for_path(const char *path, int want, int timeout_ms) {
  struct stat st;
  for (int waited = 0;; waited += 50) {
    int here = (stat(path, &st) == 0);
    if (here == want)
      return 1;
    if (waited >= timeout_ms)
      return 0;
    sleep_ms(50);
  }
}

/* ── the uevent socket ───────────────────────────────────────────────────── */

/* Bound exactly the way mdev binds it: NETLINK_KOBJECT_UEVENT, group 1. */
static int uevent_socket(void) {
  int fd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
  if (fd < 0)
    return -1;
  /* mdev asks for a very large receive buffer; a kernel that refused it would
   * not stop mdev, but a kernel that failed the call outright would be a
   * surprise worth catching here. */
  int rcvbuf = 128 * 1024 * 1024;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

  struct timeval tv;
  tv.tv_sec = 3;
  tv.tv_usec = 0;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  struct nlsa sa;
  memset(&sa, 0, sizeof(sa));
  sa.nl_family = AF_NETLINK;
  sa.nl_pid = (unsigned int)getpid();
  sa.nl_groups = 1;
  if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/* One parsed uevent. */
struct uev {
  char summary[128];
  char action[32];
  char devpath[128];
  char subsystem[32];
  char devname[64];
  int major;
  int minor;
  unsigned long seqnum;
  int have_seqnum;
};

static void uev_parse(const char *buf, int len, struct uev *e) {
  memset(e, 0, sizeof(*e));
  e->major = -1;
  e->minor = -1;
  int i = 0;
  int first = 1;
  while (i < len) {
    const char *p = buf + i;
    size_t plen = strlen(p);
    if (first) {
      snprintf(e->summary, sizeof(e->summary), "%s", p);
      first = 0;
    } else if (strncmp(p, "ACTION=", 7) == 0)
      snprintf(e->action, sizeof(e->action), "%s", p + 7);
    else if (strncmp(p, "DEVPATH=", 8) == 0)
      snprintf(e->devpath, sizeof(e->devpath), "%s", p + 8);
    else if (strncmp(p, "SUBSYSTEM=", 10) == 0)
      snprintf(e->subsystem, sizeof(e->subsystem), "%s", p + 10);
    else if (strncmp(p, "DEVNAME=", 8) == 0)
      snprintf(e->devname, sizeof(e->devname), "%s", p + 8);
    else if (strncmp(p, "MAJOR=", 6) == 0)
      e->major = atoi(p + 6);
    else if (strncmp(p, "MINOR=", 6) == 0)
      e->minor = atoi(p + 6);
    else if (strncmp(p, "SEQNUM=", 7) == 0) {
      e->seqnum = strtoul(p + 7, 0, 10);
      e->have_seqnum = 1;
    }
    i += (int)plen + 1;
  }
}

/* Read events until one whose summary is `want` arrives, or the socket times
 * out. Other devices may announce themselves in between; those are not
 * failures, they are simply not what this call is waiting for. */
static int uev_wait(int fd, const char *want, struct uev *out) {
  for (int tries = 0; tries < 16; tries++) {
    char buf[1024];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0)
      return -1;
    buf[n] = '\0';
    uev_parse(buf, (int)n, out);
    if (strcmp(out->summary, want) == 0)
      return 0;
  }
  return -1;
}

/* ── loop-control ────────────────────────────────────────────────────────── */

static int loop_ctl(int req, int n) {
  int fd = open("/dev/loop-control", O_RDWR);
  if (fd < 0)
    return -1;
  /* The argument is the loop number itself. Widened deliberately: ioctl is
   * variadic, so an int would leave the top half of the register undefined and
   * the kernel reads a pointer-sized value. */
  int rc = ioctl(fd, req, (unsigned long)n);
  int e = errno;
  close(fd);
  errno = e;
  return rc;
}

/* ── checks ──────────────────────────────────────────────────────────────── */

/*
 * What `mdev -s` reads: /sys/dev/block/<major:minor>/{dev,uevent}. The dev
 * file must agree with the directory's own name, and the uevent's DEVNAME must
 * name a node in /dev that carries that same device number — otherwise mdev
 * would create a node under the wrong name or with the wrong number, and
 * nothing would notice until something tried to open it.
 */
static void test_sysfs_dev_tree(void) {
  DIR *d = opendir("/sys/dev/block");
  if (!d) {
    failm("sysfs-dev-tree", "/sys/dev/block is not there");
    return;
  }
  int checked = 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != 0) {
    int dmaj, dmin;
    if (parse_devno(ent->d_name, &dmaj, &dmin) != 0)
      continue; /* "." / ".." / a partition's name-only stub */

    char path[256], buf[512];
    snprintf(path, sizeof(path), "/sys/dev/block/%s/dev", ent->d_name);
    if (read_file(path, buf, sizeof(buf)) <= 0) {
      note("no dev file in /sys/dev/block/%s", ent->d_name);
      continue;
    }
    int fmaj, fmin;
    if (parse_devno(buf, &fmaj, &fmin) != 0 || fmaj != dmaj || fmin != dmin) {
      char why[128];
      snprintf(why, sizeof(why), "%s/dev says %s", ent->d_name, buf);
      closedir(d);
      failm("sysfs-dev-tree", why);
      return;
    }

    snprintf(path, sizeof(path), "/sys/dev/block/%s/uevent", ent->d_name);
    if (read_file(path, buf, sizeof(buf)) <= 0) {
      closedir(d);
      failm("sysfs-dev-tree", "no uevent file beside dev");
      return;
    }
    char name[64];
    if (uevent_devname(buf, name, sizeof(name)) != 0) {
      closedir(d);
      failm("sysfs-dev-tree", "uevent carries no DEVNAME= line");
      return;
    }

    char node[128];
    snprintf(node, sizeof(node), "/dev/%s", name);
    struct stat st;
    if (stat(node, &st) != 0) {
      note("%s names %s, which is not in /dev", ent->d_name, name);
      continue; /* a device whose node userspace has not created yet */
    }
    if (!S_ISBLK(st.st_mode)) {
      closedir(d);
      failm("sysfs-dev-tree", "the node DEVNAME names is not a block device");
      return;
    }
    if ((int)major(st.st_rdev) != fmaj || (int)minor(st.st_rdev) != fmin) {
      char why[160];
      snprintf(why, sizeof(why), "%s is %d:%d, /sys says %d:%d", node,
               (int)major(st.st_rdev), (int)minor(st.st_rdev), fmaj, fmin);
      closedir(d);
      failm("sysfs-dev-tree", why);
      return;
    }
    checked++;
  }
  closedir(d);
  if (checked == 0)
    failm("sysfs-dev-tree", "no block device was verifiable end to end");
  else
    ok("sysfs-dev-tree");
}

/* The minor /sys publishes for the hot-plugged device, or -1. */
static int hot_minor(void) {
  char buf[64];
  if (read_file(HOT_SYS "/dev", buf, sizeof(buf)) <= 0)
    return -1;
  int maj, min;
  if (parse_devno(buf, &maj, &min) != 0 || maj != BLK_MAJOR)
    return -1;
  return min;
}

static unsigned long g_add_seqnum;

/*
 * A device that appears after boot, announced over netlink. The socket is
 * bound BEFORE the device is added, so the message can only be the broadcast
 * the kernel raised for it.
 */
static void test_hotplug_add(int nlfd) {
  if (loop_ctl(LOOP_CTL_ADD, HOT_LOOP) < 0) {
    failm("uevent-hotplug-add", "LOOP_CTL_ADD failed");
    return;
  }

  struct uev e;
  if (uev_wait(nlfd, "add@/block/" HOT_NAME, &e) != 0) {
    failm("uevent-hotplug-add", "no add@/block/" HOT_NAME " on the socket");
    return;
  }
  if (strcmp(e.action, "add") != 0 || strcmp(e.subsystem, "block") != 0 ||
      strcmp(e.devname, HOT_NAME) != 0 || strcmp(e.devpath, "/block/" HOT_NAME) != 0) {
    note("action=%s subsystem=%s devname=%s devpath=%s", e.action, e.subsystem,
         e.devname, e.devpath);
    failm("uevent-hotplug-add", "the event is missing a property mdev needs");
    return;
  }
  if (e.major != BLK_MAJOR || e.minor < 0 || !e.have_seqnum) {
    note("major=%d minor=%d seqnum=%d", e.major, e.minor, e.have_seqnum);
    failm("uevent-hotplug-add", "no usable MAJOR/MINOR/SEQNUM");
    return;
  }

  /* The announcement must describe a device that is actually there now. */
  int published = hot_minor();
  if (published != e.minor) {
    note("uevent said minor %d, /sys says %d", e.minor, published);
    failm("uevent-hotplug-add", "the event and /sys disagree");
    return;
  }
  g_add_seqnum = e.seqnum;
  ok("uevent-hotplug-add");
}

/*
 * `mdev -s`: with the device in /sys and no node in /dev, the scan has to
 * create one — a block special file carrying the number /sys published.
 * Nothing here creates the node, and it is removed first so that finding one
 * afterwards can only mean mdev made it.
 */
static void test_mdev_scan(void) {
  int min = hot_minor();
  if (min < 0) {
    failm("mdev-scan", "the hot-plugged device is not in /sys");
    return;
  }
  (void)unlink(HOT_NODE);
  struct stat st;
  if (stat(HOT_NODE, &st) == 0) {
    failm("mdev-scan", HOT_NODE " could not be removed first");
    return;
  }

  int rc = system("/bin/mdev -s");
  if (rc != 0)
    note("mdev -s exited %d", rc);

  if (stat(HOT_NODE, &st) != 0) {
    failm("mdev-scan", "mdev -s did not create " HOT_NODE);
    return;
  }
  if (!S_ISBLK(st.st_mode)) {
    failm("mdev-scan", "mdev created a node that is not a block device");
    return;
  }
  if ((int)major(st.st_rdev) != BLK_MAJOR || (int)minor(st.st_rdev) != min) {
    char why[160];
    snprintf(why, sizeof(why), "node is %d:%d, /sys says %d:%d",
             (int)major(st.st_rdev), (int)minor(st.st_rdev), BLK_MAJOR, min);
    failm("mdev-scan", why);
    return;
  }
  ok("mdev-scan");
}

/* The other half of the lifecycle: removal is announced too, with a sequence
 * number strictly after the add's, and the device really leaves /sys. */
static void test_hotplug_remove(int nlfd) {
  if (loop_ctl(LOOP_CTL_REMOVE, HOT_LOOP) < 0) {
    failm("uevent-hotplug-remove", "LOOP_CTL_REMOVE failed");
    return;
  }
  struct uev e;
  if (uev_wait(nlfd, "remove@/block/" HOT_NAME, &e) != 0) {
    failm("uevent-hotplug-remove", "no remove@/block/" HOT_NAME " on the socket");
    return;
  }
  if (strcmp(e.action, "remove") != 0 || strcmp(e.subsystem, "block") != 0 ||
      strcmp(e.devname, HOT_NAME) != 0) {
    failm("uevent-hotplug-remove", "the event is missing a property mdev needs");
    return;
  }
  if (!e.have_seqnum || e.seqnum <= g_add_seqnum) {
    note("add seqnum %lu, remove seqnum %lu", g_add_seqnum, e.seqnum);
    failm("uevent-hotplug-remove", "SEQNUM did not advance");
    return;
  }
  char buf[64];
  if (read_file(HOT_SYS "/dev", buf, sizeof(buf)) > 0) {
    failm("uevent-hotplug-remove", HOT_SYS " outlived the device");
    return;
  }
  ok("uevent-hotplug-remove");
}

/*
 * The end-to-end one: `mdev -d` sitting on the netlink socket maintains /dev
 * by itself. This test adds and removes the device and only ever LOOKS at
 * /dev — every creation and unlink there is mdev's.
 */
static void test_mdev_daemon(void) {
  /* Start from a known state: the device present in /sys, no node in /dev.
   * The daemon's own initial scan will create the node, which is this test's
   * "the socket is bound and the daemon is running" signal — mdev binds
   * before it scans. Nothing boot-time is disturbed to get that signal. */
  (void)loop_ctl(LOOP_CTL_REMOVE, HOT_LOOP);
  if (loop_ctl(LOOP_CTL_ADD, HOT_LOOP) < 0) {
    failm("mdev-daemon", "LOOP_CTL_ADD failed");
    return;
  }
  (void)unlink(HOT_NODE);
  struct stat st;
  if (stat(HOT_NODE, &st) == 0) {
    failm("mdev-daemon", HOT_NODE " could not be removed first");
    return;
  }

  /* Foreground daemon, so the pid is ours to wait on and to kill. */
  pid_t pid = fork();
  if (pid < 0) {
    failm("mdev-daemon", "fork failed");
    return;
  }
  if (pid == 0) {
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, 1);
      dup2(devnull, 2);
      if (devnull > 2)
        close(devnull);
    }
    execl("/bin/mdev", "mdev", "-d", "-f", (char *)0);
    _exit(127);
  }

  /* mdev binds the uevent socket and only then performs its initial scan, so
   * the node reappearing means the socket is already listening. */
  if (!wait_for_path(HOT_NODE, 1, 5000)) {
    kill(pid, SIGKILL);
    waitpid(pid, 0, 0);
    failm("mdev-daemon", "mdev -d never completed its initial scan");
    return;
  }

  /* From here on nothing but the netlink broadcast can tell mdev anything.
   * The device goes away… */
  int removed = 0;
  if (loop_ctl(LOOP_CTL_REMOVE, HOT_LOOP) >= 0)
    removed = wait_for_path(HOT_NODE, 0, 5000);

  /* …and comes back. */
  int appeared = 0, good_node = 0, min = -1;
  if (removed && loop_ctl(LOOP_CTL_ADD, HOT_LOOP) >= 0) {
    appeared = wait_for_path(HOT_NODE, 1, 5000);
    min = hot_minor();
    if (appeared && min >= 0 && stat(HOT_NODE, &st) == 0)
      good_node = S_ISBLK(st.st_mode) && (int)major(st.st_rdev) == BLK_MAJOR &&
                  (int)minor(st.st_rdev) == min;
  }

  kill(pid, SIGKILL);
  waitpid(pid, 0, 0);

  if (!removed) {
    failm("mdev-daemon", "the daemon never unlinked " HOT_NODE);
    return;
  }
  if (!appeared) {
    failm("mdev-daemon", "the daemon never created " HOT_NODE);
    return;
  }
  if (!good_node) {
    note("node did not match /sys (minor %d)", min);
    failm("mdev-daemon", "the daemon's node has the wrong device number");
    return;
  }
  ok("mdev-daemon");
}

int main(void) {
  marker("M109-UEVENT: start");

  test_sysfs_dev_tree();

  /* The device this test hot-plugs must not exist yet, whatever ran before. */
  (void)loop_ctl(LOOP_CTL_REMOVE, HOT_LOOP);
  (void)unlink(HOT_NODE);

  int nlfd = uevent_socket();
  if (nlfd < 0) {
    failm("uevent-hotplug-add", "could not bind NETLINK_KOBJECT_UEVENT");
    failm("uevent-hotplug-remove", "could not bind NETLINK_KOBJECT_UEVENT");
  } else {
    test_hotplug_add(nlfd);
    test_mdev_scan();
    test_hotplug_remove(nlfd);
    close(nlfd);
  }

  test_mdev_daemon();

  /* Leave nothing behind for the rest of the suite. */
  (void)loop_ctl(LOOP_CTL_REMOVE, HOT_LOOP);
  (void)unlink(HOT_NODE);

  marker(g_fail ? "M109-UEVENT: done with failures" : "M109-UEVENT: done");
  return g_fail ? 1 : 0;
}
