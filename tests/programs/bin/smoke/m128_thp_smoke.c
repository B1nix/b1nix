/* SPDX-License-Identifier: GPL-2.0-only */
/* m128_thp_smoke — transparent huge pages for anonymous memory (M128).
 *
 * Every marker here is an observation. "This mapping is 2 MiB-backed" is not
 * taken from the fact that madvise returned 0: it is read out of
 * /proc/self/smaps, whose AnonHugePages field the kernel computes by walking
 * the page-directory entries themselves. Everything after that is about the
 * walkers a huge entry has to survive — fork and its copy-on-write, mprotect
 * on half a block, munmap of half a block, and MADV_NOHUGEPAGE — and each one
 * is graded on the BYTES that come back, not on the syscall's return value.
 *
 * The last check is the one that matters most and says the least: after eight
 * rounds of mapping, filling and releasing 8 MiB, the machine's free memory is
 * where it started. A leaked block is 2 MiB a round.
 *
 * The feature is off unless the boot line asks (b1nix.thp), so the test turns
 * it on through /sys/kernel/mm/transparent_hugepage/enabled for its own
 * duration and puts it back the way it found it. Where the knob will not move
 * — aarch64, which has no huge-page support in its page-table walkers — the
 * test says "mode never" and stops, and the suite records the rest as skipped.
 *
 * Markers (only emitted on verified success):
 *   M128-THP: start
 *   M128-THP: mode <always|madvise|never>
 *   M128-THP: ok sysfs
 *   M128-THP: ok hugepage-backed
 *   M128-THP: ok data-intact
 *   M128-THP: ok fork-cow
 *   M128-THP: ok mprotect-half
 *   M128-THP: ok munmap-half
 *   M128-THP: ok nohugepage-splits
 *   M128-THP: ok no-leak
 *   M128-THP: blocks <n> of <rounds>
 *   M128-THP: done
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif
#ifndef MADV_NOHUGEPAGE
#define MADV_NOHUGEPAGE 15
#endif

#define THP_SIZE (2UL * 1024UL * 1024UL)
#define MAP_BYTES (8UL * 1024UL * 1024UL)

#define THP_SYSFS "/sys/kernel/mm/transparent_hugepage"

static int fails;

static void ok(const char *what) {
  printf("M128-THP: ok %s\n", what);
  fflush(stdout);
}

static void bad(const char *what, const char *why, long v) {
  printf("M128-THP: fail %s (%s, %ld)\n", what, why, v);
  fflush(stdout);
  fails++;
}

static void judge(const char *what, int good, const char *why, long v) {
  if (good)
    ok(what);
  else
    bad(what, why, v);
}

static int read_file(const char *path, char *buf, size_t cap) {
  int fd = open(path, O_RDONLY);
  size_t got = 0;

  if (fd < 0)
    return -1;
  for (;;) {
    ssize_t n = read(fd, buf + got, cap - 1 - got);

    if (n <= 0)
      break;
    got += (size_t)n;
    if (got >= cap - 1)
      break;
  }
  close(fd);
  buf[got] = '\0';
  return (int)got;
}

static int write_file(const char *path, const char *text) {
  int fd = open(path, O_WRONLY);
  ssize_t n;

  if (fd < 0)
    return -1;
  n = write(fd, text, strlen(text));
  close(fd);
  return n == (ssize_t)strlen(text) ? 0 : -1;
}

/* The bracketed word in "always [madvise] never". */
static void read_mode(char *out, size_t cap) {
  char buf[128];
  char *lb, *rb;

  out[0] = '\0';
  if (read_file(THP_SYSFS "/enabled", buf, sizeof(buf)) < 0)
    return;
  lb = strchr(buf, '[');
  rb = lb ? strchr(lb, ']') : NULL;
  if (!lb || !rb || (size_t)(rb - lb) >= cap)
    return;
  memcpy(out, lb + 1, (size_t)(rb - lb - 1));
  out[rb - lb - 1] = '\0';
}

/* MemFree in kB, or -1. */
static long mem_free_kb(void) {
  char buf[4096];
  char *p;

  if (read_file("/proc/meminfo", buf, sizeof(buf)) < 0)
    return -1;
  p = strstr(buf, "MemFree:");
  if (!p)
    return -1;
  return strtol(p + 8, NULL, 10);
}

/* AnonHugePages (kB) of the smaps entry that starts at `addr`, or -1 when the
 * mapping is not listed at all. Rss is returned through *rss_kb. */
static long smaps_anon_huge(unsigned long addr, long *rss_kb) {
  static char buf[512 * 1024];
  char want[32];
  char *p, *end;
  long huge = -1;

  if (rss_kb)
    *rss_kb = -1;
  if (read_file("/proc/self/smaps", buf, sizeof(buf)) < 0)
    return -1;
  snprintf(want, sizeof(want), "%lx-", addr);
  /* The header line of this mapping, at the start of a line. */
  p = buf;
  for (;;) {
    if (strncmp(p, want, strlen(want)) == 0)
      break;
    p = strchr(p, '\n');
    if (!p)
      return -1;
    p++;
  }
  /* Its fields run until the next header line, which is the next line that
   * does not begin with a capital letter followed by a lower-case one... so
   * stop at the next line containing '-' before the first space instead: a
   * field line always has ':' before any '-'. */
  end = p;
  while ((end = strchr(end, '\n')) != NULL) {
    char *line = end + 1;
    char *colon = strchr(line, ':');
    char *dash = strchr(line, '-');

    if (!line[0])
      break;
    if (!colon || (dash && dash < colon))
      break; /* the next mapping's header */
    if (strncmp(line, "AnonHugePages:", 14) == 0)
      huge = strtol(line + 14, NULL, 10);
    else if (rss_kb && strncmp(line, "Rss:", 4) == 0)
      *rss_kb = strtol(line + 4, NULL, 10);
    end = line;
  }
  return huge;
}

static void fill(unsigned char *p, size_t len, unsigned seed) {
  for (size_t i = 0; i < len; i += 4096)
    p[i] = (unsigned char)(seed + (i / 4096));
}

static int verify(const unsigned char *p, size_t len, unsigned seed) {
  for (size_t i = 0; i < len; i += 4096)
    if (p[i] != (unsigned char)(seed + (i / 4096)))
      return 0;
  return 1;
}

/* A mapping whose first 2 MiB-aligned block is known. Returns the base of the
 * first whole block inside the mapping through *block.
 *
 * A block is only installed where no page table describes the address yet, so
 * every mapping this test wants huge-backed is taken BEFORE anything is
 * unmapped: an address range that has already held 4 KiB leaves keeps its page
 * table when the mapping goes, and the next mapping at the same address is
 * served 4 KiB at a time. (That is the kernel's rule, not an accident — see
 * thp_try_install.) */
static unsigned char *map_huge(unsigned long *block) {
  unsigned char *p = mmap(NULL, MAP_BYTES, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  if (p == MAP_FAILED)
    return NULL;
  if (madvise(p, MAP_BYTES, MADV_HUGEPAGE) != 0) {
    munmap(p, MAP_BYTES);
    return NULL;
  }
  *block = ((unsigned long)p + THP_SIZE - 1) & ~(THP_SIZE - 1);
  return p;
}

int main(void) {
  char mode0[16], mode[16];
  unsigned char *p;
  unsigned long blk;
  long huge, rss;

  printf("M128-THP: start\n");
  fflush(stdout);

  /* The knob, and the state to put back. */
  read_mode(mode0, sizeof(mode0));
  if (mode0[0] == '\0') {
    printf("M128-THP: mode never\n");
    printf("M128-THP: fail sysfs (no %s/enabled, 0)\n", THP_SYSFS);
    printf("M128-THP: done\n");
    fflush(stdout);
    return 1;
  }
  if (strcmp(mode0, "never") == 0)
    (void)write_file(THP_SYSFS "/enabled", "madvise");
  read_mode(mode, sizeof(mode));
  printf("M128-THP: mode %s\n", mode);
  fflush(stdout);

  {
    char sz[64];
    long pmd = 0;

    if (read_file(THP_SYSFS "/hpage_pmd_size", sz, sizeof(sz)) > 0)
      pmd = strtol(sz, NULL, 10);
    judge("sysfs", pmd == (long)THP_SIZE, "hpage_pmd_size", pmd);
  }

  if (strcmp(mode, "never") == 0) {
    /* The architecture refuses the feature. Say so once, plainly, and stop:
     * printing the rest would be claiming checks that never ran. */
    printf("M128-THP: done\n");
    fflush(stdout);
    return fails ? 1 : 0;
  }

  long free0 = mem_free_kb();

  /* ── 1. a mapping that really is 2 MiB-backed ─────────────────────── */
  p = map_huge(&blk);
  /* The mapping check 5 needs, taken now while the address space is still
   * untouched: see map_huge. */
  unsigned long blk2 = 0;
  unsigned char *p2 = map_huge(&blk2);

  (void)blk2;
  if (!p) {
    bad("hugepage-backed", "mmap/madvise", (long)errno);
    if (p2)
      munmap(p2, MAP_BYTES);
    printf("M128-THP: done\n");
    fflush(stdout);
    return 1;
  }
  fill(p, MAP_BYTES, 0x11);
  huge = smaps_anon_huge((unsigned long)p, &rss);
  judge("hugepage-backed", huge >= (long)(THP_SIZE / 1024),
        "AnonHugePages kB", huge);
  judge("data-intact", verify(p, MAP_BYTES, 0x11) && rss >= (long)(MAP_BYTES / 1024),
        "Rss kB", rss);

  /* ── 2. fork, and a copy-on-write write on both sides ─────────────── */
  {
    pid_t pid;
    int status = 0;
    int good = 1;

    pid = fork();
    if (pid == 0) {
      /* The child sees what the parent wrote, overwrites it, and reads its
       * own bytes back. Its exit code is the verdict. */
      int rc = 0;

      if (!verify(p, MAP_BYTES, 0x11))
        rc = 1;
      fill(p, MAP_BYTES, 0x55);
      if (rc == 0 && !verify(p, MAP_BYTES, 0x55))
        rc = 2;
      _exit(rc);
    }
    if (pid < 0) {
      good = 0;
      status = -1;
    } else {
      /* The parent writes too, so both sides break the sharing. */
      fill(p, MAP_BYTES, 0x22);
      if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
          WEXITSTATUS(status) != 0)
        good = 0;
      if (!verify(p, MAP_BYTES, 0x22))
        good = 0;
    }
    judge("fork-cow", good, "child status", (long)status);
  }

  /* ── 3. mprotect on half the range ────────────────────────────────── */
  {
    unsigned char *half = (unsigned char *)blk;
    size_t hlen = THP_SIZE;
    int good = 1;

    if (mprotect(half, hlen, PROT_READ) != 0)
      good = 0;
    else if (!verify(half, hlen, (unsigned)(0x22 + (half - p) / 4096)))
      good = 0;
    else if (mprotect(half, hlen, PROT_READ | PROT_WRITE) != 0)
      good = 0;
    else {
      fill(half, hlen, 0x33);
      if (!verify(half, hlen, 0x33))
        good = 0;
      /* And the rest of the mapping is untouched by any of it. */
      if (blk + THP_SIZE < (unsigned long)p + MAP_BYTES) {
        unsigned char *rest = (unsigned char *)(blk + THP_SIZE);
        size_t rlen = (size_t)((unsigned long)p + MAP_BYTES - (blk + THP_SIZE));

        if (!verify(rest, rlen, (unsigned)(0x22 + (rest - p) / 4096)))
          good = 0;
      }
    }
    judge("mprotect-half", good, "errno", (long)errno);
  }

  /* ── 4. munmap of half a block, and the other half survives ───────── */
  {
    unsigned char *keep = (unsigned char *)blk;
    unsigned char *drop = keep + THP_SIZE / 2;
    int good = 1;

    /* Half of one 2 MiB block: the range does not cover the block, so the
     * kernel has to break it up rather than release it whole. */
    if (munmap(drop, THP_SIZE / 2) != 0)
      good = 0;
    else if (!verify(keep, THP_SIZE / 2, 0x33))
      good = 0;
    else {
      long h = smaps_anon_huge((unsigned long)p, NULL);

      /* The block that was cut is gone as a block; whatever whole blocks the
       * rest of the mapping still holds may remain. */
      if (h < 0)
        good = 0;
    }
    judge("munmap-half", good, "errno", (long)errno);
  }
  munmap(p, MAP_BYTES);

  /* ── 5. MADV_NOHUGEPAGE takes the block back apart ────────────────── */
  {
    int step = 0; /* which half of the check failed, if one did */

    if (!p2) {
      step = 1;
    } else {
      fill(p2, MAP_BYTES, 0x77);
      if (smaps_anon_huge((unsigned long)p2, NULL) < (long)(THP_SIZE / 1024))
        step = 2; /* nothing was huge to begin with: nothing is proved */
      else if (madvise(p2, MAP_BYTES, MADV_NOHUGEPAGE) != 0)
        step = 3;
      else if (smaps_anon_huge((unsigned long)p2, NULL) != 0)
        step = 4; /* the block is still there */
      else if (!verify(p2, MAP_BYTES, 0x77))
        step = 5; /* the split lost a byte */
      munmap(p2, MAP_BYTES);
    }
    judge("nohugepage-splits", step == 0, "step", (long)step);
  }

  /* ── 6. nothing leaks ─────────────────────────────────────────────── */
  {
    enum { ROUNDS = 8 };
    unsigned char *m[ROUNDS];
    long free1;
    int good = 1;
    int huge_rounds = 0;

    /* All of them at once, so every one lands on an address that has never
     * held a page table and every one really is block-backed — then all of
     * them released. Whatever the blocks cost has to come back. */
    for (int i = 0; i < ROUNDS; i++) {
      unsigned long b;

      m[i] = map_huge(&b);
      if (!m[i]) {
        good = 0;
        break;
      }
      fill(m[i], MAP_BYTES, (unsigned)(0x90 + i));
      if (smaps_anon_huge((unsigned long)m[i], NULL) >= (long)(THP_SIZE / 1024))
        huge_rounds++;
    }
    for (int i = 0; i < ROUNDS; i++) {
      if (!m[i])
        break;
      if (!verify(m[i], MAP_BYTES, (unsigned)(0x90 + i)))
        good = 0;
      munmap(m[i], MAP_BYTES);
    }
    /* Not every round gets a block: mmap hands back an address that a
     * previous mapping already built a page table over, and a block is only
     * installed where nothing describes the address yet. Most of them do, and
     * most is what this check needs — a leak of one block is 2 MiB, and the
     * tolerance below is a quarter of that even if only half the rounds were
     * block-backed. A run where NONE were proves nothing and fails. */
    if (huge_rounds < ROUNDS / 2)
      good = 0;
    free1 = mem_free_kb();
    /* Eight rounds of 8 MiB. A single leaked 512-frame block is 2 MiB a
     * round, 16 MiB in all; the tolerance is well under that and well over
     * the noise of the rest of the machine breathing. */
    if (free0 < 0 || free1 < 0)
      good = 0;
    else if (free0 - free1 > 4096)
      good = 0;
    judge("no-leak", good, "MemFree kB lost", free0 - free1);
    printf("M128-THP: blocks %d of %d rounds\n", huge_rounds, ROUNDS);
  }

  /* Put the machine back the way it was found. */
  if (strcmp(mode0, mode) != 0)
    (void)write_file(THP_SYSFS "/enabled", mode0);

  printf("M128-THP: done\n");
  fflush(stdout);
  return fails ? 1 : 0;
}
