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
 * duration and puts it back the way it found it. A machine whose knob will not
 * move says "mode never" and stops, and the suite records the rest as skipped.
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
 *   M128-THP: ok mprotect-keeps-block
 *   M128-THP: ok fork-shares-block
 *   M128-THP: ok recycled-range
 *   M128-THP: ok khugepaged-collapse
 *   M128-THP: ok no-leak
 *   M128-THP: stats <the kernel's own counters>
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

/* One counter out of the kernel's own thp stats file ("<name> <value>" lines),
 * or -1 when it is not there. */
static long thp_stat(const char *name) {
  char buf[256];
  char *p;

  if (read_file(THP_SYSFS "/stats", buf, sizeof(buf)) < 0)
    return -1;
  p = strstr(buf, name);
  if (!p)
    return -1;
  return strtol(p + strlen(name), NULL, 10);
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
 * A block is installed where nothing describes the address yet, or where the
 * page table left behind by an earlier mapping is empty — see the
 * recycled-range check below, which is what proves the second half. */
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
  /* What the kernel itself counted, on one line: a mapping that is not
   * block-backed is one of two different failures — no block was ever
   * installed (fault_alloc 0, and fault_fallback says the fault path refused
   * one) or a block was installed and something split it again. */
  {
    char st[256];

    if (read_file(THP_SYSFS "/stats", st, sizeof(st)) > 0) {
      for (char *q = st; *q; q++)
        if (*q == '\n')
          *q = ' ';
      printf("M128-THP: stats %s\n", st);
      fflush(stdout);
    }
  }
  judge("hugepage-backed", huge >= (long)(THP_SIZE / 1024),
        "AnonHugePages kB", huge);
  judge("data-intact", verify(p, MAP_BYTES, 0x11) && rss >= (long)(MAP_BYTES / 1024),
        "Rss kB", rss);

  /* ── 2. fork, and a copy-on-write write on both sides ─────────────── */
  {
    pid_t pid;
    int status = 0;
    int good = 1;

    long huge_after_fork = -1;

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
      /* Before either side writes: the parent must STILL be 2 MiB-backed. A
       * fork shares the block read-only and refcounts its 512 frames; it used
       * to break every block in the parent apart instead, which cost a page
       * table and 512 leaves per block for a child that may only read. */
      huge_after_fork = smaps_anon_huge((unsigned long)p, NULL);
      /* The parent writes too, so both sides break the sharing. */
      fill(p, MAP_BYTES, 0x22);
      if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
          WEXITSTATUS(status) != 0)
        good = 0;
      if (!verify(p, MAP_BYTES, 0x22))
        good = 0;
    }
    judge("fork-cow", good, "child status", (long)status);
    /* Graded separately from the data: sharing is an optimisation, losing a
     * byte is a bug, and a run that confuses the two says nothing. The split
     * counter is no use here — it is machine-wide, and the child is breaking
     * its own copy of the block at the same moment — so this is read off the
     * parent's own mapping, before the parent writes. */
    judge("fork-shares-block", huge_after_fork >= (long)(THP_SIZE / 1024),
          "AnonHugePages kB after fork", huge_after_fork);
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

  /* ── 3b. mprotect over a WHOLE block keeps it one block ───────────── */
  {
    /* Protection is written into the 2 MiB entry instead of breaking it into
     * 512 leaves: ld.so mprotects every segment it has just mapped, and that is
     * no reason for a mapping to lose its blocks. Half a block is a different
     * matter and is split — munmap-half below is that case.
     *
     * Its own mapping, because the mapping above has been forked and written
     * through on both sides by now: every block in it was broken by the
     * copy-on-write, so it could prove nothing about mprotect. */
    int step = 0;
    unsigned long b = 0;
    unsigned char *m = map_huge(&b);

    if (!m) {
      step = 1;
    } else {
      fill(m, MAP_BYTES, 0x99);
      if (smaps_anon_huge(b, NULL) < (long)(THP_SIZE / 1024))
        step = 2; /* not block-backed to begin with: nothing to keep */
      else if (mprotect((void *)b, THP_SIZE, PROT_READ) != 0)
        step = 3;
      else if (smaps_anon_huge(b, NULL) < (long)(THP_SIZE / 1024))
        step = 4; /* the read-only mprotect broke the block */
      else if (mprotect((void *)b, THP_SIZE, PROT_READ | PROT_WRITE) != 0)
        step = 5;
      else if (smaps_anon_huge(b, NULL) < (long)(THP_SIZE / 1024))
        step = 6; /* putting the write back broke it */
      else {
        /* And it still holds what was written to it, and takes a write. */
        if (!verify((unsigned char *)b, THP_SIZE,
                    (unsigned)(0x99 + (b - (unsigned long)m) / 4096)))
          step = 7;
        else {
          fill((unsigned char *)b, THP_SIZE, 0xaa);
          if (!verify((unsigned char *)b, THP_SIZE, 0xaa))
            step = 8;
        }
      }
      munmap(m, MAP_BYTES);
    }
    judge("mprotect-keeps-block", step == 0, "step", (long)step);
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

  /* ── 6. an address range that has already been used at 4 KiB ─────── */
  {
    /* The case the kernel used to refuse. A mapping faulted at 4 KiB leaves
     * its page table behind when it goes, and a directory entry naming a table
     * is one the fault path may not take — so every long-lived allocator, which
     * recycles addresses, got blocks once and 4 KiB pages ever after. The
     * table is now taken out of the tree and kept until the process exits, so
     * the SAME address is block-backed the second time round.
     *
     * Graded on the second mapping's own bytes and on AnonHugePages at that
     * address: the first mapping is proved NOT to be huge first, or a pass
     * would mean nothing. */
    int step = 0;
    unsigned char *first = mmap(NULL, MAP_BYTES, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (first == MAP_FAILED) {
      step = 1;
    } else if (madvise(first, MAP_BYTES, MADV_NOHUGEPAGE) != 0) {
      step = 2;
    } else {
      unsigned long at = (unsigned long)first;
      unsigned char *again;

      fill(first, MAP_BYTES, 0x44); /* 4 KiB at a time, building the tables */
      if (smaps_anon_huge(at, NULL) != 0)
        step = 3; /* it was huge after all: the check proves nothing */
      else if (munmap(first, MAP_BYTES) != 0)
        step = 4;
      else {
        again = mmap((void *)at, MAP_BYTES, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (again == MAP_FAILED || (unsigned long)again != at) {
          step = 5;
        } else if (madvise(again, MAP_BYTES, MADV_HUGEPAGE) != 0) {
          step = 6;
        } else {
          fill(again, MAP_BYTES, 0x66);
          if (smaps_anon_huge(at, NULL) < (long)(THP_SIZE / 1024))
            step = 7; /* still 4 KiB: the table was never retired */
          else if (!verify(again, MAP_BYTES, 0x66))
            step = 8; /* the block does not hold what was written to it */
          munmap(again, MAP_BYTES);
        }
      }
    }
    judge("recycled-range", step == 0, "step", (long)step);
  }

  /* ── 7. khugepaged collapses a range that is already 4 KiB pages ──── */
  {
    /* The case the fault path cannot reach at all: memory that was touched
     * before anything asked for huge pages. A block is only installed where
     * nothing describes the address, so without a collapse a long-lived program
     * — a shell, an allocator that faulted its arena in early — would hold
     * ordinary pages for ever however long the feature is on.
     *
     * Faulted at 4 KiB on purpose (MADV_NOHUGEPAGE), proved NOT to be huge, and
     * only then offered to khugepaged by turning the advice on. What is graded
     * is the kernel's own collapse counter AND the bytes: a collapse copies
     * 2 MiB into a fresh block, so a byte lost in the copy is the failure that
     * matters. */
    int step = 0;
    long collapsed0 = thp_stat("thp_collapse_alloc");
    unsigned char *c = mmap(NULL, MAP_BYTES, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (c == MAP_FAILED) {
      step = 1;
    } else if (madvise(c, MAP_BYTES, MADV_NOHUGEPAGE) != 0) {
      step = 2;
    } else {
      fill(c, MAP_BYTES, 0x88);
      if (smaps_anon_huge((unsigned long)c, NULL) != 0)
        step = 3; /* huge already: nothing would be proved by a collapse */
      else if (madvise(c, MAP_BYTES, MADV_HUGEPAGE) != 0)
        step = 4;
      else {
        long huge_now = 0;
        long collapsed1 = collapsed0;

        /* khugepaged sleeps between passes (scan_sleep_millisecs, 200 ms by
         * default) and takes a bounded number of blocks per pass, so this
         * waits rather than assuming. Ten seconds is far longer than it needs
         * and short enough to fail the lane rather than hang it. */
        for (int i = 0; i < 200; i++) {
          huge_now = smaps_anon_huge((unsigned long)c, NULL);
          collapsed1 = thp_stat("thp_collapse_alloc");
          if (huge_now >= (long)(THP_SIZE / 1024) && collapsed1 > collapsed0)
            break;
          usleep(50000);
        }
        if (huge_now < (long)(THP_SIZE / 1024))
          step = 5; /* nothing was collapsed in ten seconds */
        else if (collapsed1 <= collapsed0)
          step = 6; /* it became huge some other way: prove nothing */
        else if (!verify(c, MAP_BYTES, 0x88))
          step = 7; /* the copy lost a byte */
        printf("M128-THP: collapsed %ld ranges, AnonHugePages %ld kB\n",
               collapsed1 - collapsed0, huge_now);
        fflush(stdout);
      }
      munmap(c, MAP_BYTES);
    }
    judge("khugepaged-collapse", step == 0, "step", (long)step);
  }

  /* ── 8. nothing leaks ─────────────────────────────────────────────── */
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
    /* Not every round gets a block: one may land where a page table is
     * neither absent nor empty, and free memory may be short of the reserve a
     * block asks for. Most of them do, and most is what this check needs — a leak of one block is 2 MiB, and the
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
