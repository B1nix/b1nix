/* M29: Fast userspace mutex (futex) — minimal in-kernel back-end for
 * pthread mutexes/condvars.
 *
 * Design
 * ------
 * A futex is keyed on (pml4_phys, uaddr): two threads of the same process
 * share the same pml4, so a userspace address maps to the same key for
 * every sibling. The kernel holds a small hash table; each bucket is a
 * singly-linked list of waiters. FUTEX_WAIT enqueues the current task
 * (after re-checking *uaddr == val under the bucket lock — closes the
 * classic wait/wake race), then blocks via scheduler_block_on(). FUTEX_WAKE
 * removes up to `val` waiters from the bucket and transitions each to
 * READY via the usual scheduler wake path.
 *
 * The full Linux futex op set (REQUEUE, CMP_REQUEUE, WAKE_OP, PI, ...) is
 * NOT implemented — only WAIT/WAKE, which is enough for the pthread
 * primitives this milestone needs (mutex / condvar / join).
 */

#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/syscall.h>
#include <string.h>

#define FUTEX_BUCKETS 64

struct futex_waiter {
  u64 key_pml4;
  u64 key_uaddr;
  usize task_id;
  void *wait_chan;
  struct futex_waiter *next;
};

struct futex_bucket {
  spinlock_t lock;
  struct futex_waiter *head;
};

static struct futex_bucket g_futex[FUTEX_BUCKETS];

/* Hash (pml4, uaddr) → bucket index. uaddr is at least 4-byte aligned for
 * futex; shift away the low bits to spread keys across buckets. */
static inline unsigned futex_hash(u64 pml4, u64 uaddr) {
  u64 h = (pml4 >> 12) ^ (uaddr >> 2) ^ (uaddr >> 14);
  return (unsigned)(h % FUTEX_BUCKETS);
}

/* The caller-visible API requires reading *uaddr from kernel mode. The
 * userspace address lives in the active task's pml4, which is the same
 * pml4 currently loaded (futex is always called from the owning task), so
 * a direct dereference is safe — same model as syscall_copyin elsewhere.
 * Returns 0 on success, -EFAULT if the page is not mapped. */
static int futex_read_word(u64 uaddr, int *out) {
  return syscall_copyin(out, (const void *)(usize)uaddr, sizeof(int));
}

static u64 futex_key_pml4(void) {
  struct task *t = current_task;
  return t ? t->pml4_phys : 0;
}

/* Scheduler tick cadence (TIMER_HZ) is 100 Hz → 10 ms per tick. */
#define FUTEX_MS_PER_TICK 10

int scheduler_futex(u64 uaddr, int op, int val, u64 timeout_ms) {
  if ((uaddr & 0x3) != 0) return -EINVAL;

  if (op == B1NIX_FUTEX_WAIT) {
    /* Check *uaddr == val. Re-check under the bucket lock to close the
     * wait/wake race. */
    int cur = 0;
    if (futex_read_word(uaddr, &cur) != 0) return -EFAULT;
    if (cur != val) return -EAGAIN;

    u64 key_pml4 = futex_key_pml4();
    unsigned h = futex_hash(key_pml4, uaddr);
    struct futex_bucket *b = &g_futex[h];

    u64 flags;
    spin_lock_irqsave(&b->lock, &flags);

    /* Re-check value under the lock. */
    if (futex_read_word(uaddr, &cur) != 0) {
      spin_unlock_irqrestore(&b->lock, flags);
      return -EFAULT;
    }
    if (cur != val) {
      spin_unlock_irqrestore(&b->lock, flags);
      return -EAGAIN;
    }

    struct futex_waiter *w = kzalloc(sizeof(*w));
    if (!w) {
      spin_unlock_irqrestore(&b->lock, flags);
      return -ENOMEM;
    }
    w->key_pml4 = key_pml4;
    w->key_uaddr = uaddr;
    w->task_id = current_task->id;
    /* Sleep channel: combine pml4+uaddr into a unique pointer-shaped
     * value that scheduler_wake_all can match on. We park on `w` itself —
     * the wake path will mark this waiter dequeued and wake by task_id. */
    w->wait_chan = (void *)w;
    w->next = b->head;
    b->head = w;
    spin_unlock_irqrestore(&b->lock, flags);

    /* Optional relative timeout: convert ms → scheduler ticks (round up so a
     * sub-tick request still waits at least one tick) and arm the deadline. */
    u64 timeout_ticks = 0;
    u64 deadline = 0;
    if (timeout_ms) {
      timeout_ticks = (timeout_ms + (FUTEX_MS_PER_TICK - 1)) / FUTEX_MS_PER_TICK;
      if (timeout_ticks == 0) timeout_ticks = 1;
      deadline = scheduler_get_ticks() + timeout_ticks;
    }

    /* Block until woken. scheduler_block_on[_timeout] yields with state=BLOCKED
     * and wait_chan=w; the wake path uses scheduler_wake_task by id
     * because the wait_chan pointer is private to this waiter struct. */
    if (timeout_ticks)
      scheduler_block_on_timeout(w, timeout_ticks);
    else
      scheduler_block_on(w);

    /* After wake: detach our waiter from the bucket if it's still there. A
     * FUTEX_WAKE always unlinks the waiter BEFORE waking it, so finding our
     * node still linked means no wake reached us — either the timer deadline
     * fired or this was a spurious resume. */
    int still_queued = 0;
    spin_lock_irqsave(&b->lock, &flags);
    struct futex_waiter **pp = &b->head;
    while (*pp) {
      if (*pp == w) { *pp = w->next; still_queued = 1; break; }
      pp = &(*pp)->next;
    }
    spin_unlock_irqrestore(&b->lock, flags);
    kfree(w);

    /* Report ETIMEDOUT only when we were still queued and the deadline has
     * actually elapsed; a spurious early wake returns 0 so the caller re-tests
     * its predicate and recomputes the remaining timeout. */
    if (still_queued && deadline && scheduler_get_ticks() >= deadline)
      return -ETIMEDOUT;
    return 0;
  }

  if (op == B1NIX_FUTEX_WAKE) {
    if (val < 0) return -EINVAL;
    if (val == 0) return 0;

    u64 key_pml4 = futex_key_pml4();
    unsigned h = futex_hash(key_pml4, uaddr);
    struct futex_bucket *b = &g_futex[h];

    int woken = 0;
    u64 flags;
    spin_lock_irqsave(&b->lock, &flags);
    struct futex_waiter **pp = &b->head;
    while (*pp && woken < val) {
      struct futex_waiter *w = *pp;
      if (w->key_pml4 == key_pml4 && w->key_uaddr == uaddr) {
        *pp = w->next;
        spin_unlock_irqrestore(&b->lock, flags);
        scheduler_wake_task(w->task_id);
        woken++;
        spin_lock_irqsave(&b->lock, &flags);
        /* Continue from head — the list may have changed; this is O(N)
         * but N is tiny in practice. */
        pp = &b->head;
        continue;
      }
      pp = &w->next;
    }
    spin_unlock_irqrestore(&b->lock, flags);
    return woken;
  }

  return -EINVAL;
}

/* Cross-mm wake: used by scheduler_exit_current (CLONE_CHILD_CLEARTID) to
 * wake a futex on a specific address without doing a value re-check. The
 * pml4 key comes from current_task; that's correct since the dying thread
 * still has its mm mapped. */
void scheduler_futex_wake_addr(u64 uaddr, int val) {
  (void)scheduler_futex(uaddr, B1NIX_FUTEX_WAKE, val, 0);
}

/* Remove every waiter owned by an exiting task from all buckets. A task killed
 * while parked in FUTEX_WAIT (woken by a signal, then terminated before it ran
 * the self-detach in scheduler_futex) would otherwise leave a stale waiter that
 * leaks and, worse, makes a later FUTEX_WAKE on the same key call
 * scheduler_wake_task() on a recycled task id. */
void scheduler_futex_cleanup_task(usize task_id) {
  for (unsigned h = 0; h < FUTEX_BUCKETS; h++) {
    struct futex_bucket *b = &g_futex[h];
    u64 flags;
    spin_lock_irqsave(&b->lock, &flags);
    struct futex_waiter **pp = &b->head;
    while (*pp) {
      if ((*pp)->task_id == task_id) {
        struct futex_waiter *w = *pp;
        *pp = w->next;
        kfree(w);
      } else {
        pp = &(*pp)->next;
      }
    }
    spin_unlock_irqrestore(&b->lock, flags);
  }
}
