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
  /* Set by FUTEX_WAKE (under the bucket lock) before it unblocks the task.
   * The waiter re-reads it AFTER publishing TASK_BLOCKED, which is what makes
   * the wake impossible to miss: a waker that runs before the publication is
   * seen through this flag, and one that runs after finds a task already
   * BLOCKED for its state CAS to flip. Without it a wake landing in the brief
   * window where the waiter is momentarily RUNNING inside the block helper was
   * simply lost, and the thread slept forever (the M29 stress-smp wedge). */
  volatile int woken;
  int expect; /* the value the waiter was told to sleep on (diagnostics) */
  /* Wakes that arrived for this address while this waiter was parked on it.
   *
   * "woken=0 after thirteen minutes" has two opposite explanations — nobody
   * ever called FUTEX_WAKE on that address, or somebody did and it failed to
   * reach this waiter — and they need opposite fixes. The global hit/missed
   * counters cannot tell them apart because they say nothing about WHICH
   * address. This one does. */
  volatile u64 wakes_seen;
  struct futex_waiter *next;
};

struct futex_bucket {
  spinlock_t lock;
  struct futex_waiter *head;
};

static struct futex_bucket g_futex[FUTEX_BUCKETS];
static u64 g_futex_wake_hit;
static u64 g_futex_wake_missed;

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

/* The address space a futex belongs to — or, for one in shared memory, the
 * page itself.
 *
 * Keying purely on the address space is right for a private futex and wrong
 * for a shared one: the same object mapped into two processes has two address
 * spaces, so a waiter queued by one and a wake issued by the other never meet.
 * Chromium puts mutexes in memory shared between its processes, and the result
 * was measurable — 230 wakes finding nobody against 68 that did, with every
 * thread parked on a lock nobody could release.
 *
 * For a shared page the frame number is the identity both sides agree on, so
 * it takes the place of the address space. It is stable for as long as the
 * mapping is: a shared page is never copied on write, which is exactly what
 * makes it shared. */
static u64 futex_key_pml4_for(u64 uaddr) {
  extern u64 vmm_query_leaf_pte(u64 vaddr);
  struct task *t = current_task;
  u64 pte = vmm_query_leaf_pte(uaddr & ~(u64)(PAGE_SIZE - 1));

  if ((pte & VMM_PRESENT) && (pte & VMM_SHARED)) {
    /* Marked so a frame number can never collide with a pml4 address. */
    return (pte & 0x000ffffffffff000ULL) | 1ull; /* frame, tagged */
  }
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

    u64 key_pml4 = futex_key_pml4_for(uaddr);
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
    w->woken = 0;
    w->expect = val;
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

    /* Park with the prepare/commit pattern: publish TASK_BLOCKED, then re-test
     * the wake flag, and only sleep if it is still clear. A wake that arrives
     * before the publication is caught by the flag; one that arrives after it
     * finds the task BLOCKED and flips it READY. */
    if (timeout_ticks)
      scheduler_wait_prepare_timeout(w, timeout_ticks);
    else
      scheduler_wait_prepare(w);
    if (__atomic_load_n(&w->woken, __ATOMIC_ACQUIRE))
      scheduler_wait_cancel();
    else
      scheduler_wait_commit();

    /* After wake: detach our waiter from the bucket if it's still there. A
     * FUTEX_WAKE unlinks the waiter BEFORE waking it, so finding our node
     * still linked means no wake reached us — either the timer deadline fired
     * or this was a spurious resume. */
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

    u64 key_pml4 = futex_key_pml4_for(uaddr);
    unsigned h = futex_hash(key_pml4, uaddr);
    struct futex_bucket *b = &g_futex[h];

    int woken = 0;
    u64 flags;
    spin_lock_irqsave(&b->lock, &flags);
    /* Note the attempt on every waiter parked on this address, whatever key it
     * was queued under — that mismatch is exactly what we are hunting. */
    for (struct futex_waiter *w = b->head; w; w = w->next)
      if (w->key_uaddr == uaddr)
        w->wakes_seen++;

    struct futex_waiter **pp = &b->head;
    while (*pp && woken < val) {
      struct futex_waiter *w = *pp;
      if (w->key_pml4 == key_pml4 && w->key_uaddr == uaddr) {
        *pp = w->next;
        /* Publish the wake and take the task id BEFORE dropping the lock: the
         * waiter frees `w` as soon as it runs, so touching it afterwards is a
         * use-after-free, and the flag has to be visible to a waiter that is
         * about to check it. */
        __atomic_store_n(&w->woken, 1, __ATOMIC_RELEASE);
        usize wake_id = w->task_id;
        spin_unlock_irqrestore(&b->lock, flags);
        scheduler_wake_task(wake_id);
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

    /* A wake that finds nobody is the signature of a lost wake-up: the waiter
     * queued under a key this caller did not compute, or queued after the
     * value changed. Counted rather than printed — a busy process wakes
     * thousands of times a second — and reported beside the parked waiters in
     * the watchdog dump, where the two numbers can be compared. */
    if (woken == 0) {
      g_futex_wake_missed++;
      /* A wake that finds nobody is ordinary — an uncontended unlock wakes
       * unconditionally. A wake that finds nobody while somebody IS parked on
       * that very address is not: it means the two sides computed different
       * keys for the same futex, and the sleeper is never coming back. Report
       * only that case, and only a few times. */
      static u64 reported;
      u64 f2;
      int same_addr = 0;
      spin_lock_irqsave(&b->lock, &f2);
      for (struct futex_waiter *w = b->head; w; w = w->next) {
        if (w->key_uaddr == uaddr && w->key_pml4 != key_pml4) {
          same_addr = 1;
          break;
        }
      }
      spin_unlock_irqrestore(&b->lock, f2);
      if (same_addr && ++reported <= 8) {
        console_write("futex: wake missed a waiter on the same address —"
                      " uaddr=0x");
        console_write_hex64(uaddr);
        console_write(" wake key=0x");
        console_write_hex64(key_pml4);
        console_write(" (waiter queued under a different key)\n");
      }
    } else {
      g_futex_wake_hit++;
    }
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

/* Diagnostic: print every parked futex waiter (called from the watchdog's task
 * dump). A wedged thread pool is almost always either "waiter still queued and
 * the word already changed" (a wake that never came) or "waiter gone but the
 * task still blocked" (a wake that was lost), and the two need different
 * fixes — this tells them apart without a rebuild. */
void futex_dump_waiters(void) {
  console_write("futex wakes: hit ");
  console_write_dec(g_futex_wake_hit);
  console_write(" missed ");
  console_write_dec(g_futex_wake_missed);
  console_write("\nfutex waiters:\n");
  for (unsigned h = 0; h < FUTEX_BUCKETS; h++) {
    struct futex_bucket *b = &g_futex[h];
    u64 flags;
    spin_lock_irqsave(&b->lock, &flags);
    for (struct futex_waiter *w = b->head; w; w = w->next) {
      console_write("  uaddr=");
      console_write_hex64(w->key_uaddr);
      console_write(" pml4=");
      console_write_hex64(w->key_pml4);
      console_write(" task=");
      console_write_dec(w->task_id);
      console_write(" wakes_seen=");
      console_write_dec(w->wakes_seen);
      console_write(" woken=");
      console_write_dec((u64)w->woken);
      console_write(" expect=");
      console_write_dec((u64)(u32)w->expect);
      /* Read the word the waiter is parked on through its own address space,
       * so a "the value already changed but no wake arrived" wedge is visible
       * directly in the dump. */
      {
        extern u64 paging_user_frame(u64 pml4_phys, u64 vaddr);
        extern u64 vmm_direct_map_base(void);
        u64 frame = paging_user_frame(w->key_pml4,
                                      w->key_uaddr & ~(u64)(PAGE_SIZE - 1));
        if (frame) {
          u32 cur = *(volatile u32 *)(usize)(frame + vmm_direct_map_base() +
                                             (w->key_uaddr & (PAGE_SIZE - 1)));
          console_write(" cur=");
          console_write_dec((u64)cur);
        } else {
          console_write(" cur=<unmapped>");
        }
      }
      console_write("\n");
    }
    spin_unlock_irqrestore(&b->lock, flags);
  }
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
