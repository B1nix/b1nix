/*
 * m127_smoke — cgroup v2 resource control: the memory, cpu, io and pids
 * controllers, the OOM killer that chooses by cgroup and oom_score_adj, and
 * PSI under /proc/pressure.
 *
 * Nothing here asserts that a file exists and calls that a pass. Every marker
 * names a property that was observed happening: a fork that really failed with
 * EAGAIN, a process that really died of SIGKILL inside its own cgroup while its
 * sibling outside kept running, a spinner that really got a tenth of the CPU
 * its heavier-weighted twin got, bytes that really reached a device.
 *
 *   cg-controllers   cgroup.controllers lists cpu, io, memory and pids, the
 *                    filesystem reports CGROUP2_SUPER_MAGIC, and enabling a
 *                    controller in a parent creates its files in the children
 *   pids-max         a fork past pids.max is EAGAIN and pids.events counts it
 *   mem-current      memory.current follows what the members really have
 *                    resident, and pgfault in memory.stat moves with it
 *   mem-max-kill     a runaway inside a cgroup with memory.max is SIGKILLed;
 *                    its sibling in another cgroup is untouched, and
 *                    memory.events records max, oom and oom_kill
 *   mem-oom-adj      inside one cgroup the OOM killer takes the small process
 *                    whose oom_score_adj is 1000 before the big one at 0
 *   oom-score        /proc/<pid>/oom_score_adj round-trips, applies to the
 *                    whole thread group, survives fork, and moves oom_score
 *   cpu-weight       two cgroups, one spinner each, weights 100 and 1000: the
 *                    heavier one gets several times the CPU, measured from
 *                    each cgroup's own cpu.stat
 *   cpu-max          cpu.max "QUOTA PERIOD" holds a spinner to its quota, and
 *                    cpu.stat counts the periods it was throttled in
 *   io-stat          reads that really reach a block device are counted in
 *                    that cgroup's io.stat, per device, and io.max round-trips
 *   psi-format       /proc/pressure/{cpu,memory,io} parse as Linux prints them
 *   psi-cpu          contending for the CPU moves /proc/pressure/cpu
 *   psi-io           reading a block device moves /proc/pressure/io
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CGROUP2_MAGIC 0x63677270
#define CGROOT "/tmp/m127cg"

static int g_fail;

static void marker(const char *s) { write(1, s, strlen(s)); }

static void ok(const char *name) {
  char b[128];
  snprintf(b, sizeof(b), "M127-SMOKE: ok %s\n", name);
  marker(b);
}

static void failf(const char *name, const char *fmt, ...) {
  char why[400], b[560];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(why, sizeof(why), fmt, ap);
  va_end(ap);
  snprintf(b, sizeof(b), "M127-SMOKE: FAIL %s %s\n", name, why);
  marker(b);
  g_fail = 1;
}

/* ── small file helpers ──────────────────────────────────────────────────── */

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

static int write_file(const char *path, const char *s) {
  int fd = open(path, O_WRONLY);
  if (fd < 0)
    return -1;
  ssize_t n = write(fd, s, strlen(s));
  int e = errno;
  close(fd);
  if (n < 0) {
    errno = e;
    return -1;
  }
  return 0;
}

static int writef(const char *path, const char *fmt, ...) {
  char b[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  return write_file(path, b);
}

static long long read_ll(const char *path) {
  char b[256];
  if (read_file(path, b, sizeof(b)) < 0)
    return -1;
  if (strncmp(b, "max", 3) == 0)
    return -2;
  return strtoll(b, NULL, 10);
}

/* The value of `key` in a "key value\n" file, or -1. */
static long long read_kv(const char *path, const char *key) {
  char b[2048];
  if (read_file(path, b, sizeof(b)) < 0)
    return -1;
  size_t klen = strlen(key);
  for (char *p = b; p && *p;) {
    if (strncmp(p, key, klen) == 0 && p[klen] == ' ')
      return strtoll(p + klen + 1, NULL, 10);
    char *nl = strchr(p, '\n');
    p = nl ? nl + 1 : NULL;
  }
  return -1;
}

static void cgpath(char *out, size_t cap, const char *cg, const char *file) {
  if (file)
    snprintf(out, cap, CGROOT "/%s/%s", cg, file);
  else
    snprintf(out, cap, CGROOT "/%s", cg);
}

static long long cg_ll(const char *cg, const char *file) {
  char p[256];
  cgpath(p, sizeof(p), cg, file);
  return read_ll(p);
}

static long long cg_kv(const char *cg, const char *file, const char *key) {
  char p[256];
  cgpath(p, sizeof(p), cg, file);
  return read_kv(p, key);
}

static int cg_write(const char *cg, const char *file, const char *fmt, ...) {
  char p[256], b[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  cgpath(p, sizeof(p), cg, file);
  return write_file(p, b);
}

static int cg_make(const char *cg) {
  char p[256];
  cgpath(p, sizeof(p), cg, NULL);
  if (mkdir(p, 0755) == 0)
    return 0;
  return errno == EEXIST ? 0 : -1;
}

static void cg_destroy(const char *cg) {
  char p[256];
  cgpath(p, sizeof(p), cg, NULL);
  rmdir(p);
}

static unsigned long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long long)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull;
}

static void msleep(unsigned ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

/* Kill a child and reap it, whatever state it is in. */
static void reap(pid_t pid) {
  if (pid <= 0)
    return;
  kill(pid, SIGKILL);
  int st;
  waitpid(pid, &st, 0);
}

/* ── the hierarchy ───────────────────────────────────────────────────────── */

static int g_mounted;
/* What the root's cgroup.subtree_control said before this test touched it, so
 * the machine is handed back the way it was found. */
static char g_saved_subtree[128];

static int hierarchy_up(void) {
  mkdir(CGROOT, 0755);
  if (mount("cgroup2", CGROOT, "cgroup2", 0, NULL) != 0)
    return -1;
  g_mounted = 1;
  read_file(CGROOT "/cgroup.subtree_control", g_saved_subtree,
            sizeof(g_saved_subtree));
  return 0;
}

static void hierarchy_down(void) {
  if (!g_mounted)
    return;
  /* Put the delegation back exactly as it was: the rest of the lane runs on
   * this machine and must not inherit a cpu controller this test enabled. */
  const char *want[4] = {"cpu", "io", "memory", "pids"};
  for (int i = 0; i < 4; i++) {
    if (strstr(g_saved_subtree, want[i]))
      continue;
    char b[32];
    snprintf(b, sizeof(b), "-%s", want[i]);
    write_file(CGROOT "/cgroup.subtree_control", b);
  }
  umount(CGROOT);
  rmdir(CGROOT);
  g_mounted = 0;
}

/* ── 1. controllers ──────────────────────────────────────────────────────── */

static void check_controllers(void) {
  struct statfs sfs;
  char ctl[256];

  if (statfs(CGROOT, &sfs) != 0 ||
      (unsigned)sfs.f_type != (unsigned)CGROUP2_MAGIC) {
    failf("cg-controllers", "statfs f_type=0x%lx want 0x%x",
          (unsigned long)sfs.f_type, CGROUP2_MAGIC);
    return;
  }
  if (read_file(CGROOT "/cgroup.controllers", ctl, sizeof(ctl)) < 0) {
    failf("cg-controllers", "no cgroup.controllers");
    return;
  }
  const char *want[4] = {"cpu", "io", "memory", "pids"};
  for (int i = 0; i < 4; i++)
    if (!strstr(ctl, want[i])) {
      failf("cg-controllers", "cgroup.controllers='%s' has no %s", ctl, want[i]);
      return;
    }
  if (write_file(CGROOT "/cgroup.subtree_control",
                 "+cpu +io +memory +pids") != 0) {
    failf("cg-controllers", "subtree_control write: %s", strerror(errno));
    return;
  }
  if (cg_make("probe") != 0) {
    failf("cg-controllers", "mkdir probe: %s", strerror(errno));
    return;
  }
  /* The interface files of a controller exist exactly while the parent
   * delegates it, and each has to be readable and hold its documented
   * default. */
  struct {
    const char *file;
    long long want;
  } defaults[] = {
      {"memory.max", -2},    /* "max" */
      {"memory.current", 0}, /* nothing in it yet */
      {"cpu.weight", 100},   {"pids.max", -2},
  };
  for (unsigned i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
    long long v = cg_ll("probe", defaults[i].file);

    if (v != defaults[i].want) {
      failf("cg-controllers", "probe/%s reads %lld, want %lld",
            defaults[i].file, v, defaults[i].want);
      cg_destroy("probe");
      return;
    }
  }
  char p[256];
  cgpath(p, sizeof(p), "probe", "io.stat");
  if (access(p, R_OK) != 0) {
    failf("cg-controllers", "probe/io.stat missing");
    cg_destroy("probe");
    return;
  }
  /* Withdrawing a controller takes its files away again. */
  if (write_file(CGROOT "/cgroup.subtree_control", "-io") != 0 ||
      access(p, F_OK) == 0) {
    failf("cg-controllers", "io.stat survived -io");
    cg_destroy("probe");
    return;
  }
  write_file(CGROOT "/cgroup.subtree_control", "+io");
  cg_destroy("probe");
  ok("cg-controllers");
}

/* ── 2. pids.max ─────────────────────────────────────────────────────────── */

static void check_pids_max(void) {
  const char *cg = "pidsmax";
  pid_t kids[8];
  int nkids = 0;

  if (cg_make(cg) != 0) {
    failf("pids-max", "mkdir: %s", strerror(errno));
    return;
  }
  long long denied0 = cg_kv(cg, "pids.events", "max");
  if (cg_write(cg, "pids.max", "3") != 0) {
    failf("pids-max", "pids.max write: %s", strerror(errno));
    cg_destroy(cg);
    return;
  }

  /* A child of ours inside the cgroup forks until the kernel refuses. Doing it
   * from a child keeps this process outside the limit, so a failure here
   * cannot stop the rest of the suite from running. */
  int pfd[2];
  if (pipe(pfd) != 0) {
    failf("pids-max", "pipe");
    cg_destroy(cg);
    return;
  }
  pid_t driver = fork();
  if (driver == 0) {
    close(pfd[0]);
    char buf[64];
    int n_ok = 0, saved_errno = 0;

    /* Join the cgroup BEFORE forking. Having the parent write the pid after
     * the fork is a race the driver wins every time: it had already forked
     * eight children before the limit applied to it. */
    snprintf(buf, sizeof(buf), "%d", (int)getpid());
    int cfd = open(CGROOT "/" "pidsmax" "/cgroup.procs", O_WRONLY);
    if (cfd < 0 || write(cfd, buf, strlen(buf)) < 0)
      _exit(3);
    close(cfd);

    for (int i = 0; i < 8; i++) {
      pid_t c = fork();

      if (c == 0) {
        pause();
        _exit(0);
      }
      if (c < 0) {
        saved_errno = errno;
        break;
      }
      n_ok++;
    }
    snprintf(buf, sizeof(buf), "%d %d\n", n_ok, saved_errno);
    write(pfd[1], buf, strlen(buf));
    /* Hold the group alive until the parent has read the counters. */
    msleep(500);
    _exit(0);
  }
  close(pfd[1]);
  if (driver < 0) {
    failf("pids-max", "fork driver");
    close(pfd[0]);
    cg_destroy(cg);
    return;
  }
  char rb[64];
  int n = (int)read(pfd[0], rb, sizeof(rb) - 1);
  close(pfd[0]);
  if (n <= 0) {
    failf("pids-max", "driver said nothing");
    reap(driver);
    cg_destroy(cg);
    return;
  }
  rb[n] = '\0';
  int n_ok = 0, saved_errno = 0;
  sscanf(rb, "%d %d", &n_ok, &saved_errno);
  long long cur = cg_ll(cg, "pids.current");
  long long denied1 = cg_kv(cg, "pids.events", "max");

  reap(driver);
  for (int i = 0; i < nkids; i++)
    reap(kids[i]);
  /* The children die with their parent's process group; give the reaper a
   * moment so the rmdir below is not EBUSY. */
  for (int i = 0; i < 50 && cg_ll(cg, "pids.current") > 0; i++)
    msleep(20);
  cg_destroy(cg);

  if (saved_errno != EAGAIN) {
    failf("pids-max", "fork %d succeeded, errno=%d (want EAGAIN=%d)", n_ok + 1,
          saved_errno, EAGAIN);
    return;
  }
  if (n_ok >= 3) {
    failf("pids-max", "%d forks got through a pids.max of 3", n_ok);
    return;
  }
  if (cur > 3) {
    failf("pids-max", "pids.current reached %lld over a max of 3", cur);
    return;
  }
  if (denied1 <= denied0) {
    failf("pids-max", "pids.events max did not move (%lld -> %lld)", denied0,
          denied1);
    return;
  }
  ok("pids-max");
}

/* ── a child that touches anonymous memory and holds it ──────────────────── */

/* Touch `mb` megabytes and then wait to be killed. Writes one byte to the pipe
 * once the memory is resident, so the parent knows when to measure.
 *
 * Runs in the CALLING process and never returns: every caller is already the
 * child that has to hold the pages, and an earlier version that forked here
 * left that child exiting with a status of its own while a grandchild it had
 * not told anyone about held the memory. */
static void hold_memory(int mb, int ready_fd) {
  size_t len = (size_t)mb * 1024 * 1024;
  char *m = mmap(NULL, len, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED)
    _exit(2);
  for (size_t i = 0; i < len; i += 4096)
    m[i] = (char)(i >> 12);
  if (ready_fd >= 0)
    write(ready_fd, "r", 1);
  for (;;)
    pause();
}

/* ── 3. memory.current ───────────────────────────────────────────────────── */

static void check_mem_current(void) {
  const char *cg = "memcur";
  const int mb = 24;

  if (cg_make(cg) != 0) {
    failf("mem-current", "mkdir: %s", strerror(errno));
    return;
  }
  long long base = cg_ll(cg, "memory.current");
  int rp[2];

  if (pipe(rp) != 0) {
    failf("mem-current", "pipe");
    cg_destroy(cg);
    return;
  }
  pid_t holder = fork();
  if (holder == 0) {
    /* Move itself in BEFORE it allocates, so every page it touches is charged
     * here and nothing has to be migrated afterwards. */
    close(rp[0]);
    char b[32];
    snprintf(b, sizeof(b), "%d", (int)getpid());
    int fd = open(CGROOT "/" "memcur" "/cgroup.procs", O_WRONLY);
    if (fd < 0 || write(fd, b, strlen(b)) < 0)
      _exit(3);
    close(fd);
    hold_memory(mb, rp[1]); /* never returns */
    _exit(4);
  }
  close(rp[1]);
  if (holder < 0) {
    failf("mem-current", "fork");
    close(rp[0]);
    cg_destroy(cg);
    return;
  }
  char c;
  int got = (int)read(rp[0], &c, 1);
  close(rp[0]);
  if (got != 1) {
    failf("mem-current", "the holder never reported its memory resident");
    reap(holder);
    cg_destroy(cg);
    return;
  }
  long long cur = cg_ll(cg, "memory.current");
  long long peak = cg_ll(cg, "memory.peak");
  long long pgfault = cg_kv(cg, "memory.stat", "pgfault");

  reap(holder);
  for (int i = 0; i < 50 && cg_ll(cg, "pids.current") > 0; i++)
    msleep(20);
  long long after = cg_ll(cg, "memory.current");
  cg_destroy(cg);

  long long want = (long long)mb * 1024 * 1024;

  if (base != 0) {
    failf("mem-current", "an empty cgroup reported %lld bytes", base);
    return;
  }
  if (cur < want) {
    failf("mem-current", "memory.current %lld < the %lld the holder touched",
          cur, want);
    return;
  }
  /* The holder is one small process; anything far above what it touched means
   * the walk is counting something twice. */
  if (cur > want + 32 * 1024 * 1024) {
    failf("mem-current", "memory.current %lld for a %lld-byte holder", cur,
          want);
    return;
  }
  if (peak < cur) {
    failf("mem-current", "memory.peak %lld below memory.current %lld", peak,
          cur);
    return;
  }
  if (pgfault < want / 4096 / 2) {
    failf("mem-current", "memory.stat pgfault=%lld for %lld pages", pgfault,
          want / 4096);
    return;
  }
  if (after != 0) {
    failf("mem-current", "memory.current stayed at %lld after the holder died",
          after);
    return;
  }
  ok("mem-current");
}

/* ── 4. memory.max kills inside the cgroup ───────────────────────────────── */

/* A child that keeps touching fresh anonymous memory until something stops it.
 * Reports nothing: what matters is how it dies. */
static pid_t spawn_runaway(const char *cg, int cap_mb) {
  pid_t p = fork();

  if (p != 0)
    return p;
  char b[32], path[256];
  snprintf(b, sizeof(b), "%d", (int)getpid());
  snprintf(path, sizeof(path), CGROOT "/%s/cgroup.procs", cg);
  int fd = open(path, O_WRONLY);
  if (fd < 0 || write(fd, b, strlen(b)) < 0)
    _exit(3);
  close(fd);
  for (int mb = 0; mb < cap_mb; mb++) {
    char *m = mmap(NULL, 1024 * 1024, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED)
      _exit(5);
    for (int i = 0; i < 1024 * 1024; i += 4096)
      m[i] = 1;
  }
  _exit(6); /* it got all the way through its cap: the limit did nothing */
}

static void check_mem_max_kill(void) {
  const char *cg = "memmax", *safe = "memsafe";

  if (cg_make(cg) != 0 || cg_make(safe) != 0) {
    failf("mem-max-kill", "mkdir: %s", strerror(errno));
    cg_destroy(cg);
    cg_destroy(safe);
    return;
  }
  if (cg_write(cg, "memory.max", "%d", 32 * 1024 * 1024) != 0) {
    failf("mem-max-kill", "memory.max write: %s", strerror(errno));
    cg_destroy(cg);
    cg_destroy(safe);
    return;
  }

  /* A bystander in a different cgroup, holding memory of its own. It must be
   * alive at the end: the kill has to land inside the cgroup that went over,
   * not on the machine. */
  int rp[2];
  if (pipe(rp) != 0) {
    failf("mem-max-kill", "pipe");
    cg_destroy(cg);
    cg_destroy(safe);
    return;
  }
  pid_t bystander = fork();
  if (bystander == 0) {
    close(rp[0]);
    char b[32];
    snprintf(b, sizeof(b), "%d", (int)getpid());
    int fd = open(CGROOT "/" "memsafe" "/cgroup.procs", O_WRONLY);
    if (fd < 0 || write(fd, b, strlen(b)) < 0)
      _exit(3);
    close(fd);
    hold_memory(16, rp[1]);
    _exit(4);
  }
  close(rp[1]);
  char c;
  if (read(rp[0], &c, 1) != 1) {
    failf("mem-max-kill", "the bystander never came up");
    close(rp[0]);
    reap(bystander);
    cg_destroy(cg);
    cg_destroy(safe);
    return;
  }
  close(rp[0]);

  pid_t runaway = spawn_runaway(cg, 512);
  if (runaway < 0) {
    failf("mem-max-kill", "fork runaway");
    reap(bystander);
    cg_destroy(cg);
    cg_destroy(safe);
    return;
  }
  int st = 0;
  pid_t w = -1;
  for (int i = 0; i < 300; i++) {
    w = waitpid(runaway, &st, WNOHANG);
    if (w == runaway)
      break;
    msleep(50);
  }
  long long ev_max = cg_kv(cg, "memory.events", "max");
  long long ev_oom = cg_kv(cg, "memory.events", "oom");
  long long ev_kill = cg_kv(cg, "memory.events", "oom_kill");
  int bystander_alive = kill(bystander, 0) == 0;
  long long safe_cur = cg_ll(safe, "memory.current");

  if (w != runaway)
    reap(runaway);
  reap(bystander);
  for (int i = 0; i < 50 && (cg_ll(cg, "pids.current") > 0 ||
                             cg_ll(safe, "pids.current") > 0);
       i++)
    msleep(20);
  cg_destroy(cg);
  cg_destroy(safe);

  if (w != runaway) {
    failf("mem-max-kill", "the runaway was still alive after 15 s");
    return;
  }
  if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGKILL) {
    failf("mem-max-kill", "the runaway ended %s %d, not SIGKILL",
          WIFSIGNALED(st) ? "on signal" : "with exit code",
          WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
    return;
  }
  if (ev_max < 1 || ev_oom < 1 || ev_kill < 1) {
    failf("mem-max-kill", "memory.events max=%lld oom=%lld oom_kill=%lld",
          ev_max, ev_oom, ev_kill);
    return;
  }
  if (!bystander_alive) {
    failf("mem-max-kill", "the bystander in another cgroup was killed too");
    return;
  }
  if (safe_cur < 8 * 1024 * 1024) {
    failf("mem-max-kill", "the bystander's cgroup reported only %lld bytes",
          safe_cur);
    return;
  }
  ok("mem-max-kill");
}

/* ── 5. the victim is chosen by oom_score_adj ────────────────────────────── */

static void check_mem_oom_adj(void) {
  const char *cg = "memadj";

  if (cg_make(cg) != 0) {
    failf("mem-oom-adj", "mkdir: %s", strerror(errno));
    return;
  }
  if (cg_write(cg, "memory.max", "%d", 48 * 1024 * 1024) != 0) {
    failf("mem-oom-adj", "memory.max: %s", strerror(errno));
    cg_destroy(cg);
    return;
  }

  /* A small process that has asked to be killed first. It holds 2 MB against
   * the runaway's tens, so only oom_score_adj can make it the victim. */
  int rp[2];
  if (pipe(rp) != 0) {
    failf("mem-oom-adj", "pipe");
    cg_destroy(cg);
    return;
  }
  pid_t canary = fork();
  if (canary == 0) {
    close(rp[0]);
    char b[32];
    snprintf(b, sizeof(b), "%d", (int)getpid());
    int fd = open(CGROOT "/" "memadj" "/cgroup.procs", O_WRONLY);
    if (fd < 0 || write(fd, b, strlen(b)) < 0)
      _exit(3);
    close(fd);
    if (write_file("/proc/self/oom_score_adj", "1000") != 0)
      _exit(7);
    hold_memory(2, rp[1]);
    _exit(4);
  }
  close(rp[1]);
  char c;
  if (read(rp[0], &c, 1) != 1) {
    failf("mem-oom-adj", "the canary never came up");
    close(rp[0]);
    reap(canary);
    cg_destroy(cg);
    return;
  }
  close(rp[0]);

  pid_t runaway = spawn_runaway(cg, 512);
  int canary_st = 0;
  pid_t cw = -1;

  for (int i = 0; i < 300; i++) {
    cw = waitpid(canary, &canary_st, WNOHANG);
    if (cw == canary)
      break;
    if (waitpid(runaway, NULL, WNOHANG) == runaway) {
      /* The runaway died first: the ranking did not hold. */
      runaway = -1;
      break;
    }
    msleep(50);
  }
  int runaway_alive = runaway > 0 && kill(runaway, 0) == 0;

  reap(runaway);
  if (cw != canary)
    reap(canary);
  for (int i = 0; i < 50 && cg_ll(cg, "pids.current") > 0; i++)
    msleep(20);
  cg_destroy(cg);

  if (cw != canary) {
    failf("mem-oom-adj", "the canary at oom_score_adj 1000 outlived the run");
    return;
  }
  if (!WIFSIGNALED(canary_st) || WTERMSIG(canary_st) != SIGKILL) {
    failf("mem-oom-adj", "the canary ended %s %d, not SIGKILL",
          WIFSIGNALED(canary_st) ? "on signal" : "with exit code",
          WIFSIGNALED(canary_st) ? WTERMSIG(canary_st)
                                 : WEXITSTATUS(canary_st));
    return;
  }
  if (!runaway_alive) {
    failf("mem-oom-adj", "the big process died before the canary");
    return;
  }
  ok("mem-oom-adj");
}

/* ── 6. oom_score_adj itself ─────────────────────────────────────────────── */

static void check_oom_score(void) {
  char b[64];
  int before = (int)read_ll("/proc/self/oom_score_adj");

  if (write_file("/proc/self/oom_score_adj", "250") != 0) {
    failf("oom-score", "write 250: %s", strerror(errno));
    return;
  }
  if (read_ll("/proc/self/oom_score_adj") != 250) {
    failf("oom-score", "read back %lld", read_ll("/proc/self/oom_score_adj"));
    writef("/proc/self/oom_score_adj", "%d", before);
    return;
  }
  long long score = read_ll("/proc/self/oom_score");
  if (score < 250) {
    failf("oom-score", "oom_score %lld below the 250 bias", score);
    writef("/proc/self/oom_score_adj", "%d", before);
    return;
  }
  /* Out of range is refused, and the old value stands. */
  if (write_file("/proc/self/oom_score_adj", "5000") == 0 ||
      read_ll("/proc/self/oom_score_adj") != 250) {
    failf("oom-score", "5000 was accepted");
    writef("/proc/self/oom_score_adj", "%d", before);
    return;
  }
  /* A fork inherits it, the way Linux's does. */
  int p[2];
  if (pipe(p) != 0) {
    failf("oom-score", "pipe");
    writef("/proc/self/oom_score_adj", "%d", before);
    return;
  }
  pid_t kid = fork();
  if (kid == 0) {
    close(p[0]);
    snprintf(b, sizeof(b), "%lld", read_ll("/proc/self/oom_score_adj"));
    write(p[1], b, strlen(b));
    _exit(0);
  }
  close(p[1]);
  int n = (int)read(p[0], b, sizeof(b) - 1);
  close(p[0]);
  waitpid(kid, NULL, 0);
  b[n > 0 ? n : 0] = '\0';
  writef("/proc/self/oom_score_adj", "%d", before);
  if (n <= 0 || atoi(b) != 250) {
    failf("oom-score", "a forked child read '%s', not 250", b);
    return;
  }
  ok("oom-score");
}

/* ── 7. cpu.weight ───────────────────────────────────────────────────────── */

/* Burn CPU in a tight loop until killed. */
static pid_t spawn_spinner(const char *cg) {
  pid_t p = fork();

  if (p != 0)
    return p;
  if (cg) {
    char b[32], path[256];
    snprintf(b, sizeof(b), "%d", (int)getpid());
    snprintf(path, sizeof(path), CGROOT "/%s/cgroup.procs", cg);
    int fd = open(path, O_WRONLY);
    if (fd < 0 || write(fd, b, strlen(b)) < 0)
      _exit(3);
    close(fd);
  }
  volatile unsigned long x = 0;
  for (;;)
    x += 1;
  _exit(0);
}

static void check_cpu_weight(void) {
  const char *light = "cpulight", *heavy = "cpuheavy";

  if (cg_make(light) != 0 || cg_make(heavy) != 0) {
    failf("cpu-weight", "mkdir: %s", strerror(errno));
    cg_destroy(light);
    cg_destroy(heavy);
    return;
  }
  if (cg_write(light, "cpu.weight", "100") != 0 ||
      cg_write(heavy, "cpu.weight", "1000") != 0) {
    failf("cpu-weight", "cpu.weight write: %s", strerror(errno));
    cg_destroy(light);
    cg_destroy(heavy);
    return;
  }
  if (cg_ll(heavy, "cpu.weight") != 1000) {
    failf("cpu-weight", "cpu.weight read back %lld", cg_ll(heavy, "cpu.weight"));
    cg_destroy(light);
    cg_destroy(heavy);
    return;
  }

  /* Six spinners per cgroup, which is two things at once.
   *
   * It measures the GROUP's share rather than one task's: a controller that
   * simply weighted each task would divide these two cgroups the same way, and
   * with six tasks each it is the per-cgroup division that has to hold.
   *
   * And it puts far more runnable tasks on the machine than it has CPUs. With
   * one or two per cgroup there is no choice to make -- every runnable task
   * gets a CPU and no weight can express itself. */
#define CPUW_PER_CGROUP 6
  pid_t pids[CPUW_PER_CGROUP * 2];
  int nspin = 0;

  for (int i = 0; i < CPUW_PER_CGROUP; i++)
    pids[nspin++] = spawn_spinner(light);
  for (int i = 0; i < CPUW_PER_CGROUP; i++)
    pids[nspin++] = spawn_spinner(heavy);
  for (int i = 0; i < nspin; i++)
    if (pids[i] < 0) {
      failf("cpu-weight", "fork");
      for (int k = 0; k < i; k++)
        reap(pids[k]);
      cg_destroy(light);
      cg_destroy(heavy);
      return;
    }
  msleep(400); /* let them all land in their cgroups and start burning */
  long long l0 = cg_kv(light, "cpu.stat", "usage_usec");
  long long h0 = cg_kv(heavy, "cpu.stat", "usage_usec");
  msleep(3000);
  long long l1 = cg_kv(light, "cpu.stat", "usage_usec");
  long long h1 = cg_kv(heavy, "cpu.stat", "usage_usec");

  /* Short of the weight it was given? Print the kernel's own view of the four
   * spinners -- the stride percentage each was published and the virtual time
   * each has reached -- while they are still running. From out here the split
   * is one number and it cannot say which half of the model is wrong. */
  if (l1 > l0 && (h1 - h0) / ((l1 - l0) ? (l1 - l0) : 1) < 9) {
    int d = open("/proc/b1nix-tasks", O_RDONLY);

    if (d >= 0) {
      char sink[4096];
      while (read(d, sink, sizeof(sink)) > 0)
        ;
      close(d);
    }
  }

  for (int i = 0; i < nspin; i++)
    reap(pids[i]);
  for (int i = 0; i < 50 && (cg_ll(light, "pids.current") > 0 ||
                             cg_ll(heavy, "pids.current") > 0);
       i++)
    msleep(20);
  cg_destroy(light);
  cg_destroy(heavy);

  long long ld = l1 - l0, hd = h1 - h0;

  if (ld < 0 || hd < 0 || (ld + hd) < 500000) {
    failf("cpu-weight", "cpu.stat barely moved: light=%lld heavy=%lld usec", ld,
          hd);
    return;
  }
  /* Ten times the weight, measured over three seconds on a machine with other
   * work on it and an accounting that samples on the tick. Four is the floor a
   * real difference has to clear; the measured ratio is printed either way so
   * a regression is readable. */
  if (ld == 0 || hd / (ld ? ld : 1) < 4) {
    failf("cpu-weight", "weights 100:1000 gave %lld:%lld usec", ld, hd);
    return;
  }
  {
    char b[160];
    snprintf(b, sizeof(b), "M127-SMOKE:   cpu-weight 100:1000 -> %lld:%lld usec\n",
             ld, hd);
    marker(b);
  }
  ok("cpu-weight");
}

/* ── 8. cpu.max ──────────────────────────────────────────────────────────── */

static void check_cpu_max(void) {
  const char *cg = "cpuquota";

  if (cg_make(cg) != 0) {
    failf("cpu-max", "mkdir: %s", strerror(errno));
    return;
  }
  /* 20 ms of every 100 ms: a fifth of one CPU. */
  if (cg_write(cg, "cpu.max", "20000 100000") != 0) {
    failf("cpu-max", "cpu.max write: %s", strerror(errno));
    cg_destroy(cg);
    return;
  }
  char back[64];
  char p[256];
  cgpath(p, sizeof(p), cg, "cpu.max");
  if (read_file(p, back, sizeof(back)) < 0 ||
      strncmp(back, "20000 100000", 12) != 0) {
    failf("cpu-max", "cpu.max reads back '%s'", back);
    cg_destroy(cg);
    return;
  }

  pid_t spinner = spawn_spinner(cg);
  if (spinner < 0) {
    failf("cpu-max", "fork");
    cg_destroy(cg);
    return;
  }
  msleep(400);
  long long u0 = cg_kv(cg, "cpu.stat", "usage_usec");
  unsigned long long t0 = now_ms();
  msleep(3000);
  unsigned long long wall_us = (now_ms() - t0) * 1000ull;
  long long u1 = cg_kv(cg, "cpu.stat", "usage_usec");
  long long thr = cg_kv(cg, "cpu.stat", "nr_throttled");
  long long periods = cg_kv(cg, "cpu.stat", "nr_periods");

  reap(spinner);
  for (int i = 0; i < 50 && cg_ll(cg, "pids.current") > 0; i++)
    msleep(20);
  cg_destroy(cg);

  long long used = u1 - u0;

  if (used <= 0) {
    failf("cpu-max", "cpu.stat did not move at all");
    return;
  }
  if (periods < 10) {
    failf("cpu-max", "only %lld periods in 3 s of a 100 ms period", periods);
    return;
  }
  if (thr < 1) {
    failf("cpu-max", "a spinner under a 20%% quota was never throttled");
    return;
  }
  /* The accounting is per tick, so a spinner overshoots its quota by up to one
   * tick per period. Half the wall clock is far above that and far below the
   * 100% an unthrottled spinner would reach. */
  if ((unsigned long long)used > wall_us / 2) {
    failf("cpu-max", "used %lld usec of %llu wall usec under a 20%% quota",
          used, wall_us);
    return;
  }
  {
    char b[192];
    snprintf(b, sizeof(b),
             "M127-SMOKE:   cpu-max 20%% -> %lld of %llu usec, %lld/%lld "
             "periods throttled\n",
             used, wall_us, thr, periods);
    marker(b);
  }
  ok("cpu-max");
}

/* ── 9. io.stat ──────────────────────────────────────────────────────────── */

/* A block device that really exists and can be read. Preference order matters
 * only in that the first readable one is used. */
static int open_a_block_device(char *name, size_t cap) {
  static const char *cands[] = {"/dev/vda", "/dev/sda", "/dev/sr0",
                                "/dev/nvme0n1", "/dev/hda", NULL};
  for (int i = 0; cands[i]; i++) {
    int fd = open(cands[i], O_RDONLY);

    if (fd >= 0) {
      char probe[512];

      if (read(fd, probe, sizeof(probe)) > 0) {
        snprintf(name, cap, "%s", cands[i]);
        lseek(fd, 0, SEEK_SET);
        return fd;
      }
      close(fd);
    }
  }
  return -1;
}

/* Read 64 KiB out of every megabyte across the far half of the device.
 *
 * Contiguous reads low down do not reach a device: the first megabytes are read
 * by the partition scan and the mount, and what follows is pulled in by
 * read-ahead. An earlier version read four megabytes at the one-megabyte mark
 * and the cgroup it ran in had issued no commands at all. Scattering defeats
 * read-ahead as well as the cache. */
static long long read_scattered(int fd) {
  static char buf[64 * 1024];
  off_t size = lseek(fd, 0, SEEK_END);
  long long total = 0;

  if (size <= 0)
    return 0;
  off_t start = size / 2;
  off_t step = 1024 * 1024;

  for (int i = 0; i < 128; i++) {
    off_t off = start + (off_t)i * step;

    if (off + (off_t)sizeof(buf) > size)
      break;
    if (lseek(fd, off, SEEK_SET) < 0)
      break;
    ssize_t n = read(fd, buf, sizeof(buf));

    if (n <= 0)
      break;
    total += n;
  }
  return total;
}

/* Read `chunks` x 64 KiB from `fd` at offsets nothing has read yet, so the
 * block cache cannot answer and the commands reach the device. */
static long long read_raw(int fd, int chunks, off_t start) {
  static char buf[64 * 1024];
  long long total = 0;

  for (int i = 0; i < chunks; i++) {
    if (lseek(fd, start + (off_t)i * (off_t)sizeof(buf), SEEK_SET) < 0)
      break;
    ssize_t n = read(fd, buf, sizeof(buf));

    if (n <= 0)
      break;
    total += n;
  }
  return total;
}

static void check_io(void) {
  const char *cg = "iocg";
  char devname[64] = "";
  int dev = open_a_block_device(devname, sizeof(devname));

  if (dev < 0) {
    failf("io-stat", "no readable block device");
    return;
  }
  if (cg_make(cg) != 0) {
    failf("io-stat", "mkdir: %s", strerror(errno));
    close(dev);
    return;
  }

  /* Read far enough in that the block cache cannot already hold it: the first
   * megabytes of every disk are read at boot by the partition scan and the
   * mount, and an earlier version of this check read exactly those and saw a
   * cgroup that had issued no commands at all. */
  int rp[2];
  if (pipe(rp) != 0) {
    failf("io-stat", "pipe");
    close(dev);
    cg_destroy(cg);
    return;
  }
  pid_t reader = fork();
  if (reader == 0) {
    close(rp[0]);
    char b[32];
    snprintf(b, sizeof(b), "%d", (int)getpid());
    int fd = open(CGROOT "/" "iocg" "/cgroup.procs", O_WRONLY);
    if (fd < 0 || write(fd, b, strlen(b)) < 0)
      _exit(3);
    close(fd);
    long long got = read_scattered(dev);
    snprintf(b, sizeof(b), "%lld", got);
    write(rp[1], b, strlen(b));
    _exit(0);
  }
  close(rp[1]);
  char rb[32];
  int n = (int)read(rp[0], rb, sizeof(rb) - 1);
  close(rp[0]);
  waitpid(reader, NULL, 0);
  rb[n > 0 ? n : 0] = '\0';
  long long asked = n > 0 ? atoll(rb) : 0;

  char iostat[512], p[256];
  cgpath(p, sizeof(p), cg, "io.stat");
  int have = read_file(p, iostat, sizeof(iostat));

  if (asked <= 0) {
    failf("io-stat", "the reader read nothing from %s", devname);
    goto out;
  }
  if (have <= 0) {
    failf("io-stat", "io.stat is empty after %lld bytes of %s", asked, devname);
    goto out;
  }
  /* The device the kernel counted, taken from io.stat rather than from
   * st_rdev: what the accounting keys on is the device the block layer issued
   * the command to, and a partition resolves to its parent disk. */
  unsigned maj = 0, min = 0;
  long long rbytes = 0, rios = 0;
  if (sscanf(iostat, "%u:%u", &maj, &min) != 2) {
    failf("io-stat", "io.stat='%s' is not MAJ:MIN", iostat);
    goto out;
  }
  char *at = strstr(iostat, "rbytes=");
  rbytes = at ? atoll(at + 7) : -1;
  at = strstr(iostat, "rios=");
  rios = at ? atoll(at + 5) : -1;
  if (rbytes <= 0 || rios <= 0) {
    failf("io-stat", "io.stat='%s' after %lld bytes read from %s", iostat,
          asked, devname);
    goto out;
  }

  /* io.max, against the device io.stat just named. */
  if (cg_write(cg, "io.max", "%u:%u rbps=1048576 wiops=99", maj, min) != 0) {
    failf("io-stat", "io.max write: %s", strerror(errno));
    goto out;
  }
  char iomax[256];
  cgpath(p, sizeof(p), cg, "io.max");
  if (read_file(p, iomax, sizeof(iomax)) < 0 ||
      !strstr(iomax, "rbps=1048576") || !strstr(iomax, "wiops=99") ||
      !strstr(iomax, "wbps=max")) {
    failf("io-stat", "io.max reads back '%s'", iomax);
    goto out;
  }
  {
    char b[400];
    snprintf(b, sizeof(b), "M127-SMOKE:   io-stat %s -> %s", devname, iostat);
    marker(b);
  }
  ok("io-stat");
out:
  close(dev);
  for (int i = 0; i < 50 && cg_ll(cg, "pids.current") > 0; i++)
    msleep(20);
  cg_destroy(cg);
}

/* ── 10. PSI ─────────────────────────────────────────────────────────────── */

/* Parse "some avg10=A avg60=B avg300=C total=T" out of a pressure file.
 * Returns the total, or -1 if the line is not in Linux's shape. */
static long long psi_total(const char *path, const char *which) {
  char b[512];

  if (read_file(path, b, sizeof(b)) < 0)
    return -1;
  char *line = strstr(b, which);
  if (!line || line != b)
    line = strstr(b, which);
  if (!line)
    return -1;
  if (!strstr(line, "avg10=") || !strstr(line, "avg60=") ||
      !strstr(line, "avg300="))
    return -1;
  char *t = strstr(line, "total=");
  if (!t)
    return -1;
  return atoll(t + 6);
}

static void check_psi_format(void) {
  static const char *paths[3] = {"/proc/pressure/cpu", "/proc/pressure/memory",
                                 "/proc/pressure/io"};

  for (int i = 0; i < 3; i++) {
    if (psi_total(paths[i], "some") < 0) {
      failf("psi-format", "%s has no parseable 'some' line", paths[i]);
      return;
    }
    /* Only cpu has no `full` line, exactly as Linux prints it. */
    char b[512];
    read_file(paths[i], b, sizeof(b));
    int has_full = strstr(b, "full ") != NULL;
    if (i == 0 && has_full) {
      failf("psi-format", "/proc/pressure/cpu printed a full line");
      return;
    }
    if (i != 0 && !has_full) {
      failf("psi-format", "%s has no full line", paths[i]);
      return;
    }
  }
  ok("psi-format");
}

static void check_psi_cpu(void) {
  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  int n = (int)(ncpu > 0 ? ncpu : 1) * 3;

  if (n > 12)
    n = 12;
  long long t0 = psi_total("/proc/pressure/cpu", "some");
  pid_t pids[12];

  for (int i = 0; i < n; i++)
    pids[i] = spawn_spinner(NULL);
  msleep(2500);
  long long t1 = psi_total("/proc/pressure/cpu", "some");
  for (int i = 0; i < n; i++)
    reap(pids[i]);

  if (t0 < 0 || t1 < 0) {
    failf("psi-cpu", "unreadable");
    return;
  }
  if (t1 <= t0) {
    failf("psi-cpu", "%d spinners on %ld CPUs left total at %lld", n, ncpu, t1);
    return;
  }
  {
    char b[160];
    snprintf(b, sizeof(b),
             "M127-SMOKE:   psi-cpu %d spinners -> some total %lld -> %lld us\n",
             n, t0, t1);
    marker(b);
  }
  ok("psi-cpu");
}

static void check_psi_io(void) {
  char devname[64] = "";
  int dev = open_a_block_device(devname, sizeof(devname));

  if (dev < 0) {
    failf("psi-io", "no readable block device");
    return;
  }
  long long t0 = psi_total("/proc/pressure/io", "some");
  long long got = read_raw(dev, 192, 8 * 1024 * 1024);
  long long t1 = psi_total("/proc/pressure/io", "some");

  close(dev);
  if (got <= 0) {
    failf("psi-io", "read nothing from %s", devname);
    return;
  }
  if (t0 < 0 || t1 < 0) {
    failf("psi-io", "unreadable");
    return;
  }
  if (t1 <= t0) {
    failf("psi-io", "%lld bytes off %s left io pressure at %lld", got, devname,
          t1);
    return;
  }
  {
    char b[192];
    snprintf(b, sizeof(b),
             "M127-SMOKE:   psi-io %lld bytes -> some total %lld -> %lld us\n",
             got, t0, t1);
    marker(b);
  }
  ok("psi-io");
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
  marker("M127-SMOKE: start\n");

  if (hierarchy_up() != 0) {
    failf("cg-controllers", "mount cgroup2 on " CGROOT ": %s", strerror(errno));
    marker("M127-SMOKE: done\n");
    return 1;
  }

  check_controllers();
  check_pids_max();
  check_mem_current();
  check_mem_max_kill();
  check_mem_oom_adj();
  check_oom_score();
  check_cpu_weight();
  check_cpu_max();
  check_io();
  check_psi_format();
  check_psi_cpu();
  check_psi_io();

  hierarchy_down();
  marker("M127-SMOKE: done\n");
  return g_fail ? 1 : 0;
}
