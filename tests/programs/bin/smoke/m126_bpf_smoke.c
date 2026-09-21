/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * m126_bpf_smoke — eBPF: programs the kernel verifies, runs and refuses.
 *
 * Every program below is written out as instructions by hand, because that is
 * what a loader hands the kernel and it is the only way to test the verifier
 * on programs a compiler would never emit.
 *
 *   map            a hash map round-trips a key and value through bpf(2), and
 *                  an array map answers by index
 *   load-run       a verified program runs and returns what it computed
 *   map-from-prog  a program looks a key up in a map, increments the value and
 *                  stores it back; userspace reads the count afterwards
 *   helpers        ktime_get_ns and get_current_pid_tgid return real values
 *   reject-uninit  reading a register that was never written is refused
 *   reject-oob     a stack access outside the 512-byte frame is refused
 *   reject-loop    a backward jump is refused
 *   reject-null    using a map value without testing it for NULL is refused
 *   perf-attach    the program runs on a perf event's samples and its map
 *                  counts them
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <linux/bpf.h>
#include <linux/perf_event.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef __NR_bpf
#define __NR_bpf 321
#endif

static int fails;

static void ok(const char *what) {
  printf("M126-BPF: ok %s\n", what);
  fflush(stdout);
}

static void bad(const char *what, const char *why, long v) {
  printf("M126-BPF: FAIL %s — %s (%ld, errno=%d)\n", what, why, v, errno);
  fflush(stdout);
  fails++;
}

static void judge(const char *what, int good, const char *why, long v) {
  if (good)
    ok(what);
  else
    bad(what, why, v);
}

static void note(const char *fmt, ...) {
  char b[300];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  printf("M126-BPF:   %s\n", b);
  fflush(stdout);
}

/* The instruction, as the ABI lays it out. */
struct insn {
  uint8_t code;
  uint8_t dst_src;
  int16_t off;
  int32_t imm;
};

#define REGS(d, s) (uint8_t)(((s) << 4) | ((d) & 0xf))
#define ALU64_IMM(op, d, i) {(uint8_t)(0x07 | (op)), REGS(d, 0), 0, (i)}
#define ALU64_REG(op, d, s) {(uint8_t)(0x0f | (op)), REGS(d, s), 0, 0}
#define MOV64_IMM(d, i) ALU64_IMM(0xb0, d, i)
#define MOV64_REG(d, s) ALU64_REG(0xb0, d, s)
#define ADD64_IMM(d, i) ALU64_IMM(0x00, d, i)
#define ST_MEM(sz, d, o, i) {(uint8_t)(0x02 | (sz)), REGS(d, 0), (o), (i)}
#define STX_MEM(sz, d, s, o) {(uint8_t)(0x03 | (sz)), REGS(d, s), (o), 0}
#define LDX_MEM(sz, d, s, o) {(uint8_t)(0x01 | (sz)), REGS(d, s), (o), 0}
#define SZ_W 0x00
#define SZ_DW 0x18
#define JMP_IMM(op, d, i, o) {(uint8_t)(0x05 | (op)), REGS(d, 0), (o), (i)}
#define JA(o) {0x05, 0, (o), 0}
#define CALL(f) {0x85, 0, 0, (f)}
#define EXIT() {0x95, 0, 0, 0}
#define LD_MAP_FD(d, fd)                                                       \
  {0x18, REGS(d, 1), 0, (fd)}, { 0x00, 0, 0, 0 }

static int bpf(int cmd, union bpf_attr *attr, unsigned size) {
  return (int)syscall(__NR_bpf, cmd, attr, size);
}

static int map_create(int type, unsigned key, unsigned value, unsigned max) {
  union bpf_attr a;

  memset(&a, 0, sizeof(a));
  a.map_type = type;
  a.key_size = key;
  a.value_size = value;
  a.max_entries = max;
  return bpf(BPF_MAP_CREATE, &a, sizeof(a));
}

static int map_update(int fd, const void *key, const void *value,
                      uint64_t flags) {
  union bpf_attr a;

  memset(&a, 0, sizeof(a));
  a.map_fd = fd;
  a.key = (uint64_t)(uintptr_t)key;
  a.value = (uint64_t)(uintptr_t)value;
  a.flags = flags;
  return bpf(BPF_MAP_UPDATE_ELEM, &a, sizeof(a));
}

static int map_lookup(int fd, const void *key, void *value) {
  union bpf_attr a;

  memset(&a, 0, sizeof(a));
  a.map_fd = fd;
  a.key = (uint64_t)(uintptr_t)key;
  a.value = (uint64_t)(uintptr_t)value;
  return bpf(BPF_MAP_LOOKUP_ELEM, &a, sizeof(a));
}

static char g_log[256];

static int prog_load(const struct insn *insns, unsigned cnt) {
  union bpf_attr a;

  memset(&a, 0, sizeof(a));
  memset(g_log, 0, sizeof(g_log));
  a.prog_type = BPF_PROG_TYPE_PERF_EVENT;
  a.insn_cnt = cnt;
  a.insns = (uint64_t)(uintptr_t)insns;
  a.license = (uint64_t)(uintptr_t) "GPL";
  a.log_buf = (uint64_t)(uintptr_t)g_log;
  a.log_size = sizeof(g_log);
  a.log_level = 1;
  return bpf(BPF_PROG_LOAD, &a, sizeof(a));
}

static int prog_run(int fd, uint64_t ctx_first, uint32_t *retval) {
  union bpf_attr a;

  memset(&a, 0, sizeof(a));
  a.test.prog_fd = fd;
  a.test.data_in = (uint64_t)(uintptr_t)&ctx_first;
  a.test.data_size_in = sizeof(ctx_first);
  int rc = bpf(BPF_PROG_TEST_RUN, &a, sizeof(a));

  if (rc == 0 && retval)
    *retval = a.test.retval;
  return rc;
}

/* ── maps through the syscall ────────────────────────────────────────────── */

static void check_map(void) {
  int hash = map_create(BPF_MAP_TYPE_HASH, 8, 8, 16);
  int arr = map_create(BPF_MAP_TYPE_ARRAY, 4, 8, 4);

  if (hash < 0 || arr < 0) {
    bad("map", "creating maps", (long)(hash < 0 ? hash : arr));
    return;
  }
  uint64_t key = 0x1234, val = 0xdeadbeef, back = 0;
  uint32_t idx = 2;
  uint64_t aval = 77, aback = 0;

  int u1 = map_update(hash, &key, &val, BPF_ANY);
  int l1 = map_lookup(hash, &key, &back);
  int u2 = map_update(arr, &idx, &aval, BPF_ANY);
  int l2 = map_lookup(arr, &idx, &aback);
  uint64_t missing = 0x999;
  int miss = map_lookup(hash, &missing, &back);
  uint64_t back2 = 0;

  (void)map_lookup(hash, &key, &back2);

  judge("map",
        u1 == 0 && l1 == 0 && back2 == 0xdeadbeef && u2 == 0 && l2 == 0 &&
            aback == 77 && miss < 0 && errno == ENOENT,
        "a map did not round-trip its values", (long)back2);
  close(hash);
  close(arr);
}

/* ── a program that computes ─────────────────────────────────────────────── */

static void check_load_run(void) {
  /* r0 = 40; r0 += 2; exit  -> 42 */
  struct insn prog[] = {
      MOV64_IMM(0, 40),
      ADD64_IMM(0, 2),
      EXIT(),
  };
  int fd = prog_load(prog, sizeof(prog) / sizeof(prog[0]));

  if (fd < 0) {
    bad("load-run", g_log[0] ? g_log : "the program was refused", (long)fd);
    return;
  }
  uint32_t ret = 0;
  int rc = prog_run(fd, 0, &ret);

  judge("load-run", rc == 0 && ret == 42,
        "the program did not return what it computed", (long)ret);
  close(fd);
}

/* ── a program that uses a map ───────────────────────────────────────────── */

static void check_map_from_prog(void) {
  int map = map_create(BPF_MAP_TYPE_ARRAY, 4, 8, 1);

  if (map < 0) {
    bad("map-from-prog", "map create", (long)map);
    return;
  }
  /*
   *   *(u32 *)(r10 - 4) = 0          the key, on the stack
   *   r2 = r10; r2 += -4
   *   r1 = map
   *   r0 = map_lookup_elem(r1, r2)
   *   if r0 == 0 goto out
   *   r1 = *(u64 *)(r0 + 0)
   *   r1 += 1
   *   *(u64 *)(r0 + 0) = r1
   * out:
   *   r0 = 1
   *   exit
   */
  struct insn prog[] = {
      ST_MEM(SZ_W, 10, -4, 0),
      MOV64_REG(2, 10),
      ADD64_IMM(2, -4),
      LD_MAP_FD(1, map),
      CALL(BPF_FUNC_map_lookup_elem),
      JMP_IMM(0x10 /* JEQ */, 0, 0, 4),
      LDX_MEM(SZ_DW, 1, 0, 0),
      ADD64_IMM(1, 1),
      STX_MEM(SZ_DW, 0, 1, 0),
      MOV64_IMM(0, 1),
      EXIT(),
  };
  int fd = prog_load(prog, sizeof(prog) / sizeof(prog[0]));

  if (fd < 0) {
    bad("map-from-prog", g_log[0] ? g_log : "refused", (long)fd);
    close(map);
    return;
  }
  uint32_t ret = 0;

  for (int i = 0; i < 5; i++)
    (void)prog_run(fd, 0, &ret);

  uint32_t key = 0;
  uint64_t count = 0;

  (void)map_lookup(map, &key, &count);
  note("the program counted %llu runs into its map",
       (unsigned long long)count);
  judge("map-from-prog", count == 5,
        "the map did not hold what the program put there", (long)count);
  close(fd);
  close(map);
}

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void check_helpers(void) {
  struct insn time_prog[] = {
      CALL(BPF_FUNC_ktime_get_ns),
      /* r0 >>= 32, so the retval (32 bits) carries the high half: a real
       * nanosecond clock has a non-zero high word within seconds of boot. */
      {0x07 | 0x70, REGS(0, 0), 0, 32},
      EXIT(),
  };
  int fd = prog_load(time_prog, sizeof(time_prog) / sizeof(time_prog[0]));
  uint32_t t1 = 0, t2 = 0;

  if (fd < 0) {
    bad("helpers", g_log[0] ? g_log : "ktime program refused", (long)fd);
    return;
  }
  (void)prog_run(fd, 0, &t1);
  struct timespec ts = {0, 300 * 1000 * 1000};

  nanosleep(&ts, NULL);
  (void)prog_run(fd, 0, &t2);
  close(fd);

  struct insn pid_prog[] = {
      CALL(BPF_FUNC_get_current_pid_tgid),
      EXIT(), /* the low half is the tid */
  };
  int pfd = prog_load(pid_prog, sizeof(pid_prog) / sizeof(pid_prog[0]));
  uint32_t pid = 0;

  if (pfd >= 0) {
    (void)prog_run(pfd, 0, &pid);
    close(pfd);
  }
  note("ktime high word %u then %u; pid_tgid low half %u (getpid %u)", t1, t2,
       pid, (unsigned)getpid());
  judge("helpers", t2 >= t1 && pid == (uint32_t)getpid(),
        "a helper did not return a real value", (long)pid);
}

/* ── what the verifier must refuse ───────────────────────────────────────── */

static void check_rejects(void) {
  /* r0 = r3 (never written); exit */
  struct insn uninit[] = {
      MOV64_REG(0, 3),
      EXIT(),
  };
  int fd = prog_load(uninit, 2);

  judge("reject-uninit", fd < 0,
        "a program reading an uninitialised register was accepted", (long)fd);
  if (fd >= 0)
    close(fd);
  else
    note("uninit: %s", g_log);

  /* *(u64 *)(r10 + 8) = 1 -- above the frame */
  struct insn oob[] = {
      ST_MEM(SZ_DW, 10, 8, 1),
      MOV64_IMM(0, 0),
      EXIT(),
  };
  fd = prog_load(oob, 3);
  judge("reject-oob", fd < 0, "a write outside the stack frame was accepted",
        (long)fd);
  if (fd >= 0)
    close(fd);
  else
    note("oob: %s", g_log);

  /* r0 = 0; goto -1  -- a backward jump */
  struct insn loop[] = {
      MOV64_IMM(0, 0),
      JA(-1),
      EXIT(),
  };
  fd = prog_load(loop, 3);
  judge("reject-loop", fd < 0, "a backward jump was accepted", (long)fd);
  if (fd >= 0)
    close(fd);
  else
    note("loop: %s", g_log);

  /* A map lookup used without testing the result for NULL. */
  int map = map_create(BPF_MAP_TYPE_ARRAY, 4, 8, 1);
  struct insn nullderef[] = {
      ST_MEM(SZ_W, 10, -4, 0),
      MOV64_REG(2, 10),
      ADD64_IMM(2, -4),
      LD_MAP_FD(1, map),
      CALL(BPF_FUNC_map_lookup_elem),
      LDX_MEM(SZ_DW, 1, 0, 0), /* no NULL test */
      MOV64_IMM(0, 0),
      EXIT(),
  };
  fd = prog_load(nullderef, sizeof(nullderef) / sizeof(nullderef[0]));
  judge("reject-null", fd < 0,
        "a map value was used without being tested for NULL", (long)fd);
  if (fd >= 0)
    close(fd);
  else
    note("null: %s", g_log);
  if (map >= 0)
    close(map);
}

/* ── attached to a perf event ────────────────────────────────────────────── */

static void burn_ms(int ms) {
  static volatile unsigned long sink;
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

static void check_perf_attach(void) {
  int map = map_create(BPF_MAP_TYPE_ARRAY, 4, 8, 1);

  if (map < 0) {
    bad("perf-attach", "map create", (long)map);
    return;
  }
  /* The same counting program, returning 0 so the sample is dropped: the
   * count in the map is the whole output, which is how a profiler avoids
   * writing a record per sample. */
  struct insn prog[] = {
      ST_MEM(SZ_W, 10, -4, 0),
      MOV64_REG(2, 10),
      ADD64_IMM(2, -4),
      LD_MAP_FD(1, map),
      CALL(BPF_FUNC_map_lookup_elem),
      JMP_IMM(0x10, 0, 0, 4),
      LDX_MEM(SZ_DW, 1, 0, 0),
      ADD64_IMM(1, 1),
      STX_MEM(SZ_DW, 0, 1, 0),
      MOV64_IMM(0, 0),
      EXIT(),
  };
  int prog_fd = prog_load(prog, sizeof(prog) / sizeof(prog[0]));

  if (prog_fd < 0) {
    bad("perf-attach", g_log[0] ? g_log : "refused", (long)prog_fd);
    close(map);
    return;
  }

  struct perf_event_attr attr;

  memset(&attr, 0, sizeof(attr));
  attr.size = sizeof(attr);
  attr.type = PERF_TYPE_SOFTWARE;
  attr.config = PERF_COUNT_SW_CPU_CLOCK;
  attr.sample_type = PERF_SAMPLE_IP;
  attr.freq = 1;
  attr.sample_freq = 200;
  attr.disabled = 1;

  int pfd = (int)syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);

  if (pfd < 0) {
    bad("perf-attach", "perf_event_open", (long)pfd);
    close(prog_fd);
    close(map);
    return;
  }
  if (ioctl(pfd, PERF_EVENT_IOC_SET_BPF, prog_fd) != 0) {
    bad("perf-attach", "PERF_EVENT_IOC_SET_BPF", -1);
    close(pfd);
    close(prog_fd);
    close(map);
    return;
  }
  ioctl(pfd, PERF_EVENT_IOC_ENABLE, 0);
  burn_ms(300);
  ioctl(pfd, PERF_EVENT_IOC_DISABLE, 0);

  uint32_t key = 0;
  uint64_t count = 0;

  (void)map_lookup(map, &key, &count);
  note("the attached program ran on %llu samples in 300 ms at 200 Hz",
       (unsigned long long)count);
  judge("perf-attach", count >= 5,
        "the program did not run on the event's samples", (long)count);
  close(pfd);
  close(prog_fd);
  close(map);
}

int main(void) {
  printf("M126-BPF: start\n");
  fflush(stdout);

  union bpf_attr probe;

  memset(&probe, 0, sizeof(probe));
  if (bpf(BPF_MAP_CREATE, &probe, sizeof(probe)) < 0 && errno == ENOSYS) {
    printf("M126-BPF: FAIL bpf — bpf(2) is ENOSYS\n");
    printf("M126-BPF: done\n");
    fflush(stdout);
    return 1;
  }

  check_map();
  check_load_run();
  check_map_from_prog();
  check_helpers();
  check_rejects();
  check_perf_attach();

  printf("M126-BPF: done\n");
  fflush(stdout);
  return fails ? 1 : 0;
}
