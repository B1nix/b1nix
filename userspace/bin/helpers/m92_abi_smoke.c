/*
 * M92: b1nix native ABI smoke test — raw syscall verification.
 *
 * Uses raw x86_64 syscall instructions with b1nix syscall numbers
 * to verify the kernel works correctly. No libc dependency.
 */

static void test_all(void);

#if defined(__linux__) && defined(__aarch64__)
/* Linux asm-generic (AArch64) numbers. open/stat/unlink are the *at forms. */
#include <signal.h>
#include <string.h>
#define BX_write          64
#define BX_open           56   /* openat */
#define BX_close          57
#define BX_stat           79   /* newfstatat */
#define BX_unlink         35   /* unlinkat */
#define BX_getpid        172
#define BX_kill          129
#define BX_signal        134   /* rt_sigaction */
#define BX_brk           214
#define BX_mmap          222
#define BX_munmap        215
#define BX_exit           93
#define BX_clock_gettime 113
#define BX_SIGUSR1        SIGUSR1
#elif defined(__linux__)
#include <sys/syscall.h>
#include <signal.h>
#include <string.h>
#define BX_write          __NR_write
#define BX_open           __NR_open
#define BX_close          __NR_close
#define BX_stat           __NR_stat
#define BX_unlink         __NR_unlink
#define BX_getpid         __NR_getpid
#define BX_kill           __NR_kill
#define BX_signal         __NR_rt_sigaction
#define BX_brk            __NR_brk
#define BX_mmap           __NR_mmap
#define BX_munmap         __NR_munmap
#define BX_exit           __NR_exit
#define BX_clock_gettime  __NR_clock_gettime
#define BX_SIGUSR1        SIGUSR1
#else
/* b1nix syscall numbers (kernel/include/b1nix/syscall.h) */
#define BX_write          1
#define BX_open           7
#define BX_close          9
#define BX_stat          11
#define BX_unlink        20
#define BX_getpid        42
#define BX_kill          50
#define BX_signal        51
#define BX_brk           57
#define BX_mmap          58
#define BX_munmap        59
#define BX_exit          60
#define BX_clock_gettime 100
/* b1nix signal numbers (from linux_abi.c lx_signo_to_b1nix_tbl) */
#define BX_SIGUSR1      19
#endif


#ifdef __aarch64__
/* AArch64: number in x8, args x0..x5, trap with `svc #0`, result back in x0.
 * Nothing is clobbered the way x86_64's rcx/r11 are. */
static long sys(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
  register long x8 __asm__("x8") = nr;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  register long x4 __asm__("x4") = a4;
  register long x5 __asm__("x5") = a5;
  __asm__ volatile ("svc #0"
                    : "+r"(x0)
                    : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                    : "memory");
  return x0;
}
#else
static long sys(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
  long ret;
  __asm__ volatile ("syscall" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2),
                    "r"(a3), "r"(a4), "r"(a5) : "rcx", "r11", "memory");
  return ret;
}
#endif

/* open/stat/unlink do not exist on the asm-generic ABI AArch64 uses — it has
 * only the *at forms, which take a directory fd first. Wrap the three so the
 * call sites below stay one shape on both. */
#ifdef __aarch64__
#define RAW_AT_FDCWD (-100)
#define raw_open(path)        sys(BX_open, RAW_AT_FDCWD, (long)(path), 0, 0, 0, 0)
#define raw_stat(path, st)    sys(BX_stat, RAW_AT_FDCWD, (long)(path), (long)(st), 0, 0, 0)
#define raw_unlink(path)      sys(BX_unlink, RAW_AT_FDCWD, (long)(path), 0, 0, 0, 0)
#else
#define raw_open(path)        sys(BX_open, (long)(path), 0, 0, 0, 0, 0)
#define raw_stat(path, st)    sys(BX_stat, (long)(path), (long)(st), 0, 0, 0, 0)
#define raw_unlink(path)      sys(BX_unlink, (long)(path), 0, 0, 0, 0, 0)
#endif

static void raw_write(const char *s, int len) {
  sys(BX_write, 1, (long)s, len, 0, 0, 0);
}

static int ok_count = 0;
static int fail_count = 0;

static int slen(const char *s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

static void ok2(const char *name) {
  ok_count++;
  raw_write("M92-ABI: ok ", 12);
  raw_write(name, slen(name));
  raw_write("\n", 1);
}

static void fail2(const char *name) {
  fail_count++;
  raw_write("M92-ABI: fail ", 14);
  raw_write(name, slen(name));
  raw_write("\n", 1);
}

static void test_brk(void) {
  long old = sys(BX_brk, 0, 0, 0, 0, 0, 0);
  long cur = sys(BX_brk, old + 4096, 0, 0, 0, 0, 0);
  if (cur > old) ok2("brk"); else fail2("brk");
}

static void test_mmap(void) {
  long p = sys(BX_mmap, 0, 4096, 3, 0x22, -1, 0);
  if (p != -1) {
    *(volatile char *)p = 0xAA;
    sys(BX_munmap, p, 4096, 0, 0, 0, 0);
    ok2("mmap");
  } else fail2("mmap");
}

static void test_open(void) {
  long fd = raw_open("/dev/null");
  if (fd >= 0) { sys(BX_close, fd, 0, 0, 0, 0, 0); ok2("open"); }
  else fail2("open");
}

static void test_stat(void) {
  /* Linux struct stat = 144 bytes on x86_64 */
  char st[144] = {0};
  long rc = raw_stat("/dev/null", st);
  if (rc == 0) ok2("stat"); else fail2("stat");
}

static void test_getpid(void) {
  long pid = sys(BX_getpid, 0, 0, 0, 0, 0, 0);
  if (pid > 0) ok2("getpid"); else fail2("getpid");
}

static void test_clock_gettime(void) {
  unsigned long long ts[2] = {0};
  long rc = sys(BX_clock_gettime, 1, (long)ts, 0, 0, 0, 0);
  if (rc == 0) ok2("clock_gettime");
  else { raw_write("clock_gettime rc=", 16); raw_write("fail\n", 5); fail2("clock_gettime"); }
}

static volatile int sig_hit = 0;
static void handler(int s) { (void)s; sig_hit = 1; }

static void test_signal(void) {
#ifdef __linux__
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = handler;
  long rc = sys(BX_signal, BX_SIGUSR1, (long)&act, 0, 8, 0, 0);
#else
  /* b1nix struct sigaction: handler(8) + flags(8) + restorer(8) + mask(8) = 32 bytes */
  unsigned long long act[4] = {0};
  act[0] = (unsigned long long)(void (*)(int))handler;
  long rc = sys(BX_signal, BX_SIGUSR1, (long)act, 0, 8, 0, 0);
#endif
  if (rc == 0) {
    long pid = sys(BX_getpid, 0, 0, 0, 0, 0, 0);
    sys(BX_kill, pid, BX_SIGUSR1, 0, 0, 0, 0);
    if (sig_hit) ok2("signal"); else fail2("signal");
  } else fail2("signal");
}

static void test_unlink(void) {
  const char *path = "/tmp/m92abi.txt";
#ifdef __aarch64__
  long fd = sys(BX_open, RAW_AT_FDCWD, (long)path, 0x241, 0644, 0, 0);
#else
  long fd = sys(BX_open, (long)path, 0x241, 0644, 0, 0, 0);
#endif
  if (fd >= 0) {
    sys(BX_close, fd, 0, 0, 0, 0, 0);
    long rc = raw_unlink(path);
    if (rc == 0) ok2("unlink");
    else { raw_write("unlink rc=", 10); raw_write("fail\n", 5); fail2("unlink"); }
  } else { raw_write("open-for-unlink failed\n", 22); fail2("unlink"); }
}

static void test_all(void) {
  raw_write("M92-ABI: start\n", 15);
  test_brk();
  raw_write("M92-ABI: before mmap\n", 21);
  test_mmap();
  raw_write("M92-ABI: before open\n", 21);
  test_open();
  raw_write("M92-ABI: before stat\n", 21);
  test_stat();
  raw_write("M92-ABI: before getpid\n", 23);
  test_getpid();
  raw_write("M92-ABI: before clock\n", 22);
  test_clock_gettime();
  raw_write("M92-ABI: before signal\n", 23);
  test_signal();
  raw_write("M92-ABI: before unlink\n", 23);
  test_unlink();
  /* Summary */
  raw_write("M92-ABI: done\n", 14);
  sys(BX_exit, fail_count > 0 ? 1 : 0, 0, 0, 0, 0, 0);
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  test_all();
  return 0;
}
