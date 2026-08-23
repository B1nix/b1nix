/* M40 — Linux-ABI conformance test, built as a static Linux x86_64 ELF.
 *
 * Companion to tools/blobs/linux_hello.S: that blob proves a Linux binary boots
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
 * Build: sh tools/blobs/build-linux-abi-test.sh  (writes linux_abi_test.bin,
 * committed, staged into the rootfs as /bin/m40-linux-abi).
 */

typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;
typedef int i32;

/* ── Linux syscall numbers used below ──
 *
 * These are NOT shared between architectures. x86_64 keeps its historical
 * numbering; aarch64 uses asm-generic, which renumbers everything and simply
 * does not have the legacy calls (open, creat, mkdir, rmdir, unlink, fork,
 * pause, time, epoll_create, utime, getdents) — the *at forms and the 64-bit
 * variants replaced them. Those are expressed through the wrappers below so
 * the tests themselves stay architecture-neutral. Values are taken from the
 * kernel's own translation tables (kernel/syscall/syscall.c and
 * kernel/syscall/linux_abi.c), which is what actually has to agree. */
#if defined(__aarch64__)
#define SYS_read            63
#define SYS_write           64
#define SYS_close           57
#define SYS_fstat           80
#define SYS_lseek           62
#define SYS_mmap            222
#define SYS_munmap          215
#define SYS_pread64         67
#define SYS_pwrite64        68
#define SYS_mincore         232
#define SYS_rt_sigpending   136
#define SYS_uname           160
#define SYS_truncate        45
#define SYS_gettimeofday    169
#define SYS_getresuid       148
#define SYS_getresgid       150
#define SYS_capget          90
#define SYS_personality     92
#define SYS_statfs          43
#define SYS_fstatfs         44
#define SYS_sched_getparam  121
#define SYS_sched_getscheduler 120
#define SYS_sched_get_priority_max 125
#define SYS_sched_rr_get_interval  127
#define SYS_mlock           228
#define SYS_munlock         229
#define SYS_chroot          51
#define SYS_sethostname     161
#define SYS_sched_setaffinity 122
#define SYS_readlinkat      78
#define SYS_unlinkat        35
#define SYS_renameat        38
#define SYS_exit_group      94
#define SYS_readahead       213
#define SYS_unshare         97
#define SYS_syncfs          267
#define SYS_preadv          69
#define SYS_getpid          172
#define SYS_sched_getaffinity 123
#define SYS_capset          91
#define SYS_setfsuid        151
#define SYS_setfsgid        152
#define SYS_getcwd          17
#define SYS_chdir           49
#define SYS_semget          190
#define SYS_semop           193
#define SYS_semctl          191
#define SYS_msgget          186
#define SYS_msgsnd          189
#define SYS_msgrcv          188
#define SYS_msgctl          187
#define SYS_ptrace          117
#define SYS_ioprio_set      30
#define SYS_ioprio_get      31
#define SYS_tee             77
#define SYS_vmsplice        75
#define SYS_pipe2           59
#define SYS_name_to_handle_at 264
#define SYS_open_by_handle_at 265
#define SYS_rseq            293
#define SYS_swapon          224
#define SYS_swapoff         225
#define SYS_wait4           260
/* asm-generic replacements for the calls x86_64 spells legacy. */
#define SYS_openat          56
#define SYS_mkdirat         34
#define SYS_getdents64      61
#define SYS_clone           220
#define SYS_ppoll           73
#define SYS_clock_gettime   113
#define SYS_epoll_create1   20
#define SYS_utimensat       88
#define SYS_newfstatat      79
#define SYS_kill            129
#define SYS_settimeofday    170
#else
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
#define SYS_getdents        78
#define SYS_getpid          39
#define SYS_sched_getaffinity 204
#define SYS_capset          126
#define SYS_setfsuid        122
#define SYS_setfsgid        123
#define SYS_getcwd          79
#define SYS_chdir           80
#define SYS_semget          64
#define SYS_semop           65
#define SYS_semctl          66
#define SYS_msgget          68
#define SYS_msgsnd          69
#define SYS_msgrcv          70
#define SYS_msgctl          71
#define SYS_ptrace          101
#define SYS_ioprio_set      251
#define SYS_ioprio_get      252
#define SYS_tee             276
#define SYS_vmsplice        278
#define SYS_pipe2           293
#define SYS_name_to_handle_at 303
#define SYS_open_by_handle_at 304
#define SYS_rseq            334
#define SYS_swapon          167
#define SYS_swapoff         168
#define SYS_fork            57
#define SYS_wait4           61
#define SYS_mkdir           83
#define SYS_rmdir           84
#define SYS_unlink          87
#define SYS_openat          257
#define SYS_mkdirat         258
#define SYS_getdents64      217
#define SYS_clone           56
#define SYS_ppoll           271
#define SYS_clock_gettime   228
#define SYS_epoll_create1   291
#define SYS_utimensat       280
#define SYS_newfstatat      262
#define SYS_kill            62
#define SYS_settimeofday    164
#endif

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0100
#define O_TRUNC  01000

#define AT_FDCWD (-100)

#if defined(__aarch64__)
/* AArch64: number in x8, args x0..x5, trap with `svc #0`. */
static long sys6(long n, long a, long b, long c, long d, long e, long f) {
  register long x8 __asm__("x8") = n;
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e;
  register long x5 __asm__("x5") = f;
  __asm__ volatile("svc #0"
                   : "+r"(x0)
                   : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                   : "memory");
  return x0;
}
#else
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
#endif
#define sys0(n)                sys6((n), 0, 0, 0, 0, 0, 0)
#define sys1(n, a)             sys6((n), (long)(a), 0, 0, 0, 0, 0)
#define sys2(n, a, b)          sys6((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define sys3(n, a, b, c)       sys6((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define sys4(n, a, b, c, d)    sys6((n), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define sys5(n, a, b, c, d, e) sys6((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)

/* Calls x86_64 has as dedicated numbers and asm-generic expresses through the
 * *at forms. Wrapping them here keeps every test below architecture-neutral
 * while still driving the syscall the architecture really provides. */
#if defined(__aarch64__)
#define AT_REMOVEDIR_ 0x200
static long t_open(const char *p_, long fl, long mode) {
  return sys4(SYS_openat, AT_FDCWD, p_, fl, mode);
}
static long t_creat(const char *p_, long mode) {
  return sys4(SYS_openat, AT_FDCWD, p_, O_WRONLY | O_CREAT | O_TRUNC, mode);
}
static long t_mkdir(const char *p_, long mode) {
  return sys3(SYS_mkdirat, AT_FDCWD, p_, mode);
}
static long t_rmdir(const char *p_) {
  return sys3(SYS_unlinkat, AT_FDCWD, p_, AT_REMOVEDIR_);
}
__attribute__((unused)) static long t_unlink(const char *p_) {
  return sys3(SYS_unlinkat, AT_FDCWD, p_, 0);
}
static long t_fork(void) { return sys5(SYS_clone, 17 /* SIGCHLD */, 0, 0, 0, 0); }
__attribute__((unused)) static long t_pause(void) { return sys4(SYS_ppoll, 0, 0, 0, 0); }
static long t_getdents(long fd, void *buf, long n) {
  return sys3(SYS_getdents64, fd, buf, n);
}
static long t_epoll_create(long size) { (void)size; return sys1(SYS_epoll_create1, 0); }
static long t_time(void) {
  long ts[2] = {0, 0};
  long r = sys2(SYS_clock_gettime, 0 /* CLOCK_REALTIME */, ts);
  return r < 0 ? r : ts[0];
}
static long t_utime(const char *p_, const unsigned long *times) {
  /* utimensat takes struct timespec[2]; utime's utimbuf is two seconds. */
  long ts[4] = {times ? times[0] : 0, 0, times ? times[1] : 0, 0};
  return sys4(SYS_utimensat, AT_FDCWD, p_, times ? (long)ts : 0, 0);
}
#else
static long t_open(const char *p_, long fl, long mode) {
  return sys3(SYS_open, p_, fl, mode);
}
static long t_creat(const char *p_, long mode) { return sys2(SYS_creat, p_, mode); }
static long t_mkdir(const char *p_, long mode) { return sys2(SYS_mkdir, p_, mode); }
static long t_rmdir(const char *p_) { return sys1(SYS_rmdir, p_); }
__attribute__((unused)) static long t_unlink(const char *p_) { return sys1(SYS_unlink, p_); }
static long t_fork(void) { return sys0(SYS_fork); }
__attribute__((unused)) static long t_pause(void) { return sys0(SYS_pause); }
static long t_getdents(long fd, void *buf, long n) {
  return sys3(SYS_getdents, fd, buf, n);
}
static long t_epoll_create(long size) { return sys1(SYS_epoll_create, size); }
static long t_time(void) { return sys1(SYS_time, 0); }
static long t_utime(const char *p_, const unsigned long *times) {
  return sys2(SYS_utime, p_, times);
}
#endif

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
  long fd = t_open(path, O_RDONLY, 0);
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
  long fd = t_open(TMPFILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
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
  fd = t_open(TMPFILE, O_RDONLY, 0);
  struct lx_stat st;
  long sr = sys2(SYS_fstat, fd, &st);
  sys1(SYS_close, fd);
  check("truncate", tr == 0 && sr == 0 && st.st_size == 4, tr ? tr : sr);

  /* utime(2) with an explicit stamp, read back through fstat. */
  u64 times[2] = {1700000000UL, 1700000123UL};
  long ut = t_utime(TMPFILE, times);
  fd = t_open(TMPFILE, O_RDONLY, 0);
  sr = sys2(SYS_fstat, fd, &st);
  sys1(SYS_close, fd);
  check("utime", ut == 0 && sr == 0 && st.st_mtime == 1700000123, ut);

  /* creat(2) + renameat(2) + unlinkat(2). */
  long cfd = t_creat(TMPFILE2, 0644);
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
  long fd = t_open("/etc/m40-smoke.sh", O_RDONLY, 0);
  if (fd < 0)
    fd = t_open("/etc/inittab", O_RDONLY, 0);
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
  long t = t_time();
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
  /* An unassigned syscall number must report ENOSYS (38) rather than being
   * silently accepted — that is what tells a libc to use its fallback. */
  long r = sys0(1000);
  check("enosys-unmapped", r == -38, r);
}

static void test_epoll(void) {
  long fd = t_epoll_create(1);
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
#if defined(__aarch64__)
  /* Clear the frame pointer, align SP, call in. AAPCS64 wants SP 16-aligned;
   * the kernel already hands us that, so this only makes it explicit. */
  __asm__ volatile("mov x29, #0\n\t"
                   "mov x30, #0\n\t"
                   "mov x9, sp\n\t"
                   "and x9, x9, #-16\n\t"
                   "mov sp, x9\n\t"
                   "bl abi_main\n\t"
                   "brk #0");
#else
  __asm__ volatile("xorq %rbp, %rbp\n\t"
                   "andq $-16, %rsp\n\t"
                   "callq abi_main\n\t"
                   "hlt");
#endif
}


/* ── the calls closed in the M40 follow-up ─────────────────────────────── */

static void test_getdents_legacy(void) {
  long fd = t_open("/etc", O_RDONLY, 0);
  if (fd < 0) {
    fail("getdents-legacy", fd);
    return;
  }
  char buf[1024];
  long n = t_getdents(fd, buf, sizeof(buf));
  sys1(SYS_close, fd);
  if (n <= 0) {
    fail("getdents-legacy", n);
    return;
  }
  /* Walk the records: d_reclen at offset 16, name at 18, type in the last
   * byte. A directory must contain at least one entry with a plausible
   * record length and a NUL-terminated name. */
  int entries = 0, sane = 1;
  long off = 0;
  while (off < n) {
    unsigned short reclen = *(unsigned short *)&buf[off + 16];
    if (reclen < 20 || off + reclen > n) {
      sane = 0;
      break;
    }
    if (buf[off + 18] == '\0')
      sane = 0;
    entries++;
    off += reclen;
  }
  check("getdents-legacy", sane && entries > 0 && off == n, (long)entries);
}

/* getdents64's d_ino must be the filesystem's real inode number: it has to
 * match what stat(2) reports for the same file, and two entries must not share
 * one (both are true of a synthesized index only by accident). */
static void test_getdents_ino(void) {
  long fd = t_open("/etc", O_RDONLY, 0);
  if (fd < 0) {
    fail("getdents-ino", fd);
    return;
  }
  char buf[2048];
  long n = sys3(SYS_getdents64, fd, buf, sizeof(buf));
  sys1(SYS_close, fd);
  if (n <= 0) {
    fail("getdents-ino", n);
    return;
  }
  /* struct linux_dirent64 { u64 d_ino; i64 d_off; u16 d_reclen; u8 d_type;
   *                        char d_name[]; } */
  int checked = 0, distinct = 1, matched = 0;
  unsigned long seen[16];
  int nseen = 0;
  long off = 0;
  while (off < n) {
    unsigned long ino = *(unsigned long *)&buf[off];
    unsigned short reclen = *(unsigned short *)&buf[off + 16];
    const char *name = &buf[off + 19];
    if (reclen < 20 || off + reclen > n)
      break;
    if (ino == 0)
      distinct = 0;
    for (int i = 0; i < nseen; i++)
      if (seen[i] == ino)
        distinct = 0;
    if (nseen < 16)
      seen[nseen++] = ino;
    checked++;

    /* Compare against stat() for a regular entry. */
    if (!matched && name[0] != '.') {
      char path[128];
      int p = 0;
      const char *pre = "/etc/";
      while (pre[p]) {
        path[p] = pre[p];
        p++;
      }
      int q = 0;
      while (name[q] && p < 120)
        path[p++] = name[q++];
      path[p] = '\0';
      struct lx_stat st;
      if (sys4(SYS_newfstatat, AT_FDCWD, path, &st, 0) == 0 &&
          st.st_ino == ino)
        matched = 1;
    }
    off += reclen;
  }
  check("getdents-ino", checked > 2 && distinct && matched,
        (long)(checked * 100 + distinct * 10 + matched));
}

static void test_pipe_tee_vmsplice(void) {
  int a[2], b[2];
  long r1 = sys2(SYS_pipe2, a, 0);
  long r2 = sys2(SYS_pipe2, b, 0);
  if (r1 < 0 || r2 < 0) {
    fail("tee", r1 < 0 ? r1 : r2);
    return;
  }
  /* vmsplice INTO pipe a. */
  struct lx_iovec iov = {(void *)"teedata", 7};
  long vs = sys4(SYS_vmsplice, a[1], &iov, 1, 0);

  /* tee copies to pipe b without consuming a. */
  long t = sys4(SYS_tee, a[0], b[1], 16, 0);

  char x[8], y[8];
  long rx = sys3(SYS_read, a[0], x, 7);
  long ry = sys3(SYS_read, b[0], y, 7);
  x[rx > 0 ? rx : 0] = '\0';
  y[ry > 0 ? ry : 0] = '\0';

  check("vmsplice", vs == 7, vs);
  check("tee", t == 7 && rx == 7 && ry == 7 && seq(x, "teedata") &&
                   seq(y, "teedata"),
        t);
  sys1(SYS_close, a[0]);
  sys1(SYS_close, a[1]);
  sys1(SYS_close, b[0]);
  sys1(SYS_close, b[1]);
}

static void test_ioprio(void) {
  /* IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE=2, level 4) */
  int want = (2 << 13) | 4;
  long sr = sys3(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, 0, want);
  long gr = sys2(SYS_ioprio_get, 1, 0);
  check("ioprio", sr == 0 && gr == want, gr);
}

static void test_sysv_sem(void) {
  long id = sys3(SYS_semget, 0 /* IPC_PRIVATE */, 2, 0600 | 01000 /* IPC_CREAT */);
  if (id < 0) {
    fail("sysv-sem", id);
    return;
  }
  long sv = sys4(SYS_semctl, id, 0, 16 /* SETVAL */, 1);
  long v0 = sys4(SYS_semctl, id, 0, 12 /* GETVAL */, 0);

  /* Take the semaphore, then check it reads back as 0, then give it back. */
  struct {
    unsigned short num;
    short op;
    short flg;
  } down = {0, -1, 0}, up = {0, 1, 0};
  long d = sys3(SYS_semop, id, &down, 1);
  long v1 = sys4(SYS_semctl, id, 0, 12 /* GETVAL */, 0);
  long u = sys3(SYS_semop, id, &up, 1);
  long v2 = sys4(SYS_semctl, id, 0, 12, 0);

  /* A blocking-down on the already-zero second semaphore with IPC_NOWAIT must
   * report EAGAIN rather than hang. */
  struct {
    unsigned short num;
    short op;
    short flg;
  } nowait = {1, -1, 04000 /* IPC_NOWAIT */};
  long nw = sys3(SYS_semop, id, &nowait, 1);

  long rm = sys4(SYS_semctl, id, 0, 0 /* IPC_RMID */, 0);
  check("sysv-sem", sv == 0 && v0 == 1 && d == 0 && v1 == 0 && u == 0 &&
                        v2 == 1 && nw == -11 /* EAGAIN */ && rm == 0,
        v1);
}

static void test_sysv_msg(void) {
  long id = sys2(SYS_msgget, 0 /* IPC_PRIVATE */, 0600 | 01000 /* IPC_CREAT */);
  if (id < 0) {
    fail("sysv-msg", id);
    return;
  }
  struct {
    long mtype;
    char text[16];
  } m;

  m.mtype = 7;
  for (int i = 0; i < 5; i++)
    m.text[i] = "seven"[i];
  long s1 = sys4(SYS_msgsnd, id, &m, 5, 0);
  m.mtype = 3;
  for (int i = 0; i < 5; i++)
    m.text[i] = "three"[i];
  long s2 = sys4(SYS_msgsnd, id, &m, 5, 0);

  /* Type-selective receive: ask for 7, get "seven" even though 3 is queued
   * behind it — that selection is the whole point of a SysV queue. */
  for (int i = 0; i < 16; i++)
    m.text[i] = 0;
  long r1 = sys5(SYS_msgrcv, id, &m, 16, 7, 0);
  int got_seven = (r1 == 5 && m.mtype == 7 && m.text[0] == 's');
  long r2 = sys5(SYS_msgrcv, id, &m, 16, 0, 0);
  int got_three = (r2 == 5 && m.mtype == 3 && m.text[0] == 't');
  /* Nothing left: IPC_NOWAIT must report ENOMSG rather than block. */
  long r3 = sys5(SYS_msgrcv, id, &m, 16, 0, 04000 /* IPC_NOWAIT */);
  long rm = sys3(SYS_msgctl, id, 0 /* IPC_RMID */, 0);
  check("sysv-msg", s1 == 5 && s2 == 5 && got_seven && got_three &&
                        r3 == -42 /* ENOMSG */ && rm == 0,
        r1);
}

static void test_file_handles(void) {
  /* struct file_handle { u32 handle_bytes; int handle_type; u8 f_handle[]; } */
  struct {
    unsigned handle_bytes;
    int handle_type;
    unsigned payload;
  } fh = {sizeof(unsigned), 0, 0};
  int mount_id = 0;
  long r = sys5(SYS_name_to_handle_at, AT_FDCWD, "/etc/m40-smoke.sh", &fh,
                &mount_id, 0);
  if (r < 0) {
    fail("file-handle", r);
    return;
  }
  long fd = sys3(SYS_open_by_handle_at, AT_FDCWD, &fh, O_RDONLY);
  char c[4];
  long n = fd >= 0 ? sys3(SYS_read, fd, c, 2) : fd;
  if (fd >= 0)
    sys1(SYS_close, fd);
  check("file-handle", fd >= 0 && n == 2 && c[0] == '#', fd < 0 ? fd : n);

  /* A handle names a file, not a path: replace the file behind the path and
   * the handle must report ESTALE rather than open the impostor. */
  struct {
    unsigned handle_bytes;
    int handle_type;
    unsigned payload;
  } fh2 = {sizeof(unsigned), 0, 0};
  long w = t_open("/tmp/m40handle", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (w >= 0) {
    sys3(SYS_write, w, "a", 1);
    sys1(SYS_close, w);
  }
  long r2 = sys5(SYS_name_to_handle_at, AT_FDCWD, "/tmp/m40handle", &fh2,
                 &mount_id, 0);
  sys3(SYS_unlinkat, AT_FDCWD, "/tmp/m40handle", 0);
  w = t_open("/tmp/m40handle", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (w >= 0) {
    sys3(SYS_write, w, "b", 1);
    sys1(SYS_close, w);
  }
  long stale = sys3(SYS_open_by_handle_at, AT_FDCWD, &fh2, O_RDONLY);
  if (stale >= 0)
    sys1(SYS_close, stale);
  sys3(SYS_unlinkat, AT_FDCWD, "/tmp/m40handle", 0);
  check("file-handle-stale", r2 == 0 && stale == -116 /* ESTALE */, stale);
}

/* struct rseq is 32 bytes and must be 32-byte aligned. */
struct lx_rseq {
  unsigned cpu_id_start;
  unsigned cpu_id;
  unsigned long rseq_cs;
  unsigned flags;
  unsigned pad[3];
} __attribute__((aligned(32)));

static struct lx_rseq g_rseq_area;

/* struct rseq_cs: {version, flags, start_ip, post_commit_offset, abort_ip},
 * 32-byte aligned, published through rseq->rseq_cs while the sequence runs. */
struct lx_rseq_cs {
  unsigned version;
  unsigned flags;
  unsigned long start_ip;
  unsigned long post_commit_offset;
  unsigned long abort_ip;
} __attribute__((aligned(32)));

static struct lx_rseq_cs g_rseq_cs;

/* Run a critical section long enough to be preempted, and report how it
 * ended: 1 = ran to the commit instruction, 2 = the kernel restarted it at the
 * abort handler. The four bytes before the abort label are the signature the
 * kernel checks before it will ever redirect execution there. */
static long rseq_run_section(void) {
  long ret = 0;
#if defined(__aarch64__)
  __asm__ volatile(
      "adr x9, 1f\n\t"
      "str x9, [%1, #8]\n\t"          /* cs.start_ip           */
      "adr x10, 2f\n\t"
      "sub x10, x10, x9\n\t"
      "str x10, [%1, #16]\n\t"        /* cs.post_commit_offset */
      "adr x9, 4f\n\t"
      "str x9, [%1, #24]\n\t"         /* cs.abort_ip           */
      "str %1, [%2, #8]\n\t"          /* rseq->rseq_cs = &cs   */
      "1:\n\t"
      "movz x11, #0x4600\n\t"
      "movk x11, #0x23c3, lsl #16\n\t" /* 600000000 iterations, as x86 uses */
      "3: subs x11, x11, #1\n\t"
      "b.ne 3b\n\t"
      "mov %0, #1\n\t"                /* reached the commit    */
      "2:\n\t"
      "b 5f\n\t"
      ".long 0x53053053\n\t"          /* abort signature       */
      "4:\n\t"
      "mov %0, #2\n\t"                /* restarted by the kernel */
      "5:\n\t"
      : "=&r"(ret)
      : "r"(&g_rseq_cs), "r"(&g_rseq_area)
      : "x9", "x10", "x11", "memory");
#else
  __asm__ volatile(
      "leaq 1f(%%rip), %%rax\n\t"
      "movq %%rax, 8(%1)\n\t"          /* cs.start_ip           */
      "leaq 2f(%%rip), %%rdx\n\t"
      "subq %%rax, %%rdx\n\t"
      "movq %%rdx, 16(%1)\n\t"         /* cs.post_commit_offset */
      "leaq 4f(%%rip), %%rax\n\t"
      "movq %%rax, 24(%1)\n\t"         /* cs.abort_ip           */
      "movq %1, 8(%2)\n\t"             /* rseq->rseq_cs = &cs   */
      "1:\n\t"
      "movq $600000000, %%rcx\n\t"
      "3: decq %%rcx\n\t"
      "jnz 3b\n\t"
      "movq $1, %0\n\t"                /* reached the commit    */
      "2:\n\t"
      "jmp 5f\n\t"
      ".long 0x53053053\n\t"           /* abort signature       */
      "4:\n\t"
      "movq $2, %0\n\t"                /* restarted by the kernel */
      "5:\n\t"
      : "=&r"(ret)
      : "r"(&g_rseq_cs), "r"(&g_rseq_area)
      : "rax", "rcx", "rdx", "memory");
#endif
  return ret;
}

static void test_rseq(void) {
  g_rseq_area.cpu_id = 0xffffffffu;
  g_rseq_area.cpu_id_start = 0xffffffffu;
  g_rseq_area.rseq_cs = 0;
  long reg = sys4(SYS_rseq, &g_rseq_area, sizeof(struct lx_rseq), 0, 0x53053053);
  /* Any syscall is a return to userspace, so the cpu ids must be live now. */
  sys0(SYS_getpid);
  unsigned cpu = g_rseq_area.cpu_id;
  /* The other half of rseq: a critical section that gets preempted must resume
   * at its abort handler, not in the middle. The section below runs for far
   * longer than a scheduler tick, so it is always interrupted at least once. */
  long how = rseq_run_section();
  unsigned long cs_after = g_rseq_area.rseq_cs;

  long unreg = sys4(SYS_rseq, &g_rseq_area, sizeof(struct lx_rseq),
                    1 /* RSEQ_FLAG_UNREGISTER */, 0x53053053);
  check("rseq", reg == 0 && cpu != 0xffffffffu && cpu < 64 && unreg == 0,
        reg ? reg : (long)cpu);
  /* The kernel also consumes the descriptor, so a later interruption cannot
   * abort against a stale section. */
  check("rseq-abort", how == 2 && cs_after == 0, how);
}

static void test_swap_cycle(void) {
  /* Detach and re-attach the swap device. swapoff must page everything back
   * in first, so a successful round trip proves both halves. */
  long off = sys1(SYS_swapoff, "/dev/sdb");
  if (off < 0) {
    /* No swap device on this instance: the wiring is still checked by the
     * error path (a device that does not exist reports ENODEV). */
    long bogus = sys2(SYS_swapon, "/dev/nosuchdisk", 0);
    check("swapon-swapoff", bogus == -19 /* ENODEV */, bogus);
    return;
  }
  long on = sys2(SYS_swapon, "/dev/sdb", 0);
  check("swapon-swapoff", on == 0, on);
}

static void test_chroot(void) {
  /* chroot is irreversible for the calling task, so run it in a child. */
  t_mkdir("/tmp/m40jail", 0755);
  long fd = t_open("/tmp/m40jail/inside", O_WRONLY | O_CREAT | O_TRUNC,
                 0644);
  if (fd >= 0) {
    sys3(SYS_write, fd, "x", 1);
    sys1(SYS_close, fd);
  }

  long pid = t_fork();
  if (pid == 0) {
    /* Child: inside the jail, "/inside" must exist and "/etc" must not, and
     * ".." must not climb out of it. */
    long cr = sys1(SYS_chroot, "/tmp/m40jail");
    int ok_status = 0;
    if (cr == 0) {
      long f1 = t_open("/inside", O_RDONLY, 0);
      long f2 = t_open("/etc/m40-smoke.sh", O_RDONLY, 0);
      long f3 = t_open("/../../etc/m40-smoke.sh", O_RDONLY, 0);
      if (f1 >= 0)
        sys1(SYS_close, f1);
      ok_status = (f1 >= 0 && f2 < 0 && f3 < 0) ? 0 : 1;
    } else {
      ok_status = 2;
    }
    sys1(SYS_exit_group, ok_status);
    for (;;)
      ;
  }
  int status = -1;
  long w = sys4(SYS_wait4, pid, &status, 0, 0);
  check("chroot", pid > 0 && w == pid && (status & 0xff00) == 0,
        pid > 0 ? (long)status : pid);

  sys3(SYS_unlinkat, AT_FDCWD, "/tmp/m40jail/inside", 0);
  t_rmdir("/tmp/m40jail");
}

static void test_ptrace(void) {
  long pid = t_fork();
  if (pid == 0) {
    sys4(SYS_ptrace, 0 /* PTRACE_TRACEME */, 0, 0, 0);
    /* Announce ourselves as traced, then stop for the tracer. */
    sys3(SYS_write, 1, "", 0);
    /* Raise SIGUSR1 (Linux 10) on ourselves — the tracer sees the stop. */
    sys2(SYS_kill, sys0(SYS_getpid), 10);
    sys1(SYS_exit_group, 0);
    for (;;)
      ;
  }
  if (pid < 0) {
    fail("ptrace", pid);
    return;
  }
  int status = 0;
  long w = sys4(SYS_wait4, pid, &status, 2 /* WUNTRACED */, 0);
  int stopped = (w == pid) && ((status & 0xff) == 0x7f);

  /* Read the stopped child's registers: rip must be a plausible userspace
   * address inside the test binary. */
  struct {
    /* x86_64's user_regs_struct is 27 words; aarch64's is 34 (x0..x30, sp, pc,
     * pstate). Size for the larger so the kernel can fill either. */
    unsigned long r[34];
  } regs;
  long gr = sys4(SYS_ptrace, 12 /* PTRACE_GETREGS */, pid, 0, &regs);
#if defined(__aarch64__)
  /* user_regs_struct here is x0..x30, sp, pc, pstate — the program counter is
   * index 32, not x86_64's 16. The blob is linked into the user half (see
   * build-linux-abi-test.sh), so that is the range its pc must fall in. */
  unsigned long rip = regs.r[32];
  int rip_ok = (gr == 0 && rip > 0x500000000000UL && rip < 0x500001000000UL);
#else
  unsigned long rip = regs.r[16];
  int rip_ok = (gr == 0 && rip > 0x200000UL && rip < 0x400000UL);
#endif

  /* Peek the word at the child's rip — it must match this process's own copy
   * of the same code, since both run the same binary image. */
  long peeked = sys4(SYS_ptrace, 2 /* PTRACE_PEEKDATA */, pid, rip, 0);
  int peek_ok = (rip_ok && peeked == *(long *)rip);

  long cont = sys4(SYS_ptrace, 7 /* PTRACE_CONT */, pid, 0, 0);
  int exited = 0;
  for (int i = 0; i < 100; i++) {
    status = 0;
    long r = sys4(SYS_wait4, pid, &status, 0, 0);
    if (r == pid) {
      exited = 1;
      break;
    }
    if (r < 0)
      break;
  }
  check("ptrace-stop", stopped, w);
  check("ptrace-getregs", rip_ok, gr);
  check("ptrace-peek", peek_ok, peeked);
  check("ptrace-cont", cont == 0 && exited, cont);
}

/* Affinity is a real restriction: pin to CPU 0, confirm the mask reads back,
 * and confirm the task keeps running (a pinned task must still be scheduled). */
static void test_affinity(void) {
  unsigned long saved = 0;
  sys3(SYS_sched_getaffinity, 0, sizeof(saved), &saved);

  unsigned long only0 = 1;
  long sr = sys3(SYS_sched_setaffinity, 0, sizeof(only0), &only0);
  unsigned long got = 0;
  long gr = sys3(SYS_sched_getaffinity, 0, sizeof(got), &got);
  /* Still alive and scheduled after pinning: any syscall proves it. */
  long alive = sys0(SYS_getpid);

  unsigned long empty = 0;
  long bad = sys3(SYS_sched_setaffinity, 0, sizeof(empty), &empty);

  check("sched-affinity-pin",
        sr == 0 && gr > 0 && got == 1 && alive > 0 && bad < 0, (long)got);

  if (saved)
    sys3(SYS_sched_setaffinity, 0, sizeof(saved), &saved);
}

/* capset(2) really drops a capability: after dropping CAP_SYS_TIME the
 * privileged operation it guards must start failing, even though we are root. */
static void test_capdrop(void) {
  long pid = t_fork();
  if (pid == 0) {
    unsigned hdr[2] = {0x20080522u, 0}; /* _LINUX_CAPABILITY_VERSION_3 */
    unsigned caps[6] = {0, 0, 0, 0, 0, 0};
    long before = sys2(SYS_capget, hdr, caps);
    /* CAP_SYS_TIME is bit 24 of the low word. Drop just that one. */
    unsigned keep_eff = caps[0] & ~(1u << 24);
    unsigned keep_perm = caps[1] & ~(1u << 24);
    unsigned set[6] = {keep_eff, keep_perm, caps[2],
                       caps[3],  caps[4],   caps[5]};
    long cs = sys2(SYS_capset, hdr, set);

    /* settimeofday needs CAP_SYS_TIME; with it dropped it must report EPERM
     * even for uid 0. */
    struct lx_timeval tv = {1700000000, 0};
    long st = sys2(SYS_settimeofday, &tv, 0);

    /* And the drop must be visible through capget. */
    unsigned after[6] = {0, 0, 0, 0, 0, 0};
    long ag = sys2(SYS_capget, hdr, after);
    int dropped = (ag == 0 && !(after[1] & (1u << 24)));

    int rc = (before == 0 && cs == 0 && st == -1 /* EPERM */ && dropped) ? 0 : 1;
    sys1(SYS_exit_group, rc);
    for (;;)
      ;
  }
  int status = -1;
  long w = sys4(SYS_wait4, pid, &status, 0, 0);
  check("capset-drop", pid > 0 && w == pid && (status & 0xff00) == 0,
        pid > 0 ? (long)status : pid);
}

/* setfsuid(2) changes the id the filesystem checks against, without touching
 * the effective uid. Run it in a child: a root-only file must become
 * unreadable once fsuid is a plain user. */
static void test_setfsuid(void) {
  long fd = t_open("/tmp/m40fsuid", O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd >= 0) {
    sys3(SYS_write, fd, "x", 1);
    sys1(SYS_close, fd);
  }
  long pid = t_fork();
  if (pid == 0) {
    long prev = sys1(SYS_setfsuid, 1000);
    long now = sys1(SYS_setfsuid, 1000); /* returns the previous = 1000 */
    long f = t_open("/tmp/m40fsuid", O_RDONLY, 0);
    if (f >= 0)
      sys1(SYS_close, f);
    /* Back to root, and the file is readable again. */
    sys1(SYS_setfsuid, 0);
    long f2 = t_open("/tmp/m40fsuid", O_RDONLY, 0);
    if (f2 >= 0)
      sys1(SYS_close, f2);
    int rc = (prev == 0 && now == 1000 && f < 0 && f2 >= 0) ? 0 : 1;
    sys1(SYS_exit_group, rc);
    for (;;)
      ;
  }
  int status = -1;
  long w = sys4(SYS_wait4, pid, &status, 0, 0);
  check("setfsuid", pid > 0 && w == pid && (status & 0xff00) == 0,
        pid > 0 ? (long)status : pid);
  sys3(SYS_unlinkat, AT_FDCWD, "/tmp/m40fsuid", 0);
}

/* A tracer that is NOT the tracee's parent must still see its stops. The
 * grandchild is traced by its uncle... concretely: child A forks B, then A
 * attaches to B — A is B's parent, so instead we have the MAIN process attach
 * to a grandchild, whose parent is the intermediate child. */
static void test_ptrace_nonparent(void) {
  int pipefd[2];
  if (sys2(SYS_pipe2, pipefd, 0) < 0) {
    fail("ptrace-nonparent", -1);
    return;
  }
  long child = t_fork();
  if (child == 0) {
    long grand = t_fork();
    if (grand == 0) {
      /* Grandchild: spin until someone stops us, then exit. */
      for (long i = 0; i < 200000000L; i++)
        __asm__ volatile(
#if defined(__aarch64__)
            "yield"
#else
            "pause"
#endif
            );
      sys1(SYS_exit_group, 0);
      for (;;)
        ;
    }
    /* Intermediate: tell the main process the grandchild's pid, then wait. */
    sys3(SYS_write, pipefd[1], &grand, sizeof(grand));
    int st = 0;
    sys4(SYS_wait4, grand, &st, 0, 0);
    sys1(SYS_exit_group, 0);
    for (;;)
      ;
  }
  long grandpid = 0;
  long n = sys3(SYS_read, pipefd[0], &grandpid, sizeof(grandpid));

  /* Attach to a task that is NOT our child and drive it. */
  long att = sys4(SYS_ptrace, 16 /* PTRACE_ATTACH */, grandpid, 0, 0);
  int status = 0;
  long w = att == 0 ? sys4(SYS_wait4, grandpid, &status, 2 /* WUNTRACED */, 0)
                    : -1;
  int stopped = (w == grandpid) && ((status & 0xff) == 0x7f);
  long kill_rc = sys4(SYS_ptrace, 8 /* PTRACE_KILL */, grandpid, 0, 0);

  int cst = 0;
  sys4(SYS_wait4, child, &cst, 0, 0);
  sys1(SYS_close, pipefd[0]);
  sys1(SYS_close, pipefd[1]);
  check("ptrace-nonparent",
        n == (long)sizeof(grandpid) && att == 0 && stopped && kill_rc == 0,
        att);
}

/* chroot keeps a working directory that lies inside the new root, rewritten to
 * the root-relative form. */
static void test_chroot_cwd(void) {
  t_mkdir("/tmp/m40jail2", 0755);
  t_mkdir("/tmp/m40jail2/sub", 0755);
  long pid = t_fork();
  if (pid == 0) {
    long cd = sys1(SYS_chdir, "/tmp/m40jail2/sub");
    long cr = sys1(SYS_chroot, "/tmp/m40jail2");
    char cwd[128];
    long g = sys2(SYS_getcwd, cwd, sizeof(cwd));
    int rc = (cd == 0 && cr == 0 && g >= 0 && seq(cwd, "/sub")) ? 0 : 1;
    sys1(SYS_exit_group, rc);
    for (;;)
      ;
  }
  int status = -1;
  long w = sys4(SYS_wait4, pid, &status, 0, 0);
  check("chroot-keeps-cwd", pid > 0 && w == pid && (status & 0xff00) == 0,
        pid > 0 ? (long)status : pid);
  t_rmdir("/tmp/m40jail2/sub");
  t_rmdir("/tmp/m40jail2");
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
  test_getdents_legacy();
  test_getdents_ino();
  test_pipe_tee_vmsplice();
  test_ioprio();
  test_sysv_sem();
  test_sysv_msg();
  test_file_handles();
  test_rseq();
  test_swap_cycle();
  test_chroot();
  test_ptrace();
  test_ptrace_nonparent();
  test_affinity();
  test_capdrop();
  test_setfsuid();
  test_chroot_cwd();
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
