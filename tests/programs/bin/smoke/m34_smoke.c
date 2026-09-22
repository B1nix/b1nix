/* SPDX-License-Identifier: GPL-2.0-only */
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
#include <sys/utsname.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <time.h>

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

/* Runs one tool to completion; returns 0 when it exited successfully. */
static int run_tool(char *const argv[]) {
  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    char *envp[] = {(char *)"PATH=/bin", NULL};
    /* Tool output is noise for the smoke log, and top's screen repaint is
     * large enough to matter on a serial console — keep the exit status,
     * drop the bytes. */
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, 1);
      close(devnull);
    }
    execve(argv[0], argv, envp);
    _exit(127);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) != pid)
    return -1;
  return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}


/* /proc/<pid>/wchan and the state letter, on a child that really is asleep.
 *
 * Both answer the same question -- what is this process doing -- and both used
 * to answer it wrongly: there was no wchan at all, and every sleeping task was
 * reported "D", which on Linux means a wait no signal can break. A child
 * parked in poll(NULL, 0, ...) is the simplest sleep there is, so its state
 * must be "S" and its wchan must name the call it is in. */
static int check_wchan_and_state(void) {
    int pid = fork();

    if (pid < 0)
        return 0;
    if (pid == 0) {
        poll(NULL, 0, 4000);
        _exit(0);
    }

    char path[64], buf[512];
    int state_ok = 0, wchan_ok = 0;

    /* Give it time to get INTO the sleep; a child that has not been scheduled
     * yet is legitimately "R". */
    for (int i = 0; i < 40 && !(state_ok && wchan_ok); i++) {
        usleep(50000);

        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        if (!state_ok && slurp(path, buf, sizeof(buf)) > 0) {
            /* Field 3, after the parenthesised comm. */
            char *close_paren = strrchr(buf, ')');

            if (close_paren && close_paren[1] == ' ')
                state_ok = (close_paren[2] == 'S');
        }
        snprintf(path, sizeof(path), "/proc/%d/wchan", pid);
        if (!wchan_ok && slurp(path, buf, sizeof(buf)) > 0)
            wchan_ok = (buf[0] != '\0' && buf[0] != '\n');
    }

    kill(pid, SIGKILL);
    int st = 0;
    waitpid(pid, &st, 0);
    return state_ok && wchan_ok;
}


/* /proc/uptime and CLOCK_MONOTONIC are the same clock on Linux, and programs
 * compare them: an agent that reads a process's start time in ticks since boot
 * and then measures against clock_gettime is doing exactly that. This kernel
 * keeps the counter and the counter-plus-a-base apart for good reasons (the
 * vDSO cannot carry the kernel's clamp, and the S3 resume re-anchors the
 * counter), so the two readings must still agree to well inside the tick. */
static int check_uptime_matches_monotonic(void) {
    char buf[128];
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    if (slurp("/proc/uptime", buf, sizeof(buf)) <= 0)
        return 0;

    double uptime = strtod(buf, NULL);
    double mono = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    double skew = uptime - mono;

    if (skew < 0)
        skew = -skew;
    if (skew < 0.05)
        return 1;

    char why[160];
    snprintf(why, sizeof(why),
             "M34-PROC: FAIL uptime-monotonic (/proc/uptime %.3f, CLOCK_MONOTONIC %.3f, %.3f s apart)\n",
             uptime, mono, skew);
    emit(why);
    return -1;
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
  struct utsname uts;
  if (uname(&uts) != 0) {
    fail("sysfs-osrelease");
    return 1;
  }
  if (slurp("/sys/kernel/osrelease", buf, sizeof(buf)) <= 0 ||
      !strstr(buf, uts.release)) {
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

  /* The procfs/sysfs-backed monitoring tools must actually run and read back
   * kernel state (they open /proc and /sys under the hood). Driven from here
   * because the Ring 3 test runner only ever execs a binary with no arguments,
   * and these need theirs. */
  if (run_tool((char *[]){(char *)"/bin/free", NULL}) == 0 &&
      run_tool((char *[]){(char *)"/bin/sysctl", (char *)"kernel.osrelease",
                          NULL}) == 0 &&
      run_tool((char *[]){(char *)"/bin/top", (char *)"-b", (char *)"-n",
                          (char *)"1", NULL}) == 0)
    ok("tools");
  else
    fail("tools");

  {
    int r = check_uptime_matches_monotonic();

    if (r > 0)
      ok("uptime-monotonic");
    else if (r == 0)
      fail("uptime-monotonic");
    /* r < 0 printed its own line with both readings. */
  }

  if (check_wchan_and_state())
    ok("wchan-state");
  else
    fail("wchan-state");

  emit("M34-PROC: done\n");
  return 0;
}
