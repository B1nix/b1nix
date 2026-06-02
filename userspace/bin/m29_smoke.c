/* M29 POSIX threads / futex / TLS smoke test.
 *
 * Each phase emits a "M29-PTHREAD: ok <name>" marker on success or
 * "M29-PTHREAD: FAIL <name>" on failure, mirroring the existing milestone
 * smoke conventions. The host-side smoke.sh greps for the ok markers and
 * for "M29-PTHREAD: done" as the all-passed signal. */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "syscall.h"

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

int main(void) {
  emit("M29-PTHREAD: start\n");
  if (test_create_join() != 0) return 1;
  if (test_mutex() != 0)       return 1;
  if (test_condvar() != 0)     return 1;
  if (test_tls() != 0)         return 1;
  if (test_gettid() != 0)      return 1;
  emit("M29-PTHREAD: done\n");
  return 0;
}
