/* M77 global resource caps smoke. Verifies the writable sysctl knobs under
 * /proc/sys/kernel that runtime-tune the system-wide hard caps:
 *
 *   - shmmax            (max shared-memory segment size, bytes)
 *   - tcp-max-conns     (max concurrent TCP connections)
 *   - pipe-max-count    (max open VFS pipes)
 *   - coredump-max-bytes (max core-dump size)
 *
 * For each knob: read the default, write an in-range value and confirm the
 * read-back reflects it, reject an out-of-range write (EINVAL) leaving the
 * value intact, reject garbage, then restore the original default.
 *
 * Markers (`M77-CAPS: ok <name>`) are consumed by tests/smoke.sh. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static void emit(const char *s) { write(1, s, strlen(s)); }

static void ok(const char *name) {
  char buf[128];
  int n = 0;
  const char *p = "M77-CAPS: ok ";
  while (*p) buf[n++] = *p++;
  while (*name) buf[n++] = *name++;
  buf[n++] = '\n';
  write(1, buf, n);
}

static void fail(const char *name) {
  char buf[128];
  int n = 0;
  const char *p = "M77-CAPS: FAIL ";
  while (*p) buf[n++] = *p++;
  while (*name) buf[n++] = *name++;
  buf[n++] = '\n';
  write(1, buf, n);
}

/* Read a whole pseudo-file (streams, so loop until EOF). Returns length or -1. */
static long slurp(const char *path, char *buf, int cap) {
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

/* Write a whole buffer to a file. Returns 0 on success, -1 on failure. */
static int put(const char *path, const char *val) {
  int fd = open(path, O_WRONLY);
  if (fd < 0)
    return -1;
  int r = write(fd, val, strlen(val));
  close(fd);
  return r == (int)strlen(val) ? 0 : -1;
}

/* Write a value we expect to be rejected. Returns 0 when it was rejected
 * (write failed with EINVAL / no bytes), -1 when it (wrongly) succeeded. */
static int put_expect_reject(const char *path, const char *val) {
  int fd = open(path, O_WRONLY);
  if (fd < 0)
    return 0;
  int r = write(fd, val, strlen(val));
  close(fd);
  return (r < 0 && errno == EINVAL) ? 0 : -1;
}

/* Run one tool to completion; 0 when it exited cleanly. Output is dropped
 * (the exit status is all that matters). */
static int run_tool(char *const argv[]) {
  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, 1);
      dup2(devnull, 2);
      close(devnull);
    }
    execve(argv[0], argv, 0);
    _exit(127);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) != pid)
    return -1;
  return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* Prove the busybox sysctl(8) write path: write via `sysctl -w`, confirm the
 * knob via a direct read, restore, and confirm the restore stuck. `key` is the
 * dotted sysctl name (e.g. kernel.shmmax). */
static int check_sysctl_tool(const char *name, const char *key,
                             const char *path, unsigned long long value) {
  char buf[128];
  char arg[160];

  if (slurp(path, buf, sizeof(buf)) <= 0) {
    fail("tool-read");
    return -1;
  }
  unsigned long long orig = strtoull(buf, 0, 0);

  snprintf(arg, sizeof(arg), "%s=%llu", key, value);
  if (run_tool((char *[]){(char *)"/bin/sysctl", (char *)"-w", arg, NULL}) < 0) {
    fail("tool-write");
    return -1;
  }
  if (slurp(path, buf, sizeof(buf)) <= 0 ||
      strtoull(buf, 0, 0) != value) {
    fail("tool-writeback");
    return -1;
  }

  snprintf(arg, sizeof(arg), "%s=%llu", key, orig);
  if (run_tool((char *[]){(char *)"/bin/sysctl", (char *)"-w", arg, NULL}) < 0) {
    fail("tool-restore");
    return -1;
  }
  if (slurp(path, buf, sizeof(buf)) <= 0 ||
      strtoull(buf, 0, 0) != orig) {
    fail("tool-restore-back");
    return -1;
  }

  ok(name);
  return 0;
}

static int check_one(const char *name, const char *path, unsigned long long in,
                     unsigned long long bad) {
  char buf[128];
  char val[64];

  if (slurp(path, buf, sizeof(buf)) <= 0) {
    fail("read");
    return -1;
  }
  unsigned long long orig = strtoull(buf, 0, 0);

  snprintf(val, sizeof(val), "%llu\n", in);
  if (put(path, val) < 0) {
    fail("write");
    return -1;
  }
  if (slurp(path, buf, sizeof(buf)) <= 0 ||
      strtoull(buf, 0, 0) != in) {
    fail("writeback");
    return -1;
  }

  snprintf(val, sizeof(val), "%llu\n", bad);
  if (put_expect_reject(path, val) < 0) {
    fail("reject-out-of-range");
    return -1;
  }
  if (slurp(path, buf, sizeof(buf)) <= 0 ||
      strtoull(buf, 0, 0) != in) {
    fail("reject-preserves");
    return -1;
  }

  if (put_expect_reject(path, "garbage\n") < 0) {
    fail("reject-garbage");
    return -1;
  }
  if (slurp(path, buf, sizeof(buf)) <= 0 ||
      strtoull(buf, 0, 0) != in) {
    fail("garbage-preserves");
    return -1;
  }

  snprintf(val, sizeof(val), "%llu\n", orig);
  if (put(path, val) < 0 ||
      slurp(path, buf, sizeof(buf)) <= 0 ||
      strtoull(buf, 0, 0) != orig) {
    fail("restore");
    return -1;
  }

  ok(name);
  return 0;
}

/* Enforcement: a lowered cap must actually block the resource. Floors SHMMAX
 * to its minimum and proves an oversized shmget is rejected while a within-cap
 * one still works (then restores the original value). */
static int check_shmmax_enforce(void) {
  char buf[128];

  if (slurp("/proc/sys/kernel/shmmax", buf, sizeof(buf)) <= 0) {
    fail("enforce-read");
    return -1;
  }
  unsigned long long orig = strtoull(buf, 0, 0);

  /* Floor the cap: CAP_SHMMAX_MIN_MB = 4. */
  if (put("/proc/sys/kernel/shmmax", "4194304\n") < 0) {
    fail("enforce-lower");
    return -1;
  }

  int bad = 0;
  /* 8 MiB > 4 MiB cap: must be rejected. */
  int big = shmget(0x4d37UL, 8ULL * 1024 * 1024, IPC_CREAT | IPC_EXCL | 0666);
  if (big >= 0) {
    fail("enforce-shmget-big");
    shmctl(big, IPC_RMID, NULL);
    bad = 1;
  }

  /* 2 MiB within the 4 MiB cap: must succeed, and be removable. */
  if (!bad) {
    int small = shmget(0x4d38UL, 2ULL * 1024 * 1024, IPC_CREAT | IPC_EXCL | 0666);
    if (small < 0) {
      fail("enforce-shmget-small");
      bad = 1;
    } else if (shmctl(small, IPC_RMID, NULL) != 0) {
      fail("enforce-shmctl");
      bad = 1;
    }
  }

  snprintf(buf, sizeof(buf), "%llu\n", orig);
  if (put("/proc/sys/kernel/shmmax", buf) < 0) {
    fail("enforce-restore");
    return -1;
  }
  if (slurp("/proc/sys/kernel/shmmax", buf, sizeof(buf)) <= 0 ||
      strtoull(buf, 0, 0) != orig) {
    fail("enforce-restore-back");
    return -1;
  }

  if (bad)
    return -1;
  ok("enforce-shmmax");
  return 0;
}

/* Enforcement: a lowered pipe-max-count must bound the VFS pipe pool. Floors
 * the cap at its minimum (16) and opens pipes until the pool refuses; the
 * count must stop at (or below, if the harness already holds pipes) the cap.
 * Then restores the original cap and proves many more pipes open — the first
 * stop was the cap, not descriptor exhaustion. */
static int check_pipe_cap(void) {
  char buf[128];
  char val[64];

  if (slurp("/proc/sys/kernel/pipe-max-count", buf, sizeof(buf)) <= 0) {
    fail("pipe-cap-read");
    return -1;
  }
  unsigned long long orig = strtoull(buf, 0, 0);

  if (put("/proc/sys/kernel/pipe-max-count", "16\n") < 0) {
    fail("pipe-cap-lower");
    return -1;
  }

  int fds[64][2];
  int n_lo = 0;
  for (; n_lo < 48; n_lo++) {
    if (pipe(fds[n_lo]) != 0)
      break;
  }

  int rc = 0;
  /* 48 is far above the floor: running all the way there means the cap never
   * bit. Pool exhaustion surfaces as -1 from pipe() here. */
  if (n_lo >= 48 || n_lo > 16) {
    fail("pipe-cap-blocked");
    rc = -1;
  }

  /* Control: restore the original cap; the pool must open again, proving the
   * earlier stop was cap-induced rather than fd or memory exhaustion. */
  snprintf(val, sizeof(val), "%llu\n", orig);
  if (put("/proc/sys/kernel/pipe-max-count", val) < 0) {
    fail("pipe-cap-restore");
    rc = -1;
  }
  int n_extra = 0;
  for (; n_extra < 16 && n_lo + n_extra < 64; n_extra++) {
    if (pipe(fds[n_lo + n_extra]) != 0)
      break;
  }
  if (n_extra < 8) {
    fail("pipe-cap-open");
    rc = -1;
  }

  for (int i = 0; i < n_lo + n_extra; i++) {
    close(fds[i][0]);
    close(fds[i][1]);
  }

  if (slurp("/proc/sys/kernel/pipe-max-count", buf, sizeof(buf)) <= 0 ||
      strtoull(buf, 0, 0) != orig) {
    fail("pipe-cap-restore-back");
    rc = -1;
  }

  if (rc == 0)
    ok("enforce-pipe-max");
  return rc;
}

/* Enforcement: a lowered tcp-max-conns must bound the connection-slot pool.
 * Every listening socket occupies one slot. Floors the cap at its minimum
 * (16), opens listeners until the pool refuses, then proves a fresh
 * asynchronous connect has no slot to claim (immediate ECONNREFUSED). Finally
 * restores the cap and shows the pool opens again. */
static int check_tcp_cap(void) {
  char buf[128];
  char val[64];

  if (slurp("/proc/sys/kernel/tcp-max-conns", buf, sizeof(buf)) <= 0) {
    fail("tcp-cap-read");
    return -1;
  }
  unsigned long long orig = strtoull(buf, 0, 0);

  if (put("/proc/sys/kernel/tcp-max-conns", "16\n") < 0) {
    fail("tcp-cap-lower");
    return -1;
  }

  int socks[64];
  int n_ok = 0;
  for (; n_ok < 40; n_ok++) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
      break;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(58000 + n_ok);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        listen(fd, 1) != 0) {
      close(fd);
      break;
    }
    socks[n_ok] = fd;
  }

  int rc = 0;
  /* With the cap floored at 16 the pool must refuse well before 40. */
  if (n_ok >= 40 || n_ok > 16) {
    fail("tcp-cap-blocked");
    rc = -1;
  }

  /* Every slot is now taken. A nonblocking connect has nothing to claim and
   * must fail immediately with ECONNREFUSED (the pool-full abort in
   * tcp_connect_start_af), never hang in SYN_SENT. */
  int cli = socket(AF_INET, SOCK_STREAM, 0);
  if (cli >= 0) {
    int fl = fcntl(cli, F_GETFL, 0);
    fcntl(cli, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in ca;
    memset(&ca, 0, sizeof(ca));
    ca.sin_family = AF_INET;
    ca.sin_port = htons(59990);
    inet_pton(AF_INET, "127.0.0.1", &ca.sin_addr);
    int cr = connect(cli, (struct sockaddr *)&ca, sizeof(ca));
    int e = errno;
    close(cli);
    if (cr == 0 || e != ECONNREFUSED) {
      fail("tcp-cap-connect");
      rc = -1;
    }
  }

  /* Control: restore the cap, close the held listeners (their slots free), and
   * open again. Enough successes prove the earlier stop was the cap. */
  snprintf(val, sizeof(val), "%llu\n", orig);
  if (put("/proc/sys/kernel/tcp-max-conns", val) < 0) {
    fail("tcp-cap-restore");
    rc = -1;
  }
  int extra = 0;
  for (; extra < 16 && n_ok + extra < 64; extra++) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
      break;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(58100 + extra);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        listen(fd, 1) != 0) {
      close(fd);
      break;
    }
    socks[n_ok + extra] = fd;
  }
  if (extra < 8) {
    fail("tcp-cap-open");
    rc = -1;
  }

  for (int i = 0; i < n_ok + extra; i++)
    close(socks[i]);

  if (slurp("/proc/sys/kernel/tcp-max-conns", buf, sizeof(buf)) <= 0 ||
      strtoull(buf, 0, 0) != orig) {
    fail("tcp-cap-restore-back");
    rc = -1;
  }

  if (rc == 0)
    ok("enforce-tcp-max");
  return rc;
}

int main(void) {
  emit("M77-CAPS: start\n");
  int rc = 0;

  /* In-range write targets within each knob's [min, ceil] clamp. */
  if (check_one("shmmax", "/proc/sys/kernel/shmmax", 16ULL * 1024 * 1024,
                1ULL << 40) < 0)
    rc = 1;
  if (check_one("tcp-max-conns", "/proc/sys/kernel/tcp-max-conns", 100,
                1000000) < 0)
    rc = 1;
  if (check_one("pipe-max-count", "/proc/sys/kernel/pipe-max-count", 200,
                1000000) < 0)
    rc = 1;
  if (check_one("coredump-max-bytes", "/proc/sys/kernel/coredump-max-bytes",
                2ULL * 1024 * 1024, 1ULL << 40) < 0)
    rc = 1;

  /* busybox sysctl(8) integration: plain and hyphenated key names. */
  if (check_sysctl_tool("sysctl-shmmax", "kernel.shmmax",
                        "/proc/sys/kernel/shmmax", 24ULL * 1024 * 1024) < 0)
    rc = 1;
  if (check_sysctl_tool("sysctl-coredump", "kernel.coredump-max-bytes",
                        "/proc/sys/kernel/coredump-max-bytes",
                        3ULL * 1024 * 1024) < 0)
    rc = 1;

  /* Real enforcement: lowered caps must actually block the resource. */
  if (check_shmmax_enforce() < 0)
    rc = 1;
  if (check_pipe_cap() < 0)
    rc = 1;
  if (check_tcp_cap() < 0)
    rc = 1;

  emit(rc ? "M77-CAPS: FAIL\n" : "M77-CAPS: done\n");
  return rc;
}
