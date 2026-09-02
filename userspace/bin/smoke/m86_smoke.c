/* M86 smoke: per-thread CPU accounting, thread-directed signals, and
 * pthread_exit return values.
 *
 * Every marker is emitted only after the operation ran AND its result was
 * checked against something the test knows independently.
 *
 *   thread-cputime    CLOCK_THREAD_CPUTIME_ID advances with the CPU a thread
 *                     actually burns and stays flat while it sleeps — the two
 *                     cases a monotonic-uptime stand-in cannot tell apart.
 *   process-cputime   CLOCK_PROCESS_CPUTIME_ID counts every thread in the
 *                     group, so it exceeds the caller's own thread clock once a
 *                     sibling has burned CPU.
 *   getcpuclockid     the per-thread clock id pthread_getcpuclockid() hands out
 *                     reads the same time as that thread's own
 *                     CLOCK_THREAD_CPUTIME_ID, and the resolution is nanoseconds.
 *   rusage-thread     getrusage(RUSAGE_THREAD) is the calling thread while
 *                     RUSAGE_SELF is the whole process, and both carry
 *                     sub-tick (microsecond) precision.
 *   times-process     times(2) reports process CPU time that grows across a
 *                     burn and covers all threads.
 *   rusage-children   getrusage(RUSAGE_CHILDREN) picks up a reaped child's CPU
 *                     time.
 *   proc-stat-times   /proc/self/stat fields 14/15 (utime/stime) are non-zero
 *                     after a burn and field 20 counts the live threads.
 *   tkill-self        tkill(gettid(), sig) delivers to the calling thread.
 *   tgkill-thread     tgkill()/pthread_kill() delivers to the NAMED thread,
 *                     not to whichever one the kernel finds first.
 *   tgkill-esrch      tgkill() with the wrong tgid, and tkill() of a dead tid,
 *                     both fail with ESRCH instead of signalling someone else.
 *   kill-unblocked    kill(getpid(), sig) with the signal blocked in the main
 *                     thread is delivered to a sibling that has it unblocked —
 *                     process-directed, not leader-directed.
 *   rusage-maxrss     ru_maxrss is a high-water mark: it survives the munmap of
 *                     the pages that produced it.
 *   group-stop-blocked  a stop signal sent to a process stops even the threads
 *                     parked in a blocking syscall, and SIGCONT resumes them.
 *   pthread-exit-rv   pthread_exit(v) delivers v to pthread_join, including
 *                     from a deep call and after cleanup handlers run.
 *   pthread-exit-main pthread_exit() in main keeps the process alive until the
 *                     last thread finishes, and the exit status is 0.
 */
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef RUSAGE_THREAD
#define RUSAGE_THREAD 1
#endif

static int g_fail;

static void marker(const char *s) {
  write(1, s, strlen(s));
  write(1, "\n", 1);
}

static void ok(const char *name) {
  char line[128];
  snprintf(line, sizeof(line), "M86-SMOKE: ok %s", name);
  marker(line);
}

static void fail(const char *name, long v) {
  char line[160];
  snprintf(line, sizeof(line), "M86-SMOKE: FAIL %s (%ld, errno=%d)", name, v,
           errno);
  marker(line);
  g_fail = 1;
}

static void check(const char *name, int cond, long v) {
  if (cond)
    ok(name);
  else
    fail(name, v);
}

static pid_t my_tid(void) { return (pid_t)syscall(SYS_gettid); }

static long long ns_of(const struct timespec *ts) {
  return (long long)ts->tv_sec * 1000000000LL + ts->tv_nsec;
}

static long long clock_ns(clockid_t id) {
  struct timespec ts;
  if (clock_gettime(id, &ts) != 0)
    return -1;
  return ns_of(&ts);
}

static volatile unsigned long g_sink;

/* Burn at least `ms` milliseconds of this thread's own CPU TIME, doing work the
 * compiler cannot fold away.
 *
 * It used to bound the loop by wall time, and that is not the same quantity on
 * a machine that is sharing a host. The suite runs four QEMU instances at once
 * under TCG, so a thread can spend 120 ms of wall clock and be given four
 * milliseconds of CPU -- which is exactly what getrusage(RUSAGE_THREAD) then
 * honestly reported, and what the >= 50 ms assertion below read as a kernel
 * bug. Bounding by the thread clock makes the burn mean what its name says and
 * the assertion sound at any host load.
 *
 * The wall-clock backstop is not a bound on the burn, it is a bound on the
 * damage if CLOCK_THREAD_CPUTIME_ID never advances: without it a broken thread
 * clock would hang the instance instead of failing the check that is here to
 * catch it. */
static void burn_ms(int ms) {
  struct timespec wall0, now;
  long long cpu0 = clock_ns(CLOCK_THREAD_CPUTIME_ID);
  unsigned long acc = 1;

  clock_gettime(CLOCK_MONOTONIC, &wall0);
  long long last_cpu = cpu0;
  int advanced = 0;
  for (;;) {
    for (int i = 0; i < 200000; i++)
      acc = acc * 1103515245UL + 12345UL;
    g_sink = acc;

    clock_gettime(CLOCK_MONOTONIC, &now);
    long long wall = (now.tv_sec - wall0.tv_sec) * 1000LL +
                     (now.tv_nsec - wall0.tv_nsec) / 1000000LL;
    if (cpu0 < 0) {
      /* No thread clock at all: fall back to wall time so the check that is
       * about to fail still gets to run. */
      if (wall >= ms)
        return;
      continue;
    }

    long long cpu = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    if (cpu >= 0 && cpu - cpu0 >= (long long)ms * 1000000LL)
      return;

    /* Give up only if the thread clock is STUCK, never merely because it is
     * slow. A fixed wall-clock ceiling looked like the safe thing and was not:
     * with four QEMU instances on one host a thread can be given a few percent
     * of a CPU, so the ceiling cut the burn short and the caller then measured
     * the shortfall rather than the kernel -- 29 ms of sibling time where the
     * test wanted 150. As long as the clock advances, keep burning. */
    if (cpu > last_cpu) {
      last_cpu = cpu;
      advanced = 1;
    }
    /* "Broken" means the thread clock NEVER advances, not that it advances
     * slowly. Both threads in this test burn at once, so on one vCPU under host
     * contention either of them can go many seconds without being scheduled --
     * and a 5 s no-progress rule cut the sibling's burn short, leaving the
     * caller to measure the shortfall (25 ms of sibling time where the test
     * wanted 150). Once the clock has moved at all it is working; keep burning
     * until the target, under a generous absolute ceiling so a wedge is still
     * bounded. */
    if ((!advanced && wall > 10000) || wall > 60000)
      return;
  }
}

/* ── 1: per-thread CPU time ─────────────────────────────────────────────── */

static void test_thread_cputime(void) {
  long long t0 = clock_ns(CLOCK_THREAD_CPUTIME_ID);
  if (t0 < 0) {
    fail("thread-cputime-gettime", t0);
    return;
  }
  burn_ms(120);
  long long t1 = clock_ns(CLOCK_THREAD_CPUTIME_ID);
  long long burned = t1 - t0;

  /* Sleeping is not CPU time: 150 ms of sleep must cost far less than the
   * 120 ms burn did. This is what fails when the "CPU clock" is really the
   * uptime clock in disguise. */
  long long t2 = clock_ns(CLOCK_THREAD_CPUTIME_ID);
  usleep(150000);
  long long t3 = clock_ns(CLOCK_THREAD_CPUTIME_ID);
  long long slept = t3 - t2;

  if (burned < 50000000LL) { /* ≥ 50 ms of the 120 ms burn landed on us */
    fail("thread-cputime-burn", (long)(burned / 1000000));
    return;
  }
  if (slept > 40000000LL) { /* a 150 ms sleep must not look like 40 ms of CPU */
    fail("thread-cputime-sleep", (long)(slept / 1000000));
    return;
  }
  ok("thread-cputime");
}

struct burner_arg {
  int ms;
  long long cpu_ns;   /* thread's own CPU time, filled in by the thread */
  long long own_u_us; /* ...split into user and system, by the same thread */
  long long own_s_us;
  pid_t tid;
  clockid_t clk;
  volatile int started;
};

static void *burner_entry(void *p) {
  struct burner_arg *a = (struct burner_arg *)p;
  a->tid = my_tid();
  a->started = 1;
  long long t0 = clock_ns(CLOCK_THREAD_CPUTIME_ID);
  struct rusage r0;
  if (getrusage(RUSAGE_THREAD, &r0) != 0)
    memset(&r0, 0, sizeof(r0));
  burn_ms(a->ms);
  a->cpu_ns = clock_ns(CLOCK_THREAD_CPUTIME_ID) - t0;
  /* This thread's own view of the time it just burned, split into user and
   * system. The group total is assembled from exactly these numbers, so when
   * the two disagree the diagnostic in test_rusage_and_times can say which
   * side lost them. */
  struct rusage r1;
  if (getrusage(RUSAGE_THREAD, &r1) == 0) {
    a->own_u_us = (long long)(r1.ru_utime.tv_sec - r0.ru_utime.tv_sec) * 1000000LL +
                  (r1.ru_utime.tv_usec - r0.ru_utime.tv_usec);
    a->own_s_us = (long long)(r1.ru_stime.tv_sec - r0.ru_stime.tv_sec) * 1000000LL +
                  (r1.ru_stime.tv_usec - r0.ru_stime.tv_usec);
  }
  return (void *)0x8686;
}

static void test_process_cputime(void) {
  long long own0 = clock_ns(CLOCK_THREAD_CPUTIME_ID);
  long long proc0 = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
  if (proc0 < 0) {
    fail("process-cputime-gettime", proc0);
    return;
  }

  struct burner_arg a = {.ms = 150};
  pthread_t th;
  if (pthread_create(&th, 0, burner_entry, &a) != 0) {
    fail("process-cputime-create", -1);
    return;
  }
  void *rv = 0;
  if (pthread_join(th, &rv) != 0 || (unsigned long)rv != 0x8686) {
    fail("process-cputime-join", (long)(unsigned long)rv);
    return;
  }

  long long own1 = clock_ns(CLOCK_THREAD_CPUTIME_ID);
  long long proc1 = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
  long long own_delta = own1 - own0;
  long long proc_delta = proc1 - proc0;

  /* The sibling burned ≥ 150 ms of wall time; this thread only waited. So the
   * process clock must have advanced substantially more than our own. */
  if (a.cpu_ns < 50000000LL) {
    fail("process-cputime-sibling", (long)(a.cpu_ns / 1000000));
    return;
  }
  if (proc_delta <= own_delta + 40000000LL) {
    fail("process-cputime-sum", (long)((proc_delta - own_delta) / 1000000));
    return;
  }
  ok("process-cputime");
}

static void *idclock_entry(void *p) {
  struct burner_arg *a = (struct burner_arg *)p;
  a->tid = my_tid();
  if (pthread_getcpuclockid(pthread_self(), &a->clk) != 0)
    a->clk = (clockid_t)-1;
  a->started = 1;
  burn_ms(120);
  a->cpu_ns = clock_ns(CLOCK_THREAD_CPUTIME_ID);
  /* Hold the thread alive until the main thread has read our clock. */
  while (a->ms == 0)
    usleep(2000);
  return 0;
}

static void test_getcpuclockid(void) {
  struct burner_arg a = {.ms = 0};
  pthread_t th;
  if (pthread_create(&th, 0, idclock_entry, &a) != 0) {
    fail("getcpuclockid-create", -1);
    return;
  }
  while (!a.started || a.cpu_ns == 0)
    usleep(2000);

  if (a.clk == (clockid_t)-1) {
    fail("getcpuclockid-id", -1);
    a.ms = 1;
    pthread_join(th, 0);
    return;
  }
  long long via_id = clock_ns(a.clk);
  struct timespec res;
  int rres = clock_getres(a.clk, &res);
  a.ms = 1;
  pthread_join(th, 0);

  if (via_id < 0) {
    fail("getcpuclockid-gettime", (long)via_id);
    return;
  }
  /* The thread sampled its own clock just before we read it through the id, so
   * the two must agree closely (the thread only spun a couple of usleeps in
   * between). */
  long long diff = via_id - a.cpu_ns;
  if (diff < 0)
    diff = -diff;
  if (via_id < 50000000LL || diff > 50000000LL) {
    fail("getcpuclockid-match", (long)(diff / 1000000));
    return;
  }
  if (rres != 0 || res.tv_sec != 0 || res.tv_nsec > 1000) {
    fail("getcpuclockid-res", (long)res.tv_nsec);
    return;
  }
  ok("getcpuclockid");
}

/* ── 2: rusage / times ──────────────────────────────────────────────────── */

static void test_rusage_and_times(void) {
  struct rusage self0, thr0;
  if (getrusage(RUSAGE_SELF, &self0) != 0 ||
      getrusage(RUSAGE_THREAD, &thr0) != 0) {
    fail("rusage-get", -1);
    return;
  }
  struct tms tms0;
  clock_t wall0 = times(&tms0);
  if (wall0 == (clock_t)-1) {
    fail("times-call", -1);
    return;
  }

  struct burner_arg a = {.ms = 150};
  pthread_t th;
  if (pthread_create(&th, 0, burner_entry, &a) != 0) {
    fail("rusage-create", -1);
    return;
  }
  burn_ms(120);
  /* The group total with the sibling STILL ALIVE, so its time is being summed
   * from its live slot rather than from whatever the exit path folded into the
   * leader. If this already falls short, the loss is in the live accounting; if
   * only the post-join figure does, the loss is in the fold. */
  struct rusage self_live;
  long long live_us = 0;
  if (getrusage(RUSAGE_SELF, &self_live) == 0)
    live_us = (long long)(self_live.ru_utime.tv_sec - self0.ru_utime.tv_sec) *
                  1000000LL +
              (self_live.ru_utime.tv_usec - self0.ru_utime.tv_usec);
  pthread_join(th, 0);

  struct rusage self1, thr1;
  getrusage(RUSAGE_SELF, &self1);
  getrusage(RUSAGE_THREAD, &thr1);
  struct tms tms1;
  clock_t wall1 = times(&tms1);

  long long self_us =
      (long long)(self1.ru_utime.tv_sec - self0.ru_utime.tv_sec) * 1000000LL +
      (self1.ru_utime.tv_usec - self0.ru_utime.tv_usec);
  long long thr_us =
      (long long)(thr1.ru_utime.tv_sec - thr0.ru_utime.tv_sec) * 1000000LL +
      (thr1.ru_utime.tv_usec - thr0.ru_utime.tv_usec);

  if (thr_us < 50000) {
    fail("rusage-thread-burn", (long)(thr_us / 1000));
    return;
  }
  /* RUSAGE_SELF covers this thread AND the sibling, so it must exceed the
   * calling thread's own time by roughly the sibling's burn. */
  if (self_us < thr_us + 50000) {
    /* Say WHICH half is short. The sibling burns until its thread clock --
     * user PLUS system -- has advanced 150 ms, but this check reads ru_utime
     * alone, so a kernel that credits a compute loop's time to the wrong half
     * fails here with no hint that the split is the problem rather than the
     * total. Report both. */
    long long self_sys =
        (long long)(self1.ru_stime.tv_sec - self0.ru_stime.tv_sec) * 1000000LL +
        (self1.ru_stime.tv_usec - self0.ru_stime.tv_usec);
    long long thr_sys =
        (long long)(thr1.ru_stime.tv_sec - thr0.ru_stime.tv_sec) * 1000000LL +
        (thr1.ru_stime.tv_usec - thr0.ru_stime.tv_usec);
    char d[160];
    snprintf(d, sizeof(d),
             "M86-SMOKE: note rusage-split d_self_u=%ldms d_thr_u=%ldms "
             "d_live_u=%ldms sib_cpu=%ldms sib_u=%ldms sib_s=%ldms "
             "abs self0=%ldms self1=%ldms thr0=%ldms thr1=%ldms "
             "self_s=%ldms thr_s=%ldms",
             (long)(self_us / 1000), (long)(thr_us / 1000),
             (long)(live_us / 1000), (long)(a.cpu_ns / 1000000),
             (long)(a.own_u_us / 1000), (long)(a.own_s_us / 1000),
             (long)(self0.ru_utime.tv_sec * 1000 +
                    self0.ru_utime.tv_usec / 1000),
             (long)(self1.ru_utime.tv_sec * 1000 +
                    self1.ru_utime.tv_usec / 1000),
             (long)(thr0.ru_utime.tv_sec * 1000 +
                    thr0.ru_utime.tv_usec / 1000),
             (long)(thr1.ru_utime.tv_sec * 1000 +
                    thr1.ru_utime.tv_usec / 1000),
             (long)(self_sys / 1000), (long)(thr_sys / 1000));
    marker(d);
    fail("rusage-self-group", (long)((self_us - thr_us) / 1000));
    return;
  }
  /* Nanosecond accounting means the microsecond field carries information: a
   * pure 10 ms-tick counter can only ever produce multiples of 10000. */
  if ((self1.ru_utime.tv_usec % 10000) == 0 &&
      (thr1.ru_utime.tv_usec % 10000) == 0 &&
      (self1.ru_stime.tv_usec % 10000) == 0) {
    fail("rusage-precision", (long)self1.ru_utime.tv_usec);
    return;
  }
  if (self1.ru_nvcsw + self1.ru_nivcsw <= 0) {
    fail("rusage-ctxsw", (long)(self1.ru_nvcsw + self1.ru_nivcsw));
    return;
  }
  ok("rusage-thread");

  /* times(2): process CPU time, in USER_HZ ticks, covering both threads. */
  clock_t dut = tms1.tms_utime - tms0.tms_utime;
  if (wall1 == (clock_t)-1 || wall1 < wall0) {
    fail("times-wall", (long)wall1);
    return;
  }
  if (dut < 10) { /* ≥ 100 ms of the ≥ 270 ms burned by the two threads */
    fail("times-utime", (long)dut);
    return;
  }
  ok("times-process");
}

static void test_rusage_children(void) {
  struct rusage ch0, ch1;
  if (getrusage(RUSAGE_CHILDREN, &ch0) != 0) {
    fail("rusage-children-get", -1);
    return;
  }
  pid_t pid = fork();
  if (pid == 0) {
    burn_ms(150);
    _exit(0);
  }
  if (pid < 0) {
    fail("rusage-children-fork", pid);
    return;
  }
  int st = 0;
  if (waitpid(pid, &st, 0) != pid) {
    fail("rusage-children-wait", -1);
    return;
  }
  getrusage(RUSAGE_CHILDREN, &ch1);
  long long d_us =
      (long long)(ch1.ru_utime.tv_sec - ch0.ru_utime.tv_sec) * 1000000LL +
      (ch1.ru_utime.tv_usec - ch0.ru_utime.tv_usec);
  check("rusage-children", d_us >= 50000, (long)(d_us / 1000));
}

/* /proc/self/stat: pid (comm) state ppid pgrp session tty tpgid flags minflt
 * cminflt majflt cmajflt utime stime ... — fields are 1-based, so utime is 14,
 * stime 15 and num_threads 20. The comm field is parenthesised and may contain
 * spaces, so parsing starts after the last ')'. */

/* The width of the line, not just its contents.
 *
 * A reader that wants field 23 counts tokens; b1nix printed 24 of the 52
 * fields Linux does, and printed FOUR for a task that had already exited
 * ("<pid> (gone) Z 0"). Chromium reads a field by index, checks the count, and
 * traps when the line is short — and its crash handler walks every thread of
 * the process, so a thread that died mid-walk hit the short line and brought
 * the browser down, repeatedly. Both widths are checked here: a live task and
 * one that has just exited but not yet been reaped. */
static int stat_field_count(const char *path) {
  char buf[1024];
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  long n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return -1;
  buf[n] = 0;
  char *close_paren = strrchr(buf, ')');
  if (!close_paren)
    return -1;
  int count = 2; /* pid and (comm), which the paren split already accounted for */
  for (char *tok = strtok(close_paren + 1, " \t\n"); tok;
       tok = strtok(0, " \t\n"))
    count++;
  return count;
}

static void test_proc_stat_width(void) {
  int live = stat_field_count("/proc/self/stat");
  check("proc-stat-width", live == 52, live);

  /* The line for a task that is no longer there.
   *
   * The descriptor is opened while the child is alive and read after it has
   * gone, which is the same sequence a crash handler walks when a thread dies
   * mid-dump — and the case that used to answer with four fields. Reading
   * through the open handle avoids depending on how long a pid directory
   * outlives its task, which is a separate question. */
  int rep[2];
  if (pipe(rep) != 0) {
    fail("proc-stat-width-gone", -1);
    return;
  }
  pid_t pid = fork();
  if (pid < 0) {
    fail("proc-stat-width-gone", -1);
    return;
  }
  if (pid == 0) {
    char c;
    close(rep[1]);
    read(rep[0], &c, 1); /* wait until the parent has the file open */
    _exit(0);
  }
  close(rep[0]);

  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    fail("proc-stat-width-gone", fd);
    close(rep[1]);
    waitpid(pid, 0, 0);
    return;
  }
  close(rep[1]); /* release the child */
  waitpid(pid, 0, 0);

  char buf[1024];
  long n = pread(fd, buf, sizeof(buf) - 1, 0);
  close(fd);
  if (n <= 0) {
    fail("proc-stat-width-gone", n);
    return;
  }
  buf[n] = 0;
  char *close_paren = strrchr(buf, ')');
  int count = 2;
  if (close_paren)
    for (char *tok = strtok(close_paren + 1, " \t\n"); tok;
         tok = strtok(0, " \t\n"))
      count++;
  check("proc-stat-width-gone", close_paren && count == 52, count);
}

static void test_proc_stat_times(void) {
  burn_ms(120);
  struct burner_arg a = {.ms = 200};
  pthread_t th;
  if (pthread_create(&th, 0, burner_entry, &a) != 0) {
    fail("proc-stat-create", -1);
    return;
  }
  while (!a.started)
    usleep(2000);

  char buf[1024];
  int fd = open("/proc/self/stat", O_RDONLY);
  if (fd < 0) {
    fail("proc-stat-open", fd);
    pthread_join(th, 0);
    return;
  }
  long n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) {
    fail("proc-stat-read", n);
    pthread_join(th, 0);
    return;
  }
  buf[n] = 0;

  char *close_paren = strrchr(buf, ')');
  if (!close_paren) {
    fail("proc-stat-comm", 0);
    pthread_join(th, 0);
    return;
  }
  /* Field 3 (state) is the first token after ')'. */
  long fields[24];
  int nf = 0;
  char *p = close_paren + 1;
  char *tok = strtok(p, " \t\n"); /* state — skipped, not numeric */
  tok = strtok(0, " \t\n");
  while (tok && nf < 22) {
    fields[nf++] = strtol(tok, 0, 10);
    tok = strtok(0, " \t\n");
  }
  pthread_join(th, 0);

  /* fields[0] is field 4 (ppid), so field N is fields[N - 4]. */
  if (nf < 17) {
    fail("proc-stat-fields", nf);
    return;
  }
  long utime = fields[14 - 4];
  long stime = fields[15 - 4];
  long threads = fields[20 - 4];
  if (utime < 5) {
    fail("proc-stat-utime", utime);
    return;
  }
  if (stime < 0 || utime + stime < 5) {
    fail("proc-stat-stime", stime);
    return;
  }
  if (threads < 2) {
    fail("proc-stat-threads", threads);
    return;
  }
  ok("proc-stat-times");
}

/* ru_maxrss must be a PEAK, not the current footprint: touch 8 MiB, drop it,
 * and the reported figure has to remember the 8 MiB. */
static void test_rusage_maxrss(void) {
  struct rusage before;
  if (getrusage(RUSAGE_SELF, &before) != 0) {
    fail("maxrss-get", -1);
    return;
  }
  const size_t span = 8u * 1024 * 1024;
  char *p = mmap(0, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                 -1, 0);
  if (p == MAP_FAILED) {
    fail("maxrss-mmap", -1);
    return;
  }
  for (size_t i = 0; i < span; i += 4096)
    p[i] = (char)(i >> 12); /* fault every page in */
  struct rusage peak;
  getrusage(RUSAGE_SELF, &peak);
  munmap(p, span);
  struct rusage after;
  getrusage(RUSAGE_SELF, &after);

  long grew = peak.ru_maxrss - before.ru_maxrss;
  if (grew < 4096) { /* ≥ 4 MiB of the 8 MiB shows up as resident */
    fail("maxrss-growth", grew);
    return;
  }
  /* After the unmap the pages are gone, but the high-water mark is not. */
  check("rusage-maxrss", after.ru_maxrss >= peak.ru_maxrss,
        after.ru_maxrss - peak.ru_maxrss);
}

/* Read the single-character state field of /proc/<tid>/stat (field 3, right
 * after the parenthesised comm). Returns 0 if it cannot be read. */
static char proc_state(pid_t tid) {
  char path[64], buf[512];
  snprintf(path, sizeof(path), "/proc/%d/stat", (int)tid);
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return 0;
  long n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return 0;
  buf[n] = 0;
  char *close_paren = strrchr(buf, ')');
  if (!close_paren)
    return 0;
  char *p = close_paren + 1;
  while (*p == ' ')
    p++;
  return *p;
}

/* A stop signal sent to the PROCESS must reach a thread parked in a blocking
 * syscall, not just the threads that happen to be running. */
struct blocker_arg {
  volatile pid_t tid;
  int fd;
  volatile int woke;
};

static void *blocker_entry(void *p) {
  struct blocker_arg *a = (struct blocker_arg *)p;
  a->tid = my_tid();
  char c;
  /* Blocks until the main thread writes — a genuinely sleeping syscall. */
  while (read(a->fd, &c, 1) < 0 && errno == EINTR)
    ;
  a->woke = 1;
  return 0;
}

static void test_group_stop_blocked(void) {
  int fds[2];
  if (pipe(fds) != 0) {
    fail("group-stop-pipe", -1);
    return;
  }
  /* Run the whole thing in a child: stopping our own process would stop the
   * test itself, and only a separate process can be resumed from outside. */
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[1]);
    struct blocker_arg a;
    memset((void *)&a, 0, sizeof(a));
    a.fd = fds[0];
    pthread_t th;
    if (pthread_create(&th, 0, blocker_entry, &a) != 0)
      _exit(4);
    while (!a.tid)
      usleep(2000);
    /* Publish the blocked thread's tid, then idle so the parent can stop us. */
    char line[32];
    int n = snprintf(line, sizeof(line), "%d\n", (int)a.tid);
    if (write(2, line, (size_t)n) < 0)
      _exit(5);
    for (;;)
      usleep(5000);
  }
  if (pid < 0) {
    fail("group-stop-fork", pid);
    return;
  }
  close(fds[0]);
  usleep(120000); /* let the child create the thread and block it */

  /* The child's threads: its tids follow its pid, and /proc/<pid>/task lists
   * them. Find the one that is not the leader. */
  pid_t blocked = 0;
  {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/task", (int)pid);
    DIR *d = opendir(path);
    if (d) {
      struct dirent *e;
      while ((e = readdir(d)) != 0) {
        int tid = atoi(e->d_name);
        if (tid > 0 && tid != (int)pid)
          blocked = (pid_t)tid;
      }
      closedir(d);
    }
  }
  if (blocked == 0) {
    fail("group-stop-find-thread", 0);
    kill(pid, SIGKILL);
    waitpid(pid, 0, 0);
    return;
  }

  if (kill(pid, SIGSTOP) != 0) {
    fail("group-stop-kill", -1);
    kill(pid, SIGKILL);
    waitpid(pid, 0, 0);
    return;
  }
  int st = 0;
  if (waitpid(pid, &st, WUNTRACED) != pid || !WIFSTOPPED(st)) {
    fail("group-stop-wait", st);
    kill(pid, SIGKILL);
    waitpid(pid, 0, 0);
    return;
  }
  /* The blocked thread must be stopped too — 'T' in its own /proc entry. */
  char state = 0;
  for (int i = 0; i < 100; i++) {
    state = proc_state(blocked);
    if (state == 'T')
      break;
    usleep(5000);
  }
  int stopped_ok = (state == 'T');

  kill(pid, SIGCONT);
  usleep(50000);
  char after = proc_state(blocked);
  kill(pid, SIGKILL);
  waitpid(pid, 0, 0);
  close(fds[1]);

  check("group-stop-blocked", stopped_ok && after != 'T',
        (long)state * 1000 + after);
}

/* ── 3: thread-directed signals ─────────────────────────────────────────── */

static volatile pid_t g_sig_tid;
static volatile int g_sig_count;
static volatile int g_sig_seen;

static void usr_handler(int sig) {
  (void)sig;
  g_sig_tid = my_tid();
  g_sig_count++;
  g_sig_seen = 1;
}

static void test_tkill_self(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = usr_handler;
  sigaction(SIGUSR1, &sa, 0);

  g_sig_tid = 0;
  g_sig_seen = 0;
  pid_t me = my_tid();
  if (syscall(SYS_tkill, me, SIGUSR1) != 0) {
    fail("tkill-self-call", -1);
    return;
  }
  for (int i = 0; i < 200 && !g_sig_seen; i++)
    usleep(2000);
  check("tkill-self", g_sig_seen && g_sig_tid == me, (long)g_sig_tid);
}

struct target_arg {
  volatile pid_t tid;
  volatile int handled_by; /* tid that ran the handler */
  volatile int stop;
  int unblock_sig;
};

static struct target_arg *g_target;

static void target_handler(int sig) {
  (void)sig;
  if (g_target)
    g_target->handled_by = (int)my_tid();
}

static void *target_entry(void *p) {
  struct target_arg *a = (struct target_arg *)p;
  a->tid = my_tid();
  sigset_t set;
  sigemptyset(&set);
  if (a->unblock_sig) {
    /* This thread is the only one with the signal unblocked. */
    sigaddset(&set, a->unblock_sig);
    pthread_sigmask(SIG_UNBLOCK, &set, 0);
  }
  while (!a->stop)
    usleep(2000);
  return 0;
}

static void test_tgkill_thread(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = target_handler;
  sigaction(SIGUSR2, &sa, 0);

  struct target_arg a;
  memset((void *)&a, 0, sizeof(a));
  g_target = &a;
  pthread_t th;
  if (pthread_create(&th, 0, target_entry, &a) != 0) {
    fail("tgkill-create", -1);
    return;
  }
  while (!a.tid)
    usleep(2000);

  /* Address the sibling by tid: the handler must run ON that thread. */
  if (syscall(SYS_tgkill, getpid(), a.tid, SIGUSR2) != 0) {
    fail("tgkill-call", -1);
    a.stop = 1;
    pthread_join(th, 0);
    return;
  }
  for (int i = 0; i < 200 && !a.handled_by; i++)
    usleep(2000);
  int handled = a.handled_by;
  pid_t tid = a.tid;

  /* ESRCH cases: wrong thread group for a live tid, and a tid that is not a
   * task at all. */
  errno = 0;
  long wrong_group = syscall(SYS_tgkill, getpid() + 100000, tid, SIGUSR2);
  int wrong_errno = errno;
  errno = 0;
  long dead_tid = syscall(SYS_tkill, 0x7ffffff, SIGUSR2);
  int dead_errno = errno;

  a.stop = 1;
  pthread_join(th, 0);
  g_target = 0;

  check("tgkill-thread", handled == (int)tid, (long)handled);
  check("tgkill-esrch",
        wrong_group == -1 && wrong_errno == ESRCH && dead_tid == -1 &&
            dead_errno == ESRCH,
        (long)wrong_errno * 1000 + dead_errno);
}

/* kill(2) targets the PROCESS: with SIGUSR2 blocked in the main thread and
 * unblocked in a sibling, the sibling must be the one that runs the handler. */
static void test_kill_unblocked(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = target_handler;
  sigaction(SIGUSR2, &sa, 0);

  sigset_t block, old;
  sigemptyset(&block);
  sigaddset(&block, SIGUSR2);
  pthread_sigmask(SIG_BLOCK, &block, &old);

  struct target_arg a;
  memset((void *)&a, 0, sizeof(a));
  a.unblock_sig = SIGUSR2;
  g_target = &a;
  pthread_t th;
  if (pthread_create(&th, 0, target_entry, &a) != 0) {
    fail("kill-unblocked-create", -1);
    pthread_sigmask(SIG_SETMASK, &old, 0);
    return;
  }
  while (!a.tid)
    usleep(2000);
  usleep(20000); /* let the sibling reach its unblock + wait loop */

  if (kill(getpid(), SIGUSR2) != 0) {
    fail("kill-unblocked-call", -1);
    a.stop = 1;
    pthread_join(th, 0);
    pthread_sigmask(SIG_SETMASK, &old, 0);
    return;
  }
  for (int i = 0; i < 300 && !a.handled_by; i++)
    usleep(2000);
  int handled = a.handled_by;
  pid_t tid = a.tid;
  a.stop = 1;
  pthread_join(th, 0);
  g_target = 0;
  pthread_sigmask(SIG_SETMASK, &old, 0);

  check("kill-unblocked", handled == (int)tid, (long)handled);
}

/* A signal HANDLER must also run in a task that is making no syscalls at all.
 * Every other check here signals a target inside a syscall or about to enter
 * one, so they pass on a kernel whose only delivery points are the syscall and
 * fault paths. That was exactly aarch64: its IRQ handler took no register
 * frame and never looked at pending signals, while x86_64 delivers from its
 * timer vector. Default-action *termination* hides the gap, because the
 * scheduler kills such a task itself on the next switch without needing a user
 * frame — running a handler is the part that cannot be faked, since it has to
 * rewrite the interrupted userspace context.
 *
 * The child's loop is finite on purpose: a kernel that never delivers must fail
 * this check, not hang the instance. It also has to OUTLAST the parent, which
 * is the half that was wrong -- at four billion iterations the child finished
 * its loop and became a zombie inside the parent's 100 ms sleep on a loaded
 * host, and the check then failed on a kill to a pid that no longer had a live
 * task rather than on anything about signal delivery. The bound is now far
 * beyond any run's patience; what actually ends the child is the parent's
 * SIGKILL below, after its own bounded wait. */
static volatile sig_atomic_t g_compute_hit;

static void compute_loop_handler(int sig) {
  (void)sig;
  g_compute_hit = 1;
  _exit(7); /* the handler ran — report it through the exit status */
}

static void test_signal_compute_loop(void) {
  pid_t pid = fork();
  if (pid < 0) {
    fail("signal-compute-loop-fork", -1);
    return;
  }
  if (pid == 0) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = compute_loop_handler;
    sigaction(SIGUSR1, &sa, 0);
    /* No syscalls past this point, deliberately. */
    volatile unsigned long acc = 1;
    for (unsigned long i = 0; i < 400000000000UL; i++)
      acc = acc * 1103515245UL + 12345UL;
    _exit(g_compute_hit ? 7 : 0); /* 0 = the handler never ran */
  }

  usleep(100000); /* let the child leave fork bookkeeping for the loop */
  if (kill(pid, SIGUSR1) != 0) {
    /* Say what became of the child. A failed kill is almost never about the
     * kill: on a loaded host the child can finish its (deliberately finite)
     * loop and become a zombie before the parent's 100 ms sleep returns, and
     * "kill failed with errno 3" then hides the one fact that explains the
     * run -- whether the child exited on its own or died of something. */
    int s = 0;
    kill(pid, SIGKILL);
    if (waitpid(pid, &s, 0) == pid) {
      char d[128];
      snprintf(d, sizeof(d),
               "M86-SMOKE: note compute-loop child already gone: %s %d",
               WIFEXITED(s) ? "exited" : "signalled",
               WIFEXITED(s) ? WEXITSTATUS(s) : WTERMSIG(s));
      marker(d);
    }
    fail("signal-compute-loop-kill", -1);
    return;
  }

  int status = 0;
  for (int i = 0; i < 600; i++) { /* up to ~60 s; a working kernel takes one tick */
    if (waitpid(pid, &status, WNOHANG) == pid) {
      check("signal-compute-loop", WIFEXITED(status) && WEXITSTATUS(status) == 7,
            WIFEXITED(status) ? (long)WEXITSTATUS(status)
                              : -(long)WTERMSIG(status));
      return;
    }
    usleep(100000);
  }
  kill(pid, SIGKILL);
  { int s = 0; waitpid(pid, &s, 0); }
  fail("signal-compute-loop", -2);
}

/* ── 4: pthread_exit ────────────────────────────────────────────────────── */

static volatile int g_cleanup_ran;

static void cleanup_cb(void *arg) { g_cleanup_ran = (int)(long)arg; }

static void exit_deep(int depth, void *rv) {
  if (depth > 0) {
    exit_deep(depth - 1, rv);
    return;
  }
  pthread_exit(rv);
}

static void *exit_rv_entry(void *arg) {
  pthread_cleanup_push(cleanup_cb, (void *)0x77);
  exit_deep(4, arg);
  pthread_cleanup_pop(0);
  return (void *)0xBAD;
}

static void test_pthread_exit_retval(void) {
  g_cleanup_ran = 0;
  pthread_t th;
  void *want = (void *)0xFEEDBEEFUL;
  if (pthread_create(&th, 0, exit_rv_entry, want) != 0) {
    fail("pthread-exit-create", -1);
    return;
  }
  void *rv = (void *)0x1;
  if (pthread_join(th, &rv) != 0) {
    fail("pthread-exit-join", -1);
    return;
  }
  if (rv != want) {
    fail("pthread-exit-value", (long)(unsigned long)rv);
    return;
  }
  if (g_cleanup_ran != 0x77) {
    fail("pthread-exit-cleanup", g_cleanup_ran);
    return;
  }
  /* pthread_exit(PTHREAD_CANCELED-like sentinel) and a plain return must both
   * make it through the same path. */
  if (pthread_create(&th, 0, burner_entry,
                     &(struct burner_arg){.ms = 1}) != 0) {
    fail("pthread-exit-create2", -1);
    return;
  }
  rv = 0;
  pthread_join(th, &rv);
  check("pthread-exit-rv", (unsigned long)rv == 0x8686, (long)(unsigned long)rv);
}

/* pthread_exit() from main must NOT tear the process down: the remaining thread
 * keeps running and the process exits 0 once it finishes. Run in a child so the
 * test binary itself survives. */
static void *late_writer(void *arg) {
  int fd = (int)(long)arg;
  usleep(120000);
  const char msg[] = "late";
  ssize_t w = write(fd, msg, 4);
  (void)w;
  close(fd);
  return 0;
}

static void test_pthread_exit_main(void) {
  int fds[2];
  if (pipe(fds) != 0) {
    fail("pthread-exit-main-pipe", -1);
    return;
  }
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    pthread_t th;
    int rc = pthread_create(&th, 0, late_writer, (void *)(long)fds[1]);
    if (rc != 0)
      _exit(rc & 0x7f);
    pthread_exit(0); /* main thread leaves; the process must live on */
  }
  if (pid < 0) {
    fail("pthread-exit-main-fork", pid);
    return;
  }
  close(fds[1]);
  char buf[8] = {0};
  ssize_t n = read(fds[0], buf, sizeof(buf));
  close(fds[0]);
  int st = 0;
  waitpid(pid, &st, 0);
  check("pthread-exit-main",
        n == 4 && memcmp(buf, "late", 4) == 0 && WIFEXITED(st) &&
            WEXITSTATUS(st) == 0,
        (long)n * 1000 + st);
}

int main(void) {
  marker("M86-SMOKE: start");

  test_thread_cputime();
  test_process_cputime();
  test_getcpuclockid();
  test_rusage_and_times();
  test_rusage_children();
  test_proc_stat_times();
  test_proc_stat_width();
  test_rusage_maxrss();
  test_group_stop_blocked();

  test_tkill_self();
  test_tgkill_thread();
  test_kill_unblocked();
  test_signal_compute_loop();

  test_pthread_exit_retval();
  test_pthread_exit_main();

  marker(g_fail ? "M86-SMOKE: done with failures" : "M86-SMOKE: done");
  return g_fail ? 1 : 0;
}
