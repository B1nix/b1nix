#ifndef B1NIX_PTHREAD_H
#define B1NIX_PTHREAD_H

/* M29: minimal libpthread for b1nix.
 *
 * Built on the kernel's SYS_CLONE / SYS_FUTEX / SYS_SET_TLS primitives. The
 * subset implemented here covers:
 *   - thread create / exit / join / detach / self
 *   - mutexes (default + recursive flavour)
 *   - condition variables (signal / broadcast / wait)
 *   - one-time initialization (pthread_once)
 *
 * Cancellation, scheduling-attribute knobs, rwlocks, barriers, and TLS-key
 * APIs are explicitly out of scope for M29. They can be layered on top in
 * later milestones without breaking the ABI defined here.
 */

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef long pthread_t;

typedef struct {
  /* Caller may zero-fill or use PTHREAD_MUTEX_INITIALIZER. */
  unsigned int reserved;
} pthread_attr_t;

typedef struct {
  /* 0 = unlocked, 1 = locked-no-waiter, 2 = locked-with-waiter (futex word). */
  volatile int state;
  /* Recursion + ownership (used by PTHREAD_MUTEX_RECURSIVE). */
  int kind;
  pthread_t owner;
  int recursion;
} pthread_mutex_t;

typedef struct {
  int kind; /* 0 = normal, 1 = recursive */
} pthread_mutexattr_t;

typedef struct {
  volatile int seq;
} pthread_cond_t;

typedef struct {
  int unused;
} pthread_condattr_t;

typedef struct {
  volatile int state; /* 0=not yet, 1=in progress, 2=done */
} pthread_once_t;

#define PTHREAD_MUTEX_INITIALIZER { 0, 0, 0, 0 }
#define PTHREAD_COND_INITIALIZER  { 0 }
#define PTHREAD_ONCE_INIT         { 0 }

#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL

/* ── Thread lifecycle ── */
int  pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                    void *(*start_routine)(void *), void *arg);
void pthread_exit(void *retval) __attribute__((noreturn));
int  pthread_join(pthread_t thread, void **retval);
int  pthread_detach(pthread_t thread);
pthread_t pthread_self(void);
int  pthread_equal(pthread_t a, pthread_t b);

/* ── Mutex ── */
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);

int pthread_mutexattr_init(pthread_mutexattr_t *a);
int pthread_mutexattr_destroy(pthread_mutexattr_t *a);
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type);

/* ── Condition variable ── */
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *abstime);

/* ── Once ── */
int pthread_once(pthread_once_t *once, void (*init_routine)(void));

/* ── Attributes (placeholder, no real knobs honored on b1nix) ── */
int pthread_attr_init(pthread_attr_t *a);
int pthread_attr_destroy(pthread_attr_t *a);

#ifdef __cplusplus
}
#endif

#endif /* B1NIX_PTHREAD_H */
