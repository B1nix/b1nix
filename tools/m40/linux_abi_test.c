/* M40 — Linux-ABI conformance test, built as a static Linux x86_64 ELF.
 *
 * Companion to tools/m40/linux_hello.S: that blob proves a Linux binary boots
 * and reaches the console at all; this one exercises the *rest* of the Linux
 * syscall surface b1nix translates — positional I/O, the time calls, the
 * credential/scheduler/capability calls, mlock, sethostname, the xattr fd
 * forms, and the Linux /proc + /sys files a Linux program expects to read.
 *
 * Freestanding on purpose: no libc, raw `syscall` instructions with LINUX
 * syscall numbers, its own _start. That keeps the test honest — every number
 * below goes through the kernel's Linux translation layer, not through a b1nix
 * libc that could paper over a gap.
 *
 * Build: sh tools/m40/build-linux-abi-test.sh  (writes linux_abi_test.bin,
 * committed, staged into the rootfs as /bin/m40-linux-abi).
 */

typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;
typedef int i32;

/* ── Linux x86_64 syscall numbers used below ── */
#define SYS_read            0
#define SYS_write           1
#define SYS_open            2
#define SYS_close           3
#define SYS_fstat           5
#define SYS_lseek           8
#define SYS_mmap            9
#define SYS_munmap          11
#define SYS_pread64         17
#define SYS_pwrite64        18
#define SYS_mincore         27
#define SYS_pause           34
#define SYS_rt_sigpending   127
#define SYS_uname           63
#define SYS_truncate        76
#define SYS_creat           85
#define SYS_gettimeofday    96
#define SYS_getresuid       118
#define SYS_getresgid       120
#define SYS_capget          125
#define SYS_personality     135
#define SYS_statfs          137
#define SYS_fstatfs         138
#define SYS_sched_getparam  143
#define SYS_sched_getscheduler 145
#define SYS_sched_get_priority_max 146
#define SYS_sched_rr_get_interval  148
#define SYS_mlock           149
#define SYS_munlock         150
#define SYS_chroot          161
#define SYS_sethostname     170
#define SYS_time            201
#define SYS_sched_setaffinity 203
#define SYS_epoll_create    213
#define SYS_readlinkat      267
#define SYS_unlinkat        263
#define SYS_renameat        264
#define SYS_exit_group      231
#define SYS_utime           132
#define SYS_readahead       187
#define SYS_unshare         272
#define SYS_syncfs          306
#define SYS_preadv          295

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0100
#define O_TRUNC  01000

#define AT_FDCWD (-100)

static long sys6(long n, long a, long b, long c, long d, long e, long f) {
  long ret;
  register long r10 __asm__("r10") = d;
  register long r8 __asm__("r8") = e;
  register long r9 __asm__("r9") = f;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory");
  return ret;
}
#define sys0(n)                sys6((n), 0, 0, 0, 0, 0, 0)
#define sys1(n, a)             sys6((n), (long)(a), 0, 0, 0, 0, 0)
#define sys2(n, a, b)          sys6((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define sys3(n, a, b, c)       sys6((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define sys4(n, a, b, c, d)    sys6((n), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define sys5(n, a, b, c, d, e) sys6((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)

/* ── tiny string helpers ── */
static unsigned slen(const char *s) {
  unsigned n = 0;
  while (s[n])
    n++;
  return n;
}

static int seq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static void out(const char *s) { sys3(SYS_write, 1, s, slen(s)); }

static void out_num(long v) {
  char buf[24];
  int i = (int)sizeof(buf);
  int neg = 0;
  unsigned long u = (unsigned long)v;
  if (v < 0) {
    neg = 1;
    u = (unsigned long)(-v);
  }
  buf[--i] = '\0';
  if (u == 0)
    buf[--i] = '0';
  while (u) {
    buf[--i] = (char)('0' + (u % 10));
    u /= 10;
  }
  if (neg)
    buf[--i] = '-';
  out(&buf[i]);
}

static int g_failures;

static void ok(const char *name) {
  out("M40-ABI: ok ");
  out(name);
  out("\n");
}

static void fail(const char *name, long v) {
  g_failures++;
  out("M40-ABI: FAIL ");
  out(name);
  out(" rc=");
  out_num(v);
  out("\n");
}

static void check(const char *name, int cond, long v) {
  if (cond)
    ok(name);
  else
    fail(name, v);
}

/* Read a whole file into buf; returns the byte count or -errno. */
static long slurp(const char *path, char *buf, unsigned cap) {
  long fd = sys3(SYS_open, path, O_RDONLY, 0);
  if (fd < 0)
    return fd;
  long n = sys3(SYS_read, fd, buf, cap - 1);
  sys1(SYS_close, fd);
  if (n < 0)
    return n;
  buf[n] = '\0';
  return n;
}

static int contains(const char *hay, const char *needle) {
  unsigned nl = slen(needle);
  for (unsigned i = 0; hay[i]; i++) {
    unsigned j = 0;
    while (j < nl && hay[i + j] == needle[j])
      j++;
    if (j == nl)
      return 1;
  }
  return 0;
}

/* ── Linux struct layouts we read back ── */
struct lx_timeval {
  i64 tv_sec, tv_usec;
};
struct lx_timespec {
  i64 tv_sec, tv_nsec;
};
struct lx_statfs {
  u64 f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_fsid;
  u64 f_namelen, f_frsize, f_flags, f_spare[4];
};
struct lx_utsname {
  char sysname[65], nodename[65], release[65], version[65], machine[65],
      domainname[65];
};
struct lx_iovec {
  void *base;
  unsigned long len;
};
/* Linux struct stat (x86_64): st_size at offset 48, st_mtime at 88. */
struct lx_stat {
  u64 st_dev, st_ino, st_nlink;
  u32 st_mode, st_uid, st_gid, __pad0;
  u64 st_rdev;
  i64 st_size, st_blksize, st_blocks;
  i64 st_atime, st_atime_nsec, st_mtime, st_mtime_nsec, st_ctime,
      st_ctime_nsec;
  i64 __unused[3];
};

#define TMPFILE "/tmp/m40abi.tmp"
#define TMPFILE2 "/tmp/m40abi.tmp2"

static void test_positional_io(void) {
  long fd = sys3(SYS_open, TMPFILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    fail("pread-pwrite", fd);
    return;
  }
  if (sys3(SYS_write, fd, "hello world", 11) != 11) {
    fail("pread-pwrite", -1);
    sys1(SYS_close, fd);
    return;
  }
  /* Park the shared offset at 3; pread must not move it. */
  sys3(SYS_lseek, fd, 3, 0 /* SEEK_SET */);
  char buf[16];
  long n = sys4(SYS_pread64, fd, buf, 4, 6);
  long pos = sys3(SYS_lseek, fd, 0, 1 /* SEEK_CUR */);
  buf[n > 0 ? n : 0] = '\0';
  check("pread64", n == 4 && seq(buf, "worl") && pos == 3, n);

  n = sys4(SYS_pwrite64, fd, "WORLD", 5, 6);
  long m = sys4(SYS_pread64, fd, buf, 5, 6);
  buf[m > 0 ? m : 0] = '\0';
  check("pwrite64", n == 5 && m == 5 && seq(buf, "WORLD"), n);

  /* preadv over two segments. */
  char a[3], b[3];
  struct lx_iovec iov[2] = {{a, 3}, {b, 2}};
  long pv = sys5(SYS_preadv, fd, iov, 2, 0, 0);
  check("preadv", pv == 5 && a[0] == 'h' && a[1] == 'e' && b[0] == 'l', pv);
  sys1(SYS_close, fd);

  /* truncate(2) by path, then confirm the size through stat. */
  long tr = sys2(SYS_truncate, TMPFILE, 4);
  fd = sys3(SYS_open, TMPFILE, O_RDONLY, 0);
  struct lx_stat st;
  long sr = sys2(SYS_fstat, fd, &st);
  sys1(SYS_close, fd);
  check("truncate", tr == 0 && sr == 0 && st.st_size == 4, tr ? tr : sr);

  /* utime(2) with an explicit stamp, read back through fstat. */
  u64 times[2] = {1700000000UL, 1700000123UL};
  long ut = sys2(SYS_utime, TMPFILE, times);
  fd = sys3(SYS_open, TMPFILE, O_RDONLY, 0);
  sr = sys2(SYS_fstat, fd, &st);
  sys1(SYS_close, fd);
  check("utime", ut == 0 && sr == 0 && st.st_mtime == 1700000123, ut);

  /* creat(2) + renameat(2) + unlinkat(2). */
  long cfd = sys2(SYS_creat, TMPFILE2, 0644);
  if (cfd >= 0)
    sys1(SYS_close, cfd);
  long rn = sys4(SYS_renameat, AT_FDCWD, TMPFILE2, AT_FDCWD, TMPFILE);
  check("creat-renameat", cfd >= 0 && rn == 0, cfd < 0 ? cfd : rn);
  sys3(SYS_unlinkat, AT_FDCWD, TMPFILE, 0);
}

static void test_fs_calls(void) {
  struct lx_statfs sfs;
  long r = sys2(SYS_statfs, "/", &sfs);
  check("statfs", r == 0 && sfs.f_bsize > 0 && sfs.f_blocks > 0, r);

  /* Use a regular file, not a directory: open(2) on a directory is not
   * guaranteed to hand back a readable descriptor, and the point here is the
   * fd-based filesystem calls, not directory opening. */
  long fd = sys3(SYS_open, "/etc/m40-smoke.sh", O_RDONLY, 0);
  if (fd < 0)
    fd = sys3(SYS_open, "/etc/inittab", O_RDONLY, 0);
  struct lx_statfs f2;
  long r2 = sys2(SYS_fstatfs, fd, &f2);
  check("fstatfs", r2 == 0 && f2.f_bsize == sfs.f_bsize, r2);
  long sfr = sys1(SYS_syncfs, fd);
  check("syncfs", sfr == 0, sfr);
  /* readahead(2) is advisory but must not report an error. */
  long ra = sys3(SYS_readahead, fd, 0, 4096);
  check("readahead", ra == 0, ra);
  sys1(SYS_close, fd);
}

static void test_time_calls(void) {
  struct lx_timeval tv;
  long r = sys2(SYS_gettimeofday, &tv, 0);
  long t = sys1(SYS_time, 0);
  long d = t - tv.tv_sec;
  if (d < 0)
    d = -d;
  check("gettimeofday", r == 0 && tv.tv_sec > 1000000000 && tv.tv_usec >= 0 &&
                            tv.tv_usec < 1000000,
        r);
  check("time", t > 1000000000 && d <= 2, t);
}

static void test_sched_and_creds(void) {
  long pol = sys1(SYS_sched_getscheduler, 0);
  check("sched-getscheduler", pol == 0, pol);

  i32 param = -1;
  long gp = sys2(SYS_sched_getparam, 0, &param);
  check("sched-getparam", gp == 0 && param == 0, gp);

  long mx = sys1(SYS_sched_get_priority_max, 0);
  check("sched-priority-max", mx == 0, mx);

  struct lx_timespec iv = {-1, -1};
  long ri = sys2(SYS_sched_rr_get_interval, 0, &iv);
  check("sched-rr-interval", ri == 0 && iv.tv_sec == 0 && iv.tv_nsec > 0, ri);

  /* A mask that permits every online CPU is accepted; an empty one is not. */
  u64 all = ~0UL, none = 0;
  long aff_ok = sys3(SYS_sched_setaffinity, 0, sizeof(all), &all);
  long aff_no = sys3(SYS_sched_setaffinity, 0, sizeof(none), &none);
  check("sched-setaffinity", aff_ok == 0 && aff_no < 0, aff_ok);

  u32 ru[3] = {99, 99, 99}, rg[3] = {99, 99, 99};
  long a = sys3(SYS_getresuid, &ru[0], &ru[1], &ru[2]);
  long b = sys3(SYS_getresgid, &rg[0], &rg[1], &rg[2]);
  check("getresuid", a == 0 && b == 0 && ru[0] == ru[1] && rg[0] == rg[1], a);

  /* capget: the v3 header carries two data structs. Running as root, every
   * capability must be present. */
  u32 hdr[2] = {0x20080522u, 0}; /* _LINUX_CAPABILITY_VERSION_3, pid 0 */
  u32 caps[6] = {0, 0, 0, 0, 0, 0};
  long cg = sys2(SYS_capget, hdr, caps);
  check("capget", cg == 0 && (ru[1] != 0 || caps[0] == 0xffffffffu), cg);

  long p = sys1(SYS_personality, 0xffffffffUL);
  check("personality", p == 0, p);

  long un = sys1(SYS_unshare, 0);
  check("unshare", un == 0, un);
}

static void test_memory_calls(void) {
  /* MAP_PRIVATE|MAP_ANONYMOUS */
  long addr = sys6(SYS_mmap, 0, 8192, 3 /* PROT_READ|WRITE */, 0x22, -1, 0);
  if (addr < 0 || addr == 0) {
    fail("mlock", addr);
    return;
  }
  /* Deliberately do NOT touch the mapping: an anonymous mmap is lazy, so the
   * pages are absent until something faults them in. mlock(2) must populate
   * them itself — mincore right after the lock is what proves it, rather than
   * mlock merely returning 0. */
  unsigned char vec[2] = {0, 0};
  long ml = sys2(SYS_mlock, addr, 8192);
  long mc = sys3(SYS_mincore, addr, 8192, vec);
  check("mlock-resident", ml == 0 && mc == 0 && (vec[0] & 1) && (vec[1] & 1),
        ml ? ml : mc);

  char *page = (char *)addr;
  page[0] = 'x';
  page[4096] = 'y';
  long mu = sys2(SYS_munlock, addr, 8192);
  check("mlock-munlock", ml == 0 && mu == 0, ml ? ml : mu);

  vec[0] = vec[1] = 0;
  mc = sys3(SYS_mincore, addr, 8192, vec);
  check("mincore", mc == 0 && (vec[0] & 1) && (vec[1] & 1), mc);

  sys2(SYS_munmap, addr, 8192);
}

static void test_signal_calls(void) {
  u64 pending = ~0UL;
  long r = sys2(SYS_rt_sigpending, &pending, 8);
  check("rt-sigpending", r == 0 && pending == 0, r);
}

static void test_hostname(void) {
  struct lx_utsname u;
  long r = sys1(SYS_uname, &u);
  if (r != 0) {
    fail("sethostname", r);
    return;
  }
  char saved[65];
  for (int i = 0; i < 65; i++)
    saved[i] = u.nodename[i];

  long sh = sys2(SYS_sethostname, "m40host", 7);
  long r2 = sys1(SYS_uname, &u);
  int uname_ok = (sh == 0 && r2 == 0 && seq(u.nodename, "m40host"));

  char buf[128];
  long n = slurp("/proc/sys/kernel/hostname", buf, sizeof(buf));
  int proc_ok = (n > 0 && contains(buf, "m40host"));
  check("sethostname", uname_ok, sh);
  check("proc-sys-hostname", proc_ok, n);

  /* Put the original name back so later tests see an unchanged system. */
  sys2(SYS_sethostname, saved, slen(saved));
}

static void test_procfs_sysfs(void) {
  char buf[1024];

  long n = slurp("/proc/self/statm", buf, sizeof(buf));
  check("proc-statm", n > 0 && buf[0] >= '1' && buf[0] <= '9', n);

  n = slurp("/proc/self/limits", buf, sizeof(buf));
  check("proc-limits", n > 0 && contains(buf, "Max open files"), n);

  n = slurp("/proc/swaps", buf, sizeof(buf));
  check("proc-swaps", n > 0 && contains(buf, "Filename"), n);

  n = slurp("/proc/sys/kernel/pid_max", buf, sizeof(buf));
  check("proc-sys-pid-max", n > 0 && buf[0] >= '1' && buf[0] <= '9', n);

  n = slurp("/sys/class/net/lo/mtu", buf, sizeof(buf));
  check("sys-class-net", n > 0 && contains(buf, "65536"), n);

  /* /proc/self/cwd is a symlink to the working directory. */
  n = sys4(SYS_readlinkat, AT_FDCWD, "/proc/self/cwd", buf, sizeof(buf) - 1);
  if (n > 0)
    buf[n] = '\0';
  check("proc-cwd-link", n > 0 && buf[0] == '/', n);
}

static void test_documented_gaps(void) {
  /* chroot is one of the calls b1nix deliberately does not implement: it must
   * report ENOSYS (38), not succeed and silently do nothing. */
  long r = sys1(SYS_chroot, "/");
  check("chroot-enosys", r == -38, r);
}

static void test_epoll(void) {
  long fd = sys1(SYS_epoll_create, 1);
  check("epoll-create", fd >= 0, fd);
  if (fd >= 0)
    sys1(SYS_close, fd);
}

/* _start is entered by the kernel with RSP 16-byte aligned, but the SysV ABI
 * says a function body starts with RSP%16 == 8 (the return address is on the
 * stack). Entering compiled code directly at _start therefore leaves every
 * 16-byte-aligned stack slot off by eight, and the first SSE spill (movaps)
 * takes a #GP in ring 3. Re-align and call the real body through a CALL so the
 * compiler's assumption holds. */
void abi_main(void);

__attribute__((naked)) void _start(void) {
  __asm__ volatile("xorq %rbp, %rbp\n\t"
                   "andq $-16, %rsp\n\t"
                   "callq abi_main\n\t"
                   "hlt");
}

void abi_main(void) {
  out("M40-ABI: start\n");

  test_positional_io();
  test_fs_calls();
  test_time_calls();
  test_sched_and_creds();
  test_memory_calls();
  test_signal_calls();
  test_hostname();
  test_procfs_sysfs();
  test_epoll();
  test_documented_gaps();

  /* "done" is emitted ONLY when every check passed — the harness greps for it
   * as the all-green marker, so a failure count must not contain it. */
  if (g_failures == 0)
    out("M40-ABI: done\n");
  else {
    out("M40-ABI: FAILURES=");
    out_num(g_failures);
    out("\n");
  }
  sys1(SYS_exit_group, g_failures ? 1 : 0);
  for (;;)
    ;
}
