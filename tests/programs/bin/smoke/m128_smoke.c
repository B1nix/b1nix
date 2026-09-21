/* SPDX-License-Identifier: GPL-2.0-only */
/* m128_smoke — NUMA: the topology the kernel publishes, and the memory policy
 * that decides which node a page comes from (M128).
 *
 * Every check here is an observation, not a claim: a node's free memory is
 * read from /sys before and after touching pages, and the marker is printed
 * only when the pages landed on the node the policy named. On a machine with
 * one node the topology checks still run (Linux publishes node0 there too) and
 * the placement checks say so and stop, rather than passing for free.
 *
 * Markers (only emitted on verified success):
 *   M128-SMOKE: start
 *   M128-SMOKE: ok node-online
 *   M128-SMOKE: ok node-meminfo
 *   M128-SMOKE: ok node-distance
 *   M128-SMOKE: ok node-cpulist
 *   M128-SMOKE: ok mems-allowed
 *   M128-SMOKE: ok bind-allocates-there   (multi-node only)
 *   M128-SMOKE: ok mbind-range            (multi-node only)
 *   M128-SMOKE: ok interleave-spreads     (multi-node only)
 *   M128-SMOKE: ok policy-inherited
 *   M128-SMOKE: ok bad-node-refused
 *   M128-SMOKE: nodes <n>
 *   M128-SMOKE: done
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define MPOL_DEFAULT        0
#define MPOL_PREFERRED      1
#define MPOL_BIND           2
#define MPOL_INTERLEAVE     3
#define MPOL_LOCAL          4
#define MPOL_F_NODE         (1 << 0)
#define MPOL_F_ADDR         (1 << 1)
#define MPOL_F_MEMS_ALLOWED (1 << 2)
#define MPOL_MF_STRICT      (1 << 0)
#define MPOL_MF_MOVE        (1 << 1)

static int fails;

static void ok(const char *what) {
  printf("M128-SMOKE: ok %s\n", what);
  fflush(stdout);
}

static void bad(const char *what, const char *why, long v) {
  printf("M128-SMOKE: fail %s (%s, %ld)\n", what, why, v);
  fflush(stdout);
  fails++;
}

static void judge(const char *what, int good, const char *why, long v) {
  if (good)
    ok(what);
  else
    bad(what, why, v);
}

static long set_mempolicy_(int mode, const unsigned long *mask,
                           unsigned long maxnode) {
  return syscall(SYS_set_mempolicy, mode, mask, maxnode);
}

static long get_mempolicy_(int *mode, unsigned long *mask,
                           unsigned long maxnode, void *addr,
                           unsigned long flags) {
  return syscall(SYS_get_mempolicy, mode, mask, maxnode, addr, flags);
}

static long mbind_(void *addr, unsigned long len, int mode,
                   const unsigned long *mask, unsigned long maxnode,
                   unsigned flags) {
  return syscall(SYS_mbind, addr, len, mode, mask, maxnode, flags);
}

/* ── /sys readers ────────────────────────────────────────────────────────── */

static int read_file(const char *path, char *buf, size_t cap) {
  int fd = open(path, O_RDONLY);
  ssize_t n;

  if (fd < 0)
    return -1;
  n = read(fd, buf, cap - 1);
  close(fd);
  if (n < 0)
    return -1;
  buf[n] = 0;
  return (int)n;
}

/* MemFree of a node, in kB, or -1. */
static long node_free_kb(int node) {
  char path[64], buf[512], *p;

  snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/meminfo", node);
  if (read_file(path, buf, sizeof(buf)) < 0)
    return -1;
  p = strstr(buf, "MemFree:");
  if (!p)
    return -1;
  return strtol(p + 8, 0, 10);
}

static long node_total_kb(int node) {
  char path[64], buf[512], *p;

  snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/meminfo", node);
  if (read_file(path, buf, sizeof(buf)) < 0)
    return -1;
  p = strstr(buf, "MemTotal:");
  if (!p)
    return -1;
  return strtol(p + 9, 0, 10);
}

/* How many nodes /sys says are online ("0" or "0-1" or "0,2"). */
static int online_nodes(void) {
  char buf[64];
  int count = 0;

  if (read_file("/sys/devices/system/node/online", buf, sizeof(buf)) < 0)
    return -1;
  for (char *p = buf; *p;) {
    long a = strtol(p, &p, 10);
    long b = a;

    if (*p == '-')
      b = strtol(p + 1, &p, 10);
    count += (int)(b - a + 1);
    while (*p == ',')
      p++;
    if (*p == '\n' || *p == 0)
      break;
  }
  return count;
}

/* The node a mapped page actually sits on, straight from the kernel. */
static int node_of(void *addr) {
  int node = -1;

  if (get_mempolicy_(&node, 0, 0, addr, MPOL_F_NODE | MPOL_F_ADDR) != 0)
    return -1;
  return node;
}

/* Touch `pages` pages so they are really allocated. */
static void touch(char *p, int pages) {
  for (int i = 0; i < pages; i++)
    p[(size_t)i * 4096] = (char)(i + 1);
}

/* ── the checks ──────────────────────────────────────────────────────────── */

static int g_nodes;

static void check_topology(void) {
  char buf[256];
  int n = online_nodes();

  g_nodes = n;
  judge("node-online", n >= 1, "/sys/devices/system/node/online is unreadable",
        n);
  if (n < 1)
    return;
  printf("M128-SMOKE: nodes %d\n", n);
  fflush(stdout);

  long total = 0, free_kb = 0;
  int meminfo_ok = 1;

  for (int i = 0; i < n; i++) {
    long t = node_total_kb(i), f = node_free_kb(i);

    if (t <= 0 || f < 0 || f > t)
      meminfo_ok = 0;
    total += t;
    free_kb += f;
  }
  judge("node-meminfo", meminfo_ok && total > 0 && free_kb > 0,
        "a node's meminfo is missing, empty or claims more free than it has",
        total);

  /* distance: the local one is 10, and the matrix has one entry per node. */
  int dist_ok = 1;

  for (int i = 0; i < n; i++) {
    char path[64];
    int seen = 0;
    char *p;

    snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/distance", i);
    if (read_file(path, buf, sizeof(buf)) < 0) {
      dist_ok = 0;
      break;
    }
    p = buf;
    for (int j = 0; j < n; j++) {
      long d = strtol(p, &p, 10);

      if (j == i && d != 10)
        dist_ok = 0;
      if (j != i && d < 10)
        dist_ok = 0;
      seen++;
    }
    if (seen != n)
      dist_ok = 0;
  }
  judge("node-distance", dist_ok,
        "a node's distance row is missing, short, or does not start at 10", n);

  /* cpulist: every node this machine has names at least one CPU, and the
   * union of them is every CPU. */
  int cpus_named = 0;

  for (int i = 0; i < n; i++) {
    char path[64];

    snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/cpulist", i);
    if (read_file(path, buf, sizeof(buf)) < 0)
      continue;
    for (char *p = buf; *p;) {
      long a = strtol(p, &p, 10);
      long b = a;

      if (*p == '-')
        b = strtol(p + 1, &p, 10);
      cpus_named += (int)(b - a + 1);
      while (*p == ',')
        p++;
      if (*p == '\n' || *p == 0)
        break;
    }
  }
  judge("node-cpulist", cpus_named >= 1,
        "no node names a CPU, so numactl would report a node nothing runs on",
        cpus_named);
}

static void check_mems_allowed(void) {
  unsigned long mask = 0;
  int mode = -1;
  long rc = get_mempolicy_(&mode, &mask, 64, 0, MPOL_F_MEMS_ALLOWED);
  unsigned long want = (g_nodes >= 64) ? ~0UL : ((1UL << g_nodes) - 1);

  judge("mems-allowed", rc == 0 && mask == want,
        "MPOL_F_MEMS_ALLOWED does not name exactly the nodes /sys lists",
        (long)mask);
}

/* An allocation bound to a node comes from that node: measured twice, by the
 * kernel's own answer for the page and by the node's free memory moving. */
static void check_bind(void) {
  const int pages = 512; /* 2 MiB, well above the noise of a quiet lane */
  size_t len = (size_t)pages * 4096;

  for (int node = 0; node < g_nodes; node++) {
    unsigned long mask = 1UL << node;
    char *p;
    long before_here, after_here, before_other, after_other;
    int other = node ? 0 : 1;
    int wrong = 0;

    p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1,
             0);
    if (p == MAP_FAILED) {
      bad("bind-allocates-there", "mmap failed", errno);
      return;
    }
    if (mbind_(p, len, MPOL_BIND, &mask, 64, 0) != 0) {
      bad("bind-allocates-there", "mbind refused the binding", errno);
      munmap(p, len);
      return;
    }
    before_here = node_free_kb(node);
    before_other = node_free_kb(other);
    touch(p, pages);
    after_here = node_free_kb(node);
    after_other = node_free_kb(other);

    for (int i = 0; i < pages; i += 64)
      if (node_of(p + (size_t)i * 4096) != node)
        wrong++;
    /* The bound node lost at least the memory we touched; the other one did
     * not lose that much. Both sides matter: a kernel that simply allocated
     * everywhere would move the first number too. */
    if (wrong || (before_here - after_here) < pages * 4 - 512 ||
        (before_other - after_other) > pages * 2) {
      char why[128];

      snprintf(why, sizeof(why),
               "node %d: %d pages off-node, this node lost %ld kB, the other "
               "lost %ld kB",
               node, wrong, before_here - after_here,
               before_other - after_other);
      bad("bind-allocates-there", why, node);
      munmap(p, len);
      return;
    }
    munmap(p, len);
  }
  ok("bind-allocates-there");
}

/* mbind on part of a mapping binds that part and leaves the rest alone, and
 * MPOL_MF_MOVE moves pages that are already on the wrong node. */
static void check_mbind_range(void) {
  const int pages = 256;
  size_t len = (size_t)pages * 4096;
  unsigned long node1 = 1UL << 1;
  char *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                 -1, 0);
  int moved = 0, untouched_ok = 1;

  if (p == MAP_FAILED) {
    bad("mbind-range", "mmap failed", errno);
    return;
  }
  /* Bind the whole thing to node 0 and fill it, so the pages exist and are
   * knowably on the wrong node for what comes next. */
  unsigned long node0 = 1UL << 0;

  if (mbind_(p, len, MPOL_BIND, &node0, 64, 0) != 0) {
    bad("mbind-range", "mbind(node 0) refused", errno);
    munmap(p, len);
    return;
  }
  touch(p, pages);

  /* Move the first half to node 1. */
  if (mbind_(p, len / 2, MPOL_BIND, &node1, 64, MPOL_MF_MOVE) != 0) {
    bad("mbind-range", "mbind(MPOL_MF_MOVE) refused", errno);
    munmap(p, len);
    return;
  }
  for (int i = 0; i < pages / 2; i += 8)
    if (node_of(p + (size_t)i * 4096) == 1)
      moved++;
  for (int i = pages / 2; i < pages; i += 8)
    if (node_of(p + (size_t)i * 4096) != 0)
      untouched_ok = 0;
  judge("mbind-range", moved == pages / 16 && untouched_ok,
        "the moved half is not on node 1, or the other half moved with it",
        moved);
  /* The contents survived the move: a migration that loses data is worse
   * than one that never happened. */
  for (int i = 0; i < pages; i++)
    if (p[(size_t)i * 4096] != (char)(i + 1))
      untouched_ok = 0;
  if (!untouched_ok)
    bad("mbind-range", "a migrated page came back with the wrong contents", 0);
  munmap(p, len);
}

/* MPOL_INTERLEAVE spreads a mapping over the nodes in its mask. */
static void check_interleave(void) {
  const int pages = 256;
  size_t len = (size_t)pages * 4096;
  unsigned long mask = (1UL << g_nodes) - 1;
  char *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                 -1, 0);
  int per_node[8] = {0};
  int spread = 1;

  if (p == MAP_FAILED) {
    bad("interleave-spreads", "mmap failed", errno);
    return;
  }
  if (mbind_(p, len, MPOL_INTERLEAVE, &mask, 64, 0) != 0) {
    bad("interleave-spreads", "mbind(MPOL_INTERLEAVE) refused", errno);
    munmap(p, len);
    return;
  }
  touch(p, pages);
  for (int i = 0; i < pages; i++) {
    int n = node_of(p + (size_t)i * 4096);

    if (n >= 0 && n < 8)
      per_node[n]++;
  }
  for (int i = 0; i < g_nodes; i++)
    if (per_node[i] < pages / (g_nodes * 4))
      spread = 0;
  judge("interleave-spreads", spread,
        "an interleaved mapping did not reach every node in its mask",
        per_node[0]);
  munmap(p, len);
}

/* A task's policy is its child's. */
static void check_inherited(void) {
  unsigned long mask = 1UL << (g_nodes > 1 ? 1 : 0);
  int status = 0;
  pid_t child;

  if (set_mempolicy_(MPOL_BIND, &mask, 64) != 0) {
    bad("policy-inherited", "set_mempolicy refused", errno);
    return;
  }
  child = fork();
  if (child == 0) {
    int mode = -1;
    unsigned long got = 0;

    if (get_mempolicy_(&mode, &got, 64, 0, 0) != 0)
      _exit(2);
    _exit((mode == MPOL_BIND && got == mask) ? 0 : 3);
  }
  if (child < 0) {
    bad("policy-inherited", "fork failed", errno);
    return;
  }
  waitpid(child, &status, 0);
  judge("policy-inherited", WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "the child did not read back the policy its parent set",
        WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  set_mempolicy_(MPOL_DEFAULT, 0, 0);
}

/* A node that does not exist is EINVAL, whatever the mode. */
static void check_bad_node(void) {
  unsigned long far = 1UL << 40;
  int rc1 = (int)set_mempolicy_(MPOL_BIND, &far, 64);
  int e1 = errno;
  unsigned long none = 0;
  int rc2 = (int)set_mempolicy_(MPOL_BIND, &none, 64);
  int e2 = errno;
  int rc3 = (int)set_mempolicy_(99, 0, 0);
  int e3 = errno;

  judge("bad-node-refused",
        rc1 == -1 && e1 == EINVAL && rc2 == -1 && e2 == EINVAL && rc3 == -1 &&
            e3 == EINVAL,
        "a policy naming a node the machine does not have was accepted",
        (long)e1);
  set_mempolicy_(MPOL_DEFAULT, 0, 0);
}

int main(void) {
  printf("M128-SMOKE: start\n");
  fflush(stdout);

  check_topology();
  if (g_nodes < 1) {
    printf("M128-SMOKE: done\n");
    return 1;
  }
  check_mems_allowed();
  if (g_nodes > 1) {
    check_bind();
    check_mbind_range();
    check_interleave();
  } else {
    printf("M128-SMOKE: one node, placement checks need a -numa guest\n");
    fflush(stdout);
  }
  check_inherited();
  check_bad_node();

  printf("M128-SMOKE: done\n");
  fflush(stdout);
  return fails ? 1 : 0;
}
