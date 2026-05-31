/* M34 procfs / sysfs smoke. Verifies the synthetic /proc and /sys
 * filesystems expose live kernel + per-process state:
 *
 *   - /proc/meminfo carries a non-zero MemTotal;
 *   - /proc/version identifies the kernel;
 *   - /proc/self/status reports the calling pid;
 *   - /proc/self/maps lists at least one mapped region;
 *   - listing /proc shows the static files, "self", and a numeric pid dir;
 *   - /proc/<pid>/status (materialised by the listing) reports our pid;
 *   - /sys/kernel/osrelease and /sys/devices/system/cpu/online read back.
 *
 * Markers (`M34-PROC: ok <name>`) are consumed by tests/smoke.sh. */

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void emit(const char *s) { write(1, s, strlen(s)); }

static void ok(const char *name) {
  char buf[128];
  int n = 0;
  const char *p = "M34-PROC: ok ";
  while (*p) buf[n++] = *p++;
  while (*name) buf[n++] = *name++;
  buf[n++] = '\n';
  write(1, buf, n);
}

static void fail(const char *name) {
  char buf[128];
  int n = 0;
  const char *p = "M34-PROC: FAIL ";
  while (*p) buf[n++] = *p++;
  while (*name) buf[n++] = *name++;
  buf[n++] = '\n';
  write(1, buf, n);
}

/* Read an entire pseudo-file into buf (NUL-terminated). Returns bytes read,
 * or -1. Loops because synthetic files report size 0 and stream via read_cb. */
static int slurp(const char *path, char *buf, int cap) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  int total = 0;
  for (;;) {
    int r = read(fd, buf + total, cap - 1 - total);
    if (r <= 0)
      break;
    total += r;
    if (total >= cap - 1)
      break;
  }
  close(fd);
  buf[total] = '\0';
  return total;
}

int main(void) {
  emit("M34-PROC: start\n");
  char buf[2048];

  /* /proc/meminfo */
  if (slurp("/proc/meminfo", buf, sizeof(buf)) <= 0 ||
      !strstr(buf, "MemTotal")) {
    fail("meminfo");
    return 1;
  }
  ok("meminfo");

  /* /proc/version */
  if (slurp("/proc/version", buf, sizeof(buf)) <= 0 ||
      !strstr(buf, "B1NIX")) {
    fail("version");
    return 1;
  }
  ok("version");

  /* /proc/self/status — must report our own pid. */
  int mypid = getpid();
  char pidstr[16];
  snprintf(pidstr, sizeof(pidstr), "%d", mypid);
  if (slurp("/proc/self/status", buf, sizeof(buf)) <= 0 ||
      !strstr(buf, "Pid:") || !strstr(buf, pidstr)) {
    fail("proc-self-status");
    return 1;
  }
  ok("proc-self-status");

  /* /proc/self/maps — at least one VMA line (contains a '-' range). */
  if (slurp("/proc/self/maps", buf, sizeof(buf)) <= 0 || !strchr(buf, '-')) {
    fail("proc-self-maps");
    return 1;
  }
  ok("proc-self-maps");

  /* Listing /proc: static files + self + at least one numeric pid dir.
   * The readdir also materialises /proc/<pid> directories. */
  DIR *d = opendir("/proc");
  if (!d) {
    fail("proc-listing-open");
    return 1;
  }
  int saw_meminfo = 0, saw_self = 0, saw_pid = 0;
  struct dirent *e;
  while ((e = readdir(d)) != 0) {
    if (strcmp(e->d_name, "meminfo") == 0)
      saw_meminfo = 1;
    else if (strcmp(e->d_name, "self") == 0)
      saw_self = 1;
    else if (e->d_name[0] >= '1' && e->d_name[0] <= '9')
      saw_pid = 1;
  }
  closedir(d);
  if (!saw_meminfo || !saw_self || !saw_pid) {
    fail("proc-listing");
    return 1;
  }
  ok("proc-listing");

  /* /proc/<mypid>/status — materialised by the listing above. */
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/status", mypid);
  if (slurp(path, buf, sizeof(buf)) <= 0 || !strstr(buf, pidstr)) {
    fail("proc-pid-status");
    return 1;
  }
  ok("proc-pid-status");

  /* /sys/kernel/osrelease */
  if (slurp("/sys/kernel/osrelease", buf, sizeof(buf)) <= 0 ||
      !strstr(buf, "0.22.0")) {
    fail("sysfs-osrelease");
    return 1;
  }
  ok("sysfs-osrelease");

  /* /sys/devices/system/cpu/online */
  if (slurp("/sys/devices/system/cpu/online", buf, sizeof(buf)) <= 0 ||
      buf[0] != '0') {
    fail("sysfs-cpu");
    return 1;
  }
  ok("sysfs-cpu");

  emit("M34-PROC: done\n");
  return 0;
}
