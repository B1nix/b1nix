/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * m127_swap_smoke — compressed swap, and what a cgroup does with it.
 *
 * Two things are under test and they meet in the middle. zram is a block
 * device whose blocks live compressed in RAM, so a machine can swap without a
 * disk; the memory controller can now reclaim INSIDE a cgroup instead of only
 * killing in it, and every page it writes out is charged to the cgroup that
 * owned it, which is what memory.swap.current reports.
 *
 * Nothing here passes because a file exists. Each marker names something that
 * was observed: bytes that came back out of the compressed device byte for
 * byte, a page of zeroes that cost no memory at all, a swap device that took
 * real pages, a process that stayed ALIVE inside a memory.max it had exceeded
 * because its own pages went to swap, and a process that was killed once its
 * memory.swap.max left reclaim nothing to do.
 *
 *   zram-disksize     /sys/block/zram0/disksize takes a size and reports it
 *                     back, and the device appears with that many sectors
 *   zram-roundtrip    a compressible pattern written to /dev/zram0 reads back
 *                     identical, and mm_stat says it cost less than it is
 *   zram-incompress   an incompressible page reads back identical too, and is
 *                     counted as a huge page rather than silently truncated
 *   zram-zero-pages   a page of zeroes costs nothing and is counted in
 *                     same_pages
 *   swapon-zram       mkswap's signature plus swapon(2) makes it THE swap
 *                     device, whole, and /proc/swaps says so
 *   cgroup-reclaim    a cgroup over memory.max whose pages can go to swap is
 *                     reclaimed, not killed: the process is alive afterwards
 *                     and memory.stat's pgsteal counts the pages taken
 *   cgroup-swap-cur   those pages are charged to that cgroup, and the charge
 *                     is released when they come back in
 *   swap-max          memory.swap.max stops the reclaim: the cgroup has
 *                     nowhere to put pages, memory.swap.events counts the
 *                     refusals, and the runaway is killed as before
 *   swapoff-zram      swapoff pages everything back and the device is idle
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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
#include <sys/swap.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CGROOT "/tmp/m127swapcg"
#define ZRAM "/dev/zram0"
#define ZSYS "/sys/block/zram0"
#define ZRAM_MB 96
#define PAGE 4096

static int g_fail;
/* The device swap was using when we arrived, so it can be put back. */
static char g_prev_swap[128];

static void marker(const char *s) { write(1, s, strlen(s)); }

static void ok(const char *name) {
  char b[128];
  snprintf(b, sizeof(b), "M127-SWAP: ok %s\n", name);
  marker(b);
}

static void note(const char *fmt, ...) {
  char why[400], b[520];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(why, sizeof(why), fmt, ap);
  va_end(ap);
  snprintf(b, sizeof(b), "M127-SWAP:   %s\n", why);
  marker(b);
}

static void failf(const char *name, const char *fmt, ...) {
  char why[400], b[560];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(why, sizeof(why), fmt, ap);
  va_end(ap);
  snprintf(b, sizeof(b), "M127-SWAP: FAIL %s %s\n", name, why);
  marker(b);
  g_fail = 1;
}

/* ── file helpers ────────────────────────────────────────────────────────── */

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

static long long read_ll(const char *path) {
  char b[256];
  if (read_file(path, b, sizeof(b)) < 0)
    return -1;
  if (strncmp(b, "max", 3) == 0)
    return -2;
  return strtoll(b, NULL, 10);
}

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

/* mm_stat is eight numbers on one line, in Linux's order. */
static int mm_stat(long long out[8]) {
  char b[256];
  if (read_file(ZSYS "/mm_stat", b, sizeof(b)) < 0)
    return -1;
  char *p = b;
  for (int i = 0; i < 8; i++) {
    char *end = NULL;
    out[i] = strtoll(p, &end, 10);
    if (end == p)
      return -1;
    p = end;
  }
  return 0;
}

static void msleep(unsigned ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

/* ── zram ────────────────────────────────────────────────────────────────── */

static int zram_ready;

static void check_zram_disksize(void) {
  char want[32];
  snprintf(want, sizeof(want), "%dM", ZRAM_MB);
  if (write_file(ZSYS "/disksize", want) != 0) {
    failf("zram-disksize", "writing %s to " ZSYS "/disksize: %s", want,
          strerror(errno));
    return;
  }
  long long got = read_ll(ZSYS "/disksize");
  long long expect = (long long)ZRAM_MB * 1024 * 1024;
  if (got != expect) {
    failf("zram-disksize", "reads back %lld, wanted %lld", got, expect);
    return;
  }
  if (read_ll(ZSYS "/initstate") != 1) {
    failf("zram-disksize", "initstate is not 1 after sizing");
    return;
  }
  /* The block device only exists once it has a size; the sector count is the
   * proof that the block layer got the same number sysfs reports. */
  long long sectors = read_ll("/sys/block/zram0/size");
  if (sectors >= 0 && sectors != expect / 512) {
    failf("zram-disksize", "/sys/block/zram0/size is %lld sectors, wanted %lld",
          sectors, expect / 512);
    return;
  }
  int fd = open(ZRAM, O_RDWR);
  if (fd < 0) {
    failf("zram-disksize", "open " ZRAM ": %s", strerror(errno));
    return;
  }
  close(fd);
  zram_ready = 1;
  ok("zram-disksize");
}

/* A page that compresses well: long runs, which is what an LZ4 match is. */
static void fill_compressible(unsigned char *p, unsigned seed) {
  for (int i = 0; i < PAGE; i++)
    p[i] = (unsigned char)('A' + ((i / 64 + seed) % 26));
}

/* A page that does not: a cheap PRNG, no repeats within the window. */
static void fill_random(unsigned char *p, unsigned seed) {
  unsigned x = seed | 1u;
  for (int i = 0; i < PAGE; i++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    p[i] = (unsigned char)x;
  }
}

static void check_zram_roundtrip(void) {
  if (!zram_ready) {
    failf("zram-roundtrip", "no device");
    return;
  }
  static unsigned char out[PAGE * 8], back[PAGE * 8];
  for (int i = 0; i < 8; i++)
    fill_compressible(out + i * PAGE, (unsigned)i);

  int fd = open(ZRAM, O_RDWR);
  if (fd < 0) {
    failf("zram-roundtrip", "open: %s", strerror(errno));
    return;
  }
  if (pwrite(fd, out, sizeof(out), 0) != (ssize_t)sizeof(out)) {
    failf("zram-roundtrip", "write: %s", strerror(errno));
    close(fd);
    return;
  }
  if (pread(fd, back, sizeof(back), 0) != (ssize_t)sizeof(back)) {
    failf("zram-roundtrip", "read: %s", strerror(errno));
    close(fd);
    return;
  }
  close(fd);
  if (memcmp(out, back, sizeof(out)) != 0) {
    failf("zram-roundtrip", "data came back different");
    return;
  }
  /* And it really was compressed: eight pages in, and what they cost is less
   * than what they are. */
  long long st[8];
  if (mm_stat(st) != 0) {
    failf("zram-roundtrip", "mm_stat unreadable");
    return;
  }
  if (st[0] < (long long)sizeof(out)) {
    failf("zram-roundtrip", "orig_data_size %lld < the %zu bytes written",
          st[0], sizeof(out));
    return;
  }
  if (st[1] == 0 || st[1] >= st[0]) {
    failf("zram-roundtrip", "compr_data_size %lld against orig %lld: no saving",
          st[1], st[0]);
    return;
  }
  note("zram %lld bytes stored in %lld (%lld%%)", st[0], st[1],
       st[1] * 100 / (st[0] ? st[0] : 1));
  ok("zram-roundtrip");
}

static void check_zram_incompressible(void) {
  if (!zram_ready) {
    failf("zram-incompress", "no device");
    return;
  }
  static unsigned char out[PAGE * 4], back[PAGE * 4];
  for (int i = 0; i < 4; i++)
    fill_random(out + i * PAGE, 0x2545F491u + (unsigned)i * 7919u);

  long long before[8], after[8];
  if (mm_stat(before) != 0) {
    failf("zram-incompress", "mm_stat unreadable");
    return;
  }
  int fd = open(ZRAM, O_RDWR);
  off_t at = 1024 * 1024;
  if (fd < 0 || pwrite(fd, out, sizeof(out), at) != (ssize_t)sizeof(out) ||
      pread(fd, back, sizeof(back), at) != (ssize_t)sizeof(back)) {
    failf("zram-incompress", "I/O: %s", strerror(errno));
    if (fd >= 0)
      close(fd);
    return;
  }
  close(fd);
  if (memcmp(out, back, sizeof(out)) != 0) {
    failf("zram-incompress", "data came back different");
    return;
  }
  if (mm_stat(after) != 0) {
    failf("zram-incompress", "mm_stat unreadable");
    return;
  }
  if (after[7] <= before[7]) {
    failf("zram-incompress", "huge_pages %lld -> %lld: not counted", before[7],
          after[7]);
    return;
  }
  ok("zram-incompress");
}

static void check_zram_zero_pages(void) {
  if (!zram_ready) {
    failf("zram-zero-pages", "no device");
    return;
  }
  static unsigned char zeroes[PAGE * 4], back[PAGE * 4];
  memset(zeroes, 0, sizeof(zeroes));
  long long before[8], after[8];
  if (mm_stat(before) != 0) {
    failf("zram-zero-pages", "mm_stat unreadable");
    return;
  }
  int fd = open(ZRAM, O_RDWR);
  off_t at = 4 * 1024 * 1024;
  if (fd < 0 || pwrite(fd, zeroes, sizeof(zeroes), at) != (ssize_t)sizeof(zeroes) ||
      pread(fd, back, sizeof(back), at) != (ssize_t)sizeof(back)) {
    failf("zram-zero-pages", "I/O: %s", strerror(errno));
    if (fd >= 0)
      close(fd);
    return;
  }
  close(fd);
  for (size_t i = 0; i < sizeof(back); i++) {
    if (back[i] != 0) {
      failf("zram-zero-pages", "byte %zu came back as %u", i, back[i]);
      return;
    }
  }
  if (mm_stat(after) != 0) {
    failf("zram-zero-pages", "mm_stat unreadable");
    return;
  }
  if (after[5] < before[5] + 4) {
    failf("zram-zero-pages", "same_pages %lld -> %lld for four zero pages",
          before[5], after[5]);
    return;
  }
  if (after[1] != before[1]) {
    failf("zram-zero-pages", "zero pages cost %lld bytes", after[1] - before[1]);
    return;
  }
  ok("zram-zero-pages");
}

/* ── swap on zram ────────────────────────────────────────────────────────── */

/* What mkswap(8) writes: version 1, the last usable page, and the signature
 * the kernel looks for at the end of the first page. */
static int write_swap_header(void) {
  static unsigned char hdr[PAGE];
  memset(hdr, 0, sizeof(hdr));
  uint32_t version = 1;
  uint32_t last_page = (uint32_t)(((long long)ZRAM_MB * 1024 * 1024) / PAGE) - 1;
  memcpy(hdr + 1024, &version, 4);
  memcpy(hdr + 1028, &last_page, 4);
  memcpy(hdr + PAGE - 10, "SWAPSPACE2", 10);

  int fd = open(ZRAM, O_RDWR);
  if (fd < 0)
    return -1;
  ssize_t n = pwrite(fd, hdr, sizeof(hdr), 0);
  close(fd);
  return n == (ssize_t)sizeof(hdr) ? 0 : -1;
}

/* The device /proc/swaps names right now, if any. */
static int current_swap(char *out, size_t cap) {
  char b[1024];
  if (read_file("/proc/swaps", b, sizeof(b)) < 0)
    return -1;
  char *nl = strchr(b, '\n');
  if (!nl || !nl[1])
    return -1; /* header only: nothing is on */
  char *p = nl + 1;
  size_t i = 0;
  while (p[i] && p[i] != ' ' && p[i] != '\t' && p[i] != '\n' && i + 1 < cap) {
    out[i] = p[i];
    i++;
  }
  out[i] = '\0';
  return i ? 0 : -1;
}

static int swap_on_zram;
/* What check_cgroup_reclaim left charged to the "hog" cgroup: pages its child
 * shared with us. swapoff has to bring exactly this to zero. */
static long long g_hog_residual;

static void check_swapon_zram(void) {
  if (!zram_ready) {
    failf("swapon-zram", "no device");
    return;
  }
  /* The lane boots with a swap disk already attached. Take it off first --
   * one swap device at a time -- and remember it so it can go back. */
  if (current_swap(g_prev_swap, sizeof(g_prev_swap)) == 0) {
    if (swapoff(g_prev_swap) != 0) {
      failf("swapon-zram", "swapoff %s: %s", g_prev_swap, strerror(errno));
      g_prev_swap[0] = '\0';
      return;
    }
  }
  if (write_swap_header() != 0) {
    failf("swapon-zram", "writing the swap header: %s", strerror(errno));
    return;
  }
  if (swapon(ZRAM, 0) != 0) {
    failf("swapon-zram", "swapon " ZRAM ": %s", strerror(errno));
    return;
  }
  char now[128] = {0};
  if (current_swap(now, sizeof(now)) != 0 || !strstr(now, "zram0")) {
    failf("swapon-zram", "/proc/swaps says '%s'", now);
    swapoff(ZRAM);
    return;
  }
  /* Whole device, not a corner of it: the signature said what it is for. A
   * quarter-device would report about 24 MiB here. */
  long long total_kb = read_kv("/proc/swaps", now);
  char b[1024];
  if (read_file("/proc/swaps", b, sizeof(b)) >= 0) {
    char *line = strstr(b, "zram0");
    if (line) {
      long long size_kb = 0;
      /* "/dev/zram0  partition  <size>  <used>  <prio>" */
      char *p = line;
      while (*p && *p != ' ' && *p != '\t')
        p++;
      size_kb = strtoll(p, &p, 10);        /* type field is not a number */
      if (size_kb == 0)
        while (*p && (*p < '0' || *p > '9'))
          p++;
      size_kb = strtoll(p, NULL, 10);
      if (size_kb > 0 && size_kb < (long long)ZRAM_MB * 1024 / 2) {
        failf("swapon-zram", "only %lld KiB of a %d MiB device is swap",
              size_kb, ZRAM_MB);
        swapoff(ZRAM);
        return;
      }
      note("swap area %lld KiB of a %d MiB device", size_kb, ZRAM_MB);
    }
  }
  (void)total_kb;
  swap_on_zram = 1;
  ok("swapon-zram");
}

/* ── the cgroup half ─────────────────────────────────────────────────────── */

static int hierarchy_up(void) {
  mkdir(CGROOT, 0755);
  if (mount("cgroup2", CGROOT, "cgroup2", 0, NULL) != 0)
    return -1;
  if (write_file(CGROOT "/cgroup.subtree_control", "+memory") != 0)
    return -1;
  return 0;
}

static void hierarchy_down(void) {
  umount(CGROOT);
  rmdir(CGROOT);
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

static int cg_write(const char *cg, const char *file, const char *val) {
  char p[256];
  cgpath(p, sizeof(p), cg, file);
  return write_file(p, val);
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

/*
 * A child that joins `cg`, takes `mb` megabytes of anonymous memory it then
 * stops touching, and says so down the pipe. It writes a compressible pattern
 * on purpose: this is about whether the pages GO somewhere, and a page that
 * compresses is the case both zswap and zram are built for.
 *
 * After the write it sits still. Nothing it does afterwards may touch the
 * memory again, or the pages it just filled would be the hot ones and the
 * reclaim scan would rightly leave them alone.
 */
static pid_t spawn_hog(const char *cg, int mb, int notify_fd) {
  pid_t pid = fork();
  if (pid != 0)
    return pid;

  char me[32];
  snprintf(me, sizeof(me), "%d", (int)getpid());
  if (cg_write(cg, "cgroup.procs", me) != 0)
    _exit(90);

  size_t bytes = (size_t)mb * 1024 * 1024;
  unsigned char *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED)
    _exit(91);
  for (size_t off = 0; off < bytes; off += PAGE) {
    fill_compressible(p + off, (unsigned)(off / PAGE));
    /* Allocating as fast as the loop can go outruns the reclaim the charge
     * path does on the way past the limit. A real program does other work
     * between pages; this is the smallest stand-in for that. */
    if (((off / PAGE) & 255) == 255)
      msleep(2);
  }
  char c = 'k';
  if (write(notify_fd, &c, 1) != 1)
    _exit(92);
  for (;;)
    pause();
  _exit(0);
}

static int child_alive(pid_t pid, int *status_out) {
  int st = 0;
  pid_t r = waitpid(pid, &st, WNOHANG);
  if (r == 0)
    return 1;
  if (r == pid && status_out)
    *status_out = st;
  return 0;
}

static void reap(pid_t pid) {
  if (pid <= 0)
    return;
  kill(pid, SIGKILL);
  int st;
  waitpid(pid, &st, 0);
}

/* A cgroup over memory.max, with somewhere to put its pages: it must survive.
 * The same test also proves the charge: the pages that left are counted in
 * this cgroup's memory.swap.current and nowhere else. */
static void check_cgroup_reclaim(void) {
  if (!swap_on_zram) {
    failf("cgroup-reclaim", "swap is not on zram");
    return;
  }
  if (cg_make("hog") != 0) {
    failf("cgroup-reclaim", "mkdir: %s", strerror(errno));
    return;
  }
  /* 12 MiB of limit for 36 MiB of memory: two thirds of it has to go out.
   * The limit files take bytes, as they do on Linux -- "12M" would be twelve. */
  if (cg_write("hog", "memory.max", "12582912") != 0) {
    failf("cgroup-reclaim", "memory.max: %s", strerror(errno));
    cg_destroy("hog");
    return;
  }
  int pfd[2];
  if (pipe(pfd) != 0) {
    failf("cgroup-reclaim", "pipe: %s", strerror(errno));
    cg_destroy("hog");
    return;
  }
  pid_t pid = spawn_hog("hog", 36, pfd[1]);
  if (pid < 0) {
    failf("cgroup-reclaim", "fork: %s", strerror(errno));
    close(pfd[0]);
    close(pfd[1]);
    cg_destroy("hog");
    return;
  }
  close(pfd[1]);

  /* Wait for it to finish filling, or to die trying. */
  char c = 0;
  ssize_t got = read(pfd[0], &c, 1);
  close(pfd[0]);
  /* The limit is enforced when the cgroup crosses it, and crossings are rate
   * limited -- so the last one may still be ahead of us. */
  msleep(300);

  int status = 0;
  int alive = child_alive(pid, &status);
  long long swap_cur = cg_ll("hog", "memory.swap.current");
  long long pgsteal = cg_kv("hog", "memory.stat", "pgsteal");
  long long pgscan = cg_kv("hog", "memory.stat", "pgscan");
  long long current = cg_ll("hog", "memory.current");
  long long oom_kill = cg_kv("hog", "memory.events", "oom_kill");

  if (got != 1) {
    failf("cgroup-reclaim",
          "the child never finished: alive=%d status=%d oom_kill=%lld", alive,
          status, oom_kill);
    reap(pid);
    cg_destroy("hog");
    return;
  }
  if (!alive) {
    failf("cgroup-reclaim", "killed anyway (status %d, oom_kill %lld)", status,
          oom_kill);
    cg_destroy("hog");
    return;
  }
  if (pgsteal <= 0) {
    failf("cgroup-reclaim", "pgsteal is %lld: nothing was reclaimed", pgsteal);
    reap(pid);
    cg_destroy("hog");
    return;
  }
  if (current > 24ll * 1024 * 1024) {
    failf("cgroup-reclaim",
          "memory.current is %lld with a 12M limit: the limit did not hold",
          current);
    reap(pid);
    cg_destroy("hog");
    return;
  }
  note("36 MiB inside a 12M limit: current %lld, pgscan %lld, pgsteal %lld",
       current, pgscan, pgsteal);
  ok("cgroup-reclaim");

  /* The charge. Every page that went out is two bytes of attribution beside
   * its slot, and this is where it surfaces. */
  if (swap_cur <= 0) {
    failf("cgroup-swap-cur", "memory.swap.current is %lld after %lld pgsteal",
          swap_cur, pgsteal);
    reap(pid);
    cg_destroy("hog");
    return;
  }
  long long swap_stat = cg_kv("hog", "memory.stat", "swap");
  if (swap_stat != swap_cur) {
    failf("cgroup-swap-cur", "memory.stat swap %lld != memory.swap.current %lld",
          swap_stat, swap_cur);
    reap(pid);
    cg_destroy("hog");
    return;
  }
  note("memory.swap.current %lld bytes charged to the cgroup", swap_cur);

  /* And it is released. Killing the process frees the slots it owned, and the
   * charge has to follow them down.
   *
   * All the way to zero: every slot the child owned is freed when its address
   * space goes, whether the page sat on the device or in the compressed pool,
   * and each of those frees carries the charge back with it. Anything left
   * would be a charge that outlived the pages it was counting -- which is what
   * a swap-in from the compressed pool used to do. */
  reap(pid);
  long long after = swap_cur;
  for (int i = 0; i < 100; i++) {
    after = cg_ll("hog", "memory.swap.current");
    if (after == 0)
      break;
    msleep(20);
  }
  if (after != 0) {
    failf("cgroup-swap-cur",
          "%lld of %lld bytes still charged after the process died", after,
          swap_cur);
    cg_destroy("hog");
    return;
  }
  note("%lld bytes of swap charge released when the process died", swap_cur);
  g_hog_residual = after;
  ok("cgroup-swap-cur");
  /* The cgroup directory stays until the swapoff check has read it: removing a
   * cgroup that still holds a charge is a case of its own, and it is the next
   * thing this file tests. */
}

/* memory.swap.max: with nowhere to put the pages, the limit is enforced the
 * only way that is left. */
static void check_swap_max(void) {
  if (!swap_on_zram) {
    failf("swap-max", "swap is not on zram");
    return;
  }
  if (cg_make("capped") != 0) {
    failf("swap-max", "mkdir: %s", strerror(errno));
    return;
  }
  if (cg_write("capped", "memory.max", "12582912") != 0 ||
      cg_write("capped", "memory.swap.max", "1048576") != 0) {
    failf("swap-max", "limits: %s", strerror(errno));
    cg_destroy("capped");
    return;
  }
  if (cg_ll("capped", "memory.swap.max") != 1024 * 1024) {
    failf("swap-max", "memory.swap.max reads back %lld",
          cg_ll("capped", "memory.swap.max"));
    cg_destroy("capped");
    return;
  }
  int pfd[2];
  if (pipe(pfd) != 0) {
    failf("swap-max", "pipe: %s", strerror(errno));
    cg_destroy("capped");
    return;
  }
  pid_t pid = spawn_hog("capped", 36, pfd[1]);
  if (pid < 0) {
    failf("swap-max", "fork: %s", strerror(errno));
    close(pfd[0]);
    close(pfd[1]);
    cg_destroy("capped");
    return;
  }
  close(pfd[1]);
  char c = 0;
  ssize_t got = read(pfd[0], &c, 1);
  close(pfd[0]);

  int status = 0;
  int alive = child_alive(pid, &status);
  long long fail_ev = cg_kv("capped", "memory.swap.events", "fail");
  long long swap_cur = cg_ll("capped", "memory.swap.current");
  long long oom_kill = cg_kv("capped", "memory.events", "oom_kill");

  if (swap_cur > 2ll * 1024 * 1024) {
    failf("swap-max", "%lld bytes in swap against a 1M memory.swap.max",
          swap_cur);
    reap(pid);
    cg_destroy("capped");
    return;
  }
  if (fail_ev <= 0) {
    failf("swap-max", "memory.swap.events fail is %lld: nothing was refused",
          fail_ev);
    reap(pid);
    cg_destroy("capped");
    return;
  }
  if (got == 1 && alive) {
    failf("swap-max",
          "36 MiB fitted in a 12M limit with 1M of swap and nothing died");
    reap(pid);
    cg_destroy("capped");
    return;
  }
  if (oom_kill <= 0) {
    failf("swap-max", "the child is gone but memory.events oom_kill is %lld",
          oom_kill);
    reap(pid);
    cg_destroy("capped");
    return;
  }
  note("swap capped at 1M: %lld refusals, %lld OOM kills, %lld bytes out",
       fail_ev, oom_kill, swap_cur);
  reap(pid);
  cg_destroy("capped");
  ok("swap-max");
}

/* ── putting it back ─────────────────────────────────────────────────────── */

static void check_swapoff(void) {
  if (!swap_on_zram)
    return;
  if (swapoff(ZRAM) != 0) {
    failf("swapoff-zram", "swapoff: %s", strerror(errno));
    return;
  }
  /* swapoff pages every swapped page back into memory, so every slot is freed
   * and every charge with it -- including the ones the check above left
   * standing. Anything still counted here is a charge that leaked. */
  if (g_hog_residual > 0) {
    long long left = cg_ll("hog", "memory.swap.current");

    if (left != 0) {
      failf("swapoff-zram",
            "memory.swap.current is %lld after swapoff paged everything back",
            left);
      cg_destroy("hog");
      return;
    }
    note("the %lld bytes left charged went to zero when swap came off",
         g_hog_residual);
  }
  cg_destroy("hog");
  char now[128] = {0};
  if (current_swap(now, sizeof(now)) == 0 && strstr(now, "zram0")) {
    failf("swapoff-zram", "/proc/swaps still names %s", now);
    return;
  }
  swap_on_zram = 0;
  ok("swapoff-zram");
}

int main(void) {
  marker("M127-SWAP: start\n");

  check_zram_disksize();
  check_zram_roundtrip();
  check_zram_incompressible();
  check_zram_zero_pages();

  if (hierarchy_up() != 0) {
    failf("cgroup-reclaim", "mount cgroup2 on " CGROOT ": %s", strerror(errno));
    check_swapoff();
  } else {
    check_swapon_zram();
    check_cgroup_reclaim();
    check_swap_max();
    /* Before the hierarchy goes away: the swapoff check reads what is left
     * charged to a cgroup, and that file only exists while it is mounted. */
    check_swapoff();
    hierarchy_down();
  }

  /* Leave the machine as it was found: the lane's own swap disk back on, and
   * the compressed device released. The checks after this point belong to
   * other tests, and they are entitled to the swap they booted with. */
  if (g_prev_swap[0] && swapon(g_prev_swap, 0) != 0)
    note("could not put %s back: %s", g_prev_swap, strerror(errno));
  if (write_file(ZSYS "/reset", "1") != 0)
    note("zram reset: %s", strerror(errno));

  marker("M127-SWAP: done\n");
  return g_fail ? 1 : 0;
}
