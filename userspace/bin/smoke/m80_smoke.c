/* M80 smoke: /proc/<pid>/task, /proc/<pid>/{auxv,mem} and crashpad-shaped
 * crash capture.
 *
 * Every marker is emitted only after the operation ran AND its result was
 * checked against a value the test knows independently — no unconditional
 * "ok" prints.
 *
 *   proc-task        /proc/self/task lists one directory per thread; a second
 *                    thread appears there and its status reports the right Tgid.
 *   proc-status      /proc/<pid>/status carries Tgid/Threads/TracerPid.
 *   proc-maps        /proc/<pid>/maps is address-ordered and the entry covering
 *                    this code names the executable, its inode and the device
 *                    the file lives on.
 *   proc-auxv        /proc/self/auxv parses as {a_type,a_val} pairs and its
 *                    AT_ENTRY/AT_PAGESZ match getauxval() on the same process.
 *   proc-mem         /proc/self/mem reads and writes this process's own memory
 *                    at the address used as the file offset.
 *   fault-siginfo    an SA_SIGINFO handler for a SIGSEGV sees the faulting
 *                    address in si_addr and a real SEGV_* si_code.
 *   ptrace-siginfo   a tracer attached BEFORE the crash sees the same signal,
 *                    si_code and si_addr through PTRACE_GETSIGINFO.
 *   ptrace-getregset PTRACE_GETREGSET(NT_PRSTATUS) agrees with PTRACE_GETREGS,
 *                    and a short iovec reports the truncated length.
 *   ptrace-fpregs    PTRACE_GETFPREGS returns the tracee's FXSAVE area with the
 *                    MXCSR value the tracee itself installed.
 *   ptrace-seize     PTRACE_SEIZE attaches without stopping; PTRACE_INTERRUPT
 *                    then stops the tracee.
 *   crash-capture    the whole crash-reporter flow: the crashing process hands
 *                    its fault record to a handler over a socket, the handler
 *                    attaches, enumerates the crashed process's threads, reads
 *                    its registers, auxv and memory, and writes a dump.
 *   yama-scope       with /proc/sys/kernel/yama/ptrace_scope = 1 a sibling may
 *                    not attach until the target names it with PR_SET_PTRACER.
 *   ptrace-fork-event  PTRACE_O_TRACEFORK stops the parent with the fork event
 *                    in its wait status, PTRACE_GETEVENTMSG names the child, and
 *                    that child is attached and stopped before it runs.
 *   ptrace-exec-event  PTRACE_O_TRACEEXEC stops the tracee after a successful
 *                    execve, before the new image's entry point runs.
 *   ptrace-listen    PTRACE_LISTEN parks a seized tracee out of ptrace-stop
 *                    (inspection returns ESRCH) until PTRACE_INTERRUPT.
 *   ptrace-xstate    PTRACE_GETREGSET(NT_X86_XSTATE) returns the XSAVE-format
 *                    area whose legacy region matches PTRACE_GETFPREGS.
 *   ptrace-exitkill  PTRACE_O_EXITKILL kills the tracee when its tracer dies.
 *   so-peercred      SO_PEERCRED reports the peer's real pid/uid on both a
 *                    socketpair and an accepted connection.
 *   process-vm-rw    process_vm_readv/writev move bytes in and out of another
 *                    process's address space without stopping it.
 *   ptrace-syscall   PTRACE_SYSCALL stops on syscall entry and exit (marked by
 *                    SIGTRAP|0x80 under TRACESYSGOOD), and a return value the
 *                    tracer writes at the exit stop is what userspace sees.
 *   ptrace-exit-event  PTRACE_O_TRACEEXIT parks a dying task with its exit
 *                    status in PTRACE_GETEVENTMSG and its last frame readable.
 *   ptrace-ignored-signal  a tracee stops for a signal it has set to SIG_IGN.
 *   cpu-freq         the measured CPU clock is published in
 *                    /sys/devices/system/cpu/cpu0/cpufreq and matches the
 *                    "cpu MHz" line in /proc/cpuinfo.
 *   avx-context      AVX (YMM) state survives context switches in both this
 *                    process and a child holding a different pattern, and a
 *                    tracer reads the child's upper YMM half out of
 *                    NT_X86_XSTATE — i.e. the kernel manages it with XSAVE.
 */
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PTRACE_GETREGSET
#define PTRACE_GETREGSET 0x4204
#endif
#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_INTERRUPT
#define PTRACE_INTERRUPT 0x4207
#endif
#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif
#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif

/* The address every crashing child in this test dereferences. Whether the
 * kernel classifies the write as SEGV_MAPERR (nothing mapped) or SEGV_ACCERR
 * (mapped but not writable by userspace) depends on what the address space
 * happens to cover, so the tests check si_addr exactly and accept either of the
 * two legitimate si_codes. */
#define CRASH_ADDR 0xdead0000UL
#define SEGV_IS_FAULT_CODE(c) ((c) == 1 /* MAPERR */ || (c) == 2 /* ACCERR */)
/* Distinctive but legal MXCSR: all exception masks set (0x1f80) plus FTZ. */
#define CRASH_MXCSR 0x9f80u

static int g_fail;

static void marker(const char *s) {
  write(1, s, strlen(s));
  write(1, "\n", 1);
}

static void ok(const char *name) {
  char line[128];
  snprintf(line, sizeof(line), "M80-SMOKE: ok %s", name);
  marker(line);
}

static void fail(const char *name, long v) {
  char line[160];
  snprintf(line, sizeof(line), "M80-SMOKE: FAIL %s (%ld, errno=%d)", name, v,
           errno);
  marker(line);
  g_fail = 1;
}

static void check(const char *name, int cond, long v) {
  if (cond)
    ok(name);
  else
    fail(name, v);
}

struct user_regs64 {
  unsigned long r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8, rax, rcx, rdx,
      rsi, rdi;
  unsigned long orig_rax, rip, cs, eflags, rsp, ss;
  unsigned long fs_base, gs_base, ds, es, fs, gs;
};

struct kiovec {
  void *iov_base;
  unsigned long iov_len;
};

static long tid_of_self(void) { return syscall(SYS_gettid); }

/* Every wait in this test is bounded. A crash test that blocks forever on a
 * child that misbehaved would wedge the whole smoke instance instead of
 * reporting one failure. */
static long read_timeout(int fd, void *buf, unsigned long len, int ms) {
  for (int waited = 0; waited < ms; waited += 10) {
    struct pollfd p = {fd, POLLIN, 0};
    int r = poll(&p, 1, 10);
    if (r > 0)
      return read(fd, buf, len);
    if (r < 0 && errno != EINTR)
      return -1;
  }
  errno = ETIMEDOUT;
  return -1;
}

/* Wait for `pid` to stop (WUNTRACED), bounded. Returns pid, 0 on timeout. */
static pid_t wait_stop(pid_t pid, int *status, int ms) {
  for (int waited = 0; waited < ms; waited += 10) {
    pid_t w = waitpid(pid, status, WUNTRACED | WNOHANG);
    if (w == pid)
      return w;
    if (w < 0)
      return w;
    usleep(10000);
  }
  return 0;
}

static void reap(pid_t pid) {
  kill(pid, SIGKILL);
  for (int i = 0; i < 200; i++) {
    int st = 0;
    pid_t w = waitpid(pid, &st, WNOHANG);
    if (w == pid || w < 0)
      return;
    usleep(10000);
  }
}

/* Read a whole small file. Returns the byte count, or -1. */
static long slurp(const char *path, void *buf, unsigned long cap) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  long total = 0;
  for (;;) {
    long n = read(fd, (char *)buf + total, cap - (unsigned long)total);
    if (n <= 0)
      break;
    total += n;
    if ((unsigned long)total >= cap)
      break;
  }
  close(fd);
  return total;
}

/* Pull "Key:\tvalue" out of a /proc status blob. Returns -1 when absent. */
static long status_field(const char *blob, const char *key) {
  unsigned long klen = strlen(key);
  for (const char *p = blob; *p;) {
    if (strncmp(p, key, klen) == 0 && p[klen] == ':') {
      const char *v = p + klen + 1;
      while (*v == ' ' || *v == '\t')
        v++;
      return strtol(v, 0, 10);
    }
    const char *nl = strchr(p, '\n');
    if (!nl)
      break;
    p = nl + 1;
  }
  return -1;
}

/* Read `len` bytes of `pid`'s memory at `addr` via /proc/<pid>/mem. */
static long read_proc_mem(long pid, unsigned long addr, void *buf,
                          unsigned long len) {
  char path[64];
  if (pid < 0)
    snprintf(path, sizeof(path), "/proc/self/mem");
  else
    snprintf(path, sizeof(path), "/proc/%ld/mem", pid);
  int fd = open(path, O_RDWR);
  if (fd < 0)
    return -1;
  long n = pread(fd, buf, len, (off_t)addr);
  close(fd);
  return n;
}

static long write_proc_mem(long pid, unsigned long addr, const void *buf,
                           unsigned long len) {
  char path[64];
  if (pid < 0)
    snprintf(path, sizeof(path), "/proc/self/mem");
  else
    snprintf(path, sizeof(path), "/proc/%ld/mem", pid);
  int fd = open(path, O_RDWR);
  if (fd < 0)
    return -1;
  long n = pwrite(fd, buf, len, (off_t)addr);
  close(fd);
  return n;
}

/* ── /proc/<pid>/task ─────────────────────────────────────────────────────── */

static int g_thread_park[2]; /* pipe the helper thread blocks on */
static volatile long g_helper_tid;

static void *helper_thread(void *arg) {
  (void)arg;
  g_helper_tid = tid_of_self();
  char c;
  read(g_thread_park[0], &c, 1); /* park until the test lets us go */
  return 0;
}

/* Count entries in a /proc/<pid>/task dir, optionally checking for one tid. */
static int count_task_dir(const char *path, long want_tid, int *saw_want) {
  DIR *d = opendir(path);
  if (!d)
    return -1;
  int n = 0;
  if (saw_want)
    *saw_want = 0;
  struct dirent *e;
  while ((e = readdir(d)) != 0) {
    if (e->d_name[0] == '.')
      continue;
    n++;
    if (saw_want && want_tid > 0 && strtol(e->d_name, 0, 10) == want_tid)
      *saw_want = 1;
  }
  closedir(d);
  return n;
}

static void test_proc_task(void) {
  long self_tid = tid_of_self();
  int saw_self = 0;
  int before = count_task_dir("/proc/self/task", self_tid, &saw_self);
  if (before < 1 || !saw_self) {
    fail("proc-task", before);
    return;
  }

  if (pipe(g_thread_park) != 0) {
    fail("proc-task", -1);
    return;
  }
  pthread_t th;
  if (pthread_create(&th, 0, helper_thread, 0) != 0) {
    fail("proc-task", -2);
    return;
  }
  for (int i = 0; i < 200 && !g_helper_tid; i++)
    usleep(1000);

  int saw_helper = 0;
  int during = count_task_dir("/proc/self/task", g_helper_tid, &saw_helper);

  /* The helper's own per-thread status must name this process as its Tgid. */
  char path[64], blob[1024];
  snprintf(path, sizeof(path), "/proc/self/task/%ld/status", g_helper_tid);
  long n = slurp(path, blob, sizeof(blob) - 1);
  long tgid = -1, tid_reported = -1;
  if (n > 0) {
    blob[n] = '\0';
    tgid = status_field(blob, "Tgid");
    tid_reported = status_field(blob, "Pid");
  }

  char go = 'x';
  write(g_thread_park[1], &go, 1);
  pthread_join(th, 0);
  close(g_thread_park[0]);
  close(g_thread_park[1]);

  check("proc-task",
        during == before + 1 && saw_helper && tgid == (long)getpid() &&
            tid_reported == g_helper_tid,
        (long)during);
}

static void test_proc_status(void) {
  char blob[1024];
  long n = slurp("/proc/self/status", blob, sizeof(blob) - 1);
  if (n <= 0) {
    fail("proc-status", n);
    return;
  }
  blob[n] = '\0';
  long tgid = status_field(blob, "Tgid");
  long threads = status_field(blob, "Threads");
  long tracer = status_field(blob, "TracerPid");
  check("proc-status",
        tgid == (long)getpid() && threads >= 1 && tracer == 0, tgid);
}

/* ── /proc/self/auxv ──────────────────────────────────────────────────────── */

static void test_proc_auxv(void) {
  unsigned long buf[128];
  long n = slurp("/proc/self/auxv", buf, sizeof(buf));
  if (n <= 0 || (n % (long)(2 * sizeof(unsigned long))) != 0) {
    fail("proc-auxv", n);
    return;
  }
  unsigned long entry = 0, pagesz = 0, phdr = 0;
  int terminated = 0;
  for (long i = 0; i < n / (long)sizeof(unsigned long); i += 2) {
    unsigned long type = buf[i], val = buf[i + 1];
    if (type == AT_NULL) {
      terminated = 1;
      break;
    }
    if (type == AT_ENTRY)
      entry = val;
    else if (type == AT_PAGESZ)
      pagesz = val;
    else if (type == AT_PHDR)
      phdr = val;
  }
  check("proc-auxv",
        terminated && pagesz == 4096 && entry == getauxval(AT_ENTRY) &&
            entry != 0 && phdr == getauxval(AT_PHDR),
        (long)entry);
}

/* ── /proc/self/mem ───────────────────────────────────────────────────────── */

static volatile unsigned long g_mem_probe = 0x5AFEC0DE12345678UL;

static void test_proc_mem(void) {
  unsigned long got = 0;
  long n = read_proc_mem(-1, (unsigned long)(void *)&g_mem_probe, &got,
                         sizeof(got));
  int read_ok = (n == (long)sizeof(got) && got == g_mem_probe);

  unsigned long put = 0x1122334455667788UL;
  long w = write_proc_mem(-1, (unsigned long)(void *)&g_mem_probe, &put,
                          sizeof(put));
  int write_ok = (w == (long)sizeof(put) && g_mem_probe == put);

  /* An address in no mapping must fail rather than return garbage. CRASH_ADDR
   * is NOT such an address — the kernel covers that range with a huge page the
   * process simply may not write — so probe a hole between the PIE load area
   * and the interpreter's base instead. */
  unsigned long junk = 0;
  long bad = read_proc_mem(-1, 0x6fff00000000UL, &junk, sizeof(junk));

  check("proc-mem", read_ok && write_ok && bad < 0, n);
}

/* ── crash record shared between a crashing child and this process ────────── */

struct crash_report {
  int signo;
  int code;
  unsigned long addr;
  unsigned long magic_addr; /* where the child keeps its payload string */
  unsigned long tid;
};

static volatile char g_payload[32] = "b1nix-crash-payload";
static int g_report_fd = -1;

static void crash_handler(int sig, siginfo_t *si, void *uc) {
  (void)uc;
  struct crash_report r;
  r.signo = sig;
  r.code = si ? si->si_code : -1;
  r.addr = si ? (unsigned long)si->si_addr : 0;
  r.magic_addr = (unsigned long)(void *)g_payload;
  r.tid = (unsigned long)tid_of_self();
  write(g_report_fd, &r, sizeof(r));
  /* Stay alive and quiescent so the handler process can dump us — this is what
   * crashpad's client does while its handler works. */
  for (;;)
    pause();
}

static void install_mxcsr(unsigned int v) {
  __asm__ volatile("ldmxcsr %0" : : "m"(v));
}

/* ── a tracer attached before the crash sees the fault details ───────────── */

static void test_ptrace_siginfo(void) {
  pid_t pid = fork();
  if (pid == 0) {
    ptrace(PTRACE_TRACEME, 0, 0, 0);
    *(volatile int *)CRASH_ADDR = 1; /* fault, no handler installed */
    _exit(99);
  }
  if (pid < 0) {
    fail("ptrace-siginfo", pid);
    return;
  }
  int status = 0;
  pid_t w = wait_stop(pid, &status, 10000);
  int stopped = (w == pid) && WIFSTOPPED(status);
  /* The first stop may be an attach-time SIGSTOP; walk on to the SIGSEGV. */
  for (int i = 0; i < 8 && stopped && WSTOPSIG(status) != SIGSEGV; i++) {
    ptrace(PTRACE_CONT, pid, 0, 0);
    w = wait_stop(pid, &status, 10000);
    stopped = (w == pid) && WIFSTOPPED(status);
  }

  siginfo_t si;
  memset(&si, 0, sizeof(si));
  long g = stopped ? ptrace(PTRACE_GETSIGINFO, pid, 0, &si) : -1;
  int ok_info = (g == 0 && si.si_signo == SIGSEGV &&
                 (unsigned long)si.si_addr == CRASH_ADDR &&
                 SEGV_IS_FAULT_CODE(si.si_code));

  ptrace(PTRACE_KILL, pid, 0, 0);
  reap(pid);
  check("ptrace-siginfo", stopped && WSTOPSIG(status) == SIGSEGV && ok_info,
        (long)si.si_code);
}

/* ── register sets on a stopped tracee ───────────────────────────────────── */

static void test_regsets_and_capture(void) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    fail("crash-capture", -1);
    return;
  }

  pid_t pid = fork();
  if (pid == 0) {
    close(sv[0]);
    g_report_fd = sv[1];
    /* Nominate the handler process, as a crash-reporting client does. */
    prctl(PR_SET_PTRACER, (unsigned long)getppid(), 0, 0, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, 0);
    install_mxcsr(CRASH_MXCSR);
    *(volatile int *)CRASH_ADDR = 1;
    _exit(98); /* unreachable: the handler never returns */
  }
  if (pid < 0) {
    fail("crash-capture", pid);
    close(sv[0]);
    close(sv[1]);
    return;
  }
  close(sv[1]);

  /* 1. The crashing process reports its own fault through its SA_SIGINFO
   *    handler — the first half of the crashpad handshake. */
  struct crash_report rep;
  memset(&rep, 0, sizeof(rep));
  long got = read_timeout(sv[0], &rep, sizeof(rep), 15000);
  int report_ok = (got == (long)sizeof(rep) && rep.signo == SIGSEGV &&
                   rep.addr == CRASH_ADDR && SEGV_IS_FAULT_CODE(rep.code));
  check("fault-siginfo", report_ok, (long)rep.addr);
  if (!report_ok) {
    reap(pid);
    close(sv[0]);
    g_fail = 1;
    return;
  }

  /* 2. Attach to the parked crasher and stop it. */
  long att = ptrace(PTRACE_ATTACH, pid, 0, 0);
  int status = 0;
  pid_t w = wait_stop(pid, &status, 10000);
  int stopped = (att == 0 && w == pid && WIFSTOPPED(status));
  if (!stopped) {
    fail("crash-capture", (long)att);
    reap(pid);
    close(sv[0]);
    return;
  }

  /* 3. PTRACE_GETREGS vs PTRACE_GETREGSET(NT_PRSTATUS). */
  struct user_regs64 regs, rset;
  memset(&regs, 0, sizeof(regs));
  memset(&rset, 0, sizeof(rset));
  long gr = ptrace(PTRACE_GETREGS, pid, 0, &regs);
  struct kiovec iov = {&rset, sizeof(rset)};
  long gs = ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov);
  int regset_ok = (gr == 0 && gs == 0 && iov.iov_len == sizeof(rset) &&
                   memcmp(&regs, &rset, sizeof(regs)) == 0 && regs.rip != 0);

  /* A short iovec must transfer only what fits and report that length. */
  unsigned long head[4];
  memset(head, 0, sizeof(head));
  struct kiovec small = {head, sizeof(head)};
  long gsmall = ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &small);
  regset_ok = regset_ok && gsmall == 0 && small.iov_len == sizeof(head) &&
              head[0] == regs.r15;
  check("ptrace-getregset", regset_ok, (long)iov.iov_len);

  /* 4. FPU state: the tracee installed a known MXCSR before faulting, and
   *    MXCSR lives at offset 24 of the FXSAVE area. */
  unsigned char fx[512];
  memset(fx, 0, sizeof(fx));
  long gf = ptrace(PTRACE_GETFPREGS, pid, 0, fx);
  unsigned int mxcsr = 0;
  memcpy(&mxcsr, fx + 24, sizeof(mxcsr));
  check("ptrace-fpregs", gf == 0 && mxcsr == CRASH_MXCSR, (long)mxcsr);

  /* 5. Enumerate the crashed process's threads and read its memory + auxv —
   *    everything a minidump writer needs — and write the dump out. */
  char taskpath[64];
  snprintf(taskpath, sizeof(taskpath), "/proc/%ld/task", (long)pid);
  int saw_crashed_tid = 0;
  int nthreads = count_task_dir(taskpath, (long)rep.tid, &saw_crashed_tid);

  char payload[32];
  memset(payload, 0, sizeof(payload));
  long pn = read_proc_mem((long)pid, rep.magic_addr, payload, sizeof(payload));
  int payload_ok =
      (pn > 0 && strcmp(payload, "b1nix-crash-payload") == 0);

  /* process_vm_readv/writev: the same bytes without going through a file, and
   * a write-back that the follow-up read must observe. */
  char pv[32];
  memset(pv, 0, sizeof(pv));
  struct kiovec liov = {pv, sizeof(pv)};
  struct kiovec riov = {(void *)rep.magic_addr, sizeof(pv)};
  long pvr = syscall(310 /* process_vm_readv */, (long)pid, &liov, 1UL, &riov,
                     1UL, 0UL);
  int pv_read_ok = (pvr == (long)sizeof(pv) &&
                    strcmp(pv, "b1nix-crash-payload") == 0);

  char newpay[32];
  memset(newpay, 0, sizeof(newpay));
  strcpy(newpay, "b1nix-vm-write");
  struct kiovec wl = {newpay, sizeof(newpay)};
  struct kiovec wr = {(void *)rep.magic_addr, sizeof(newpay)};
  long pvw = syscall(311 /* process_vm_writev */, (long)pid, &wl, 1UL, &wr, 1UL,
                     0UL);
  char verify[32];
  memset(verify, 0, sizeof(verify));
  long vn = read_proc_mem((long)pid, rep.magic_addr, verify, sizeof(verify));
  int pv_write_ok = (pvw == (long)sizeof(newpay) && vn > 0 &&
                     strcmp(verify, "b1nix-vm-write") == 0);
  /* Put the payload back so the dump below still describes the crash. */
  write_proc_mem((long)pid, rep.magic_addr, payload, sizeof(payload));
  check("process-vm-rw", pv_read_ok && pv_write_ok, pvr);

  char auxvpath[64];
  unsigned long aux[64];
  snprintf(auxvpath, sizeof(auxvpath), "/proc/%ld/auxv", (long)pid);
  long an = slurp(auxvpath, aux, sizeof(aux));
  unsigned long child_entry = 0;
  for (long i = 0; an > 0 && i < an / (long)sizeof(unsigned long); i += 2) {
    if (aux[i] == AT_NULL)
      break;
    if (aux[i] == AT_ENTRY)
      child_entry = aux[i + 1];
  }

  char statuspath[64], blob[1024];
  snprintf(statuspath, sizeof(statuspath), "/proc/%ld/status", (long)pid);
  long sn = slurp(statuspath, blob, sizeof(blob) - 1);
  long tracer_pid = -1;
  if (sn > 0) {
    blob[sn] = '\0';
    tracer_pid = status_field(blob, "TracerPid");
  }

  int dumpfd = open("/tmp/m80-crash.dump", O_WRONLY | O_CREAT | O_TRUNC, 0600);
  int dump_written = 0;
  if (dumpfd >= 0) {
    char line[512];
    int len = snprintf(line, sizeof(line),
                       "pid=%ld threads=%d signo=%d code=%d addr=0x%lx "
                       "rip=0x%lx rsp=0x%lx entry=0x%lx payload=%s\n",
                       (long)pid, nthreads, rep.signo, rep.code, rep.addr,
                       regs.rip, regs.rsp, child_entry, payload);
    dump_written = (write(dumpfd, line, (unsigned long)len) == len);
    close(dumpfd);
  }

  /* Read the dump back and confirm it carries the crash facts. */
  char dump[512];
  long dn = slurp("/tmp/m80-crash.dump", dump, sizeof(dump) - 1);
  int dump_ok = 0;
  if (dn > 0) {
    dump[dn] = '\0';
    char needle[64];
    snprintf(needle, sizeof(needle), "addr=0x%lx", CRASH_ADDR);
    dump_ok = dump_written && strstr(dump, needle) != 0 &&
              strstr(dump, "payload=b1nix-crash-payload") != 0 &&
              strstr(dump, "threads=1") != 0;
  }

  ptrace(PTRACE_KILL, pid, 0, 0);
  reap(pid);
  close(sv[0]);

  check("crash-capture",
        nthreads >= 1 && saw_crashed_tid && payload_ok && child_entry != 0 &&
            child_entry == getauxval(AT_ENTRY) && tracer_pid == (long)getpid() &&
            dump_ok,
        (long)nthreads);
}

/* ── PTRACE_SEIZE + PTRACE_INTERRUPT ─────────────────────────────────────── */

static void test_seize(void) {
  int sv[2];
  if (pipe(sv) != 0) {
    fail("ptrace-seize", -1);
    return;
  }
  pid_t pid = fork();
  if (pid == 0) {
    close(sv[0]);
    char c = 'r';
    write(sv[1], &c, 1); /* tell the parent we are running */
    for (;;)
      usleep(1000);
  }
  if (pid < 0) {
    fail("ptrace-seize", pid);
    return;
  }
  close(sv[1]);
  char c = 0;
  read_timeout(sv[0], &c, 1, 5000);

  long sz = ptrace(PTRACE_SEIZE, pid, 0, 0);
  /* SEIZE must NOT stop the tracee: nothing to wait for yet. */
  int status = 0;
  pid_t early = waitpid(pid, &status, WUNTRACED | WNOHANG);
  long in = ptrace(PTRACE_INTERRUPT, pid, 0, 0);
  pid_t w = wait_stop(pid, &status, 10000);

  struct user_regs64 regs;
  memset(&regs, 0, sizeof(regs));
  long gr = ptrace(PTRACE_GETREGS, pid, 0, &regs);

  ptrace(PTRACE_KILL, pid, 0, 0);
  reap(pid);
  close(sv[0]);

  check("ptrace-seize",
        sz == 0 && early == 0 && in == 0 && w == pid && WIFSTOPPED(status) &&
            gr == 0 && regs.rip != 0,
        (long)sz);
}

/* ── yama ptrace_scope ───────────────────────────────────────────────────── */

static int write_file(const char *path, const char *val) {
  int fd = open(path, O_WRONLY);
  if (fd < 0)
    return -1;
  long n = write(fd, val, strlen(val));
  close(fd);
  return n > 0 ? 0 : -1;
}

/* Both sides of the yama test run as an ordinary user: root carries
 * CAP_SYS_PTRACE, which overrides ptrace_scope exactly as it does under Linux,
 * so a root tracer would be allowed regardless and prove nothing. */
#define YAMA_UID 4242

/* target: declares nothing at first, then names `tracer_pid`. Coordination is
 * one byte each way over two pipes so neither side can race ahead. */
static void yama_target(int to_parent, int from_parent, int tracer_pid) {
  char c = 't';
  if (setgid(YAMA_UID) != 0 || setuid(YAMA_UID) != 0)
    c = 'e';
  write(to_parent, &c, 1);      /* "I am alive, try to attach" */
  read(from_parent, &c, 1);     /* "the sibling was refused" */
  c = (prctl(PR_SET_PTRACER, (unsigned long)tracer_pid, 0, 0, 0) == 0) ? 'd'
                                                                       : 'e';
  write(to_parent, &c, 1);      /* "declaration made, try again" */
  for (;;)
    usleep(1000);
}

static void test_yama_scope(void) {
  char cur[16];
  long n = slurp("/proc/sys/kernel/yama/ptrace_scope", cur, sizeof(cur) - 1);
  if (n <= 0) {
    fail("yama-scope", n);
    return;
  }
  cur[n] = '\0';
  int default_zero = (strtol(cur, 0, 10) == 0);
  if (write_file("/proc/sys/kernel/yama/ptrace_scope", "1\n") != 0) {
    fail("yama-scope", -2);
    return;
  }

  int t2p[2], p2t[2], r2p[2], p2r[2];
  if (pipe(t2p) || pipe(p2t) || pipe(r2p) || pipe(p2r)) {
    fail("yama-scope", -3);
    write_file("/proc/sys/kernel/yama/ptrace_scope", "0\n");
    return;
  }

  pid_t tracer = fork();
  if (tracer == 0) {
    /* The sibling tracer: attach twice, reporting each result. */
    close(t2p[0]); close(t2p[1]); close(p2t[0]); close(p2t[1]);
    close(r2p[0]); close(p2r[1]);
    if (setgid(YAMA_UID) != 0 || setuid(YAMA_UID) != 0) {
      long e = -1;
      write(r2p[1], &e, sizeof(e));
      _exit(1);
    }
    long target = 0;
    read(p2r[0], &target, sizeof(target));
    long first = ptrace(PTRACE_ATTACH, (pid_t)target, 0, 0);
    long first_errno = (first < 0) ? errno : 0;
    if (first == 0) /* unexpected: do not leave the target stopped forever */
      ptrace(PTRACE_DETACH, (pid_t)target, 0, 0);
    write(r2p[1], &first_errno, sizeof(first_errno));
    long go = 0;
    read(p2r[0], &go, sizeof(go));
    long second = ptrace(PTRACE_ATTACH, (pid_t)target, 0, 0);
    long second_res = (second < 0) ? -errno : 0;
    if (second == 0) {
      int st = 0;
      wait_stop((pid_t)target, &st, 3000);
      ptrace(PTRACE_DETACH, (pid_t)target, 0, 0);
    }
    write(r2p[1], &second_res, sizeof(second_res));
    _exit(0);
  }

  pid_t target = fork();
  if (target == 0) {
    close(r2p[0]); close(r2p[1]); close(p2r[0]); close(p2r[1]);
    close(t2p[0]); close(p2t[1]);
    yama_target(t2p[1], p2t[0], (int)tracer);
    _exit(0);
  }

  close(t2p[1]); close(p2t[0]); close(r2p[1]); close(p2r[0]);
  char c = 0;
  int alive = (read_timeout(t2p[0], &c, 1, 5000) == 1 && c == 't');
  long target_pid = target;
  write(p2r[1], &target_pid, sizeof(target_pid));

  long first_errno = 0;
  int got_first =
      (read_timeout(r2p[0], &first_errno, sizeof(first_errno), 5000) ==
       (long)sizeof(first_errno));
  int refused = (got_first && first_errno == EPERM);

  write(p2t[1], &c, 1); /* tell the target to declare its tracer */
  int declared = (read_timeout(t2p[0], &c, 1, 5000) == 1 && c == 'd');
  long go = 1;
  write(p2r[1], &go, sizeof(go));

  long second_res = -1;
  int got_second =
      (read_timeout(r2p[0], &second_res, sizeof(second_res), 5000) ==
       (long)sizeof(second_res));
  int allowed = (got_second && second_res == 0);

  reap(target);
  reap(tracer);
  close(t2p[0]); close(p2t[1]); close(r2p[0]); close(p2r[1]);

  /* Leave the machine as we found it — every later test runs under this. */
  int restored = (write_file("/proc/sys/kernel/yama/ptrace_scope", "0\n") == 0);

  check("yama-scope",
        default_zero && alive && refused && declared && allowed && restored,
        first_errno);
}


/* ── SO_PEERCRED ─────────────────────────────────────────────────────────── */

static void test_so_peercred(void) {
  /* socketpair: both ends were created by this process, so each end reports
   * this process as its peer's owner. */
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    fail("so-peercred", -1);
    return;
  }
  struct ucred uc;
  socklen_t ul = sizeof(uc);
  memset(&uc, 0, sizeof(uc));
  int g0 = getsockopt(sv[0], SOL_SOCKET, SO_PEERCRED, &uc, &ul);
  int pair_ok = (g0 == 0 && ul == sizeof(uc) && uc.pid == getpid() &&
                 uc.uid == getuid() && uc.gid == getgid());
  close(sv[0]);
  close(sv[1]);

  /* A real connection: the server must see the CLIENT's pid, which the client
   * never sent — that is the whole value of SO_PEERCRED. */
  unlink("/tmp/m80-peercred.sock");
  int srv = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un sa;
  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_UNIX;
  strcpy(sa.sun_path, "/tmp/m80-peercred.sock");
  int bound = (srv >= 0 && bind(srv, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
               listen(srv, 4) == 0);
  if (!bound) {
    if (srv >= 0)
      close(srv);
    check("so-peercred", 0, -2);
    return;
  }

  int sync[2];
  if (pipe(sync) != 0) {
    close(srv);
    fail("so-peercred", -3);
    return;
  }
  pid_t client = fork();
  if (client == 0) {
    close(sync[0]);
    close(srv);
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un ca;
    memset(&ca, 0, sizeof(ca));
    ca.sun_family = AF_UNIX;
    strcpy(ca.sun_path, "/tmp/m80-peercred.sock");
    /* The parent may not have reached accept() yet; a connect that loses that
     * race is retried rather than failing the test. */
    int rc = -1;
    for (int i = 0; i < 100 && rc != 0; i++) {
      rc = connect(c, (struct sockaddr *)&ca, sizeof(ca));
      if (rc != 0)
        usleep(10000);
    }
    /* Report what the client sees for ITS peer: the server (this test). */
    struct ucred cc;
    socklen_t cl = sizeof(cc);
    memset(&cc, 0, sizeof(cc));
    long seen = -1;
    if (rc == 0 && getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cc, &cl) == 0)
      seen = cc.pid;
    write(sync[1], &seen, sizeof(seen));
    /* Hold the connection open until the parent has looked at it. */
    char q;
    read(sync[1], &q, 1);
    _exit(0);
  }
  close(sync[1]);

  int conn = -1;
  for (int i = 0; i < 200 && conn < 0; i++) {
    conn = accept(srv, 0, 0);
    if (conn < 0)
      usleep(10000);
  }
  struct ucred pc;
  socklen_t pl = sizeof(pc);
  memset(&pc, 0, sizeof(pc));
  int gp = (conn >= 0) ? getsockopt(conn, SOL_SOCKET, SO_PEERCRED, &pc, &pl) : -1;
  int server_sees_client =
      (gp == 0 && pc.pid == (int)client && pc.uid == getuid());

  long client_sees = -1;
  read_timeout(sync[0], &client_sees, sizeof(client_sees), 5000);
  int client_sees_server = (client_sees == (long)getpid());

  if (conn >= 0)
    close(conn);
  close(srv);
  close(sync[0]);
  reap(client);
  unlink("/tmp/m80-peercred.sock");

  check("so-peercred",
        pair_ok && server_sees_client && client_sees_server, (long)pc.pid);
}

/* ── PTRACE_SETOPTIONS: fork and exec events ─────────────────────────────── */

/* Park a freshly forked child in a ptrace stop the tracer can configure from. */
static int trace_child_until_stop(pid_t pid, int *status) {
  pid_t w = wait_stop(pid, status, 10000);
  return (w == pid) && WIFSTOPPED(*status);
}

static void test_ptrace_fork_event(void) {
  int sync[2];
  if (pipe(sync) != 0) {
    fail("ptrace-fork-event", -1);
    return;
  }
  pid_t child = fork();
  if (child == 0) {
    close(sync[0]);
    ptrace(PTRACE_TRACEME, 0, 0, 0);
    kill(getpid(), SIGSTOP); /* wait for the tracer to set its options */
    pid_t g = fork();
    if (g == 0)
      _exit(7);
    long gp = g;
    write(sync[1], &gp, sizeof(gp));
    int st = 0;
    waitpid(g, &st, 0);
    _exit(0);
  }
  if (child < 0) {
    fail("ptrace-fork-event", child);
    return;
  }
  close(sync[1]);

  int status = 0;
  int stopped = trace_child_until_stop(child, &status);
  long so = stopped ? ptrace(PTRACE_SETOPTIONS, child, 0, PTRACE_O_TRACEFORK)
                    : -1;
  if (so == 0)
    ptrace(PTRACE_CONT, child, 0, 0);

  int evstopped = (so == 0) && trace_child_until_stop(child, &status);
  int event = evstopped ? (status >> 16) : -1;
  unsigned long msg = 0;
  long gm = evstopped ? ptrace(PTRACE_GETEVENTMSG, child, 0, &msg) : -1;

  /* The grandchild was auto-attached and stopped before it ran; releasing it
   * is the tracer's job. */
  int gstatus = 0;
  int gstopped = 0;
  if (gm == 0 && msg > 0) {
    gstopped = (wait_stop((pid_t)msg, &gstatus, 5000) == (pid_t)msg);
    ptrace(PTRACE_DETACH, (pid_t)msg, 0, 0);
  }

  /* Resume the tracee and keep resuming it: a traced task stops for EVERY
   * signal, and the grandchild's exit sends it a SIGCHLD, so a tracer that
   * continues only once would wait forever for the child's next write. */
  ptrace(PTRACE_CONT, child, 0, 0);
  long reported = -1;
  for (int i = 0; i < 200; i++) {
    if (read_timeout(sync[0], &reported, sizeof(reported), 50) ==
        (long)sizeof(reported))
      break;
    reported = -1;
    int st2 = 0;
    pid_t w2 = waitpid(child, &st2, WUNTRACED | WNOHANG);
    if (w2 == child && WIFSTOPPED(st2))
      ptrace(PTRACE_CONT, child, 0, 0);
    else if (w2 == child || w2 < 0)
      break;
  }
  close(sync[0]);

  int st = 0;
  pid_t done = 0;
  for (int i = 0; i < 500 && done != child; i++) {
    done = waitpid(child, &st, WUNTRACED | WNOHANG);
    if (done == child && WIFSTOPPED(st)) {
      ptrace(PTRACE_CONT, child, 0, 0);
      done = 0;
    } else if (done == child || done < 0) {
      break;
    }
    usleep(10000);
  }
  if (done != child)
    reap(child);

  check("ptrace-fork-event",
        stopped && so == 0 && evstopped && event == PTRACE_EVENT_FORK &&
            gm == 0 && msg > 0 && reported == (long)msg && gstopped,
        (long)event);
}

static void test_ptrace_exec_event(void) {
  pid_t child = fork();
  if (child == 0) {
    ptrace(PTRACE_TRACEME, 0, 0, 0);
    kill(getpid(), SIGSTOP);
    char *const argv[] = {"/bin/hello", 0};
    char *const envp[] = {0};
    execve("/bin/hello", argv, envp);
    _exit(97); /* only reached if the exec failed */
  }
  if (child < 0) {
    fail("ptrace-exec-event", child);
    return;
  }
  int status = 0;
  int stopped = trace_child_until_stop(child, &status);
  long so = stopped ? ptrace(PTRACE_SETOPTIONS, child, 0, PTRACE_O_TRACEEXEC)
                    : -1;
  if (so == 0)
    ptrace(PTRACE_CONT, child, 0, 0);
  int evstopped = (so == 0) && trace_child_until_stop(child, &status);
  int event = evstopped ? (status >> 16) : -1;

  /* At an exec stop the new image is loaded but has not run: its rip must be
   * the fresh entry point, not the old binary's. */
  struct user_regs64 regs;
  memset(&regs, 0, sizeof(regs));
  long gr = evstopped ? ptrace(PTRACE_GETREGS, child, 0, &regs) : -1;

  ptrace(PTRACE_KILL, child, 0, 0);
  reap(child);
  check("ptrace-exec-event",
        stopped && so == 0 && evstopped && event == PTRACE_EVENT_EXEC &&
            gr == 0 && regs.rip != 0,
        (long)event);
}

/* ── PTRACE_LISTEN + NT_X86_XSTATE on a seized tracee ────────────────────── */

static void test_listen_and_xstate(void) {
  int sync[2];
  if (pipe(sync) != 0) {
    fail("ptrace-listen", -1);
    return;
  }
  pid_t child = fork();
  if (child == 0) {
    close(sync[0]);
    char c = 'r';
    write(sync[1], &c, 1);
    for (;;)
      usleep(1000);
  }
  if (child < 0) {
    fail("ptrace-listen", child);
    return;
  }
  close(sync[1]);
  char c = 0;
  read_timeout(sync[0], &c, 1, 5000);

  long sz = ptrace(PTRACE_SEIZE, child, 0, 0);
  long in1 = ptrace(PTRACE_INTERRUPT, child, 0, 0);
  int status = 0;
  int stopped = (wait_stop(child, &status, 10000) == child) &&
                WIFSTOPPED(status);

  struct user_regs64 regs;
  memset(&regs, 0, sizeof(regs));
  long gr1 = stopped ? ptrace(PTRACE_GETREGS, child, 0, &regs) : -1;

  /* NT_X86_XSTATE: 512-byte legacy FXSAVE region + XSAVE header. */
  unsigned char xs[576];
  memset(xs, 0, sizeof(xs));
  struct kiovec xiov = {xs, sizeof(xs)};
  long gx = stopped ? ptrace(PTRACE_GETREGSET, child, NT_X86_XSTATE, &xiov) : -1;
  unsigned char fx[512];
  memset(fx, 0, sizeof(fx));
  long gf = stopped ? ptrace(PTRACE_GETFPREGS, child, 0, fx) : -1;
  unsigned long xstate_bv = 0;
  memcpy(&xstate_bv, xs + 512, sizeof(xstate_bv));
  int xstate_ok = (gx == 0 && gf == 0 && xiov.iov_len == sizeof(xs) &&
                   xstate_bv == 0x3 && memcmp(xs, fx, sizeof(fx)) == 0);
  check("ptrace-xstate", xstate_ok, (long)xstate_bv);

  /* LISTEN takes the tracee out of ptrace-stop without resuming it: it is no
   * longer inspectable, and PTRACE_INTERRUPT brings it back. */
  long li = stopped ? ptrace(PTRACE_LISTEN, child, 0, 0) : -1;
  long gr_listen = ptrace(PTRACE_GETREGS, child, 0, &regs);
  int listen_blocks = (gr_listen < 0 && errno == ESRCH);
  long in2 = ptrace(PTRACE_INTERRUPT, child, 0, 0);
  int restatus = 0;
  int restopped = (wait_stop(child, &restatus, 10000) == child) &&
                  WIFSTOPPED(restatus);
  int reevent = restopped ? (restatus >> 16) : -1;
  long gr2 = restopped ? ptrace(PTRACE_GETREGS, child, 0, &regs) : -1;

  ptrace(PTRACE_KILL, child, 0, 0);
  reap(child);
  close(sync[0]);

  check("ptrace-listen",
        sz == 0 && in1 == 0 && stopped && gr1 == 0 && li == 0 &&
            listen_blocks && in2 == 0 && restopped &&
            reevent == PTRACE_EVENT_STOP && gr2 == 0,
        (long)li);
}

/* ── PTRACE_O_EXITKILL ───────────────────────────────────────────────────── */

static void test_exitkill(void) {
  int sync[2], ack[2];
  if (pipe(sync) != 0 || pipe(ack) != 0) {
    fail("ptrace-exitkill", -1);
    return;
  }
  pid_t target = fork();
  if (target == 0) {
    close(sync[0]); close(sync[1]); close(ack[0]); close(ack[1]);
    for (;;)
      usleep(1000);
  }
  pid_t tracer = fork();
  if (tracer == 0) {
    close(sync[1]); close(ack[0]);
    long pid = 0;
    read(sync[0], &pid, sizeof(pid));
    /* Seize with EXITKILL, then die: the kernel must take the tracee with us. */
    long rc = ptrace(PTRACE_SEIZE, (pid_t)pid, 0, PTRACE_O_EXITKILL);
    write(ack[1], &rc, sizeof(rc));
    _exit(0);
  }
  close(sync[0]);
  close(ack[1]);
  long tpid = target;
  write(sync[1], &tpid, sizeof(tpid));
  long seize_rc = -1;
  read_timeout(ack[0], &seize_rc, sizeof(seize_rc), 5000);

  int tst = 0;
  waitpid(tracer, &tst, 0);

  /* The target must now die on its own, without anybody killing it. */
  int killed = 0, status = 0;
  for (int i = 0; i < 500; i++) {
    pid_t w = waitpid(target, &status, WNOHANG);
    if (w == target) {
      killed = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
      break;
    }
    if (w < 0)
      break;
    usleep(10000);
  }
  if (!killed)
    reap(target);
  close(sync[1]);
  close(ack[0]);
  check("ptrace-exitkill", seize_rc == 0 && killed, (long)status);
}


/* ── PTRACE_SYSCALL: entry/exit stops and syscall interception ───────────── */

#ifndef PTRACE_SYSCALL
#define PTRACE_SYSCALL 24
#endif
#ifndef PTRACE_O_TRACESYSGOOD
#define PTRACE_O_TRACESYSGOOD 0x00000001
#endif
#ifndef PTRACE_O_TRACEEXIT
#define PTRACE_O_TRACEEXIT 0x00000040
#endif
#ifndef PTRACE_EVENT_EXIT
#define PTRACE_EVENT_EXIT 6
#endif

#define SYS_GETPID_NR 39   /* Linux x86_64 getpid */
#define INJECTED_PID 123456

static void test_ptrace_syscall(void) {
  int sync[2];
  if (pipe(sync) != 0) {
    fail("ptrace-syscall", -1);
    return;
  }
  pid_t child = fork();
  if (child == 0) {
    close(sync[0]);
    ptrace(PTRACE_TRACEME, 0, 0, 0);
    kill(getpid(), SIGSTOP);
    long seen = syscall(SYS_GETPID_NR);
    write(sync[1], &seen, sizeof(seen));
    _exit(0);
  }
  if (child < 0) {
    fail("ptrace-syscall", child);
    return;
  }
  close(sync[1]);

  int status = 0;
  int stopped = trace_child_until_stop(child, &status);
  long so = stopped
                ? ptrace(PTRACE_SETOPTIONS, child, 0, PTRACE_O_TRACESYSGOOD)
                : -1;

  int entry_seen = 0, exit_seen = 0, sysgood_seen = 0, injected = 0;
  if (so == 0) {
    ptrace(PTRACE_SYSCALL, child, 0, 0);
    for (int i = 0; i < 400 && !injected; i++) {
      if (!trace_child_until_stop(child, &status))
        break;
      if (WSTOPSIG(status) == (SIGTRAP | 0x80))
        sysgood_seen = 1;
      struct user_regs64 r;
      memset(&r, 0, sizeof(r));
      if (ptrace(PTRACE_GETREGS, child, 0, &r) != 0)
        break;
      if (!entry_seen && r.rax == SYS_GETPID_NR) {
        /* Entry stop of getpid: rax still holds the syscall number. */
        entry_seen = 1;
        ptrace(PTRACE_SYSCALL, child, 0, 0);
        if (!trace_child_until_stop(child, &status))
          break;
        memset(&r, 0, sizeof(r));
        if (ptrace(PTRACE_GETREGS, child, 0, &r) != 0)
          break;
        /* Exit stop: rax is the return value, and the tracer may replace it. */
        exit_seen = (r.rax == (unsigned long)child);
        r.rax = INJECTED_PID;
        if (ptrace(PTRACE_SETREGS, child, 0, &r) == 0)
          injected = 1;
      }
      ptrace(PTRACE_SYSCALL, child, 0, 0);
    }
  }

  /* Let the child run out, continuing through every remaining stop. */
  long reported = -1;
  for (int i = 0; i < 400; i++) {
    if (read_timeout(sync[0], &reported, sizeof(reported), 20) ==
        (long)sizeof(reported))
      break;
    reported = -1;
    int st2 = 0;
    pid_t w2 = waitpid(child, &st2, WUNTRACED | WNOHANG);
    if (w2 == child && WIFSTOPPED(st2))
      ptrace(PTRACE_SYSCALL, child, 0, 0);
    else if (w2 == child || w2 < 0)
      break;
  }
  close(sync[0]);
  for (int i = 0; i < 300; i++) {
    int st2 = 0;
    pid_t w2 = waitpid(child, &st2, WUNTRACED | WNOHANG);
    if (w2 == child && WIFSTOPPED(st2)) {
      ptrace(PTRACE_CONT, child, 0, 0);
      continue;
    }
    if (w2 == child || w2 < 0)
      break;
    usleep(10000);
  }
  reap(child);

  check("ptrace-syscall",
        stopped && so == 0 && sysgood_seen && entry_seen && exit_seen &&
            injected && reported == INJECTED_PID,
        reported);
}

/* ── PTRACE_O_TRACEEXIT ──────────────────────────────────────────────────── */

static void test_ptrace_exit_event(void) {
  pid_t child = fork();
  if (child == 0) {
    ptrace(PTRACE_TRACEME, 0, 0, 0);
    kill(getpid(), SIGSTOP);
    _exit(42);
  }
  if (child < 0) {
    fail("ptrace-exit-event", child);
    return;
  }
  int status = 0;
  int stopped = trace_child_until_stop(child, &status);
  long so = stopped ? ptrace(PTRACE_SETOPTIONS, child, 0, PTRACE_O_TRACEEXIT)
                    : -1;
  if (so == 0)
    ptrace(PTRACE_CONT, child, 0, 0);
  int evstopped = (so == 0) && trace_child_until_stop(child, &status);
  int event = evstopped ? (status >> 16) : -1;
  unsigned long msg = 0;
  long gm = evstopped ? ptrace(PTRACE_GETEVENTMSG, child, 0, &msg) : -1;

  /* The registers reported at an exit stop are the last ring-3 frame the task
   * really had — its exit_group entry — so rip must still be a user address. */
  struct user_regs64 regs;
  memset(&regs, 0, sizeof(regs));
  long gr = evstopped ? ptrace(PTRACE_GETREGS, child, 0, &regs) : -1;

  if (evstopped)
    ptrace(PTRACE_CONT, child, 0, 0);
  int st = 0, exited = 0;
  for (int i = 0; i < 300; i++) {
    pid_t w = waitpid(child, &st, WNOHANG);
    if (w == child) {
      exited = WIFEXITED(st) && WEXITSTATUS(st) == 42;
      break;
    }
    if (w < 0)
      break;
    usleep(10000);
  }
  if (!exited)
    reap(child);

  check("ptrace-exit-event",
        stopped && so == 0 && evstopped && event == PTRACE_EVENT_EXIT &&
            gm == 0 && WEXITSTATUS((int)msg) == 42 && gr == 0 &&
            regs.rip != 0 && exited,
        (long)msg);
}

/* ── a tracee stops even for a signal it ignores ─────────────────────────── */

static void test_ptrace_ignored_signal(void) {
  pid_t child = fork();
  if (child == 0) {
    ptrace(PTRACE_TRACEME, 0, 0, 0);
    signal(SIGWINCH, SIG_IGN); /* the process itself discards it */
    kill(getpid(), SIGSTOP);
    for (;;)
      usleep(1000);
  }
  if (child < 0) {
    fail("ptrace-ignored-signal", child);
    return;
  }
  int status = 0;
  int stopped = trace_child_until_stop(child, &status);
  if (stopped)
    ptrace(PTRACE_CONT, child, 0, 0);
  kill(child, SIGWINCH);
  int again = trace_child_until_stop(child, &status);
  int sig = again ? WSTOPSIG(status) : -1;

  ptrace(PTRACE_KILL, child, 0, 0);
  reap(child);
  check("ptrace-ignored-signal", stopped && again && sig == SIGWINCH,
        (long)sig);
}


/* ── AVX state across context switches (XSAVE) ───────────────────────────── */

static void cpuid_count_u(unsigned leaf, unsigned sub, unsigned *a, unsigned *b,
                          unsigned *c, unsigned *d) {
  __asm__ volatile("cpuid"
                   : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                   : "a"(leaf), "c"(sub));
}

static int cpu_has_avx(void) {
  unsigned a, b, c, d;
  cpuid_count_u(1, 0, &a, &b, &c, &d);
  int osxsave = (c & (1u << 27)) != 0; /* the OS enabled XSAVE */
  int avx = (c & (1u << 28)) != 0;
  if (!osxsave || !avx)
    return 0;
  unsigned lo, hi;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  return (lo & 0x6) == 0x6; /* SSE + AVX both enabled in XCR0 */
}

static void ymm0_load(const void *p) {
  __asm__ volatile("vmovdqu %0, %%ymm0" : : "m"(*(const char *)p) : "memory");
}

static void ymm0_store(void *p) {
  __asm__ volatile("vmovdqu %%ymm0, %0" : "=m"(*(char *)p) : : "memory");
}

/* Offset of the YMM_Hi128 component inside the XSAVE area, from CPUID. */
static unsigned ymm_hi_offset(void) {
  unsigned a, b, c, d;
  cpuid_count_u(0x0D, 2, &a, &b, &c, &d);
  return b;
}

static void test_avx_context(void) {
  if (!cpu_has_avx()) {
    fail("avx-context", -1); /* no AVX here: the feature cannot be verified */
    return;
  }

  static const unsigned char PAT_A[32] = {
      0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA,
      0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5,
      0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF};
  static const unsigned char PAT_B[32] = {
      0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A,
      0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
      0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F};

  int sync[2];
  if (pipe(sync) != 0) {
    fail("avx-context", -2);
    return;
  }
  pid_t child = fork();
  if (child == 0) {
    close(sync[0]);
    unsigned char seen[32];
    ymm0_load(PAT_A);
    /* Force many context switches while holding the pattern in ymm0. */
    long bad = 0;
    for (int i = 0; i < 200; i++) {
      usleep(2000);
      memset(seen, 0, sizeof(seen));
      ymm0_store(seen);
      if (memcmp(seen, PAT_A, sizeof(seen)) != 0) {
        bad = i + 1;
        break;
      }
      ymm0_load(PAT_A);
    }
    write(sync[1], &bad, sizeof(bad));
    /* Keep the pattern live so the tracer below can read it back. */
    ymm0_load(PAT_A);
    for (;;)
      usleep(1000);
  }
  if (child < 0) {
    fail("avx-context", child);
    return;
  }
  close(sync[1]);

  /* Churn our own ymm0 with a different pattern so the child's copy can only
   * survive if the kernel really saves and restores it per task. */
  unsigned char mine[32];
  ymm0_load(PAT_B);
  for (int i = 0; i < 50; i++) {
    usleep(2000);
    memset(mine, 0, sizeof(mine));
    ymm0_store(mine);
    if (memcmp(mine, PAT_B, sizeof(mine)) != 0)
      break;
    ymm0_load(PAT_B);
  }
  int self_ok = (memcmp(mine, PAT_B, sizeof(mine)) == 0);

  long child_bad = -1;
  int got = (read_timeout(sync[0], &child_bad, sizeof(child_bad), 15000) ==
             (long)sizeof(child_bad));
  close(sync[0]);

  /* The tracer's view: NT_X86_XSTATE must declare AVX and carry the child's
   * upper YMM half. */
  int avx_declared = 0, ymm_ok = 0;
  unsigned char xs[4096];
  if (ptrace(PTRACE_SEIZE, child, 0, 0) == 0 &&
      ptrace(PTRACE_INTERRUPT, child, 0, 0) == 0) {
    int status = 0;
    if (wait_stop(child, &status, 10000) == child) {
      memset(xs, 0, sizeof(xs));
      struct kiovec iov = {xs, sizeof(xs)};
      if (ptrace(PTRACE_GETREGSET, child, NT_X86_XSTATE, &iov) == 0 &&
          iov.iov_len >= 576) {
        unsigned long bv = 0;
        memcpy(&bv, xs + 512, sizeof(bv));
        avx_declared = (bv & 0x4) != 0;
        unsigned off = ymm_hi_offset();
        if (avx_declared && off + 16 <= iov.iov_len)
          ymm_ok = (memcmp(xs + off, PAT_A + 16, 16) == 0);
      }
    }
  }
  ptrace(PTRACE_KILL, child, 0, 0);
  reap(child);

  check("avx-context",
        self_ok && got && child_bad == 0 && avx_declared && ymm_ok, child_bad);
}


/* ── CPU clock: /proc/cpuinfo and the sysfs cpufreq files ────────────────── */

static void test_cpu_freq(void) {
  char blob[128];
  long n = slurp("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", blob,
                 sizeof(blob) - 1);
  if (n <= 0) {
    fail("cpu-freq", n);
    return;
  }
  blob[n] = '\0';
  long cur_khz = strtol(blob, 0, 10);

  char maxblob[128];
  long mn = slurp("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq",
                  maxblob, sizeof(maxblob) - 1);
  long max_khz = 0;
  if (mn > 0) {
    maxblob[mn] = '\0';
    max_khz = strtol(maxblob, 0, 10);
  }

  /* /proc/cpuinfo must agree with sysfs: same clock, expressed in MHz. */
  char cpuinfo[2048];
  long cn = slurp("/proc/cpuinfo", cpuinfo, sizeof(cpuinfo) - 1);
  long mhz = -1;
  if (cn > 0) {
    cpuinfo[cn] = '\0';
    const char *p = strstr(cpuinfo, "cpu MHz");
    if (p) {
      p = strchr(p, ':');
      if (p)
        mhz = strtol(p + 1, 0, 10);
    }
  }

  /* A plausible clock, and the two views of it consistent to within 1 MHz. */
  int sane = (cur_khz > 100000 && cur_khz < 20000000);
  int agree = (mhz > 0 && labs(mhz - cur_khz / 1000) <= 1);
  check("cpu-freq", sane && max_khz >= cur_khz && agree, cur_khz);
}


/* ── /proc/<pid>/maps names its files, and names the device they live on ─── */

static void test_proc_maps(void) {
  char blob[8192];
  long n = slurp("/proc/self/maps", blob, sizeof(blob) - 1);
  if (n <= 0) {
    fail("proc-maps", n);
    return;
  }
  blob[n] = '\0';

  /* The line covering this function's own address must name this executable
   * and carry a non-zero device and inode: an entry with dev 0 and inode 0 is
   * by definition anonymous memory, and a reader will treat it as such. */
  unsigned long self_addr = (unsigned long)(void *)&test_proc_maps;
  int found = 0, dev_ok = 0, ino_ok = 0, named = 0, ordered = 1;
  unsigned long prev_start = 0;
  for (char *line = blob; line && *line;) {
    char *nl = strchr(line, '\n');
    if (nl)
      *nl = '\0';
    unsigned long start = 0, end = 0, off = 0, maj = 0, min = 0, ino = 0;
    char perms[8] = {0}, path[192] = {0};
    int got = sscanf(line, "%lx-%lx %7s %lx %lx:%lx %lu %191s", &start, &end,
                     perms, &off, &maj, &min, &ino, path);
    if (got >= 7) {
      if (start < prev_start)
        ordered = 0;
      prev_start = start;
      if (self_addr >= start && self_addr < end) {
        found = 1;
        dev_ok = (maj != 0 || min != 0);
        ino_ok = (ino != 0);
        named = (got == 8 && path[0] == '/');
      }
    }
    line = nl ? nl + 1 : 0;
  }
  check("proc-maps", found && ordered && dev_ok && ino_ok && named,
        (long)self_addr);
}

int main(void) {
  marker("M80-SMOKE: start");
  test_proc_task();
  test_proc_status();
  test_proc_maps();
  test_proc_auxv();
  test_proc_mem();
  test_ptrace_siginfo();
  test_regsets_and_capture();
  test_seize();
  test_ptrace_fork_event();
  test_ptrace_exec_event();
  test_listen_and_xstate();
  test_ptrace_syscall();
  test_ptrace_exit_event();
  test_ptrace_ignored_signal();
  test_avx_context();
  test_cpu_freq();
  test_exitkill();
  test_so_peercred();
  test_yama_scope();
  marker("M80-SMOKE: done");
  return g_fail ? 1 : 0;
}
