/* M29 POSIX threads / futex / TLS smoke test.
 *
 * Each phase emits a "M29-PTHREAD: ok <name>" marker on success or
 * "M29-PTHREAD: FAIL <name>" on failure, mirroring the existing milestone
 * smoke conventions. The host-side smoke.sh greps for the ok markers and
 * for "M29-PTHREAD: done" as the all-passed signal. */

#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "syscall.h"
#include <syslog.h>
#include <utmp.h>
#include <security/pam_appl.h>
#include <locale.h>
#include <langinfo.h>
#include <iconv.h>
#include <time.h>
#include <sys/time.h>

static void emit(const char *s) {
  write(1, s, strlen(s));
}

static void ok(const char *name) {
  char buf[128];
  int n = 0;
  const char *p = "M29-PTHREAD: ok ";
  while (*p) buf[n++] = *p++;
  while (*name) buf[n++] = *name++;
  buf[n++] = '\n';
  write(1, buf, n);
}

static void fail(const char *name) {
  char buf[128];
  int n = 0;
  const char *p = "M29-PTHREAD: FAIL ";
  while (*p) buf[n++] = *p++;
  while (*name) buf[n++] = *name++;
  buf[n++] = '\n';
  write(1, buf, n);
}

/* ── 1: create + join ── */

static void *t1_entry(void *arg) {
  int *p = (int *)arg;
  *p = 0xC0FFEE;
  return (void *)0xBEEF;
}

static int test_create_join(void) {
  pthread_t th;
  int slot = 0;
  if (pthread_create(&th, 0, t1_entry, &slot) != 0) {
    fail("create"); return -1;
  }
  void *rv = 0;
  if (pthread_join(th, &rv) != 0) {
    fail("join"); return -1;
  }
  if (slot != 0xC0FFEE) { fail("create-side-effect"); return -1; }
  if ((unsigned long)rv != 0xBEEF) { fail("join-retval"); return -1; }
  ok("create-join");
  return 0;
}

/* ── 1b: thread attributes (stack size + detach state) ── */

static volatile int g_attr_detached_ran = 0;

static void *t_attr_detached(void *arg) {
  (void)arg;
  __atomic_store_n(&g_attr_detached_ran, 1, __ATOMIC_RELEASE);
  return 0;
}

static void *t_attr_bigstack(void *arg) {
  /* Touch a 384 KB on-stack buffer — larger than the 256 KB default stack, so
   * this only runs without faulting if the 512 KB request was honored. The
   * volatile reads keep the compiler from optimizing the buffer away. */
  volatile char buf[384 * 1024];
  for (size_t i = 0; i < sizeof(buf); i += 4096)
    buf[i] = (char)i;
  long sum = 0;
  for (size_t i = 0; i < sizeof(buf); i += 4096)
    sum += buf[i];
  *(volatile long *)arg = sum;
  return 0;
}

static int test_attr(void) {
  pthread_attr_t a;
  if (pthread_attr_init(&a) != 0) { fail("attr-init"); return -1; }

  /* Detach-state get/set round-trip + EINVAL on a bogus value. */
  int ds = -1;
  if (pthread_attr_getdetachstate(&a, &ds) != 0 ||
      ds != PTHREAD_CREATE_JOINABLE) { fail("attr-detach-default"); return -1; }
  if (pthread_attr_setdetachstate(&a, 999) != EINVAL) {
    fail("attr-detach-einval"); return -1;
  }
  if (pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED) != 0 ||
      pthread_attr_getdetachstate(&a, &ds) != 0 ||
      ds != PTHREAD_CREATE_DETACHED) { fail("attr-detach-set"); return -1; }

  /* Stack-size get/set round-trip + minimum-size rejection. */
  size_t sz = 0;
  if (pthread_attr_setstacksize(&a, 1024) != EINVAL) {
    fail("attr-stack-toosmall"); return -1;
  }
  if (pthread_attr_setstacksize(&a, 512 * 1024) != 0 ||
      pthread_attr_getstacksize(&a, &sz) != 0 ||
      sz != 512 * 1024) { fail("attr-stack-set"); return -1; }

  /* A detached thread must run without a join and not be join-able. */
  g_attr_detached_ran = 0;
  pthread_t dt;
  if (pthread_create(&dt, &a, t_attr_detached, 0) != 0) {
    fail("attr-detached-create"); return -1;
  }
  for (int spins = 0; spins < 1000000 &&
       !__atomic_load_n(&g_attr_detached_ran, __ATOMIC_ACQUIRE); spins++) {
    if ((spins & 0xFFF) == 0) syscall(SYS_YIELD);
  }
  if (!__atomic_load_n(&g_attr_detached_ran, __ATOMIC_ACQUIRE)) {
    fail("attr-detached-run"); return -1;
  }
  if (pthread_join(dt, 0) != EINVAL) { fail("attr-detached-nojoin"); return -1; }

  /* A thread created with a large explicit stack must run a deep frame. */
  pthread_attr_t big;
  pthread_attr_init(&big);
  if (pthread_attr_setstacksize(&big, 512 * 1024) != 0) {
    fail("attr-bigstack-set"); return -1;
  }
  volatile long sum = 0;
  pthread_t bt;
  if (pthread_create(&bt, &big, t_attr_bigstack, (void *)&sum) != 0) {
    fail("attr-bigstack-create"); return -1;
  }
  if (pthread_join(bt, 0) != 0) { fail("attr-bigstack-join"); return -1; }
  pthread_attr_destroy(&big);
  pthread_attr_destroy(&a);
  ok("attr");
  return 0;
}

/* ── 2: mutex mutual exclusion ── */

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_counter = 0;

static void *t2_inc(void *arg) {
  int n = (int)(long)arg;
  for (int i = 0; i < n; i++) {
    pthread_mutex_lock(&g_mx);
    int v = g_counter;
    /* Force a window for the race to manifest if the mutex is broken. */
    for (volatile int j = 0; j < 64; j++) { (void)j; }
    g_counter = v + 1;
    pthread_mutex_unlock(&g_mx);
  }
  return 0;
}

static int test_mutex(void) {
  g_counter = 0;
  pthread_t a, b;
  if (pthread_create(&a, 0, t2_inc, (void *)200L) != 0 ||
      pthread_create(&b, 0, t2_inc, (void *)200L) != 0) {
    fail("mutex-create"); return -1;
  }
  pthread_join(a, 0);
  pthread_join(b, 0);
  if (g_counter != 400) { fail("mutex-count"); return -1; }
  ok("mutex");
  return 0;
}

/* ── 3: condition variable signal/wait ── */

static pthread_mutex_t g_cv_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv    = PTHREAD_COND_INITIALIZER;
static volatile int    g_ready = 0;
static volatile int    g_woke  = 0;

static void *t3_wait(void *arg) {
  (void)arg;
  pthread_mutex_lock(&g_cv_mx);
  while (!g_ready) {
    pthread_cond_wait(&g_cv, &g_cv_mx);
  }
  g_woke = 1;
  pthread_mutex_unlock(&g_cv_mx);
  return 0;
}

static int test_condvar(void) {
  g_ready = 0;
  g_woke = 0;
  pthread_t w;
  if (pthread_create(&w, 0, t3_wait, 0) != 0) {
    fail("cv-create"); return -1;
  }
  /* Yield to let the waiter park on the futex. */
  for (volatile int i = 0; i < 100000; i++) { (void)i; }
  syscall(SYS_YIELD);

  pthread_mutex_lock(&g_cv_mx);
  g_ready = 1;
  pthread_cond_signal(&g_cv);
  pthread_mutex_unlock(&g_cv_mx);

  pthread_join(w, 0);
  if (!g_woke) { fail("cv-wake"); return -1; }
  ok("condvar");
  return 0;
}

/* ── 3b: real ELF __thread storage across pthreads (the V8 thread_local path) ──
 * Each spawned thread must get its OWN copy of a __thread variable, initialised
 * from the .tdata image, with no bleed between siblings or the main thread.
 * Before per-thread ELF TLS in pthread_create, spawned threads ran with no real
 * TLS block and a thread_local read faulted (V8 TurboFan #fs:0 null-deref). */

static __thread long tl_value = 0xABBA; /* .tdata init image */
static __thread long tl_zero;           /* .tbss — must start zero per thread */

static volatile int g_tl_fail = 0;

static void *t_tls_worker(void *arg) {
  long id = (long)arg;
  /* Every thread starts from the init image, not a sibling's writes. */
  if (tl_value != 0xABBA || tl_zero != 0) {
    __atomic_store_n(&g_tl_fail, 1, __ATOMIC_RELEASE);
    return 0;
  }
  tl_value = id;
  tl_zero = id ^ 0x5555;
  /* Spin so siblings run concurrently — a shared slot would get clobbered. */
  for (volatile int i = 0; i < 200000; i++) { (void)i; }
  if (tl_value != id || tl_zero != (id ^ 0x5555))
    __atomic_store_n(&g_tl_fail, 1, __ATOMIC_RELEASE);
  return 0;
}

static int test_thread_local(void) {
  tl_value = 0x1111; /* main thread's own copy (loader-set TLS) */
  g_tl_fail = 0;
  pthread_t th[4];
  for (long i = 0; i < 4; i++) {
    if (pthread_create(&th[i], 0, t_tls_worker, (void *)(i + 1)) != 0) {
      fail("thread-local-create"); return -1;
    }
  }
  for (int i = 0; i < 4; i++) pthread_join(th[i], 0);
  if (__atomic_load_n(&g_tl_fail, __ATOMIC_ACQUIRE)) {
    fail("thread-local-bleed"); return -1;
  }
  if (tl_value != 0x1111) { /* workers must not touch main's copy */
    fail("thread-local-main-clobbered"); return -1;
  }
  ok("thread-local");
  return 0;
}

/* ── 4: TLS round-trip ── */

static int test_tls(void) {
  long val = 0xCAFE;
  /* The SYS_SET_TLS handler writes the TLS segment base.
   * On x86_64, TLS is accessed via %fs:0.
   * On x86 32-bit, TLS is accessed via %gs:0. */
  long rc = syscall(SYS_SET_TLS, &val);
  if (rc != 0) { fail("tls-set"); return -1; }
  long readback = 0;
#ifdef __x86_64__
  __asm__ volatile("movq %%fs:0, %0" : "=r"(readback));
#else
  __asm__ volatile("movl %%gs:0, %0" : "=r"(readback));
#endif
  if (readback != 0xCAFE) { fail("tls-read"); return -1; }
  ok("tls");
  return 0;
}

/* ── 5: gettid distinct from main ── */

static volatile long g_child_tid = 0;
static void *t5_tid(void *arg) {
  (void)arg;
  g_child_tid = syscall(SYS_GETTID);
  return 0;
}

static int test_gettid(void) {
  long my = syscall(SYS_GETTID);
  pthread_t th;
  if (pthread_create(&th, 0, t5_tid, 0) != 0) { fail("gettid-create"); return -1; }
  pthread_join(th, 0);
  if (g_child_tid == 0 || g_child_tid == my) { fail("gettid-distinct"); return -1; }
  ok("gettid");
  return 0;
}

/* ── 6: stress — leader exit() WITHOUT joining its threads ── */

static void *t6_spin(void *arg) {
  /* Variable-length spin so across rounds the threads die in every order
   * relative to the leader's _exit (before, racing, after). */
  long n = (long)arg;
  for (volatile long i = 0; i < n * 4000; i++) { (void)i; }
  return 0;
}

int test_stress_smp(void) {
  for (int round = 0; round < 120; round++) {
    int pid = fork();
    if (pid < 0) { fail("stress-fork"); return -1; }
    if (pid == 0) {
      /* Child: spawn 3 CLONE_VM threads and _exit immediately, never
       * joining. The kernel must free the shared address space when the
       * LAST thread is reaped — leaking it (pml4 + page tables + vmas)
       * every round ends in a PMM OOM panic well before round 120. */
      pthread_t th[3];
      for (int t = 0; t < 3; t++) {
        if (pthread_create(&th[t], 0, t6_spin, (void *)(long)(round % 7)) != 0)
          _exit(2);
      }
      _exit(0);
    }
    int status = -1;
    int wr = (int)syscall(SYS_WAITPID, pid, &status, 0);
    if (wr != pid) { fail("stress-waitpid"); return -1; }
    if (status != 0) { fail("stress-exit-code"); return -1; }
  }
  ok("stress-smp");
  return 0;
}

/* ── 7: Thread-Specific Data (TSD) ── */

static pthread_key_t g_key;
static volatile int g_dtor_calls = 0;

static void tsd_dtor(void *value) {
  int val = (int)(long)value;
  if (val == 0x1234) {
    __atomic_fetch_add(&g_dtor_calls, 1, __ATOMIC_SEQ_CST);
  }
}

static void *t7_entry(void *arg) {
  (void)arg;
  if (pthread_setspecific(g_key, (void *)0x1234) != 0) {
    return (void *)1;
  }
  void *val = pthread_getspecific(g_key);
  if ((long)val != 0x1234) {
    return (void *)2;
  }
  return 0;
}

static int test_tsd(void) {
  g_dtor_calls = 0;
  if (pthread_key_create(&g_key, tsd_dtor) != 0) {
    fail("tsd-key-create"); return -1;
  }

  pthread_t th;
  if (pthread_create(&th, 0, t7_entry, 0) != 0) {
    fail("tsd-thread-create"); return -1;
  }

  void *retval = 0;
  pthread_join(th, &retval);
  if ((long)retval != 0) {
    fail("tsd-getspecific-failed"); return -1;
  }

  if (g_dtor_calls != 1) {
    fail("tsd-dtor-not-called"); return -1;
  }

  if (pthread_key_delete(g_key) != 0) {
    fail("tsd-key-delete"); return -1;
  }

  ok("tsd");
  return 0;
}

/* ── 8: Syslog, Utmp, PAM tests ── */
static int test_syslog(void) {
  openlog("m29_smoke_syslog", LOG_PID | LOG_PERROR, LOG_USER);
  syslog(LOG_INFO, "syslog test message: %s", "success");
  closelog();
  ok("syslog");

  /* Sink delivery: no LOG_PERROR/LOG_CONS, so this line can only reach the
   * serial log via the kernel /dev/log sink. The smoke harness greps for the
   * sentinel to prove the sink (not the stderr fallback) delivered it. */
  openlog("m54sink", LOG_PID, LOG_USER);
  syslog(LOG_INFO, "M54-LOG sink-delivers-ok");
  closelog();
  return 0;
}

static int test_utmp(void) {
  utmpname("/tmp/utmp_test");
  struct utmp ut;
  memset(&ut, 0, sizeof(ut));
  ut.ut_type = USER_PROCESS;
  ut.ut_pid = 9999;
  strcpy(ut.ut_line, "ttyS0");
  strcpy(ut.ut_id, "s0");
  strcpy(ut.ut_user, "testuser");
  strcpy(ut.ut_host, "localhost");
  ut.ut_tv.tv_sec = 12345678;
  
  setutent();
  if (!pututline(&ut)) {
    fail("utmp-pututline-failed");
    return -1;
  }
  endutent();
  
  setutent();
  struct utmp *r = getutline(&ut);
  if (!r) {
    fail("utmp-getutline-failed");
    endutent();
    return -1;
  }
  if (r->ut_pid != 9999 || strcmp(r->ut_user, "testuser") != 0) {
    fail("utmp-data-mismatch");
    endutent();
    return -1;
  }
  endutent();
  
  ok("utmp");
  return 0;
}

static int mock_pam_conv(int num_msg, const struct pam_message **msg,
                         struct pam_response **resp, void *appdata) {
  (void)num_msg; (void)msg; (void)appdata;
  struct pam_response *r = calloc(1, sizeof(struct pam_response));
  r->resp = strdup("testpass");
  *resp = r;
  return PAM_SUCCESS;
}

static int test_pam(void) {
  pam_handle_t *pamh = NULL;
  struct pam_conv conv = { mock_pam_conv, NULL };
  
  if (pam_start("test_service", "nonexistent_user", &conv, &pamh) != PAM_SUCCESS) {
    fail("pam-start-failed");
    return -1;
  }
  
  const char *user = NULL;
  if (pam_get_item(pamh, PAM_USER, (const void **)&user) != PAM_SUCCESS || 
      strcmp(user, "nonexistent_user") != 0) {
    fail("pam-get-item-failed");
    pam_end(pamh, PAM_AUTH_ERR);
    return -1;
  }
  
  int rc = pam_authenticate(pamh, 0);
  if (rc != PAM_USER_UNKNOWN) {
    fail("pam-authenticate-nonexistent-user-should-fail");
    pam_end(pamh, PAM_AUTH_ERR);
    return -1;
  }
  
  pam_end(pamh, PAM_SUCCESS);
  ok("pam");
  return 0;
}

static int test_locale(void) {
  char *loc = setlocale(LC_ALL, NULL);
  if (!loc || strcmp(loc, "C.UTF-8") != 0) {
    fail("locale-get-default-failed");
    return -1;
  }
  
  loc = setlocale(LC_CTYPE, "en_US.UTF-8");
  if (!loc || strcmp(loc, "en_US.UTF-8") != 0) {
    fail("locale-set-failed");
    return -1;
  }
  
  char *codeset = nl_langinfo(CODESET);
  if (!codeset || strcmp(codeset, "UTF-8") != 0) {
    fail("langinfo-codeset-failed");
    return -1;
  }
  
  struct lconv *lc = localeconv();
  if (!lc || strcmp(lc->decimal_point, ".") != 0) {
    fail("localeconv-failed");
    return -1;
  }
  
  ok("locale");
  return 0;
}

static int test_iconv(void) {
  /* UTF-8 → Latin-1: "Café" (é = U+00E9, 0xC3 0xA9 in UTF-8 → 0xE9). */
  iconv_t cd = iconv_open("ISO-8859-1", "UTF-8");
  if (cd == (iconv_t)-1) { fail("iconv-open"); return -1; }
  char in[] = { 'C', 'a', 'f', (char)0xC3, (char)0xA9 };
  char out[16];
  char *ip = in, *op = out;
  size_t il = sizeof(in), ol = sizeof(out);
  size_t r = iconv(cd, &ip, &il, &op, &ol);
  if (r == (size_t)-1 || il != 0) { fail("iconv-utf8-latin1"); iconv_close(cd); return -1; }
  if ((size_t)(op - out) != 4 || (unsigned char)out[3] != 0xE9) {
    fail("iconv-utf8-latin1-bytes"); iconv_close(cd); return -1;
  }
  iconv_close(cd);

  /* Round-trip Latin-1 → UTF-8 reproduces the 2-byte é. */
  cd = iconv_open("UTF-8", "LATIN1");
  if (cd == (iconv_t)-1) { fail("iconv-open2"); return -1; }
  ip = out; il = 4; op = in; ol = sizeof(in);
  r = iconv(cd, &ip, &il, &op, &ol);
  if (r == (size_t)-1 || (size_t)(op - in) != 5 ||
      (unsigned char)in[3] != 0xC3 || (unsigned char)in[4] != 0xA9) {
    fail("iconv-latin1-utf8"); iconv_close(cd); return -1;
  }
  iconv_close(cd);

  /* E2BIG when the output buffer is too small. */
  cd = iconv_open("UTF-8", "UTF-8");
  if (cd == (iconv_t)-1) { fail("iconv-open3"); return -1; }
  char src[] = "hello";
  char tiny[2];
  ip = src; il = 5; op = tiny; ol = sizeof(tiny);
  errno = 0;
  r = iconv(cd, &ip, &il, &op, &ol);
  if (r != (size_t)-1 || errno != E2BIG) { fail("iconv-e2big"); iconv_close(cd); return -1; }
  iconv_close(cd);

  /* EILSEQ on a non-ASCII code point converted to US-ASCII. */
  cd = iconv_open("ASCII", "UTF-8");
  if (cd == (iconv_t)-1) { fail("iconv-open4"); return -1; }
  char bad[] = { (char)0xC3, (char)0xA9 };
  char obuf[8];
  ip = bad; il = 2; op = obuf; ol = sizeof(obuf);
  errno = 0;
  r = iconv(cd, &ip, &il, &op, &ol);
  if (r != (size_t)-1 || errno != EILSEQ) { fail("iconv-eilseq"); iconv_close(cd); return -1; }
  iconv_close(cd);

  /* Unsupported conversion name → EINVAL. */
  errno = 0;
  if (iconv_open("NO-SUCH-ENC", "UTF-8") != (iconv_t)-1 || errno != EINVAL) {
    fail("iconv-badname"); return -1;
  }

  ok("iconv");
  return 0;
}

/* Regression guard for the NetSurf fb-scheduler workaround: hammering
 * gettimeofday()/clock_gettime() in a tight loop was reported to crash i686
 * intermittently. Drive both hard and check the clocks stay sane — if this
 * survives, the busy-poll path is safe and the nanosleep pacing in the NetSurf
 * test pump is pacing only, not papering over a kernel fault. */
static int test_time_hammer(void) {
  struct timeval tv, prev_tv = {0, 0};
  struct timespec mono, prev_mono = {0, 0};
  const int ITERS = 300000;
  for (int i = 0; i < ITERS; i++) {
    if (gettimeofday(&tv, NULL) != 0) { fail("time-gettimeofday"); return -1; }
    if (tv.tv_usec < 0 || tv.tv_usec >= 1000000) { fail("time-usec-range"); return -1; }
    if (tv.tv_sec < prev_tv.tv_sec) { fail("time-wall-backwards"); return -1; }
    prev_tv = tv;

    if (clock_gettime(CLOCK_MONOTONIC, &mono) != 0) { fail("time-clock-mono"); return -1; }
    if (mono.tv_nsec < 0 || mono.tv_nsec >= 1000000000L) { fail("time-nsec-range"); return -1; }
    /* Monotonic clock must never run backwards. */
    if (mono.tv_sec < prev_mono.tv_sec ||
        (mono.tv_sec == prev_mono.tv_sec && mono.tv_nsec < prev_mono.tv_nsec)) {
      fail("time-mono-backwards"); return -1;
    }
    prev_mono = mono;
  }
  ok("time-hammer");
  return 0;
}

/* ── Deferred thread cancellation ── */
static volatile int g_cancel_started = 0;
static volatile int g_cancel_completed = 0;

static void *t_cancel_loop(void *arg) {
  (void)arg;
  __atomic_store_n(&g_cancel_started, 1, __ATOMIC_RELEASE);
  /* Bounded so a missed cancellation fails deterministically instead of
   * hanging: when cancellation is honored the thread exits long before the
   * cap at the testcancel point below. */
  for (long i = 0; i < 50000000L; i++) {
    pthread_testcancel();             /* cancellation point */
  }
  g_cancel_completed = 1;             /* reached only if never cancelled */
  return (void *)0xABCD;
}

static int test_cancel(void) {
  /* setcancelstate round-trip on the calling thread. */
  int old = -1;
  if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old) != 0) {
    fail("cancel-setstate"); return -1;
  }
  if (pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &old) != 0 ||
      old != PTHREAD_CANCEL_DISABLE) { fail("cancel-setstate-old"); return -1; }

  g_cancel_started = 0;
  g_cancel_completed = 0;
  pthread_t th;
  if (pthread_create(&th, 0, t_cancel_loop, 0) != 0) {
    fail("cancel-create"); return -1;
  }
  /* Wait until the worker is actually spinning, then cancel it. */
  for (int s = 0; s < 2000000 &&
       !__atomic_load_n(&g_cancel_started, __ATOMIC_ACQUIRE); s++) {
    if ((s & 0xFFF) == 0) syscall(SYS_YIELD);
  }
  syscall(SYS_YIELD);
  if (pthread_cancel(th) != 0) { fail("cancel-request"); return -1; }
  pthread_join(th, 0);
  /* Cancellation delivered → the worker stopped at a testcancel point and
   * never set completed. */
  if (g_cancel_completed) { fail("cancel-ignored"); return -1; }
  ok("cancel");
  return 0;
}

int main(void) {
  emit("M29-PTHREAD: start\n");
  if (test_create_join() != 0) return 1;
  if (test_attr() != 0)        return 1;
  if (test_mutex() != 0)       return 1;
  if (test_condvar() != 0)     return 1;
  if (test_thread_local() != 0) return 1;
  if (test_tls() != 0)         return 1;
  if (test_gettid() != 0)      return 1;
  if (test_tsd() != 0)         return 1;
  if (test_stress_smp() != 0)  return 1;
  if (test_syslog() != 0)      return 1;
  if (test_utmp() != 0)        return 1;
  if (test_pam() != 0)         return 1;
  if (test_locale() != 0)      return 1;
  if (test_iconv() != 0)       return 1;
  if (test_time_hammer() != 0) return 1;
  if (test_cancel() != 0)      return 1;
  emit("M29-PTHREAD: done\n");
  return 0;
}
