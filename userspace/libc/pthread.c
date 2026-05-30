/* M29: libpthread for b1nix.
 *
 * Mutex / condvar / join all built on the kernel's SYS_FUTEX primitive.
 * Thread creation goes through SYS_CLONE with CLONE_VM | CLONE_FS |
 * CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_CHILD_CLEARTID. The
 * kernel atomically writes 0 to the joiner-visible tid slot and futex_wakes
 * it when the thread exits, which is exactly what pthread_join needs.
 *
 * No CLONE_SETTLS in this initial cut — TLS is set explicitly via
 * SYS_SET_TLS from the bootstrap entry below, so we control the layout
 * deterministically (caller doesn't have to supply an FS-base address).
 *
 * Thread state and the user stack are heap-allocated. Detached threads
 * have nothing to join; their state struct is freed by the dying thread
 * itself just before it issues SYS_EXIT_THREAD.
 */

#include "syscall.h"
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define DEFAULT_STACK_SIZE (256 * 1024)
#define STACK_GUARD_HI     0xDEADC0DEDEADBEEFULL

#ifndef PROT_READ
#define PROT_READ  0x1
#endif
#ifndef PROT_WRITE
#define PROT_WRITE 0x2
#endif
#ifndef MAP_PRIVATE
#define MAP_PRIVATE   0x02
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif

/* Per-thread state. Kept reference-stable for the joiner — pthread_t is a
 * `long` cast of `struct pthread_state *`. */
struct pthread_state {
  /* The CLONE_CHILD_CLEARTID slot. Set to the child's TID on create; the
   * kernel writes 0 + futex_wakes here on thread exit, which is exactly
   * what pthread_join waits on. */
  volatile int child_tid;
  int detached;
  void *retval;
  void *stack_base;
  size_t stack_size;
  /* Entry forwarding. */
  void *(*start_routine)(void *);
  void *arg;
};

/* Currently-active TLS pointer per thread. b1nix doesn't compile with
 * `-fpic` or ELF TLS sections (no .tdata yet), so we store the state
 * pointer at the bottom of the per-thread stack and point %fs at it. The
 * thread function reaches it via `%fs:0`. For now we don't expose
 * thread-local-storage to user code — the FS base just keeps the dispatch
 * correct so SYS_SET_TLS / arch_set_fs_base round-trip cleanly under the
 * smoke. */

/* ── Internal trampoline ── */

static void pthread_bootstrap(void *raw) {
  struct pthread_state *st = (struct pthread_state *)raw;

  /* Publish %fs base so userspace TLS reads land at our state pointer.
   * b1nix's libc doesn't use TLS-qualified globals yet; this is a
   * placeholder that exercises the SYS_SET_TLS path end-to-end. */
  syscall(SYS_SET_TLS, st);

  void *r = st->start_routine(st->arg);
  st->retval = r;
  if (st->detached) {
    /* Detached: nobody will join us. Unmap the user stack and free state
     * BEFORE exit — exit_thread doesn't return, so this is the last
     * chance. But munmap'ing the stack we're running on is a use-after-
     * free; instead, leak the stack here and let pthread_exit unmap via
     * a small bounce. For M29 simplicity, leak the stack of detached
     * threads (kernel reclaims it when the address space is freed at
     * process exit). The state struct can be freed safely — we're not
     * standing on it. */
    free(st);
  }
  syscall(SYS_EXIT_THREAD, 0);
  __builtin_unreachable();
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
  (void)attr;
  if (!thread || !start_routine) return EINVAL;

  struct pthread_state *st = (struct pthread_state *)malloc(sizeof(*st));
  if (!st) return EAGAIN;
  st->child_tid = 0;
  st->detached = 0;
  st->retval = 0;
  st->start_routine = start_routine;
  st->arg = arg;

  size_t stack_size = DEFAULT_STACK_SIZE;
  void *stack = mmap(0, stack_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (stack == (void *)-1) {
    free(st);
    return EAGAIN;
  }
  st->stack_base = stack;
  st->stack_size = stack_size;

  /* x86-64 SysV: stack grows down; align top to 16 bytes; leave room for a
   * minimal initial frame. We don't need argc/argv on the stack — the
   * kernel transfers control to pthread_bootstrap(state) directly via
   * x86_user_jump, which puts `state` in %rdi. */
  unsigned long top = (unsigned long)stack + stack_size;
  top &= ~15UL;

  unsigned long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND
                      | CLONE_THREAD | CLONE_CHILD_CLEARTID;
  long tid = syscall(SYS_CLONE, flags,
                     (unsigned long)pthread_bootstrap,
                     top,
                     (unsigned long)st,
                     0,
                     (unsigned long)&st->child_tid);
  if (tid < 0) {
    munmap(stack, stack_size);
    free(st);
    return EAGAIN;
  }
  st->child_tid = (int)tid;
  *thread = (pthread_t)(long)st;
  return 0;
}

void pthread_exit(void *retval) {
  /* The bootstrap stores retval; calling pthread_exit from main thread
   * is equivalent to SYS_EXIT for our purposes. The clean path is to
   * stash the retval where the joiner can find it — but our state struct
   * is keyed on the bootstrap and not reachable from arbitrary frames.
   * Issue a thread-only exit; if this is the main thread, the kernel
   * treats it as a process exit. */
  (void)retval;
  syscall(SYS_EXIT_THREAD, 0);
  __builtin_unreachable();
}

int pthread_join(pthread_t thread, void **retval) {
  struct pthread_state *st = (struct pthread_state *)(long)thread;
  if (!st) return EINVAL;
  if (st->detached) return EINVAL;

  /* Wait for the kernel to clear child_tid. The kernel publishes 0 +
   * futex_wake when the thread reaches scheduler_exit_current. We loop
   * because spurious returns are possible. */
  while (1) {
    int v = __atomic_load_n(&st->child_tid, __ATOMIC_ACQUIRE);
    if (v == 0) break;
    /* FUTEX_WAIT returns -EAGAIN if the value mismatched (the thread
     * raced us), 0 if we slept and got woken, -EINTR on signal — all
     * are handled by simply re-checking the value. */
    (void)syscall(SYS_FUTEX, &st->child_tid, FUTEX_WAIT, v);
  }

  if (retval) *retval = st->retval;
  if (st->stack_base) munmap(st->stack_base, st->stack_size);
  free(st);
  return 0;
}

int pthread_detach(pthread_t thread) {
  struct pthread_state *st = (struct pthread_state *)(long)thread;
  if (!st) return EINVAL;
  st->detached = 1;
  return 0;
}

pthread_t pthread_self(void) {
  /* The kernel TID is our pthread "id" only inside the kernel. Userspace
   * pthread_t is the state-pointer cast; we don't have a generic way to
   * find our own state struct without TLS keys. Return the kernel TID as
   * a coarse-but-unique handle — pthread_equal sees the same value. */
  return (pthread_t)syscall(SYS_GETTID);
}

int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

/* ── Mutex ── */

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr) {
  if (!m) return EINVAL;
  m->state = 0;
  m->kind = attr ? attr->kind : PTHREAD_MUTEX_NORMAL;
  m->owner = 0;
  m->recursion = 0;
  return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m) {
  if (!m) return EINVAL;
  if (m->state != 0) return EBUSY;
  return 0;
}

/* Three-state lock: 0=unlocked, 1=locked-no-waiter, 2=locked-with-waiter.
 * This is the standard "futex" pattern — avoids syscalls on the fast path. */
int pthread_mutex_lock(pthread_mutex_t *m) {
  if (!m) return EINVAL;
  pthread_t self = pthread_self();

  if (m->kind == PTHREAD_MUTEX_RECURSIVE) {
    /* Recursive: if we already own it, just bump the counter. The owner
     * read is benign — if we're not the owner, the value is irrelevant
     * (we'll go through the contended path and the real owner sees its
     * own value). */
    if (m->state != 0 &&
        __atomic_load_n(&m->owner, __ATOMIC_ACQUIRE) == self) {
      m->recursion++;
      return 0;
    }
  }

  int expected = 0;
  if (__atomic_compare_exchange_n(&m->state, &expected, 1, 0,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    /* Uncontended fast path. */
    __atomic_store_n(&m->owner, self, __ATOMIC_RELEASE);
    m->recursion = (m->kind == PTHREAD_MUTEX_RECURSIVE) ? 1 : 0;
    return 0;
  }

  /* Contended path. Mark "locked with waiter" and futex_wait. */
  int prev;
  do {
    /* If state is 1 (locked, no waiter), bump to 2 (locked, waiter). */
    int one = 1;
    if (__atomic_compare_exchange_n(&m->state, &one, 2, 0,
                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
      /* fall through to wait */
    }
    /* Wait while state == 2. */
    (void)syscall(SYS_FUTEX, &m->state, FUTEX_WAIT, 2);

    /* Try again — claim with waiter-bit set (2), since we may not be the
     * last waiter. */
    prev = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE);
  } while (prev != 0);

  __atomic_store_n(&m->owner, self, __ATOMIC_RELEASE);
  m->recursion = (m->kind == PTHREAD_MUTEX_RECURSIVE) ? 1 : 0;
  return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
  if (!m) return EINVAL;
  pthread_t self = pthread_self();
  if (m->kind == PTHREAD_MUTEX_RECURSIVE &&
      m->state != 0 &&
      __atomic_load_n(&m->owner, __ATOMIC_ACQUIRE) == self) {
    m->recursion++;
    return 0;
  }
  int expected = 0;
  if (__atomic_compare_exchange_n(&m->state, &expected, 1, 0,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    __atomic_store_n(&m->owner, self, __ATOMIC_RELEASE);
    m->recursion = (m->kind == PTHREAD_MUTEX_RECURSIVE) ? 1 : 0;
    return 0;
  }
  return EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
  if (!m) return EINVAL;
  if (m->kind == PTHREAD_MUTEX_RECURSIVE) {
    if (m->recursion > 1) { m->recursion--; return 0; }
  }
  __atomic_store_n(&m->owner, 0, __ATOMIC_RELEASE);
  m->recursion = 0;
  /* If state was 2, there are waiters — wake one. */
  int prev = __atomic_exchange_n(&m->state, 0, __ATOMIC_RELEASE);
  if (prev == 2) {
    (void)syscall(SYS_FUTEX, &m->state, FUTEX_WAKE, 1);
  }
  return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *a) {
  if (!a) return EINVAL;
  a->kind = PTHREAD_MUTEX_NORMAL;
  return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *a) {
  (void)a;
  return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type) {
  if (!a) return EINVAL;
  if (type != PTHREAD_MUTEX_NORMAL && type != PTHREAD_MUTEX_RECURSIVE)
    return EINVAL;
  a->kind = type;
  return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type) {
  if (!a || !type) return EINVAL;
  *type = a->kind;
  return 0;
}

/* ── Condition variable ── */

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr) {
  (void)attr;
  if (!c) return EINVAL;
  c->seq = 0;
  return 0;
}

int pthread_cond_destroy(pthread_cond_t *c) {
  (void)c;
  return 0;
}

int pthread_cond_signal(pthread_cond_t *c) {
  if (!c) return EINVAL;
  __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
  (void)syscall(SYS_FUTEX, &c->seq, FUTEX_WAKE, 1);
  return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) {
  if (!c) return EINVAL;
  __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
  /* Wake "everyone" — INT_MAX would be ideal; use a large number to be
   * compatible with our kernel's int-sized count. */
  (void)syscall(SYS_FUTEX, &c->seq, FUTEX_WAKE, 0x7fffffff);
  return 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
  if (!c || !m) return EINVAL;
  int seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
  pthread_mutex_unlock(m);
  /* Futex_wait will return EAGAIN if seq has already advanced (someone
   * signalled between unlock and wait) — that's a successful wait. */
  (void)syscall(SYS_FUTEX, &c->seq, FUTEX_WAIT, seq);
  pthread_mutex_lock(m);
  return 0;
}

/* ── Once ── */

int pthread_once(pthread_once_t *once, void (*init_routine)(void)) {
  if (!once || !init_routine) return EINVAL;
  int expected = 0;
  if (__atomic_compare_exchange_n(&once->state, &expected, 1, 0,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    init_routine();
    __atomic_store_n(&once->state, 2, __ATOMIC_RELEASE);
    (void)syscall(SYS_FUTEX, &once->state, FUTEX_WAKE, 0x7fffffff);
    return 0;
  }
  /* Wait until state == 2. */
  while (__atomic_load_n(&once->state, __ATOMIC_ACQUIRE) != 2) {
    (void)syscall(SYS_FUTEX, &once->state, FUTEX_WAIT, 1);
  }
  return 0;
}

/* ── Attributes (no-op) ── */

int pthread_attr_init(pthread_attr_t *a) {
  if (!a) return EINVAL;
  a->reserved = 0;
  return 0;
}

int pthread_attr_destroy(pthread_attr_t *a) {
  (void)a;
  return 0;
}
