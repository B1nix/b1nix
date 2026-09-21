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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
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

static void check_refusals(void) {
  struct perf_event_attr a;
  int fd;

  /* No PMU here: a hardware counter must be refused, not answered with zero. */
  attr_init(&a, PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES);
  fd = perf_open(&a, 0, -1, -1, 0);
  judge("refuses-hardware", fd < 0 && errno == EOPNOTSUPP,
        "a hardware counter was accepted although there is no PMU driver",
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
  check_refusals();
  check_sampling();
  check_refresh();

  printf("M126-SMOKE: done\n");
  fflush(stdout);
  return fails ? 1 : 0;
}
