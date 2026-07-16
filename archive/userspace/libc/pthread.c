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
#include <stdarg.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* glibc compatibility: indicates whether process is single-threaded.
 * 1 = single-threaded (fast path), 0 = multi-threaded.
 * Used by Skia and other libraries for lock-free optimizations. */
int __libc_single_threaded = 1;

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
  int exited;
  struct pthread_state *next_dead;
  /* Per-thread ELF TLS block (variant II). tls_tp is the thread pointer the
   * bootstrap installs as the FS base (0 = the image has no PT_TLS, fall back
   * to the legacy state-pointer placeholder). tls_map/tls_map_size is the
   * mmap'd backing, munmap'd at thread teardown. See build_thread_tls(). */
  unsigned long tls_tp;
  void *tls_map;
  size_t tls_map_size;
};

static struct pthread_state *g_dead_detached = NULL;
static pthread_mutex_t g_dead_detached_lock = PTHREAD_MUTEX_INITIALIZER;

static void __pthread_cleanup_dead_detached(void) {
  pthread_mutex_lock(&g_dead_detached_lock);
  struct pthread_state **curr = &g_dead_detached;
  while (*curr) {
    struct pthread_state *dead = *curr;
    if (__atomic_load_n(&dead->child_tid, __ATOMIC_ACQUIRE) == 0) {
      *curr = dead->next_dead;
      if (dead->tls_map) {
        munmap(dead->tls_map, dead->tls_map_size);
      }
      if (dead->stack_base) {
        munmap(dead->stack_base, dead->stack_size);
      }
      free(dead);
    } else {
      curr = &dead->next_dead;
    }
  }
  pthread_mutex_unlock(&g_dead_detached_lock);
}

/* Currently-active TLS pointer per thread. b1nix doesn't compile with
 * `-fpic` or ELF TLS sections (no .tdata yet), so we store the state
 * pointer at the bottom of the per-thread stack and point %fs at it. The
 * thread function reaches it via `%fs:0`. For now we don't expose
 * thread-local-storage to user code — the FS base just keeps the dispatch
 * correct so SYS_SET_TLS / arch_set_fs_base round-trip cleanly under the
 * smoke. */

/* ── Internal trampoline ── */

void __pthread_tsd_run_dtors(void);   /* defined with the TSD code below */
void __pthread_cancel_forget(long tid); /* defined with the cancellation code */

/* Build a per-thread ELF TLS block (x86 variant II) for `st`, mirroring the
 * layout the kernel loader gives the main thread (process.c): a region holding
 * [ .tdata | .tbss ] with the thread pointer (TP) just above it, TP[0] = TP (the
 * self pointer `mov %fs:0` reads), and thread-locals at negative offsets from
 * TP. The PT_TLS template (size/align + the .tdata init image) comes from the
 * kernel via SYS_GET_TLS_INFO; it's identical for every thread, so cache it.
 * Sets st->tls_tp (0 when the image has no TLS — keep the legacy placeholder).
 * Returns 0 on success, -1 on allocation failure. */
static int build_thread_tls(struct pthread_state *st) {
  static struct b1nix_tls_info info;
  static unsigned char *init_img; /* cached .tdata init image (filesz bytes) */
  static int probed, has_tls;

  st->tls_tp = 0;
  st->tls_map = 0;
  st->tls_map_size = 0;

  /* The PT_TLS template is identical for every thread, so fetch it once. Guard
   * the one-time probe: V8 (and other libraries) spawn threads concurrently, and
   * an unlocked probe could race two creators on init_img (one reading a
   * half-filled buffer). The lock is held only for the cold path. */
  if (!__atomic_load_n(&probed, __ATOMIC_ACQUIRE)) {
    static pthread_mutex_t probe_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&probe_lock);
    if (!probed) {
      if (syscall(SYS_GET_TLS_INFO, &info, 0, 0) == 0 && info.memsz > 0) {
        has_tls = 1;
        if (info.filesz) {
          init_img = (unsigned char *)malloc(info.filesz);
          if (init_img)
            syscall(SYS_GET_TLS_INFO, 0, init_img, info.filesz);
        }
      }
      __atomic_store_n(&probed, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&probe_lock);
  }
  if (!has_tls)
    return 0;

  unsigned long align = info.align < 8 ? 8 : info.align;
  unsigned long tls_size = (info.memsz + align - 1) & ~(align - 1);
  unsigned long tcb = 64; /* self pointer + spare slots */
  /* +align of slack so we can align `region` up within the mapping. */
  unsigned long want = tls_size + tcb + align;
  unsigned long map_size = (want + 0xFFF) & ~0xFFFUL;

  unsigned char *blk = (unsigned char *)mmap(0, map_size, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (blk == (void *)-1)
    return -1;

  uintptr_t region = ((uintptr_t)blk + align - 1) & ~(uintptr_t)(align - 1);
  /* TP sits at region + the UN-rounded memsz: the b1nix linker emits local-exec
   * offsets as `symbol_offset - p_memsz`, so a rounded TP would shift every
   * thread-local read by the alignment padding when memsz isn't an align
   * multiple (e.g. V8 wasm d8: memsz=0x108, align=0x10). tls_size (rounded) is
   * still used for the mapping size. Matches kernel/user/process.c. */
  uintptr_t tp = region + info.memsz;
  /* mmap memory is zero-filled (so .tbss is already clear); lay the .tdata init
   * image at the bottom of the region. */
  if (info.filesz && init_img)
    memcpy((void *)region, init_img, info.filesz);
  *(uintptr_t *)tp = tp; /* variant-II self pointer at TP[0] */

  st->tls_tp = tp;
  st->tls_map = blk;
  st->tls_map_size = map_size;
  return 0;
}

/* --- glibc-style clone() ----------------------------------------------------
 * b1nix SYS_CLONE jumps the child to entry(arg) on a fresh stack and a returning
 * entry would `ret` to the kernel's zeroed return slot (= a fault), so we can't
 * hand the user's fn straight to the kernel. Instead the child enters a
 * trampoline that calls fn(arg) and then SYS_EXIT_THREADs with the result —
 * matching the glibc contract (fn-return => exit). fn+arg are stashed in a
 * 16-byte slot at the top of the child stack (above the kernel's rsp), reachable
 * via shared (CLONE_VM) or COW-inherited (fork-like) memory. */
struct __clone_args { int (*fn)(void *); void *arg; };

__attribute__((noreturn))
static void __clone_trampoline(void *p) {
  struct __clone_args *a = (struct __clone_args *)p;
  int (*fn)(void *) = a->fn;
  void *arg = a->arg;
  int r = fn(arg);
  syscall(SYS_EXIT_THREAD, (long)r);
  __builtin_unreachable();
}

int clone(int (*fn)(void *), void *stack, int flags, void *arg, ...) {
  va_list ap;
  va_start(ap, arg);
  pid_t *ptid = va_arg(ap, pid_t *);
  void *tls = va_arg(ap, void *);
  pid_t *ctid = va_arg(ap, pid_t *);
  va_end(ap);

  if (!fn || !stack) { errno = EINVAL; return -1; }

  /* Reserve the top 16 bytes (below the 16-aligned top) for {fn,arg}; the child
   * runs with rsp = top-8 (kernel realigns), so this slot sits above it. */
  unsigned long top = ((unsigned long)stack & ~15UL) - 16;
  struct __clone_args *a = (struct __clone_args *)(unsigned long)top;
  a->fn = fn;
  a->arg = arg;

  long tid = syscall(SYS_CLONE,
                     (unsigned long)(unsigned int)flags,
                     (unsigned long)__clone_trampoline,
                     top,
                     (unsigned long)a,
                     (unsigned long)tls,
                     (unsigned long)ctid);
  if (tid < 0) { errno = (int)-tid; return -1; }

  /* b1nix SYS_CLONE has no parent-tid slot; emulate CLONE_PARENT_SETTID. */
  if (ptid && (flags & CLONE_PARENT_SETTID)) *ptid = (pid_t)tid;
  return (int)tid;
}

static void pthread_bootstrap(void *raw) {
  struct pthread_state *st = (struct pthread_state *)raw;

  /* Publish the %fs base. With a real per-thread TLS block (st->tls_tp, built in
   * pthread_create) ELF thread-locals work in spawned threads exactly as on the
   * main thread; without one (image has no PT_TLS) fall back to the legacy
   * state-pointer placeholder. The libc itself never reads %fs:0 (pthread_self
   * uses SYS_GETTID), so either base is safe for libc internals. */
  syscall(SYS_SET_TLS, st->tls_tp ? (void *)st->tls_tp : (void *)st);

  void *r = st->start_routine(st->arg);
  st->retval = r;
  __pthread_tsd_run_dtors(); /* POSIX: TSD destructors run at thread exit */
  __pthread_cancel_forget(syscall(SYS_GETTID));

  pthread_mutex_lock(&g_dead_detached_lock);
  st->exited = 1;
  if (st->detached) {
    st->next_dead = g_dead_detached;
    g_dead_detached = st;
  }
  pthread_mutex_unlock(&g_dead_detached_lock);

  syscall(SYS_EXIT_THREAD, 0);
  __builtin_unreachable();
}

#define MIN_STACK_SIZE (16 * 1024)

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
  if (!thread || !start_routine) return EINVAL;

  /* Honor caller-supplied attributes: an explicit stack size (clamped to a
   * sane minimum and page-aligned) and the detach state. A NULL attr means
   * the defaults (libc stack size, joinable). */
  size_t stack_size = DEFAULT_STACK_SIZE;
  int detached = 0;
  if (attr) {
    if (attr->stack_size != 0) {
      stack_size = attr->stack_size;
      if (stack_size < MIN_STACK_SIZE)
        stack_size = MIN_STACK_SIZE;
    }
    detached = (attr->detach_state == PTHREAD_CREATE_DETACHED);
  }
  /* Round the stack up to a page so mmap gets a whole number of pages. */
  stack_size = (stack_size + 0xFFF) & ~(size_t)0xFFF;

  __pthread_cleanup_dead_detached();

  struct pthread_state *st = (struct pthread_state *)malloc(sizeof(*st));
  if (!st) return EAGAIN;
  st->child_tid = 0;
  st->detached = detached;
  st->retval = 0;
  st->start_routine = start_routine;
  st->arg = arg;
  st->exited = 0;
  st->next_dead = NULL;

  void *stack = mmap(0, stack_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (stack == (void *)-1) {
    free(st);
    return EAGAIN;
  }
  st->stack_base = stack;
  st->stack_size = stack_size;

  /* Build this thread's ELF TLS block up front (in the parent) so the bootstrap
   * just installs the ready thread pointer. Failure here is non-fatal — leave
   * tls_tp 0 and fall back to the placeholder base (only matters for binaries
   * that actually use thread-locals in spawned threads). */
  build_thread_tls(st);

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
  __pthread_tsd_run_dtors(); /* POSIX: TSD destructors run at thread exit */
  __pthread_cancel_forget(syscall(SYS_GETTID));
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
  if (st->tls_map) munmap(st->tls_map, st->tls_map_size);
  if (st->stack_base) munmap(st->stack_base, st->stack_size);
  free(st);
  return 0;
}

int pthread_detach(pthread_t thread) {
  struct pthread_state *st = (struct pthread_state *)(long)thread;
  if (!st) return EINVAL;

  pthread_mutex_lock(&g_dead_detached_lock);
  if (st->detached) {
    pthread_mutex_unlock(&g_dead_detached_lock);
    return EINVAL;
  }
  st->detached = 1;
  if (st->exited) {
    st->next_dead = g_dead_detached;
    g_dead_detached = st;
  }
  pthread_mutex_unlock(&g_dead_detached_lock);
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

/* Send a signal to a thread. b1nix pthread_t is the kernel TID; route through
 * kill() on that TID (no separate tkill syscall). Best-effort: adequate for
 * profilers that signal a sampled thread. */
int pthread_kill(pthread_t thread, int sig) {
  int rc = kill((int)thread, sig);
  if (rc < 0) return errno;
  return 0;
}

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

/* Milliseconds from now until an absolute CLOCK_REALTIME deadline. Returns 0 if
 * the deadline is already in the past (caller should treat as timed out). */
static long __abstime_rel_ms(const struct timespec *abstime) {
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  long ms = (abstime->tv_sec - now.tv_sec) * 1000L +
            (abstime->tv_nsec - now.tv_nsec) / 1000000L;
  return ms > 0 ? ms : 0;
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *abstime) {
  if (!c || !m) return EINVAL;
  if (!abstime) return pthread_cond_wait(c, m);

  int seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
  long ms = __abstime_rel_ms(abstime);
  pthread_mutex_unlock(m);

  int ret = 0;
  if (ms == 0) {
    /* Deadline already elapsed — still must re-acquire the mutex per POSIX. */
    ret = ETIMEDOUT;
  } else {
    /* Timed futex wait. Returns -ETIMEDOUT on expiry, -EAGAIN if seq already
     * advanced (signalled before we parked) — both leave the caller to re-test
     * its predicate under the mutex. */
    long rc = syscall(SYS_FUTEX, &c->seq, FUTEX_WAIT, seq, ms);
    if (rc == -ETIMEDOUT)
      ret = ETIMEDOUT;
  }

  pthread_mutex_lock(m);
  return ret;
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
  a->stack_size = 0; /* 0 → libc default */
  a->detach_state = PTHREAD_CREATE_JOINABLE;
  a->stack_addr = 0;
  return 0;
}

int pthread_attr_destroy(pthread_attr_t *a) {
  (void)a;
  return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *a, size_t stacksize) {
  if (!a || stacksize < MIN_STACK_SIZE) return EINVAL;
  a->stack_size = stacksize;
  return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *stacksize) {
  if (!a || !stacksize) return EINVAL;
  *stacksize = a->stack_size ? a->stack_size : (size_t)DEFAULT_STACK_SIZE;
  return 0;
}

int pthread_attr_getstack(const pthread_attr_t *a, void **stackaddr,
                          size_t *stacksize) {
  if (!a || !stackaddr || !stacksize) return EINVAL;
  *stackaddr = a->stack_addr;
  *stacksize = a->stack_size ? a->stack_size : (size_t)DEFAULT_STACK_SIZE;
  return 0;
}

/* Parse a leading "<hex>-<hex>" range from a /proc/self/maps line. */
static int parse_maps_range(const char *line, uintptr_t *start, uintptr_t *end) {
  uintptr_t s = 0, e = 0;
  const char *p = line;
  int n = 0;
  for (; *p && *p != '-'; p++) {
    int d;
    if (*p >= '0' && *p <= '9') d = *p - '0';
    else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
    else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
    else return 0;
    s = (s << 4) | (uintptr_t)d; n++;
  }
  if (*p != '-' || n == 0) return 0;
  p++; n = 0;
  for (; *p && *p != ' '; p++) {
    int d;
    if (*p >= '0' && *p <= '9') d = *p - '0';
    else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
    else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
    else return 0;
    e = (e << 4) | (uintptr_t)d; n++;
  }
  if (n == 0) return 0;
  *start = s; *end = e;
  return 1;
}

/* glibc extension: report the calling thread's stack region. b1nix's pthread_t
 * is the kernel TID with no TID->state map, so this only supports the *current*
 * thread (it locates the stack by finding the VMA that contains the current
 * frame in /proc/self/maps). That is exactly how V8 uses it. */
int pthread_getattr_np(pthread_t thread, pthread_attr_t *a) {
  if (!a) return EINVAL;
  (void)thread;
  pthread_attr_init(a);
  uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
  int found = 0;
  int fd = open("/proc/self/maps", O_RDONLY);
  if (fd >= 0) {
    char buf[512];
    char line[256];
    size_t ll = 0;
    ssize_t n;
    while (!found && (n = read(fd, buf, sizeof buf)) > 0) {
      for (ssize_t i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n' || ll == sizeof(line) - 1) {
          line[ll] = 0;
          uintptr_t start = 0, end = 0;
          if (parse_maps_range(line, &start, &end) && sp >= start &&
              sp < end) {
            a->stack_addr = (void *)start;
            a->stack_size = end - start;
            found = 1;
            break;
          }
          ll = 0;
        } else {
          line[ll++] = c;
        }
      }
    }
    close(fd);
  }
  if (!found) {
    /* Fallback: a conservative window around the current frame so the high end
     * (stack_addr + stack_size) stays above the live stack. */
    uintptr_t page = sp & ~(uintptr_t)0xFFF;
    a->stack_addr = (void *)(page - (uintptr_t)DEFAULT_STACK_SIZE);
    a->stack_size = (size_t)DEFAULT_STACK_SIZE + 0x2000;
  }
  return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *a, int *detachstate) {
  if (!a || !detachstate) return EINVAL;
  *detachstate = a->detach_state;
  return 0;
}

/* ── Thread-specific data (TLS keys) + timed mutex lock ──────────────────────
 * ponytail: fixed table — PTHREAD_KEYS_MAX keys x TSD_MAX_THREADS threads,
 * linear tid scan, one global lock. Right for ports like Mesa (a handful of
 * keys, few worker threads). Grow the bounds or switch to a real TLS slab if a
 * future port needs many threads/keys. */
static struct {
  int used;
  void (*dtor)(void *);
} g_keys[PTHREAD_KEYS_MAX];

struct tsd_thread {
  long tid;
  const void *vals[PTHREAD_KEYS_MAX];
  struct tsd_thread *next;
};

static struct tsd_thread *g_tsd_head = NULL;

static pthread_mutex_t g_tsd_lock = PTHREAD_MUTEX_INITIALIZER;

static struct tsd_thread *tsd_find(long tid, int create) {
  struct tsd_thread *curr = g_tsd_head;
  while (curr) {
    if (curr->tid == tid)
      return curr;
    curr = curr->next;
  }
  if (!create)
    return NULL;
  struct tsd_thread *new_thread = (struct tsd_thread *)malloc(sizeof(struct tsd_thread));
  if (!new_thread)
    return NULL;
  new_thread->tid = tid;
  new_thread->next = g_tsd_head;
  for (int k = 0; k < PTHREAD_KEYS_MAX; k++)
    new_thread->vals[k] = NULL;
  g_tsd_head = new_thread;
  return new_thread;
}

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
  pthread_mutex_lock(&g_tsd_lock);
  for (unsigned i = 0; i < PTHREAD_KEYS_MAX; i++) {
    if (!g_keys[i].used) {
      g_keys[i].used = 1;
      g_keys[i].dtor = destructor;
      *key = i;
      pthread_mutex_unlock(&g_tsd_lock);
      return 0;
    }
  }
  pthread_mutex_unlock(&g_tsd_lock);
  return EAGAIN;
}

int pthread_key_delete(pthread_key_t key) {
  if (key >= PTHREAD_KEYS_MAX)
    return EINVAL;
  pthread_mutex_lock(&g_tsd_lock);
  g_keys[key].used = 0;
  g_keys[key].dtor = NULL;
  struct tsd_thread *curr = g_tsd_head;
  while (curr) {
    curr->vals[key] = NULL;
    curr = curr->next;
  }
  pthread_mutex_unlock(&g_tsd_lock);
  return 0;
}

int pthread_setspecific(pthread_key_t key, const void *value) {
  if (key >= PTHREAD_KEYS_MAX || !g_keys[key].used)
    return EINVAL;
  long tid = (long)pthread_self();
  pthread_mutex_lock(&g_tsd_lock);
  struct tsd_thread *t = tsd_find(tid, 1);
  int rc = t ? 0 : ENOMEM;
  if (t)
    t->vals[key] = value;
  pthread_mutex_unlock(&g_tsd_lock);
  return rc;
}

void *pthread_getspecific(pthread_key_t key) {
  if (key >= PTHREAD_KEYS_MAX)
    return NULL;
  long tid = (long)pthread_self();
  pthread_mutex_lock(&g_tsd_lock);
  struct tsd_thread *t = tsd_find(tid, 0);
  void *v = t ? (void *)t->vals[key] : NULL;
  pthread_mutex_unlock(&g_tsd_lock);
  return v;
}

/* ── C++11 thread_local destructors (__cxa_thread_atexit_impl) ────────────────
 * The C++ runtime (libc++abi / libstdc++) registers a destructor for every
 * thread_local object with a non-trivial destructor via __cxa_thread_atexit_impl,
 * which the Itanium C++ ABI expects the C library to provide. Each destructor
 * runs when its owning thread terminates — and at process exit for the main
 * thread, which leaves via exit() rather than pthread_exit(). We keep a per-thread
 * LIFO list keyed by tid and run it from __cxa_thread_run_dtors(), called from the
 * worker thread-exit path (__pthread_tsd_run_dtors, below) and from exit(). */
struct thread_atexit_entry {
  void (*func)(void *);
  void *obj;
  struct thread_atexit_entry *next;
};
struct thread_atexit_list {
  long tid;
  struct thread_atexit_entry *head; /* LIFO */
  struct thread_atexit_list *next;
};
static struct thread_atexit_list *g_thread_atexit = NULL; /* guarded by g_tsd_lock */

static struct thread_atexit_list *thread_atexit_find(long tid, int create) {
  struct thread_atexit_list *c = g_thread_atexit;
  while (c) {
    if (c->tid == tid)
      return c;
    c = c->next;
  }
  if (!create)
    return NULL;
  struct thread_atexit_list *n =
      (struct thread_atexit_list *)malloc(sizeof(*n));
  if (!n)
    return NULL;
  n->tid = tid;
  n->head = NULL;
  n->next = g_thread_atexit;
  g_thread_atexit = n;
  return n;
}

int __cxa_thread_atexit_impl(void (*func)(void *), void *obj,
                             void *dso_handle) {
  (void)dso_handle; /* b1nix unloads no DSOs before exit, so dso_handle is moot */
  struct thread_atexit_entry *e =
      (struct thread_atexit_entry *)malloc(sizeof(*e));
  if (!e)
    return -1;
  e->func = func;
  e->obj = obj;
  long tid = (long)pthread_self();
  pthread_mutex_lock(&g_tsd_lock);
  struct thread_atexit_list *l = thread_atexit_find(tid, 1);
  if (!l) {
    pthread_mutex_unlock(&g_tsd_lock);
    free(e);
    return -1;
  }
  e->next = l->head; /* LIFO: last registered runs first */
  l->head = e;
  pthread_mutex_unlock(&g_tsd_lock);
  return 0;
}

/* Run (and free) the calling thread's thread_local destructors, LIFO. Drops the
 * lock around each destructor — a destructor may itself register more or re-enter
 * pthread_*. Safe to call when the thread registered none. */
void __cxa_thread_run_dtors(void) {
  long tid = (long)pthread_self();
  for (;;) {
    pthread_mutex_lock(&g_tsd_lock);
    struct thread_atexit_list *l = thread_atexit_find(tid, 0);
    if (!l || !l->head) {
      if (l) { /* unlink the now-empty per-thread node */
        struct thread_atexit_list **pp = &g_thread_atexit;
        while (*pp) {
          if (*pp == l) {
            *pp = l->next;
            free(l);
            break;
          }
          pp = &(*pp)->next;
        }
      }
      pthread_mutex_unlock(&g_tsd_lock);
      return;
    }
    struct thread_atexit_entry *e = l->head;
    l->head = e->next;
    pthread_mutex_unlock(&g_tsd_lock);
    e->func(e->obj);
    free(e);
  }
}

/* Run the calling thread's TSD destructors and free its slot. Called from
 * pthread_exit(). POSIX iterates destructors a bounded number of times because
 * a destructor may set new values. */
void __pthread_tsd_run_dtors(void) {
  long tid = (long)pthread_self();
  /* C++ thread_local destructors run before POSIX TSD (pthread_key) ones. */
  __cxa_thread_run_dtors();
  for (int iter = 0; iter < PTHREAD_DESTRUCTOR_ITERATIONS; iter++) {
    int ran = 0;
    pthread_mutex_lock(&g_tsd_lock);
    struct tsd_thread *t = tsd_find(tid, 0);
    if (!t) {
      pthread_mutex_unlock(&g_tsd_lock);
      return;
    }
    for (unsigned k = 0; k < PTHREAD_KEYS_MAX; k++) {
      void *v = (void *)t->vals[k];
      if (v && g_keys[k].used && g_keys[k].dtor) {
        void (*dtor)(void *) = g_keys[k].dtor;
        t->vals[k] = NULL;
        pthread_mutex_unlock(&g_tsd_lock);
        dtor(v); /* may re-enter pthread_*; must not hold the lock */
        ran = 1;
        pthread_mutex_lock(&g_tsd_lock);
        t = tsd_find(tid, 0);
        if (!t)
          break;
      }
    }
    pthread_mutex_unlock(&g_tsd_lock);
    if (!ran)
      break;
  }
  pthread_mutex_lock(&g_tsd_lock);
  struct tsd_thread **curr = &g_tsd_head;
  while (*curr) {
    if ((*curr)->tid == tid) {
      struct tsd_thread *to_free = *curr;
      *curr = to_free->next;
      free(to_free);
      break;
    }
    curr = &(*curr)->next;
  }
  pthread_mutex_unlock(&g_tsd_lock);
}

int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abstime) {
  if (!m) return EINVAL;
  /* Fast/recursive path. */
  if (pthread_mutex_trylock(m) == 0)
    return 0;
  if (!abstime)
    return pthread_mutex_lock(m); /* no deadline → block indefinitely */

  /* Contended path mirroring pthread_mutex_lock, but each futex park is bounded
   * by the remaining time until abstime. No busy-spin: the thread sleeps in the
   * kernel and is woken either by an unlock or by the timer deadline. */
  pthread_t self = pthread_self();
  int prev;
  do {
    int one = 1;
    __atomic_compare_exchange_n(&m->state, &one, 2, 0,
                                __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    long ms = __abstime_rel_ms(abstime);
    if (ms == 0)
      return ETIMEDOUT;
    long rc = syscall(SYS_FUTEX, &m->state, FUTEX_WAIT, 2, ms);
    if (rc == -ETIMEDOUT)
      return ETIMEDOUT;
    prev = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE);
  } while (prev != 0);

  __atomic_store_n(&m->owner, self, __ATOMIC_RELEASE);
  m->recursion = (m->kind == PTHREAD_MUTEX_RECURSIVE) ? 1 : 0;
  return 0;
}

/* ── Barrier ── (mutex + condvar; phase counter avoids lost wakeups) */
int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *attr,
                         unsigned count) {
  (void)attr;
  if (!b || count == 0)
    return EINVAL;
  pthread_mutex_init(&b->lock, NULL);
  pthread_cond_init(&b->cond, NULL);
  b->count = count;
  b->waiting = 0;
  b->phase = 0;
  return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *b) {
  if (!b)
    return EINVAL;
  pthread_mutex_destroy(&b->lock);
  pthread_cond_destroy(&b->cond);
  return 0;
}

int pthread_barrier_wait(pthread_barrier_t *b) {
  if (!b)
    return EINVAL;
  pthread_mutex_lock(&b->lock);
  unsigned phase = b->phase;
  if (++b->waiting == b->count) {
    b->phase++;
    b->waiting = 0;
    pthread_cond_broadcast(&b->cond);
    pthread_mutex_unlock(&b->lock);
    return PTHREAD_BARRIER_SERIAL_THREAD;
  }
  while (phase == b->phase)
    pthread_cond_wait(&b->cond, &b->lock);
  pthread_mutex_unlock(&b->lock);
  return 0;
}

/* ── Read-write lock ── (mutex-backed)
 * ponytail: a single mutex serializes readers too — correct but no read
 * parallelism. Right for a 1-worker software renderer; swap for a real
 * reader-count + writer rwlock if a port becomes read-contended. */
int pthread_rwlock_init(pthread_rwlock_t *rw, const pthread_rwlockattr_t *attr) {
  (void)attr;
  if (!rw)
    return EINVAL;
  return pthread_mutex_init(&rw->mtx, NULL);
}
int pthread_rwlock_destroy(pthread_rwlock_t *rw) {
  return rw ? pthread_mutex_destroy(&rw->mtx) : EINVAL;
}
int pthread_rwlock_rdlock(pthread_rwlock_t *rw) {
  return rw ? pthread_mutex_lock(&rw->mtx) : EINVAL;
}
int pthread_rwlock_wrlock(pthread_rwlock_t *rw) {
  return rw ? pthread_mutex_lock(&rw->mtx) : EINVAL;
}
int pthread_rwlock_tryrdlock(pthread_rwlock_t *rw) {
  return rw ? pthread_mutex_trylock(&rw->mtx) : EINVAL;
}
int pthread_rwlock_trywrlock(pthread_rwlock_t *rw) {
  return rw ? pthread_mutex_trylock(&rw->mtx) : EINVAL;
}
int pthread_rwlock_unlock(pthread_rwlock_t *rw) {
  return rw ? pthread_mutex_unlock(&rw->mtx) : EINVAL;
}

/* ── Deferred cancellation ───────────────────────────────────────────────────
 * b1nix has no kernel-level thread cancellation, so cancellation is delivered
 * cooperatively: pthread_cancel() flags the target, and the target acts on the
 * flag the next time it passes a cancellation point (pthread_testcancel, or the
 * join/cond_wait/sleep wrappers). State is tracked per kernel TID — the same
 * key both sides can compute (the canceller resolves the target's TID from its
 * pthread_state handle; the target uses gettid()), which sidesteps the
 * self()=TID vs create()=state-pointer duality of pthread_t. */
struct cancel_entry {
  long tid;
  volatile int requested;
  int disabled;            /* PTHREAD_CANCEL_DISABLE */
  struct cancel_entry *next;
};
static struct cancel_entry *g_cancel_head = NULL;
static pthread_mutex_t g_cancel_lock = PTHREAD_MUTEX_INITIALIZER;

static struct cancel_entry *cancel_find(long tid, int create) {
  pthread_mutex_lock(&g_cancel_lock);
  for (struct cancel_entry *e = g_cancel_head; e; e = e->next) {
    if (e->tid == tid) { pthread_mutex_unlock(&g_cancel_lock); return e; }
  }
  struct cancel_entry *e = NULL;
  if (create) {
    e = (struct cancel_entry *)malloc(sizeof(*e));
    if (e) {
      e->tid = tid;
      e->requested = 0;
      e->disabled = 0;
      e->next = g_cancel_head;
      g_cancel_head = e;
    }
  }
  pthread_mutex_unlock(&g_cancel_lock);
  return e;
}

/* Drop a thread's cancellation record on exit so a recycled TID starts clean. */
void __pthread_cancel_forget(long tid) {
  pthread_mutex_lock(&g_cancel_lock);
  struct cancel_entry **pp = &g_cancel_head;
  while (*pp) {
    if ((*pp)->tid == tid) {
      struct cancel_entry *dead = *pp;
      *pp = dead->next;
      free(dead);
      break;
    }
    pp = &(*pp)->next;
  }
  pthread_mutex_unlock(&g_cancel_lock);
}

/* Resolve a pthread_t to a kernel TID. pthread_create() hands back the
 * heap-allocated state pointer (>= the 0x2000000 userspace load base), whereas
 * pthread_self() returns the small kernel TID directly — the value range tells
 * them apart unambiguously. */
static long cancel_target_tid(pthread_t thread) {
  unsigned long v = (unsigned long)(long)thread;
  if (v >= 0x2000000UL) {
    struct pthread_state *st = (struct pthread_state *)(long)thread;
    return (long)st->child_tid;
  }
  return (long)v;
}

int pthread_cancel(pthread_t thread) {
  long tid = cancel_target_tid(thread);
  if (tid <= 0) return ESRCH;
  struct cancel_entry *e = cancel_find(tid, 1);
  if (!e) return EAGAIN;
  __atomic_store_n(&e->requested, 1, __ATOMIC_RELEASE);
  return 0;
}

int pthread_setcancelstate(int state, int *oldstate) {
  if (state != PTHREAD_CANCEL_ENABLE && state != PTHREAD_CANCEL_DISABLE)
    return EINVAL;
  struct cancel_entry *e = cancel_find(syscall(SYS_GETTID), 1);
  if (!e) return EAGAIN;
  if (oldstate)
    *oldstate = e->disabled ? PTHREAD_CANCEL_DISABLE : PTHREAD_CANCEL_ENABLE;
  e->disabled = (state == PTHREAD_CANCEL_DISABLE);
  return 0;
}

/* b1nix delivers cancellation only at cancellation points, so async vs deferred
 * are equivalent here; the call is accepted for source compatibility. */
int pthread_setcanceltype(int type, int *oldtype) {
  if (type != PTHREAD_CANCEL_DEFERRED && type != PTHREAD_CANCEL_ASYNCHRONOUS)
    return EINVAL;
  if (oldtype)
    *oldtype = PTHREAD_CANCEL_DEFERRED;
  return 0;
}

void pthread_testcancel(void) {
  struct cancel_entry *e = cancel_find(syscall(SYS_GETTID), 0);
  if (e && !e->disabled && __atomic_load_n(&e->requested, __ATOMIC_ACQUIRE))
    pthread_exit(PTHREAD_CANCELED);
}
int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate) {
  if (!attr) return EINVAL;
  if (detachstate != PTHREAD_CREATE_JOINABLE &&
      detachstate != PTHREAD_CREATE_DETACHED)
    return EINVAL;
  attr->detach_state = detachstate;
  return 0;
}
int pthread_getschedparam(pthread_t thread, int *policy,
                          struct sched_param *param) {
  (void)thread;
  if (policy)
    *policy = SCHED_OTHER;
  if (param)
    param->sched_priority = 0;
  return 0;
}
int pthread_setschedparam(pthread_t thread, int policy,
                          const struct sched_param *param) {
  (void)thread;
  (void)policy;
  (void)param;
  return 0;
}

/* Thread-utility shims used by ports (Mesa's u_thread). b1nix has no per-thread
 * signal masks distinct from the process mask, thread names, or per-thread CPU
 * clocks — these map to the process equivalents or no-op. */
int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
  return sigprocmask(how, set, oldset);
}
int pthread_setname_np(pthread_t thread, const char *name) {
  (void)thread;
  (void)name;
  return 0;
}
int pthread_getname_np(pthread_t thread, char *name, size_t len) {
  (void)thread;
  if (name && len > 0)
    name[0] = '\0';
  return 0;
}
int pthread_getcpuclockid(pthread_t thread, clockid_t *clock_id) {
  (void)thread;
  if (clock_id)
    *clock_id = CLOCK_MONOTONIC;
  return 0;
}
