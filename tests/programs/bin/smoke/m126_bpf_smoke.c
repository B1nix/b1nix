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
 *   btf-vmlinux    /sys/kernel/btf/vmlinux is the kernel's BTF: a valid
 *                  header and the kernel's own type names
 *   btf-load       a hand-built BTF loads, and OBJ_GET_INFO_BY_FD gives the
 *                  same bytes back under an id
 *   btf-kernel-id  the kernel's BTF has an id of its own, named vmlinux
 *   btf-reject     a malformed BTF is refused with a reason in the log
 *   prog-btf       a program loads with its BTF and func_info, and a
 *                  func_info naming something that is not a FUNC is refused
 *   reject-merge   a path that never tested a map value, reaching the same
 *                  load as one that did, is refused -- not waved through
 *                  because the other path was proved there first
 *   reject-helper-size  a helper told to write more than the stack holds is
 *                  refused
 *   probe-read     probe_read_user reads the caller's memory, a bad address
 *                  is EFAULT with the buffer zeroed, and probe_read_kernel
 *                  refuses a user address
 *   ringbuf        ringbuf_output and reserve/submit records reach a mapped
 *                  ring buffer in Linux's layout, poll says when there is one,
 *                  and a reservation left unsubmitted is refused
 *   stackmap       get_stackid stores the stack under an id the map answers
 *   atomic         an atomic add on a map value counts
 *   link           a bpf_link attaches a program to a perf event, holds the
 *                  event's slot, is found by id, and LINK_DETACH lets it go
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <linux/perf_event.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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
/* BPF_MEM (0x60) is the mode: the plain load and store. Without it the
 * opcode names no instruction, and a verifier that reads the mode refuses it. */
#define ST_MEM(sz, d, o, i) {(uint8_t)(0x62 | (sz)), REGS(d, 0), (o), (i)}
#define STX_MEM(sz, d, s, o) {(uint8_t)(0x63 | (sz)), REGS(d, s), (o), 0}
#define LDX_MEM(sz, d, s, o) {(uint8_t)(0x61 | (sz)), REGS(d, s), (o), 0}
#define SZ_W 0x00
#define SZ_DW 0x18
#define JMP_IMM(op, d, i, o) {(uint8_t)(0x05 | (op)), REGS(d, 0), (o), (i)}
#define JMP_REG(op, d, s, o) {(uint8_t)(0x0d | (op)), REGS(d, s), (o), 0}
#define JA(o) {0x05, 0, (o), 0}
#define ATOMIC_DW(d, s, o, op) {0xdb, REGS(d, s), (o), (op)}
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

static int prog_load_type(const struct insn *insns, unsigned cnt, int type) {
  union bpf_attr a;

  memset(&a, 0, sizeof(a));
  memset(g_log, 0, sizeof(g_log));
  a.prog_type = type;
  a.insn_cnt = cnt;
  a.insns = (uint64_t)(uintptr_t)insns;
  a.license = (uint64_t)(uintptr_t) "GPL";
  a.log_buf = (uint64_t)(uintptr_t)g_log;
  a.log_size = sizeof(g_log);
  a.log_level = 1;
  return bpf(BPF_PROG_LOAD, &a, sizeof(a));
}

static int prog_load(const struct insn *insns, unsigned cnt) {
  return prog_load_type(insns, cnt, BPF_PROG_TYPE_PERF_EVENT);
}

/* Where pt_regs keeps the instruction pointer, in u64s: x86_64's ip follows
 * orig_ax; arm64's user_pt_regs has x0-x30, sp, then pc. */
#if defined(__aarch64__)
#define REGS_IP 32
#define REGS_ARG0 0
#define REGS_WORDS 34
#else
#define REGS_IP 16
#define REGS_ARG0 14 /* di */
#define REGS_WORDS 21
#endif

/* Run a PERF_EVENT program once. Its context is bpf_perf_event_data, passed
 * as ctx_in the way Linux's test run takes it; `ip` goes in its regs. */
static int prog_run(int fd, uint64_t ip, uint32_t *retval) {
  uint64_t ctx[REGS_WORDS + 2];
  union bpf_attr a;

  memset(ctx, 0, sizeof(ctx));
  ctx[REGS_IP] = ip;
  memset(&a, 0, sizeof(a));
  a.test.prog_fd = fd;
  a.test.ctx_in = (uint64_t)(uintptr_t)ctx;
  a.test.ctx_size_in = sizeof(ctx);
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

/* ── BTF ── */

struct btf_hdr {
  uint16_t magic;
  uint8_t version;
  uint8_t flags;
  uint32_t hdr_len, type_off, type_len, str_off, str_len;
};

/* int, a function prototype returning it, and a FUNC "main" of that type:
 * the least a program's BTF carries. Type ids 1, 2, 3. */
static unsigned build_btf(uint8_t *out, int bad_ref) {
  static const char strs[] = "\0int\0main";
  uint32_t types[] = {
      1, (1u << 24), 4, (1u << 24) | 32,  /* [1] INT "int" 32 bits, signed */
      0, (13u << 24), 1,                  /* [2] FUNC_PROTO () -> int     */
      5, (12u << 24) | 1, bad_ref ? 9 : 2 /* [3] FUNC "main" : [2]        */
  };
  struct btf_hdr h = {0xeB9F, 1, 0, sizeof(h), 0, sizeof(types),
                      sizeof(types), sizeof(strs)};

  memcpy(out, &h, sizeof(h));
  memcpy(out + sizeof(h), types, sizeof(types));
  memcpy(out + sizeof(h) + sizeof(types), strs, sizeof(strs));
  return sizeof(h) + sizeof(types) + sizeof(strs);
}

static char g_btf_log[256];

static int btf_load(const uint8_t *blob, unsigned size) {
  union bpf_attr a;

  memset(&a, 0, sizeof(a));
  memset(g_btf_log, 0, sizeof(g_btf_log));
  a.btf = (uint64_t)(uintptr_t)blob;
  a.btf_size = size;
  a.btf_log_buf = (uint64_t)(uintptr_t)g_btf_log;
  a.btf_log_size = sizeof(g_btf_log);
  a.btf_log_level = 1;
  return bpf(BPF_BTF_LOAD, &a, sizeof(a));
}

static int btf_info(int fd, struct bpf_btf_info *in) {
  union bpf_attr a;

  memset(&a, 0, sizeof(a));
  a.info.bpf_fd = fd;
  a.info.info_len = sizeof(*in);
  a.info.info = (uint64_t)(uintptr_t)in;
  return bpf(BPF_OBJ_GET_INFO_BY_FD, &a, sizeof(a));
}

static void check_btf(void) {
  /* The kernel's, as a CO-RE loader reads it. */
  {
    FILE *f = fopen("/sys/kernel/btf/vmlinux", "rb");
    static uint8_t buf[4u << 20];
    size_t n = f ? fread(buf, 1, sizeof(buf), f) : 0;
    struct btf_hdr h;
    int good = 0;

    if (f)
      fclose(f);
    if (n > sizeof(h)) {
      memcpy(&h, buf, sizeof(h));
      if (h.magic == 0xeB9F && h.version == 1 &&
          (size_t)h.hdr_len + h.str_off + h.str_len <= n) {
        const char *strs = (const char *)buf + h.hdr_len + h.str_off;

        /* Names that exist only in this kernel: it is its own BTF, not a
         * stand-in. */
        good = memmem(strs, h.str_len, "vfs_node", 9) &&
               memmem(strs, h.str_len, "bpf_prog", 9);
      }
    }
    note("/sys/kernel/btf/vmlinux: %zu bytes", n);
    judge("btf-vmlinux", good,
          "the kernel's BTF is missing or does not describe this kernel",
          (long)n);
  }

  uint8_t blob[128];
  unsigned size = build_btf(blob, 0);
  int fd = btf_load(blob, size);
  struct bpf_btf_info in;
  uint8_t back[128];

  memset(&in, 0, sizeof(in));
  in.btf = (uint64_t)(uintptr_t)back;
  in.btf_size = sizeof(back);
  int irc = fd >= 0 ? btf_info(fd, &in) : -1;

  judge("btf-load",
        fd >= 0 && irc == 0 && in.id > 0 && in.btf_size == size &&
            !in.kernel_btf && memcmp(back, blob, size) == 0,
        "a valid BTF did not load, or did not come back as it was loaded",
        fd >= 0 ? irc : fd);
  if (fd < 0)
    note("log: %s", g_btf_log);

  /* The kernel's own object, by id. */
  {
    union bpf_attr a;
    int found = 0;
    uint32_t id = 0;

    for (int guard = 0; guard < 64; guard++) {
      memset(&a, 0, sizeof(a));
      a.start_id = id;
      if (bpf(BPF_BTF_GET_NEXT_ID, &a, sizeof(a)) < 0)
        break;
      id = a.next_id;
      memset(&a, 0, sizeof(a));
      a.btf_id = id;
      int kfd = bpf(BPF_BTF_GET_FD_BY_ID, &a, sizeof(a));
      struct bpf_btf_info ki;
      char name[16] = {0};

      if (kfd < 0)
        continue;
      memset(&ki, 0, sizeof(ki));
      ki.name = (uint64_t)(uintptr_t)name;
      ki.name_len = sizeof(name);
      if (btf_info(kfd, &ki) == 0 && ki.kernel_btf &&
          strcmp(name, "vmlinux") == 0)
        found = 1;
      close(kfd);
      if (found)
        break;
    }
    judge("btf-kernel-id", found,
          "no BTF id answers as the kernel's (vmlinux)", (long)id);
  }

  /* Malformed: a FUNC whose type does not exist. */
  {
    uint8_t badb[128];
    unsigned bs = build_btf(badb, 1);
    int bfd = btf_load(badb, bs);

    judge("btf-reject", bfd < 0 && errno == EINVAL && g_btf_log[0],
          "a BTF referring to a missing type was accepted, or refused silently",
          bfd);
    if (bfd >= 0)
      close(bfd);
  }

  /* A program with its BTF: func_info names FUNC [3] at instruction 0. */
  if (fd >= 0) {
    struct insn prog[] = {MOV64_IMM(0, 7), EXIT()};
    uint32_t fi[2] = {0, 3};
    union bpf_attr a;

    memset(&a, 0, sizeof(a));
    a.prog_type = BPF_PROG_TYPE_PERF_EVENT;
    a.insn_cnt = 2;
    a.insns = (uint64_t)(uintptr_t)prog;
    a.license = (uint64_t)(uintptr_t) "GPL";
    a.prog_btf_fd = fd;
    a.func_info_rec_size = sizeof(fi);
    a.func_info = (uint64_t)(uintptr_t)fi;
    a.func_info_cnt = 1;
    int pfd = bpf(BPF_PROG_LOAD, &a, sizeof(a));
    uint32_t ret = 0;
    int good = pfd >= 0 && prog_run(pfd, 0, &ret) == 0 && ret == 7;

    if (pfd >= 0)
      close(pfd);
    fi[1] = 1; /* the INT, not a FUNC */
    int bad = bpf(BPF_PROG_LOAD, &a, sizeof(a));

    judge("prog-btf", good && bad < 0 && errno == EINVAL,
          "a program with BTF and func_info did not load and run, or a wrong"
          " func_info was accepted",
          pfd >= 0 ? bad : pfd);
    if (bad >= 0)
      close(bad);
  }
  if (fd >= 0)
    close(fd);
}


/* ── the verifier: soundness ─────────────────────────────────────────────── */

static void check_verifier_soundness(void) {
  int map = map_create(BPF_MAP_TYPE_ARRAY, 4, 8, 1);

  if (map < 0) {
    bad("reject-merge", "map create", (long)map);
    return;
  }
  /* Two paths meet at the load in insn 11. On one, r6 -- the lookup's result
   * -- was tested; on the other it was not. Proving the first path at the load
   * proves nothing about the second. */
  struct insn merge[] = {
      ST_MEM(SZ_W, 10, -4, 0),
      MOV64_REG(2, 10),
      ADD64_IMM(2, -4),
      LD_MAP_FD(1, map),
      CALL(BPF_FUNC_map_lookup_elem),
      MOV64_REG(6, 0),
      CALL(BPF_FUNC_ktime_get_ns),
      JMP_IMM(0x20, 0, 5, 1),  /* if r0 > 5 goto 10 */
      JA(1),                   /* goto 11, r6 untested */
      JMP_IMM(0x10, 6, 0, 2),  /* if r6 == 0 goto 13 */
      LDX_MEM(SZ_DW, 0, 6, 0), /* 11 */
      EXIT(),
      MOV64_IMM(0, 0), /* 13 */
      EXIT(),
  };
  int fd = prog_load(merge, sizeof(merge) / sizeof(merge[0]));

  judge("reject-merge", fd < 0 && errno == EINVAL,
        "a map value was used untested on one path", (long)fd);
  if (fd >= 0)
    close(fd);

  struct insn big[] = {
      MOV64_REG(1, 10),
      ADD64_IMM(1, -16),
      MOV64_IMM(2, 4096),
      CALL(BPF_FUNC_get_current_comm),
      MOV64_IMM(0, 0),
      EXIT(),
  };

  fd = prog_load(big, sizeof(big) / sizeof(big[0]));
  judge("reject-helper-size", fd < 0 && errno == EINVAL,
        "a helper was allowed to write 4096 bytes into a 16-byte slot",
        (long)fd);
  if (fd >= 0)
    close(fd);
  close(map);
}

/* ── probe_read ──────────────────────────────────────────────────────────── */

static void check_probe_read(void) {
  int map = map_create(BPF_MAP_TYPE_ARRAY, 4, 16, 1);
  static volatile uint64_t secret = 0x1122334455667788ull;

  if (map < 0) {
    bad("probe-read", "map create", (long)map);
    return;
  }
  /* The address to read is the map value's first word; what was read lands
   * in its second, and the helper's answer is the program's return value. */
  for (int kernel = 0; kernel < 2; kernel++) {
    struct insn prog[] = {
        ST_MEM(SZ_W, 10, -4, 0),
        MOV64_REG(2, 10),
        ADD64_IMM(2, -4),
        LD_MAP_FD(1, map),
        CALL(BPF_FUNC_map_lookup_elem),
        JMP_IMM(0x10, 0, 0, 7),
        MOV64_REG(6, 0),
        LDX_MEM(SZ_DW, 3, 6, 0),
        MOV64_REG(1, 6),
        ADD64_IMM(1, 8),
        MOV64_IMM(2, 8),
        CALL(kernel ? BPF_FUNC_probe_read_kernel : BPF_FUNC_probe_read_user),
        EXIT(),
        MOV64_IMM(0, 1),
        EXIT(),
    };
    int fd = prog_load(prog, sizeof(prog) / sizeof(prog[0]));

    if (fd < 0) {
      bad("probe-read", g_log[0] ? g_log : "refused", (long)fd);
      close(map);
      return;
    }
    uint32_t key = 0, ret = 1;
    uint64_t val[2] = {(uint64_t)(uintptr_t)&secret, 0};

    map_update(map, &key, val, 0);
    prog_run(fd, 0, &ret);
    map_lookup(map, &key, val);
    if (!kernel) {
      int good = ret == 0 && val[1] == secret;

      val[0] = 0x10; /* nothing is mapped there */
      val[1] = 0xdead;
      map_update(map, &key, val, 0);
      prog_run(fd, 0, &ret);
      map_lookup(map, &key, val);
      judge("probe-read", good && (int32_t)ret == -EFAULT && val[1] == 0,
            "probe_read_user did not read the caller's memory, or a bad "
            "address was not EFAULT with the buffer zeroed",
            (long)(int32_t)ret);
    } else {
      judge("probe-read-kernel-refuses-user", (int32_t)ret == -EFAULT,
            "probe_read_kernel read a user address", (long)(int32_t)ret);
    }
    close(fd);
  }
  close(map);
}

/* ── ring buffers ────────────────────────────────────────────────────────── */

#define RB_SIZE (4 * 4096)

static void check_ringbuf(void) {
  int rb = map_create(BPF_MAP_TYPE_RINGBUF, 0, 0, RB_SIZE);

  if (rb < 0) {
    bad("ringbuf", "ring buffer map create", (long)rb);
    return;
  }
  struct insn out[] = {
      ST_MEM(SZ_W, 10, -8, 0xabcd),
      ST_MEM(SZ_W, 10, -4, 0x1234),
      MOV64_REG(2, 10),
      ADD64_IMM(2, -8),
      LD_MAP_FD(1, rb),
      MOV64_IMM(3, 8),
      MOV64_IMM(4, 0),
      CALL(BPF_FUNC_ringbuf_output),
      EXIT(),
  };
  struct insn resv[] = {
      LD_MAP_FD(1, rb),
      MOV64_IMM(2, 16),
      MOV64_IMM(3, 0),
      CALL(BPF_FUNC_ringbuf_reserve),
      JMP_IMM(0x10, 0, 0, 5),
      MOV64_IMM(1, 7),
      STX_MEM(SZ_DW, 0, 1, 0),
      MOV64_REG(1, 0),
      MOV64_IMM(2, 0),
      CALL(BPF_FUNC_ringbuf_submit),
      MOV64_IMM(0, 0),
      EXIT(),
  };
  struct insn leak[] = {
      LD_MAP_FD(1, rb),
      MOV64_IMM(2, 16),
      MOV64_IMM(3, 0),
      CALL(BPF_FUNC_ringbuf_reserve),
      MOV64_IMM(0, 0),
      EXIT(),
  };
  int fo = prog_load(out, sizeof(out) / sizeof(out[0]));
  int fr = prog_load(resv, sizeof(resv) / sizeof(resv[0]));
  int fl = prog_load(leak, sizeof(leak) / sizeof(leak[0]));

  judge("reject-ringbuf-leak", fl < 0 && errno == EINVAL,
        "a reservation left unsubmitted was accepted", (long)fl);
  if (fl >= 0)
    close(fl);
  if (fo < 0 || fr < 0) {
    bad("ringbuf", g_log[0] ? g_log : "a ring-buffer program was refused",
        (long)(fo < 0 ? fo : fr));
    goto out;
  }

  uint64_t *cons = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, rb, 0);
  uint8_t *prod = mmap(0, 4096 + 2 * RB_SIZE, PROT_READ, MAP_SHARED, rb, 4096);

  if (cons == MAP_FAILED || prod == MAP_FAILED) {
    bad("ringbuf", "the ring buffer would not map", -1);
    goto out;
  }
  uint32_t ret = 1;
  struct pollfd pf = {.fd = rb, .events = POLLIN};
  int empty_before = poll(&pf, 1, 0) == 0;

  prog_run(fo, 0, &ret);
  uint64_t p1 = *(volatile uint64_t *)prod;
  uint8_t *data = prod + 4096;
  uint32_t hdr = *(volatile uint32_t *)data;
  uint32_t w0 = *(uint32_t *)(data + 8), w1 = *(uint32_t *)(data + 12);
  int readable = poll(&pf, 1, 0) == 1 && (pf.revents & POLLIN);
  int good_out = ret == 0 && p1 == 16 && hdr == 8 && w0 == 0xabcd &&
                 w1 == 0x1234;

  prog_run(fr, 0, &ret);
  uint64_t p2 = *(volatile uint64_t *)prod;
  uint32_t hdr2 = *(volatile uint32_t *)(data + p1);
  uint64_t v2 = *(uint64_t *)(data + p1 + 8);
  int good_resv = ret == 0 && p2 == p1 + 24 && hdr2 == 16 && v2 == 7;

  *(volatile uint64_t *)cons = p2; /* consumed */
  int empty_after = poll(&pf, 1, 0) == 0;

  note("ring buffer: producer %llu then %llu, headers %#x and %#x",
       (unsigned long long)p1, (unsigned long long)p2, hdr, hdr2);
  judge("ringbuf-output", good_out,
        "ringbuf_output did not put the record where libbpf reads it",
        (long)ret);
  judge("ringbuf-reserve", good_resv,
        "a reserved and submitted record is not in the ring", (long)ret);
  judge("ringbuf-poll", empty_before && readable && empty_after,
        "poll on the ring buffer did not follow its contents", 0);
  munmap(cons, 4096);
  munmap(prod, 4096 + 2 * RB_SIZE);
out:
  if (fo >= 0)
    close(fo);
  if (fr >= 0)
    close(fr);
  close(rb);
}

/* ── stack maps ──────────────────────────────────────────────────────────── */

static void check_stackmap(void) {
  int sm = map_create(BPF_MAP_TYPE_STACK_TRACE, 4, 4 * 8, 16);

  if (sm < 0) {
    bad("stackmap", "stack map create", (long)sm);
    return;
  }
  for (int user = 0; user < 2; user++) {
    struct insn prog[] = {
        MOV64_REG(6, 1),
        LD_MAP_FD(2, sm),
        MOV64_REG(1, 6),
        MOV64_IMM(3, user ? 1 << 8 : 0),
        CALL(BPF_FUNC_get_stackid),
        EXIT(),
    };
    int fd = prog_load(prog, sizeof(prog) / sizeof(prog[0]));

    if (fd < 0) {
      bad("stackmap", g_log[0] ? g_log : "refused", (long)fd);
      close(sm);
      return;
    }
    uint32_t ret = 0;
    uint64_t ip = 0xffffffff80200000ull, stack[4] = {0};

    prog_run(fd, ip, &ret);
    if (!user) {
      uint32_t id = ret;
      int got = (int32_t)ret >= 0 && ret < 16 && map_lookup(sm, &id, stack) == 0;

      judge("stackmap", got && stack[0] == ip && stack[1] == 0,
            "get_stackid did not store the stack under the id it returned",
            (long)(int32_t)ret);
    } else {
      /* Run from a system call there are no user frames at hand. */
      judge("stackmap-user", (int32_t)ret == -EFAULT,
            "a user stack was reported where there is none",
            (long)(int32_t)ret);
    }
    close(fd);
  }
  close(sm);
}

/* ── atomics ─────────────────────────────────────────────────────────────── */

static void check_atomic(void) {
  int map = map_create(BPF_MAP_TYPE_ARRAY, 4, 8, 1);
  struct insn prog[] = {
      ST_MEM(SZ_W, 10, -4, 0),
      MOV64_REG(2, 10),
      ADD64_IMM(2, -4),
      LD_MAP_FD(1, map),
      CALL(BPF_FUNC_map_lookup_elem),
      JMP_IMM(0x10, 0, 0, 3),
      MOV64_IMM(1, 1),
      ATOMIC_DW(0, 1, 0, 0x01 /* BPF_ADD | BPF_FETCH */),
      MOV64_REG(0, 1), /* the value before the add */
      EXIT(),
  };
  int fd = map >= 0 ? prog_load(prog, sizeof(prog) / sizeof(prog[0])) : -1;

  if (fd < 0) {
    bad("atomic", g_log[0] ? g_log : "refused", (long)fd);
    if (map >= 0)
      close(map);
    return;
  }
  uint32_t key = 0, r1 = 9, r2 = 9;
  uint64_t v = 0;

  prog_run(fd, 0, &r1);
  prog_run(fd, 0, &r2);
  map_lookup(map, &key, &v);
  judge("atomic", v == 2 && r1 == 0 && r2 == 1,
        "an atomic fetch-and-add on a map value did not count", (long)v);
  close(fd);
  close(map);
}

/* ── bpf_link ────────────────────────────────────────────────────────────── */

static int prog_id_of(int fd) {
  struct bpf_prog_info info;
  union bpf_attr a;

  memset(&info, 0, sizeof(info));
  memset(&a, 0, sizeof(a));
  a.info.bpf_fd = fd;
  a.info.info_len = sizeof(info);
  a.info.info = (uint64_t)(uintptr_t)&info;
  return bpf(BPF_OBJ_GET_INFO_BY_FD, &a, sizeof(a)) == 0 ? (int)info.id : -1;
}

static int link_info(int fd, struct bpf_link_info *info) {
  union bpf_attr a;

  memset(info, 0, sizeof(*info));
  memset(&a, 0, sizeof(a));
  a.info.bpf_fd = fd;
  a.info.info_len = sizeof(*info);
  a.info.info = (uint64_t)(uintptr_t)info;
  return bpf(BPF_OBJ_GET_INFO_BY_FD, &a, sizeof(a));
}

static void check_link(void) {
  int map = map_create(BPF_MAP_TYPE_ARRAY, 4, 8, 1);
  struct insn prog[] = {
      ST_MEM(SZ_W, 10, -4, 0),
      MOV64_REG(2, 10),
      ADD64_IMM(2, -4),
      LD_MAP_FD(1, map),
      CALL(BPF_FUNC_map_lookup_elem),
      JMP_IMM(0x10, 0, 0, 2),
      MOV64_IMM(1, 1),
      ATOMIC_DW(0, 1, 0, 0x00 /* BPF_ADD */),
      MOV64_IMM(0, 0),
      EXIT(),
  };
  int pfd2 = -1, link = -1;
  int prog_fd = map >= 0 ? prog_load(prog, sizeof(prog) / sizeof(prog[0])) : -1;
  struct perf_event_attr attr;

  if (prog_fd < 0) {
    bad("link", g_log[0] ? g_log : "refused", (long)prog_fd);
    goto out;
  }
  memset(&attr, 0, sizeof(attr));
  attr.size = sizeof(attr);
  attr.type = PERF_TYPE_SOFTWARE;
  attr.config = PERF_COUNT_SW_CPU_CLOCK;
  attr.sample_type = PERF_SAMPLE_IP;
  attr.freq = 1;
  attr.sample_freq = 200;
  attr.disabled = 1;
  pfd2 = (int)syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
  if (pfd2 < 0) {
    bad("link", "perf_event_open", (long)pfd2);
    goto out;
  }

  union bpf_attr a;

  memset(&a, 0, sizeof(a));
  a.link_create.prog_fd = prog_fd;
  a.link_create.target_fd = pfd2;
  a.link_create.attach_type = BPF_PERF_EVENT;
  link = bpf(BPF_LINK_CREATE, &a, sizeof(a));
  if (link < 0) {
    bad("link", "BPF_LINK_CREATE", (long)link);
    goto out;
  }
  int busy = ioctl(pfd2, PERF_EVENT_IOC_SET_BPF, prog_fd) != 0 && errno == EEXIST;
  struct bpf_link_info li;
  int pid = prog_id_of(prog_fd);

  memset(&a, 0, sizeof(a));
  a.start_id = 0;
  int found = 0;

  while (bpf(BPF_LINK_GET_NEXT_ID, &a, sizeof(a)) == 0) {
    if (link_info(link, &li) == 0 && a.next_id == li.id)
      found = 1;
    a.start_id = a.next_id;
  }
  link_info(link, &li);
  judge("link", busy && found && li.type == BPF_LINK_TYPE_PERF_EVENT &&
                    (int)li.prog_id == pid,
        "a bpf_link did not hold the event, or is not found by id with its "
        "program", (long)li.type);

  ioctl(pfd2, PERF_EVENT_IOC_ENABLE, 0);
  burn_ms(300);
  ioctl(pfd2, PERF_EVENT_IOC_DISABLE, 0);

  uint32_t key = 0;
  uint64_t count = 0;

  map_lookup(map, &key, &count);
  judge("link-runs", count >= 5,
        "the linked program did not run on the event's samples", (long)count);

  memset(&a, 0, sizeof(a));
  a.link_detach.link_fd = link;
  int det = bpf(BPF_LINK_DETACH, &a, sizeof(a));

  link_info(link, &li);
  int freed = ioctl(pfd2, PERF_EVENT_IOC_SET_BPF, prog_fd) == 0;

  judge("link-detach", det == 0 && li.prog_id == 0 && freed,
        "BPF_LINK_DETACH did not let the event go", (long)det);
out:
  if (link >= 0)
    close(link);
  if (pfd2 >= 0)
    close(pfd2);
  if (prog_fd >= 0)
    close(prog_fd);
  if (map >= 0)
    close(map);
}

/* ── contexts, and programs on tracepoints and kprobes ───────────────────── */

#define TRACEFS "/sys/kernel/tracing"

static long tracefs_id(const char *group, const char *event) {
  char path[256], buf[32];
  int fd;
  ssize_t n;

  snprintf(path, sizeof(path), TRACEFS "/events/%s/%s/id", group, event);
  fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return -1;
  buf[n] = 0;
  return strtol(buf, NULL, 10);
}

static int tracefs_write(const char *file, const char *text) {
  int fd = open(TRACEFS "/" "kprobe_events", O_WRONLY);
  ssize_t n;

  (void)file;
  if (fd < 0)
    return -1;
  n = write(fd, text, strlen(text));
  close(fd);
  return n < 0 ? -1 : 0;
}

/* A counting program for a trace event: it adds one to a map when the u64 at
 * ctx + `off` is `want` (or on every hit, with `always`), and keeps the last
 * value it read there in the map's second slot. */
static int trace_counter(int map, int type, int off, int want, int always) {
  struct insn prog[] = {
      MOV64_REG(6, 1),
      ST_MEM(SZ_W, 10, -4, 0),
      MOV64_REG(2, 10),
      ADD64_IMM(2, -4),
      LD_MAP_FD(1, map),
      CALL(BPF_FUNC_map_lookup_elem),
      JMP_IMM(0x10, 0, 0, 8),                  /* 7: null -> 16 */
      LDX_MEM(SZ_DW, 1, 6, off),               /* the field */
      STX_MEM(SZ_DW, 0, 1, 8),                 /* the value seen */
      JMP_IMM(0x10, 1, want, always ? 0 : 1),  /* 10: match -> 12 */
      JA(always ? 0 : 3),                      /* 11: no match -> 15 */
      MOV64_IMM(1, 1),                         /* 12 */
      ATOMIC_DW(0, 1, 0, 0x00),
      JA(0),
      JA(0), /* 15 */
      MOV64_IMM(0, 0), /* 16 */
      EXIT(),
  };

  return prog_load_type(prog, sizeof(prog) / sizeof(prog[0]), type);
}

/* A KPROBE program that counts its hits and keeps the first 8 bytes its
 * probed function's first argument points at (bpf_probe_read_kernel), so the
 * test can check it found the argument by what it points to rather than by
 * what a kernel address looks like on one architecture. */
static int kprobe_arg_reader(int map) {
  struct insn prog[] = {
      MOV64_REG(6, 1),                          /* 0: r6 = ctx */
      LDX_MEM(SZ_DW, 7, 6, REGS_ARG0 * 8),      /* 1: r7 = first argument */
      MOV64_REG(1, 10),                         /* 2 */
      ADD64_IMM(1, -16),                        /* 3: r1 = fp - 16 */
      MOV64_IMM(2, 8),                          /* 4: r2 = 8 */
      MOV64_REG(3, 7),                          /* 5: r3 = the argument */
      CALL(BPF_FUNC_probe_read_kernel),         /* 6 */
      ST_MEM(SZ_W, 10, -4, 0),                  /* 7: key = 0 */
      MOV64_REG(2, 10),                         /* 8 */
      ADD64_IMM(2, -4),                         /* 9 */
      LD_MAP_FD(1, map),                        /* 10, 11 */
      CALL(BPF_FUNC_map_lookup_elem),           /* 12 */
      JMP_IMM(0x10, 0, 0, 5),                   /* 13: null -> 19 */
      LDX_MEM(SZ_DW, 1, 10, -16),               /* 14: the bytes read */
      STX_MEM(SZ_DW, 0, 1, 8),                  /* 15: value[1] */
      MOV64_IMM(1, 1),                          /* 16 */
      ATOMIC_DW(0, 1, 0, 0x00),                 /* 17: value[0] += 1 */
      JA(0),                                    /* 18 */
      MOV64_IMM(0, 0),                          /* 19 */
      EXIT(),                                   /* 20 */
  };

  return prog_load_type(prog, sizeof(prog) / sizeof(prog[0]),
                        BPF_PROG_TYPE_KPROBE);
}

static int trace_open(long id) {
  struct perf_event_attr attr;

  memset(&attr, 0, sizeof(attr));
  attr.size = sizeof(attr);
  attr.type = PERF_TYPE_TRACEPOINT;
  attr.config = (uint64_t)id;
  attr.sample_period = 1;
  attr.disabled = 1;
  return (int)syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
}

static void check_contexts(void) {
  /* A PERF_EVENT program reads the sample's instruction pointer where
   * bpf_perf_event_data puts it. */
  struct insn ip_prog[] = {
      LDX_MEM(SZ_DW, 0, 1, REGS_IP * 8),
      EXIT(),
  };
  int fd = prog_load(ip_prog, 2);
  uint32_t ret = 0;

  if (fd >= 0)
    prog_run(fd, 0x12345678, &ret);
  judge("ctx-regs", fd >= 0 && ret == 0x12345678,
        "a perf-event program does not find the ip in its pt_regs",
        (long)ret);
  if (fd >= 0)
    close(fd);

  /* ... and nothing past bpf_perf_event_data. */
  struct insn past[] = {
      LDX_MEM(SZ_DW, 0, 1, (REGS_WORDS + 2) * 8),
      EXIT(),
  };

  fd = prog_load(past, 2);
  judge("ctx-bounds", fd < 0 && errno == EINVAL,
        "a read past the context was accepted", (long)fd);
  if (fd >= 0)
    close(fd);
}

static void check_trace_programs(void) {
  int map = map_create(BPF_MAP_TYPE_ARRAY, 4, 16, 1);
  long id = tracefs_id("raw_syscalls", "sys_enter");
  uint32_t key = 0;
  uint64_t val[2] = {0, 0};

  if (map < 0 || id <= 0) {
    bad("tp-prog", "no map, or raw_syscalls:sys_enter is not in tracefs", id);
    if (map >= 0)
      close(map);
    return;
  }

  /* Pairing: a tracepoint takes a TRACEPOINT program, not a PERF_EVENT one. */
  int pev = trace_open(id);
  int wrong = trace_counter(map, BPF_PROG_TYPE_PERF_EVENT, 8, SYS_getpid, 0);
  int prog = trace_counter(map, BPF_PROG_TYPE_TRACEPOINT, 8, SYS_getpid, 0);

  if (pev < 0 || wrong < 0 || prog < 0) {
    bad("tp-prog", g_log[0] ? g_log : "perf_event_open or a program load",
        (long)pev);
    goto out;
  }
  judge("attach-type", ioctl(pev, PERF_EVENT_IOC_SET_BPF, wrong) != 0 &&
                           errno == EINVAL,
        "a PERF_EVENT program was attached to a tracepoint", 0);
  if (ioctl(pev, PERF_EVENT_IOC_SET_BPF, prog) != 0) {
    bad("tp-prog", "PERF_EVENT_IOC_SET_BPF", -1);
    goto out;
  }
  ioctl(pev, PERF_EVENT_IOC_ENABLE, 0);
  for (int i = 0; i < 20; i++)
    syscall(SYS_getpid);
  ioctl(pev, PERF_EVENT_IOC_DISABLE, 0);
  map_lookup(map, &key, val);
  note("sys_enter program: %llu getpid hits", (unsigned long long)val[0]);
  judge("tp-prog", val[0] >= 20,
        "the program on sys_enter did not see twenty getpid calls in its "
        "record", (long)val[0]);

  /* A kprobe program sees the probed function's registers. */
  {
    static const char *cands[] = {"vfs_find_node", "find_child", NULL};
    int kp = -1, kprog = -1, armed = 0;
    long kid = -1;
    char line[96];

    for (int i = 0; cands[i] && !armed; i++) {
      snprintf(line, sizeof(line), "p:b1bpf %s\n", cands[i]);
      armed = tracefs_write("kprobe_events", line) == 0;
    }
    kid = armed ? tracefs_id("kprobes", "b1bpf") : -1;
    val[0] = val[1] = 0;
    map_update(map, &key, val, 0);
    kprog = kprobe_arg_reader(map);
    kp = kid > 0 ? trace_open(kid) : -1;
    if (kp >= 0 && kprog >= 0 && ioctl(kp, PERF_EVENT_IOC_SET_BPF, kprog) == 0) {
      ioctl(kp, PERF_EVENT_IOC_ENABLE, 0);
      for (int i = 0; i < 20; i++)
        (void)access("/proc/self/stat", F_OK);
      ioctl(kp, PERF_EVENT_IOC_DISABLE, 0);
      map_lookup(map, &key, val);
      char head[9];

      memcpy(head, &val[1], 8);
      head[8] = 0;
      note("kprobe program: %llu hits, first argument points at \"%s\"",
           (unsigned long long)val[0], head);
      /* The probed lookup's first argument is the path being looked up. */
      judge("kprobe-prog", val[0] >= 20 && memcmp(head, "/proc/", 6) == 0,
            "the program on the kprobe did not run, or did not see the "
            "probed function's first argument in its pt_regs",
            (long)val[0]);
    } else {
      bad("kprobe-prog", g_log[0] ? g_log : "could not arm or attach", kid);
    }
    if (kp >= 0)
      close(kp);
    if (kprog >= 0)
      close(kprog);
    if (armed)
      tracefs_write("kprobe_events", "-:b1bpf\n");
  }
out:
  if (pev >= 0)
    close(pev);
  if (wrong >= 0)
    close(wrong);
  if (prog >= 0)
    close(prog);
  close(map);
}

/* ── per-CPU maps and perf_event_output ──────────────────────────────────── */

static int possible_cpus(void) {
  char buf[64];
  int fd = open("/sys/devices/system/cpu/possible", O_RDONLY), a = 0, b = 0;
  ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;

  if (fd >= 0)
    close(fd);
  if (n <= 0)
    return 1;
  buf[n] = 0;
  if (sscanf(buf, "%d-%d", &a, &b) == 2)
    return b + 1;
  return 1;
}

static void check_percpu(void) {
  int ncpu = possible_cpus();
  int map = map_create(BPF_MAP_TYPE_PERCPU_ARRAY, 4, 8, 1);
  struct insn prog[] = {
      ST_MEM(SZ_W, 10, -4, 0),
      MOV64_REG(2, 10),
      ADD64_IMM(2, -4),
      LD_MAP_FD(1, map),
      CALL(BPF_FUNC_map_lookup_elem),
      JMP_IMM(0x10, 0, 0, 3),
      LDX_MEM(SZ_DW, 1, 0, 0),
      ADD64_IMM(1, 1),
      STX_MEM(SZ_DW, 0, 1, 0),
      MOV64_IMM(0, 0),
      EXIT(),
  };
  int fd = map >= 0 ? prog_load(prog, sizeof(prog) / sizeof(prog[0])) : -1;
  uint64_t vals[64];
  uint32_t key = 0, ret;
  uint64_t sum = 0;

  if (fd < 0 || ncpu > 64) {
    bad("percpu", g_log[0] ? g_log : "map or program", (long)fd);
    goto out;
  }
  for (int i = 0; i < 7; i++)
    prog_run(fd, 0, &ret);
  memset(vals, 0xff, sizeof(vals));
  if (map_lookup(map, &key, vals) == 0)
    for (int c = 0; c < ncpu; c++)
      sum += vals[c];
  note("per-CPU array over %d CPUs sums to %llu", ncpu,
       (unsigned long long)sum);
  judge("percpu", sum == 7,
        "a per-CPU array's values, one per possible CPU, do not add up to "
        "the program's runs",
        (long)sum);
out:
  if (fd >= 0)
    close(fd);
  if (map >= 0)
    close(map);
}

static void check_perf_output(void) {
  int ncpu = possible_cpus();
  int arr = map_create(BPF_MAP_TYPE_PERF_EVENT_ARRAY, 4, 4, ncpu);
  int evfd[64];
  void *rings[64];
  int ok_ = 0;

  for (int c = 0; c < 64; c++) {
    evfd[c] = -1;
    rings[c] = MAP_FAILED;
  }
  if (arr < 0 || ncpu > 64) {
    bad("perf-output", "perf event array", (long)arr);
    return;
  }
  for (int c = 0; c < ncpu; c++) {
    struct perf_event_attr attr;

    memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_SOFTWARE;
    attr.config = PERF_COUNT_SW_BPF_OUTPUT;
    attr.sample_type = PERF_SAMPLE_RAW;
    attr.sample_period = 1;
    attr.wakeup_events = 1;
    evfd[c] = (int)syscall(__NR_perf_event_open, &attr, -1, c, -1, 0);
    if (evfd[c] < 0) {
      bad("perf-output", "perf_event_open of a BPF_OUTPUT event", evfd[c]);
      goto out;
    }
    rings[c] = mmap(0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, evfd[c], 0);
    if (rings[c] == MAP_FAILED) {
      bad("perf-output", "mmap of the event's ring", -1);
      goto out;
    }
    uint32_t k = (uint32_t)c, v = (uint32_t)evfd[c];

    if (map_update(arr, &k, &v, 0) != 0) {
      bad("perf-output", "the event did not go into the array", -1);
      goto out;
    }
  }
  struct insn prog[] = {
      ST_MEM(SZ_W, 10, -8, 0x5eed),
      ST_MEM(SZ_W, 10, -4, 0xcafe),
      MOV64_REG(4, 10),
      ADD64_IMM(4, -8),
      LD_MAP_FD(2, arr),
      /* BPF_F_CURRENT_CPU: 0xffffffff, zero-extended by a 32-bit move */
      {(uint8_t)(0x04 | 0xb0), REGS(3, 0), 0, -1},
      MOV64_IMM(5, 8),
      CALL(BPF_FUNC_perf_event_output),
      EXIT(),
  };
  int fd = prog_load(prog, sizeof(prog) / sizeof(prog[0]));
  uint32_t ret = 1;

  if (fd < 0) {
    bad("perf-output", g_log[0] ? g_log : "refused", (long)fd);
    goto out;
  }
  prog_run(fd, 0, &ret);
  close(fd);
  for (int c = 0; c < ncpu && !ok_; c++) {
    struct perf_event_mmap_page *pg = rings[c];
    uint8_t *data = (uint8_t *)rings[c] + 4096;
    uint64_t head = __atomic_load_n(&pg->data_head, __ATOMIC_ACQUIRE);

    /* Walk the records: the event may have written others (the task's own
     * COMM and MMAP) before the sample. */
    for (uint64_t off = 0; off + 8 <= head && off < 2 * 4096 && !ok_;) {
      struct perf_event_header *h = (struct perf_event_header *)(data + off);

      if (!h->size)
        break;
      if (h->type == PERF_RECORD_SAMPLE && h->size == 24) {
        uint32_t raw = *(uint32_t *)(data + off + 8);
        uint32_t w0 = *(uint32_t *)(data + off + 12);
        uint32_t w1 = *(uint32_t *)(data + off + 16);

        ok_ = raw == 12 && w0 == 0x5eed && w1 == 0xcafe;
      }
      off += h->size;
    }
  }
  judge("perf-output", (int32_t)ret == 0 && ok_,
        "perf_event_output's record is not in this CPU's BPF_OUTPUT ring",
        (long)(int32_t)ret);
out:
  for (int c = 0; c < 64; c++) {
    if (rings[c] != MAP_FAILED)
      munmap(rings[c], 3 * 4096);
    if (evfd[c] >= 0)
      close(evfd[c]);
  }
  close(arr);
}

/* ── uprobes ─────────────────────────────────────────────────────────────── */

/* The probed function: not inlined, and it computes something, so a probe
 * that broke its first instruction would show in the answer. */
__attribute__((noinline)) int uprobe_target(int x) {
  __asm__ volatile("" ::: "memory");
  return x * 3 + 1;
}

/* The file offset of an address in this program: its mapping's offset plus
 * the distance into the mapping, from /proc/self/maps. */
static long file_offset_of(const void *addr, char *path, size_t pathlen) {
  FILE *f = fopen("/proc/self/maps", "r");
  char line[512];
  unsigned long a = (unsigned long)addr;

  if (!f)
    return -1;
  while (fgets(line, sizeof(line), f)) {
    unsigned long lo, hi, off;
    char perms[8], p[256] = "";

    if (sscanf(line, "%lx-%lx %7s %lx %*s %*s %255s", &lo, &hi, perms, &off,
               p) < 4)
      continue;
    if (a >= lo && a < hi && perms[2] == 'x' && p[0] == '/') {
      fclose(f);
      snprintf(path, pathlen, "%s", p);
      return (long)(a - lo + off);
    }
  }
  fclose(f);
  return -1;
}

static void check_uprobe(void) {
  char path[256];
  long off = file_offset_of((const void *)uprobe_target, path, sizeof(path));
  int map = map_create(BPF_MAP_TYPE_ARRAY, 4, 16, 1);
  int prog = -1, pfd = -1, type = -1;
  uint32_t key = 0;
  uint64_t val[2] = {0, 0};
  int sum = 0;

  {
    int fd = open("/sys/bus/event_source/devices/uprobe/type", O_RDONLY);
    char b[16] = "";

    if (fd >= 0) {
      if (read(fd, b, sizeof(b) - 1) > 0)
        type = atoi(b);
      close(fd);
    }
  }
  if (off < 0 || map < 0 || type < 0) {
    bad("uprobe", "no mapping of the probed function, map, or uprobe PMU",
        off);
    goto out;
  }
  prog = trace_counter(map, BPF_PROG_TYPE_KPROBE, REGS_ARG0 * 8, 0, 1);
  struct perf_event_attr attr;

  memset(&attr, 0, sizeof(attr));
  attr.size = sizeof(attr);
  attr.type = (uint32_t)type;
  attr.config1 = (uint64_t)(uintptr_t)path;
  attr.config2 = (uint64_t)off;
  attr.sample_period = 1;
  pfd = (int)syscall(__NR_perf_event_open, &attr, -1, 0, -1, 0);
  if (prog < 0 || pfd < 0 || ioctl(pfd, PERF_EVENT_IOC_SET_BPF, prog) != 0) {
    bad("uprobe", g_log[0] ? g_log : "the uprobe PMU event or its attach",
        (long)pfd);
    goto out;
  }
  ioctl(pfd, PERF_EVENT_IOC_ENABLE, 0);
  for (int i = 0; i < 10; i++)
    sum += uprobe_target(i);
  ioctl(pfd, PERF_EVENT_IOC_DISABLE, 0);
  map_lookup(map, &key, val);
  note("uprobe on %s+%#lx: %llu hits, last argument %llu, sum %d", path, off,
       (unsigned long long)val[0], (unsigned long long)val[1], sum);
  /* 3*(0+..+9) + 10 = 145: the probed function still does its job. */
  judge("uprobe", val[0] == 10 && val[1] == 9 && sum == 145,
        "the uprobe did not fire once per call with the argument in pt_regs, "
        "or the probed function stopped working",
        (long)val[0]);
out:
  if (pfd >= 0)
    close(pfd);
  /* Gone with its event: the function runs clean again. */
  judge("uprobe-removed", uprobe_target(4) == 13,
        "the probed function misbehaves after the probe is gone", 0);
  if (prog >= 0)
    close(prog);
  if (map >= 0)
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
  check_btf();
  check_verifier_soundness();
  check_probe_read();
  check_ringbuf();
  check_stackmap();
  check_atomic();
  check_link();
  check_contexts();
  check_trace_programs();
  check_percpu();
  check_perf_output();
  check_uprobe();

  printf("M126-BPF: done\n");
  fflush(stdout);
  return fails ? 1 : 0;
}
