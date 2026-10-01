/* SPDX-License-Identifier: GPL-2.0-only */
/* m135_iommu_s3_smoke — an IOMMU through an ACPI S3 (M135).
 *
 * S3 resets the remapping unit with the rest of the machine: translation off,
 * no root table, interrupt remapping off. On the VT-d instance the NVMe
 * controller's completions are message interrupts in the REMAPPABLE format,
 * which mean nothing to a unit whose remapping table is gone, so a resume that
 * did not bring the unit back leaves the controller completing commands whose
 * interrupts never arrive. This test sleeps, then writes to the NVMe disk and
 * syncs it, and counts the controller's own vector in /proc/interrupts across
 * that write: the count must move. The kernel's S3 counter must move too, or
 * the sleep was not S3.
 *
 * All the while a child writes and syncs another stretch of the same disk in
 * a loop, so the sleep is likely to find it with a command in flight. The
 * freezer must let such a task finish the command before it stops it: a
 * request the controller was holding when the power went is completed by
 * nobody, and the child would never return from its fsync. After the resume
 * the child must keep going, and what it last wrote must read back.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* From <linux/rtc.h>, spelled out as m129_suspend_smoke does. */
#define RTC_AIE_OFF   0x7002
#define RTC_RD_TIME   0x80247009
#define RTC_WKALM_SET 0x4028700f

struct rtc_time_u {
  int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday,
      tm_isdst;
};
struct rtc_wkalrm_u {
  unsigned char enabled, pending, pad[2];
  struct rtc_time_u time;
};

static int fails;

static void ok(const char *what) {
  printf("M135-IOMMU: ok %s\n", what);
  fflush(stdout);
}

static void bad(const char *what, const char *why, long v) {
  printf("M135-IOMMU: FAIL %s — %s (%ld, errno=%d)\n", what, why, v, errno);
  fflush(stdout);
  fails++;
}

static int read_file(const char *path, char *buf, size_t cap) {
  int fd = open(path, O_RDONLY);
  ssize_t n;

  buf[0] = 0;
  if (fd < 0)
    return -1;
  n = read(fd, buf, cap - 1);
  close(fd);
  if (n < 0)
    return -1;
  buf[n] = 0;
  return (int)n;
}

static long s3_count(void) {
  static char proc[1024];
  char *p;

  if (read_file("/proc/b1nix-suspend", proc, sizeof(proc)) < 0)
    return -1;
  p = strstr(proc, "s3_count ");
  return p ? strtol(p + 9, 0, 10) : -1;
}

/* The vector the NVMe driver says its completions arrive on. */
static int nvme_vector(void) {
  static char log[1 << 17];
  const char *key = "nvme: MSI-X completions on vector ";
  int n = klogctl(3 /* SYSLOG_ACTION_READ_ALL */, log, (int)sizeof(log) - 1);
  char *p, *last = 0;

  if (n <= 0)
    return -1;
  log[n] = 0;
  for (p = strstr(log, key); p; p = strstr(p + 1, key))
    last = p;
  return last ? atoi(last + strlen(key)) : -1;
}

static long irq_count(int vector) {
  static char buf[8192];
  char *line;

  if (read_file("/proc/interrupts", buf, sizeof(buf)) < 0)
    return -1;
  for (line = buf; line && *line; line = strchr(line, '\n'), line = line ? line + 1 : 0) {
    char *end;
    long v = strtol(line, &end, 10);

    if (end != line && *end == ':' && v == vector)
      return strtol(end + 1, 0, 10);
  }
  return 0; /* a line that never fired is not listed */
}

/* The kernel refuses an S3 nothing armed could end: the RTC alarm, a few
 * seconds out (the lane's waker is what really ends it, see s3-waker.py). */
static int arm_alarm(int rtc) {
  struct rtc_time_u t;
  struct rtc_wkalrm_u alarm;

  if (ioctl(rtc, RTC_RD_TIME, &t) != 0)
    return -1;
  memset(&alarm, 0, sizeof(alarm));
  alarm.enabled = 1;
  alarm.time = t;
  alarm.time.tm_sec += 3;
  if (alarm.time.tm_sec >= 60) {
    alarm.time.tm_sec -= 60;
    alarm.time.tm_min++;
  }
  if (alarm.time.tm_min >= 60) {
    alarm.time.tm_min -= 60;
    alarm.time.tm_hour++;
  }
  if (alarm.time.tm_hour >= 24)
    alarm.time.tm_hour -= 24;
  return ioctl(rtc, RTC_WKALM_SET, &alarm);
}

/* Writes that have to reach the controller: fsync on the device node sends
 * them, and waits for their completions. */
static int nvme_write(const char *dev) {
  static char block[64 * 1024];
  int fd = open(dev, O_WRONLY);
  off_t size;
  int rc = 0;

  if (fd < 0)
    return -1;
  size = lseek(fd, 0, SEEK_END);
  if (size < (off_t)(2 * sizeof(block))) {
    close(fd);
    errno = ENOSPC;
    return -1;
  }
  memset(block, 0x5b, sizeof(block));
  if (lseek(fd, size - (off_t)sizeof(block), SEEK_SET) < 0 ||
      write(fd, block, sizeof(block)) != (ssize_t)sizeof(block) || fsync(fd) != 0)
    rc = -1;
  close(fd);
  return rc;
}

struct writer {
  volatile long rounds; /* writes that were synced */
  volatile int stop;
  volatile int failed;
};

#define WRITER_BLOCK (32 * 1024)

/* The child: a different stretch from nvme_write's, each round stamped with
 * its number, until told to stop. */
static void writer_loop(const char *dev, struct writer *w) {
  static char block[WRITER_BLOCK];
  int fd = open(dev, O_RDWR);
  off_t at;

  if (fd < 0) {
    w->failed = 1;
    _exit(1);
  }
  at = lseek(fd, 0, SEEK_END) - (off_t)(4 * WRITER_BLOCK);
  while (!w->stop) {
    long r = w->rounds + 1;

    memset(block, (int)(r & 0xff), sizeof(block));
    memcpy(block, &r, sizeof(r));
    if (lseek(fd, at, SEEK_SET) < 0 ||
        write(fd, block, sizeof(block)) != (ssize_t)sizeof(block) || fsync(fd) != 0) {
      w->failed = 1;
      break;
    }
    w->rounds = r;
  }
  close(fd);
  _exit(0);
}

/* What the writer's last synced round left on the disk. */
static long writer_readback(const char *dev) {
  static char block[WRITER_BLOCK];
  int fd = open(dev, O_RDONLY);
  off_t at;
  long r = -1;

  if (fd < 0)
    return -1;
  at = lseek(fd, 0, SEEK_END) - (off_t)(4 * WRITER_BLOCK);
  if (lseek(fd, at, SEEK_SET) >= 0 && read(fd, block, sizeof(block)) == (ssize_t)sizeof(block)) {
    memcpy(&r, block, sizeof(r));
    for (size_t i = sizeof(r); i < sizeof(block); i++)
      if (block[i] != (char)(r & 0xff)) {
        r = -2;
        break;
      }
  }
  close(fd);
  return r;
}

static unsigned long long now_ms(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}

int main(void) {
  const char *dev = "/dev/nvme0n1";
  char states[64];
  long before, after, irq0, irq1, r0, back;
  int vec, fd, rtc, status = 0;
  struct writer *w;
  pid_t child;
  unsigned long long t0;

  printf("M135-IOMMU: start\n");
  fflush(stdout);
  read_file("/sys/power/mem_sleep", states, sizeof(states));
  if (!strstr(states, "deep")) {
    bad("s3", "this machine offers no S3", 0);
    printf("M135-IOMMU: done\n");
    return 1;
  }
  vec = nvme_vector();
  if (vec < 0) {
    bad("nvme-vector", "the NVMe driver named no message vector", vec);
    printf("M135-IOMMU: done\n");
    return 1;
  }
  before = s3_count();

  w = mmap(0, sizeof(*w), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (w == MAP_FAILED) {
    bad("writer", "no shared page for the writer", 0);
    printf("M135-IOMMU: done\n");
    return 1;
  }
  memset((void *)w, 0, sizeof(*w));
  child = fork();
  if (child == 0)
    writer_loop(dev, w);
  /* Going before the sleep, so the freezer meets it mid-stream. */
  t0 = now_ms();
  while (w->rounds < 20 && !w->failed && now_ms() - t0 < 10000)
    usleep(1000);

  rtc = open("/dev/rtc0", O_RDONLY);
  if (rtc < 0 || arm_alarm(rtc) != 0) {
    bad("s3", "could not arm the RTC alarm", rtc);
    printf("M135-IOMMU: done\n");
    return 1;
  }
  fd = open("/sys/power/state", O_WRONLY);
  if (fd < 0 || write(fd, "mem", 3) != 3) {
    bad("s3", "writing mem to /sys/power/state failed", fd);
    if (fd >= 0)
      close(fd);
    printf("M135-IOMMU: done\n");
    return 1;
  }
  close(fd);
  ioctl(rtc, RTC_AIE_OFF, 0);
  close(rtc);
  after = s3_count();
  if (before >= 0 && after == before + 1)
    ok("s3-slept");
  else
    bad("s3-slept", "the kernel's S3 counter did not move", after);

  /* The writer, still going: a few more synced rounds within seconds. */
  r0 = w->rounds;
  t0 = now_ms();
  while (w->rounds < r0 + 5 && !w->failed && now_ms() - t0 < 10000)
    usleep(1000);
  printf("M135-IOMMU: writer rounds %ld at the resume, %ld after %llu ms%s\n", r0,
         (long)w->rounds, now_ms() - t0, w->failed ? ", a write failed" : "");
  w->stop = 1;
  t0 = now_ms();
  while (waitpid(child, &status, WNOHANG) == 0 && now_ms() - t0 < 10000)
    usleep(10000);
  if (now_ms() - t0 >= 10000) {
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
  }
  back = writer_readback(dev);
  if (!w->failed && w->rounds >= r0 + 5 && back == w->rounds)
    ok("io-across-s3");
  else
    bad("io-across-s3", "the disk writer that ran through the sleep stalled, "
                        "failed, or its last round did not read back", back);

  irq0 = irq_count(vec);
  if (nvme_write(dev) != 0)
    bad("nvme-io", "a write and fsync on the NVMe disk failed", 0);
  irq1 = irq_count(vec);
  printf("M135-IOMMU: vector %d: %ld -> %ld across the write\n", vec, irq0, irq1);
  if (irq0 >= 0 && irq1 > irq0)
    ok("nvme-irq-after-s3");
  else
    bad("nvme-irq-after-s3",
        "the NVMe completions after the resume raised no interrupt", irq1);
  printf("M135-IOMMU: done\n");
  fflush(stdout);
  return fails ? 1 : 0;
}
