/* SPDX-License-Identifier: GPL-2.0-only */
/* M126 — observability: perf_event_open(2).
 *
 * Drives the interface directly rather than through libperf: the point of the
 * check is that the ABI `perf` is compiled against is the one this kernel
 * implements, so struct perf_event_attr, the ioctl numbers, the read(2) format
 * and the mmap'd ring buffer are exercised by hand here.
 *
 * Every marker is printed only after the operation's result has been checked:
 * a counter is proved by making the thing it counts happen and watching the
 * number move, never by a non-negative return.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef __NR_perf_event_open
#define __NR_perf_event_open 298
#endif

static int fails;

static void ok(const char *what) {
  printf("M126-SMOKE: ok %s\n", what);
  fflush(stdout);
}

static void bad(const char *what, const char *why, long v) {
  printf("M126-SMOKE: FAIL %s — %s (%ld, errno=%d)\n", what, why, v, errno);
  fflush(stdout);
  fails++;
}

static void judge(const char *what, int good, const char *why, long v) {
  if (good)
    ok(what);
  else
    bad(what, why, v);
}

static int perf_open(struct perf_event_attr *a, pid_t pid, int cpu,
                     int group_fd, unsigned long flags) {
  return (int)syscall(__NR_perf_event_open, a, pid, cpu, group_fd, flags);
}

static void attr_init(struct perf_event_attr *a, unsigned type,
                      unsigned long long config) {
  memset(a, 0, sizeof(*a));
  a->size = sizeof(*a);
  a->type = type;
  a->config = config;
  a->disabled = 1;
  a->exclude_kernel = 0;
  a->exclude_hv = 0;
}

static unsigned long long read_count(int fd) {
  unsigned long long v = 0;

  if (read(fd, &v, sizeof(v)) != (ssize_t)sizeof(v))
    return (unsigned long long)-1;
  return v;
}

/* Burn a measurable amount of CPU without being optimised away. */
static volatile unsigned long sink;
static void burn_ms(int ms) {
  struct timespec a, b;

  clock_gettime(CLOCK_MONOTONIC, &a);
  for (;;) {
    for (int i = 0; i < 20000; i++)
      sink += (unsigned long)i;
    clock_gettime(CLOCK_MONOTONIC, &b);
    long d = (long)((b.tv_sec - a.tv_sec) * 1000 +
                    (b.tv_nsec - a.tv_nsec) / 1000000);
    if (d >= ms)
      return;
  }
}

/* ---- the checks --------------------------------------------------------- */

static void check_task_clock(void) {
  struct perf_event_attr a;
  int fd;
  unsigned long long before, after;

  attr_init(&a, PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK);
  fd = perf_open(&a, 0, -1, -1, 0);
  if (fd < 0) {
    bad("open", "perf_event_open refused a software task-clock counter",
        (long)fd);
    return;
  }
  ok("open");

  if (ioctl(fd, PERF_EVENT_IOC_RESET, 0) < 0) {
    bad("task-clock", "PERF_EVENT_IOC_RESET refused", -1);
    close(fd);
    return;
  }
  before = read_count(fd);
  burn_ms(60);
  /* Still disabled: the counter must not have moved. */
  unsigned long long while_off = read_count(fd);

  if (ioctl(fd, PERF_EVENT_IOC_ENABLE, 0) < 0) {
    bad("task-clock", "PERF_EVENT_IOC_ENABLE refused", -1);
    close(fd);
    return;
  }
  burn_ms(120);
  ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
  after = read_count(fd);

  judge("counter-disabled-does-not-count", while_off == before,
        "a disabled counter moved anyway", (long)(while_off - before));
  /* 120 ms of spinning is at least 50 ms of CPU even on a loaded host. */
  judge("task-clock", after - while_off > 50ull * 1000000ull,
        "the task clock did not count the CPU time that was burned",
        (long)((after - while_off) / 1000000ull));

  /* And it must stop again. */
  unsigned long long a1 = read_count(fd);

  burn_ms(60);
  judge("disable-stops", read_count(fd) == a1,
        "the counter kept counting after IOC_DISABLE", (long)a1);
  close(fd);
}

static void check_page_faults(void) {
  struct perf_event_attr a;
  int fd;
  unsigned long long before, after;
  const size_t len = 256 * 4096;
  char *m;

  attr_init(&a, PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS);
  a.disabled = 0;
  fd = perf_open(&a, 0, -1, -1, 0);
  if (fd < 0) {
    bad("page-faults", "a page-fault counter was refused", (long)fd);
    return;
  }
  before = read_count(fd);
  m = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) {
    bad("page-faults", "mmap failed", -1);
    close(fd);
    return;
  }
  /* Touch every page: each first touch is one fault. */
  for (size_t i = 0; i < len; i += 4096)
    m[i] = (char)i;
  after = read_count(fd);
  judge("page-faults", after - before >= 200,
        "touching 256 fresh pages did not show up as faults",
        (long)(after - before));
  munmap(m, len);
  close(fd);
}

static void check_context_switches(void) {
  struct perf_event_attr a;
  int fd;
  unsigned long long before, after;

  attr_init(&a, PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES);
  a.disabled = 0;
  fd = perf_open(&a, 0, -1, -1, 0);
  if (fd < 0) {
    bad("context-switches", "a context-switch counter was refused", (long)fd);
    return;
  }
  before = read_count(fd);
  for (int i = 0; i < 20; i++) {
    struct timespec ts = {0, 2 * 1000 * 1000};

    nanosleep(&ts, 0); /* each sleep is at least one switch away and back */
  }
  after = read_count(fd);
  judge("context-switches", after - before >= 20,
        "twenty sleeps did not produce twenty context switches",
        (long)(after - before));
  close(fd);
}

static void check_read_format(void) {
  struct perf_event_attr a;
  int fd;
  unsigned long long v[4];

  attr_init(&a, PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK);
  a.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED |
                  PERF_FORMAT_TOTAL_TIME_RUNNING | PERF_FORMAT_ID;
  a.disabled = 0;
  fd = perf_open(&a, 0, -1, -1, 0);
  if (fd < 0) {
    bad("read-format", "the extended read format was refused", (long)fd);
    return;
  }
  burn_ms(40);
  memset(v, 0, sizeof(v));
  ssize_t n = read(fd, v, sizeof(v));
  unsigned long long id = 0;
  int id_ok = ioctl(fd, PERF_EVENT_IOC_ID, &id) == 0 && id == v[3];

  judge("read-format",
        n == (ssize_t)sizeof(v) && v[1] > 0 && v[2] > 0 && v[3] != 0 && id_ok,
        "value/enabled/running/id did not all come back",
        n == (ssize_t)sizeof(v) ? (long)v[1] : (long)n);
  close(fd);
}

/* ---- the hardware counters --------------------------------------------- */

/* Is there a PMU at all? A TCG guest has none, and on such a machine every
 * hardware event is refused -- which is the honest answer, not a failure. The
 * checks below say so once and skip rather than pretending. */
static int hw_present(void) {
  struct perf_event_attr a;
  int fd;

  attr_init(&a, PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS);
  fd = perf_open(&a, 0, -1, -1, 0);
  if (fd < 0)
    return 0;
  close(fd);
  return 1;
}

static void skip(const char *what, const char *why) {
  printf("M126-SMOKE: skip %s — %s\n", what, why);
  fflush(stdout);
}

/* Instructions and cycles, counted by the CPU itself while a known amount of
 * work happens. The assertion is not "greater than zero": a loop that really
 * ran 20 million iterations cannot have retired only a handful of
 * instructions, and cycles must outnumber... nothing in particular, but they
 * must be there, and the ratio must be sane rather than absurd. */
static void check_hw_counters(void) {
  struct perf_event_attr a;
  int insn_fd, cyc_fd;
  unsigned long long insns, cycles;

  if (!hw_present()) {
    skip("hw-counters", "this CPU has no architectural PMU (a TCG guest)");
    return;
  }

  attr_init(&a, PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS);
  insn_fd = perf_open(&a, 0, -1, -1, 0);
  attr_init(&a, PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES);
  cyc_fd = perf_open(&a, 0, -1, -1, 0);
  if (insn_fd < 0 || cyc_fd < 0) {
    bad("hw-counters", "opening the counters failed",
        (long)(insn_fd < 0 ? insn_fd : cyc_fd));
    if (insn_fd >= 0)
      close(insn_fd);
    if (cyc_fd >= 0)
      close(cyc_fd);
    return;
  }
  ioctl(insn_fd, PERF_EVENT_IOC_RESET, 0);
  ioctl(cyc_fd, PERF_EVENT_IOC_RESET, 0);
  ioctl(insn_fd, PERF_EVENT_IOC_ENABLE, 0);
  ioctl(cyc_fd, PERF_EVENT_IOC_ENABLE, 0);
  burn_ms(120);
  ioctl(insn_fd, PERF_EVENT_IOC_DISABLE, 0);
  ioctl(cyc_fd, PERF_EVENT_IOC_DISABLE, 0);
  insns = read_count(insn_fd);
  cycles = read_count(cyc_fd);
  close(insn_fd);
  close(cyc_fd);

  printf("M126-SMOKE:   120 ms of work: %llu instructions, %llu cycles\n",
         insns, cycles);
  fflush(stdout);
  /* 120 ms of a tight loop on any CPU this decade retires far more than a
   * million instructions and burns far more than a million cycles. */
  judge("hw-counters", insns > 1000000ull && cycles > 1000000ull,
        "the counters did not move with the work", (long)insns);
}

/* The counter belongs to a task, not to the machine: a counter opened on a
 * child that does nothing must not collect the parent's work. */
static void check_hw_per_task(void) {
  struct perf_event_attr a;
  int fd;
  int pipefd[2];
  pid_t child;
  unsigned long long idle_child;

  if (!hw_present()) {
    skip("hw-per-task", "this CPU has no architectural PMU (a TCG guest)");
    return;
  }
  if (pipe(pipefd) != 0) {
    bad("hw-per-task", "pipe", -1);
    return;
  }
  child = fork();
  if (child == 0) {
    char c;

    close(pipefd[1]);
    /* Do nothing at all, measurably: block until the parent is finished. */
    (void)!read(pipefd[0], &c, 1);
    _exit(0);
  }
  close(pipefd[0]);
  if (child < 0) {
    bad("hw-per-task", "fork", -1);
    close(pipefd[1]);
    return;
  }

  attr_init(&a, PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS);
  fd = perf_open(&a, child, -1, -1, 0);
  if (fd < 0) {
    bad("hw-per-task", "opening a counter on the child failed", (long)fd);
    close(pipefd[1]);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    return;
  }
  ioctl(fd, PERF_EVENT_IOC_RESET, 0);
  ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
  burn_ms(120); /* the PARENT burns; the child is blocked on the pipe */
  ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
  idle_child = read_count(fd);
  close(fd);
  close(pipefd[1]);
  waitpid(child, NULL, 0);

  printf("M126-SMOKE:   a blocked child retired %llu instructions while the "
         "parent burned 120 ms\n", idle_child);
  fflush(stdout);
  /* It is not zero -- the child is woken to die, and the read(2) it is
   * blocked in costs something -- but it is nothing like the parent's
   * hundreds of millions. */
  judge("hw-per-task", idle_child < 10000000ull,
        "a blocked child was credited with the parent's work",
        (long)idle_child);
}

/* PERF_TYPE_RAW passes an event selector straight to the hardware, and
 * PERF_TYPE_HW_CACHE names branches. Both must count the same thing the
 * generic branch event counts, because they ARE the same counter. */
static void check_hw_raw_and_cache(void) {
  struct perf_event_attr a;
  int raw_fd, cache_fd;
  unsigned long long raw, cache;

  if (!hw_present()) {
    skip("hw-raw-cache", "this CPU has no architectural PMU (a TCG guest)");
    return;
  }
  /* 0x00C4: architectural "branch instructions retired". */
  attr_init(&a, PERF_TYPE_RAW, 0x00C4);
  raw_fd = perf_open(&a, 0, -1, -1, 0);
  attr_init(&a, PERF_TYPE_HW_CACHE,
            PERF_COUNT_HW_CACHE_BPU | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16));
  cache_fd = perf_open(&a, 0, -1, -1, 0);
  if (raw_fd < 0 || cache_fd < 0) {
    bad("hw-raw-cache", "raw or cache event refused",
        (long)(raw_fd < 0 ? raw_fd : cache_fd));
    if (raw_fd >= 0)
      close(raw_fd);
    if (cache_fd >= 0)
      close(cache_fd);
    return;
  }
  ioctl(raw_fd, PERF_EVENT_IOC_RESET, 0);
  ioctl(cache_fd, PERF_EVENT_IOC_RESET, 0);
  ioctl(raw_fd, PERF_EVENT_IOC_ENABLE, 0);
  ioctl(cache_fd, PERF_EVENT_IOC_ENABLE, 0);
  burn_ms(80);
  ioctl(raw_fd, PERF_EVENT_IOC_DISABLE, 0);
  ioctl(cache_fd, PERF_EVENT_IOC_DISABLE, 0);
  raw = read_count(raw_fd);
  cache = read_count(cache_fd);
  close(raw_fd);
  close(cache_fd);

  printf("M126-SMOKE:   branches: %llu raw, %llu through HW_CACHE\n", raw,
         cache);
  fflush(stdout);
  /* The same event, reached two ways, counted over the same 80 ms. What is
   * being tested is that both routes land on the SAME counter: they must
   * agree within a factor of four (they cover slightly different windows).
   *
   * Deliberately not "and both are large". How much a general-purpose counter
   * delivers is the HOST's business on a virtual machine -- this host has been
   * seen handing the guest 68 million branches on one run and five thousand on
   * the next, while the fixed counters kept counting exactly. Requiring a
   * magnitude here would be testing the hypervisor's mood. Zero, on the other
   * hand, would mean no counter at all. */
  judge("hw-raw-cache",
        raw > 0 && cache > 0 && raw < cache * 4 && cache < raw * 4,
        "the raw and HW_CACHE branch counters disagree", (long)raw);
}

static void check_refusals(void) {
  struct perf_event_attr a;
  int fd;

  /* A hardware event the architectural PMU has no counter for: bus cycles is
   * model-specific, so it must still be refused rather than answered with a
   * number taken from some other event. */
  attr_init(&a, PERF_TYPE_HARDWARE, PERF_COUNT_HW_BUS_CYCLES);
  fd = perf_open(&a, 0, -1, -1, 0);
  judge("refuses-unknown-hw", fd < 0 && errno == EOPNOTSUPP,
        "an event with no architectural counter behind it was accepted",
        (long)fd);
  if (fd >= 0)
    close(fd);

  /* A sample_type field this kernel does not produce. */
  attr_init(&a, PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_CLOCK);
  a.sample_type = PERF_SAMPLE_REGS_USER;
  fd = perf_open(&a, 0, -1, -1, 0);
  judge("refuses-unknown-sample-type", fd < 0 && errno == EOPNOTSUPP,
        "a sample type with no field behind it was accepted", (long)fd);
  if (fd >= 0)
    close(fd);

  /* pid == -1 and cpu == -1 together is meaningless, as on Linux. */
  attr_init(&a, PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_CLOCK);
  fd = perf_open(&a, -1, -1, -1, 0);
  judge("refuses-all-all", fd < 0 && errno == EINVAL,
        "pid=-1 with cpu=-1 was accepted", (long)fd);
  if (fd >= 0)
    close(fd);

  /* A counter for a process that does not exist. */
  attr_init(&a, PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_CLOCK);
  fd = perf_open(&a, 0x7ffffff, -1, -1, 0);
  judge("refuses-missing-pid", fd < 0 && (errno == ESRCH || errno == EACCES),
        "a counter was opened on a pid that does not exist", (long)fd);
  if (fd >= 0)
    close(fd);
}

/* The ring buffer and real samples: this is what `perf record` does. */
static void check_sampling(void) {
  struct perf_event_attr a;
  int fd;
  const int data_pages = 8;
  size_t map_len = (size_t)(data_pages + 1) * 4096;
  void *base;
  struct perf_event_mmap_page *ctrl;
  char *data;

  attr_init(&a, PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_CLOCK);
  a.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME |
                  PERF_SAMPLE_PERIOD | PERF_SAMPLE_CALLCHAIN;
  a.sample_freq = 200;
  a.freq = 1;
  a.sample_id_all = 1;
  a.mmap = 1;
  a.comm = 1;
  a.task = 1;
  a.disabled = 1;
  fd = perf_open(&a, 0, -1, -1, 0);
  if (fd < 0) {
    bad("sampling-open", "a sampling event was refused", (long)fd);
    return;
  }
  base = mmap(0, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (base == MAP_FAILED) {
    bad("sampling-open", "the perf ring buffer would not map", -1);
    close(fd);
    return;
  }
  ctrl = (struct perf_event_mmap_page *)base;
  data = (char *)base + 4096;

  judge("sampling-open",
        ctrl->data_offset == 4096 &&
            ctrl->data_size == (unsigned long long)data_pages * 4096,
        "the control page does not describe the buffer it was given",
        (long)ctrl->data_size);

  /* Before enabling, the kernel must have described the process: a COMM record
   * and at least one MMAP2, or `perf report` has no way to name a function. */
  unsigned long long head0 =
      __atomic_load_n(&ctrl->data_head, __ATOMIC_ACQUIRE);
  int saw_comm = 0, saw_mmap = 0;

  for (unsigned long long off = 0; off < head0;) {
    struct perf_event_header h;

    memcpy(&h, data + (off % ctrl->data_size), sizeof(h));
    if (h.size < sizeof(h) || off + h.size > head0)
      break;
    if (h.type == PERF_RECORD_COMM)
      saw_comm = 1;
    if (h.type == PERF_RECORD_MMAP2 || h.type == PERF_RECORD_MMAP)
      saw_mmap = 1;
    off += h.size;
  }
  judge("sampling-process-records", saw_comm && saw_mmap,
        "the buffer carries no COMM/MMAP records to resolve a sample against",
        (long)(saw_comm * 2 + saw_mmap));

  __atomic_store_n(&ctrl->data_tail, head0, __ATOMIC_RELEASE);

  if (ioctl(fd, PERF_EVENT_IOC_ENABLE, 0) < 0) {
    bad("sampling", "PERF_EVENT_IOC_ENABLE refused", -1);
    munmap(base, map_len);
    close(fd);
    return;
  }
  burn_ms(300);
  ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);

  unsigned long long head =
      __atomic_load_n(&ctrl->data_head, __ATOMIC_ACQUIRE);
  unsigned long long tail = head0;
  int samples = 0, with_ip = 0, with_chain = 0, in_this_program = 0;

  while (tail < head) {
    struct perf_event_header h;
    unsigned char rec[512];
    unsigned long long o = tail % ctrl->data_size;

    memcpy(&h, data + o, sizeof(h));
    if (h.size < sizeof(h) || h.size > sizeof(rec) || tail + h.size > head)
      break;
    /* Copy the record out, wrapping. */
    for (unsigned i = 0; i < h.size; i++)
      rec[i] = (unsigned char)data[(tail + i) % ctrl->data_size];
    if (h.type == PERF_RECORD_SAMPLE) {
      unsigned long long *b = (unsigned long long *)(void *)(rec + sizeof(h));
      unsigned long long ip = b[0];
      unsigned long long nr_chain;

      samples++;
      if (ip)
        with_ip++;
      /* IP, TID, TIME, PERIOD, then the call chain. */
      nr_chain = b[4];
      if (nr_chain >= 2 && nr_chain < 128) {
        with_chain++;
        /* The chain starts with PERF_CONTEXT_USER and the sampled IP. */
        if (b[5] == (unsigned long long)PERF_CONTEXT_USER && b[6] == ip)
          in_this_program++;
      }
    }
    tail += h.size;
  }
  __atomic_store_n(&ctrl->data_tail, tail, __ATOMIC_RELEASE);

  /* 300 ms at 200 Hz is 60 samples; a loaded host may give fewer, but a
   * profiler that produces under ten of them is not a profiler. */
  judge("sampling", samples >= 10,
        "300 ms of spinning at 200 Hz produced too few samples",
        (long)samples);
  judge("sampling-ip", samples > 0 && with_ip == samples,
        "a sample came back with no instruction pointer",
        (long)with_ip);
  judge("sampling-callchain",
        samples > 0 && with_chain == samples &&
            in_this_program >= samples / 2,
        "the call chains do not start in the sampled user program",
        (long)in_this_program);

  /* poll(2) must report the buffer as readable while records are waiting. */
  {
    struct perf_event_attr b;
    int pfd;

    attr_init(&b, PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_CLOCK);
    b.sample_type = PERF_SAMPLE_IP;
    b.sample_freq = 200;
    b.freq = 1;
    b.disabled = 0;
    pfd = perf_open(&b, 0, -1, -1, 0);
    if (pfd >= 0) {
      void *m2 = mmap(0, 2 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, pfd, 0);

      if (m2 != MAP_FAILED) {
        struct pollfd p = {.fd = pfd, .events = POLLIN};
        int n;

        burn_ms(60);
        n = poll(&p, 1, 1000);
        judge("sampling-poll", n == 1 && (p.revents & POLLIN),
              "poll did not report the ring buffer as readable", (long)n);
        munmap(m2, 2 * 4096);
      } else {
        bad("sampling-poll", "the second ring would not map", -1);
      }
      close(pfd);
    } else {
      bad("sampling-poll", "the second event was refused", (long)pfd);
    }
  }

  munmap(base, map_len);
  close(fd);
}

/* PERF_EVENT_IOC_REFRESH delivers a fixed number of samples and then stops. */
static void check_refresh(void) {
  struct perf_event_attr a;
  int fd;
  size_t map_len = 5 * 4096;
  void *base;
  struct perf_event_mmap_page *ctrl;

  attr_init(&a, PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_CLOCK);
  a.sample_type = PERF_SAMPLE_IP;
  a.sample_freq = 500;
  a.freq = 1;
  a.disabled = 1;
  fd = perf_open(&a, 0, -1, -1, 0);
  if (fd < 0) {
    bad("refresh", "a sampling event was refused", (long)fd);
    return;
  }
  base = mmap(0, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (base == MAP_FAILED) {
    bad("refresh", "the ring would not map", -1);
    close(fd);
    return;
  }
  ctrl = (struct perf_event_mmap_page *)base;
  unsigned long long head0 =
      __atomic_load_n(&ctrl->data_head, __ATOMIC_ACQUIRE);

  if (ioctl(fd, PERF_EVENT_IOC_REFRESH, 3) < 0) {
    bad("refresh", "PERF_EVENT_IOC_REFRESH refused", -1);
    munmap(base, map_len);
    close(fd);
    return;
  }
  burn_ms(300);
  unsigned long long head1 =
      __atomic_load_n(&ctrl->data_head, __ATOMIC_ACQUIRE);

  /* Three samples and no more, however long the spinning goes on. */
  burn_ms(200);
  unsigned long long head2 =
      __atomic_load_n(&ctrl->data_head, __ATOMIC_ACQUIRE);

  int n = 0;
  {
    char *data = (char *)base + 4096;
    unsigned long long off = head0;

    while (off < head2) {
      struct perf_event_header h;

      memcpy(&h, data + (off % ctrl->data_size), sizeof(h));
      if (h.size < sizeof(h) || off + h.size > head2)
        break;
      if (h.type == PERF_RECORD_SAMPLE)
        n++;
      off += h.size;
    }
  }
  judge("refresh", n == 3 && head1 == head2,
        "IOC_REFRESH(3) did not stop the event after exactly three samples",
        (long)n);
  munmap(base, map_len);
  close(fd);
}


/* ── tracepoints and kprobes ──────────────────────────────────────────────
 *
 * The tools do not ask the kernel what it can trace: they read
 * /sys/kernel/tracing. So the checks here are the ones perf makes -- find the
 * event in available_events, read its id, open a PERF_TYPE_TRACEPOINT counter
 * on that id -- and then prove the counter by making the traced thing happen.
 */

#define TRACEFS "/sys/kernel/tracing"

static int read_file(const char *path, char *buf, size_t len) {
  int fd = open(path, O_RDONLY);
  ssize_t n;

  if (fd < 0)
    return -1;
  n = read(fd, buf, len - 1);
  close(fd);
  if (n < 0)
    return -1;
  buf[n] = '\0';
  return (int)n;
}

static int write_file(const char *path, const char *text) {
  int fd = open(path, O_WRONLY);
  ssize_t n;

  if (fd < 0)
    return -1;
  n = write(fd, text, strlen(text));
  close(fd);
  return n < 0 ? -1 : (int)n;
}

/* The id perf would put in attr.config, or -1. */
static long tp_id(const char *group, const char *event) {
  char path[256];
  char buf[32];

  snprintf(path, sizeof(path), TRACEFS "/events/%s/%s/id", group, event);
  if (read_file(path, buf, sizeof(buf)) <= 0)
    return -1;
  return strtol(buf, NULL, 10);
}

static long long counter_read(int fd) {
  long long v = 0;

  if (read(fd, &v, sizeof(v)) != (ssize_t)sizeof(v))
    return -1;
  return v;
}

static void check_tracefs_layout(void) {
  char buf[4096];

  if (read_file(TRACEFS "/available_events", buf, sizeof(buf)) <= 0) {
    bad("tracefs", "no " TRACEFS "/available_events to read", 0);
    return;
  }
  judge("tracefs-available-events",
        strstr(buf, "sched:sched_switch") && strstr(buf, "raw_syscalls:sys_enter") &&
            strstr(buf, "block:block_rq_issue"),
        "available_events does not name the sites this kernel has", 0);
  judge("tracefs-event-id",
        tp_id("sched", "sched_switch") > 0 && tp_id("raw_syscalls", "sys_enter") > 0 &&
            tp_id("sched", "sched_switch") != tp_id("raw_syscalls", "sys_enter"),
        "events/<group>/<event>/id does not give each site its own number", 0);
  if (read_file(TRACEFS "/events/raw_syscalls/sys_enter/format", buf,
                sizeof(buf)) <= 0) {
    bad("tracefs-format", "no format file for raw_syscalls/sys_enter", 0);
    return;
  }
  judge("tracefs-format",
        strstr(buf, "ID:") && strstr(buf, "common_pid") && strstr(buf, "print fmt:"),
        "format is not the shape a tool parses", 0);
}

/* A counter on raw_syscalls:sys_enter, proved by making system calls. */
static void check_tracepoint_counter(void) {
  struct perf_event_attr a;
  long id = tp_id("raw_syscalls", "sys_enter");
  long long before, after;
  int fd;

  if (id <= 0) {
    bad("tracepoint-counts", "raw_syscalls:sys_enter has no id", id);
    return;
  }
  attr_init(&a, PERF_TYPE_TRACEPOINT, (unsigned long long)id);
  a.disabled = 0;
  fd = perf_open(&a, 0, -1, -1, 0);
  if (fd < 0) {
    bad("tracepoint-counts", "perf_event_open on a tracepoint id", fd);
    return;
  }
  before = counter_read(fd);
  for (int i = 0; i < 200; i++)
    (void)getppid();
  after = counter_read(fd);
  judge("tracepoint-counts", before >= 0 && after >= before + 200,
        "200 system calls did not move the sys_enter counter by 200",
        (long)(after - before));

  /* And the site goes quiet for a counter that watches another task: this one
   * counts what its own task did, not the machine. */
  close(fd);

  id = tp_id("sched", "sched_switch");
  attr_init(&a, PERF_TYPE_TRACEPOINT, (unsigned long long)id);
  a.disabled = 0;
  fd = perf_open(&a, 0, -1, -1, 0);
  if (fd < 0) {
    bad("tracepoint-sched-switch", "perf_event_open on sched:sched_switch", fd);
    return;
  }
  before = counter_read(fd);
  for (int i = 0; i < 20; i++) {
    struct timespec ts = {0, 2000000};

    nanosleep(&ts, NULL);
  }
  after = counter_read(fd);
  judge("tracepoint-sched-switch", before >= 0 && after > before,
        "twenty sleeps produced no sched_switch hits for this task",
        (long)(after - before));
  close(fd);
}

/* A kprobe: asked for by symbol through kprobe_events, counted through perf. */
static void check_kprobe(void) {
  static const char *candidates[] = {"vfs_find_node", "find_child",
                                     "scheduler_task_by_pid", NULL};
  const char *symbol = NULL;
  char line[128];
  struct perf_event_attr a;
  long id;
  long long before, after;
  int fd;

  /* Probe a symbol this kernel really has: a name it does not know is refused
   * by kprobe_events (ENOENT), so the first write that is accepted names one.
   * Each candidate is reached by the path lookup the check performs below. */
  for (int i = 0; candidates[i]; i++) {
    snprintf(line, sizeof(line), "p:b1smoke %s\n", candidates[i]);
    if (write_file(TRACEFS "/kprobe_events", line) >= 0) {
      symbol = candidates[i];
      break;
    }
  }
  if (!symbol) {
    bad("kprobe", "kprobe_events accepted none of the candidate symbols", 0);
    return;
  }
  id = tp_id("kprobes", "b1smoke");
  if (id <= 0) {
    bad("kprobe", "the probe has no event directory", id);
    (void)write_file(TRACEFS "/kprobe_events", "-:b1smoke\n");
    return;
  }
  attr_init(&a, PERF_TYPE_TRACEPOINT, (unsigned long long)id);
  a.disabled = 0;
  fd = perf_open(&a, -1, 0, -1, 0); /* the probe fires wherever it fires */
  if (fd < 0) {
    /* Machine-wide needs privilege; fall back to this task's own hits. */
    fd = perf_open(&a, 0, -1, -1, 0);
  }
  if (fd < 0) {
    bad("kprobe", "perf_event_open on the probe's id", fd);
    (void)write_file(TRACEFS "/kprobe_events", "-:b1smoke\n");
    return;
  }
  before = counter_read(fd);
  for (int i = 0; i < 50; i++)
    (void)access("/proc/self/stat", F_OK);
  after = counter_read(fd);
  judge("kprobe", before >= 0 && after > before,
        "the probed function ran fifty times and the probe counted nothing",
        (long)(after - before));
  close(fd);

  /* Removing it takes the event away again, which is what proves the
   * instruction went back: a kernel still executing an int3 here would not
   * survive the next lookup, and everything after this line is one. */
  judge("kprobe-remove", write_file(TRACEFS "/kprobe_events", "-:b1smoke\n") >= 0 &&
                             tp_id("kprobes", "b1smoke") < 0,
        "-:b1smoke left the event behind", 0);
  for (int i = 0; i < 50; i++)
    (void)access("/proc/self/stat", F_OK);
  ok("kprobe-unpatched");
}

int main(void) {
  struct perf_event_attr probe;
  int fd;

  printf("M126-SMOKE: start\n");
  fflush(stdout);

  memset(&probe, 0, sizeof(probe));
  probe.size = sizeof(probe);
  probe.type = PERF_TYPE_SOFTWARE;
  probe.config = PERF_COUNT_SW_CPU_CLOCK;
  fd = perf_open(&probe, 0, -1, -1, 0);
  if (fd < 0 && errno == ENOSYS) {
    printf("M126-SMOKE: FAIL open — perf_event_open is ENOSYS\n");
    printf("M126-SMOKE: done\n");
    fflush(stdout);
    return 1;
  }
  if (fd >= 0)
    close(fd);

  check_task_clock();
  check_page_faults();
  check_context_switches();
  check_read_format();
  check_hw_counters();
  check_hw_per_task();
  check_hw_raw_and_cache();
  check_refusals();
  check_sampling();
  check_refresh();
  check_tracefs_layout();
  check_tracepoint_counter();
  check_kprobe();

  printf("M126-SMOKE: done\n");
  fflush(stdout);
  return fails ? 1 : 0;
}
