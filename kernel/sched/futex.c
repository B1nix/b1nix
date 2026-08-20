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

#include <b1nix/vfs.h>
#include <b1nix/bootinfo.h>
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
  /* The word's identity within its key: a file offset for a futex in a shared
   * object, the virtual address for a private one. Two processes that mapped
   * the same object at different addresses must produce the same value here. */
  u64 key_uaddr;
  /* Where this waiter sees the word in its own address space, and in which
   * address space — the key alone no longer says, and the dump below reads the
   * live value through them. */
  u64 diag_vaddr;
  u64 diag_pml4;
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
  /* Which bucket this waiter is currently linked into. A requeue moves it to
   * another one, and the waiter has to know where to unlink itself from. */
  volatile unsigned bucket;
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
#define FUTEX_MISSED_LOG 32
static u64 g_futex_missed_addr[FUTEX_MISSED_LOG];
static unsigned g_futex_missed_pos;

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
/* Identity of a shared futex, and the offset within it, or 0 when the address
 * is not in a shared mapping.
 *
 * Keying a shared futex by its page frame was right only while the page
 * happened to be resident in the caller's address space, and only while every
 * participant mapped it at the same virtual address. Neither holds: a process
 * that has not yet touched the page has no frame to name, so its wake was
 * keyed on its own pml4 and never met the waiter that had one; and two
 * processes mapping the same object at different addresses produced two
 * different (frame, uaddr) pairs for the same futex word. Both show up as a
 * wake that finds nobody — 176 of them against 159 that landed, with the
 * browser's threads parked on locks that had already been released.
 *
 * Linux keys a shared futex by the backing object and the offset into it. So
 * does this: the inode identifies the object across address spaces, and the
 * file offset identifies the word regardless of where each process mapped it. */
static int futex_shared_key(u64 uaddr, u64 *obj_out, u64 *off_out) {
  extern u64 vmm_query_leaf_pte(u64 vaddr);
  struct task *t = current_task;

  if (!t)
    return 0;

  /* Cheap disqualification before the expensive one. Walking the VMA list is
   * O(mappings), the browser has thousands of them, and it is done under the
   * lock the page-fault handler also needs — doing it on every futex call cost
   * more in fault latency than the wrong key ever cost in lost wakes. A page
   * that is present and not marked shared cannot be in a shared mapping, so
   * the walk is only needed when the leaf says shared, or says nothing. */
  u64 pte = vmm_query_leaf_pte(uaddr & ~(u64)(PAGE_SIZE - 1));

  if ((pte & VMM_PRESENT) && !(pte & VMM_SHARED))
    return 0;

  u64 flags;
  u64 obj = 0, off = 0;
  int found = 0;

  vma_list_lock(&flags);
  for (struct vm_area *v = t->vma_list; v; v = v->next) {
    if (uaddr < v->start || uaddr >= v->end)
      continue;
    if ((v->flags & MAP_SHARED) && v->node && v->node->inode) {
      /* Mixed so an inode number cannot collide with a pml4 address, and
       * tagged odd for the same reason the frame key was. */
      u64 ino = v->node->inode->ino;
      u64 fsid = (u64)v->node->inode->fs_id;

      obj = ((ino * 0x9E3779B97F4A7C15ULL) ^ (fsid * 0xC6BC279692B5C323ULL)) | 1ull;
      off = (u64)v->offset + (uaddr - v->start);
      found = 1;
    }
    break;
  }
  vma_list_unlock(flags);

  if (!found)
    return 0;
  *obj_out = obj;
  *off_out = off;
  return 1;
}

/* The address space a futex belongs to — or, for one in shared memory, the
 * object it lives in.
 *
 * Keying purely on the address space is right for a private futex and wrong
 * for a shared one: the same object mapped into two processes has two address
 * spaces, so a waiter queued by one and a wake issued by the other never meet.
 * Chromium puts mutexes in memory shared between its processes, and the result
 * was measurable — wakes finding nobody while every thread sat on a lock that
 * had been released.
 *
 * An anonymous MAP_SHARED page has no inode to name, so it keeps the frame as
 * its identity; that case is a fork-shared page, whose frame is stable and
 * whose mappers agree on the address. */
static u64 futex_key_pml4_for(u64 uaddr, int priv) {
  extern u64 vmm_query_leaf_pte(u64 vaddr);
  struct task *t = current_task;
  u64 obj, off;

  if (!priv && futex_shared_key(uaddr, &obj, &off))
    return obj;

  u64 pte = vmm_query_leaf_pte(uaddr & ~(u64)(PAGE_SIZE - 1));

  if ((pte & VMM_PRESENT) && (pte & VMM_SHARED)) {
    /* Marked so a frame number can never collide with a pml4 address. */
    return (pte & 0x000ffffffffff000ULL) | 1ull; /* frame, tagged */
  }
  return t ? t->pml4_phys : 0;
}

/* The word's identity within its key: the offset into the shared object when
 * there is one, so two processes that mapped it at different addresses still
 * name the same futex, and the virtual address otherwise. */
static u64 futex_key_word_for(u64 uaddr, int priv) {
  u64 obj, off;

  if (!priv && futex_shared_key(uaddr, &obj, &off))
    return off;
  return uaddr;
}

/* Scheduler tick cadence (TIMER_HZ) is 100 Hz → 10 ms per tick. */
#define FUTEX_MS_PER_TICK 10

void futex_watch_arm(u64 uaddr);

int scheduler_futex(u64 uaddr, int op, int val, u64 timeout_ms) {
  if ((uaddr & 0x3) != 0) return -EINVAL;

  /* A private futex is named only inside this address space, so that is what
   * keys it; a shared one is keyed by the object it lives in. */
  int priv = (op & B1NIX_FUTEX_PRIVATE) ? 1 : 0;

  op &= ~B1NIX_FUTEX_PRIVATE;

  if (op == B1NIX_FUTEX_WAIT) {
    /* Check *uaddr == val. Re-check under the bucket lock to close the
     * wait/wake race. */
    int cur = 0;
    if (futex_read_word(uaddr, &cur) != 0) return -EFAULT;
    if (cur != val) return -EAGAIN;

    u64 key_pml4 = futex_key_pml4_for(uaddr, priv);
    u64 key_word = futex_key_word_for(uaddr, priv);
    unsigned h = futex_hash(key_pml4, key_word);
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

    /* Does this CPU's view of the word match the page itself?
     *
     * A stale translation on the CPU taking the syscall would make both the
     * thread and this re-check read an old copy of the page: the thread parks
     * on a value the rest of the process has already moved past, and the
     * release it is waiting for was written somewhere else entirely — so no
     * wake is ever sent. That is indistinguishable, from here, from a lost
     * wake, and the two need opposite fixes. Reading the word a second time
     * through the frame the page tables name, rather than through the CPU's
     * TLB, tells them apart. `b1nix.futex-check` turns it on. */
    if (bootinfo_has_flag("b1nix.futex-check")) {
      extern u64 paging_user_frame(u64 pml4_phys, u64 vaddr);
      extern u64 vmm_direct_map_base(void);
      u64 frame = paging_user_frame(current_task->pml4_phys,
                                    uaddr & ~(u64)(PAGE_SIZE - 1));

      if (frame) {
        u32 truth = *(volatile u32 *)(usize)(frame + vmm_direct_map_base() +
                                             (uaddr & (PAGE_SIZE - 1)));

        if (truth != (u32)cur) {
          static unsigned reported;

          if (++reported <= 16) {
            console_write("futex: stale view at 0x");
            console_write_hex64(uaddr);
            console_write(" cpu-sees ");
            console_write_dec((u64)(u32)cur);
            console_write(" page-holds ");
            console_write_dec((u64)truth);
            console_write("\n");
          }
        }
      }
    }

    struct futex_waiter *w = kzalloc(sizeof(*w));
    if (!w) {
      spin_unlock_irqrestore(&b->lock, flags);
      return -ENOMEM;
    }
    w->key_pml4 = key_pml4;
    w->key_uaddr = key_word;
    w->diag_vaddr = uaddr;
    w->diag_pml4 = current_task->pml4_phys;
    w->task_id = current_task->id;
    /* Sleep channel: combine pml4+uaddr into a unique pointer-shaped
     * value that scheduler_wake_all can match on. We park on `w` itself —
     * the wake path will mark this waiter dequeued and wake by task_id. */
    w->wait_chan = (void *)w;
    w->woken = 0;
    w->expect = val;
    w->bucket = h;
    w->next = b->head;
    b->head = w;
    spin_unlock_irqrestore(&b->lock, flags);

    /* A contended lock in the stack region is the shape of the wedge under
     * investigation; watch the first one that appears. */
    /* Watch a word named on the command line, or — with no name given — the
     * first contended lock seen in the stack region. `b1nix.futex-watch-addr=
     * <hex>` is how a specific lock is followed: the browser's allocator root,
     * for instance, which sixteen threads were found queued on with nobody
     * holding it that any of them could see. */
    {
      static u64 want = ~0ULL;

      if (want == ~0ULL) {
        char buf[24];

        want = 0;
        if (bootinfo_get_kv("b1nix.futex-watch-addr", buf, sizeof(buf))) {
          const char *p = buf;

          if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
            p += 2;
          for (; *p; p++) {
            u64 d;

            if (*p >= '0' && *p <= '9')
              d = (u64)(*p - '0');
            else if (*p >= 'a' && *p <= 'f')
              d = (u64)(*p - 'a') + 10;
            else if (*p >= 'A' && *p <= 'F')
              d = (u64)(*p - 'A') + 10;
            else
              break;
            want = want * 16 + d;
          }
        }
      }
      if (want ? (uaddr == want) : (val == 2 && uaddr >= 0x00007f0000000000ULL))
        futex_watch_arm(uaddr);
    }

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
    /* Unlink from whichever bucket holds us NOW. A FUTEX_REQUEUE moves a
     * waiter to the queue of another address, and unlinking from the bucket we
     * originally enqueued into would leave the node linked in the new one and
     * then free it — a use-after-free the moment anybody walked that queue. */
    int still_queued = 0;

    for (unsigned pass = 0; pass < 2 && !still_queued; pass++) {
      unsigned hh = (pass == 0) ? w->bucket : 0;
      unsigned last = (pass == 0) ? w->bucket : FUTEX_BUCKETS - 1;

      for (; hh <= last; hh++) {
        struct futex_bucket *ob = &g_futex[hh];

        spin_lock_irqsave(&ob->lock, &flags);
        struct futex_waiter **pp = &ob->head;

        while (*pp) {
          if (*pp == w) {
            *pp = w->next;
            still_queued = 1;
            break;
          }
          pp = &(*pp)->next;
        }
        spin_unlock_irqrestore(&ob->lock, flags);
        if (still_queued)
          break;
      }
    }
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

    u64 key_pml4 = futex_key_pml4_for(uaddr, priv);
    u64 key_word = futex_key_word_for(uaddr, priv);
    unsigned h = futex_hash(key_pml4, key_word);
    struct futex_bucket *b = &g_futex[h];

    int woken = 0;
    u64 flags;
    spin_lock_irqsave(&b->lock, &flags);
    /* Note the attempt on every waiter parked on this address, whatever key it
     * was queued under — that mismatch is exactly what we are hunting. */
    for (struct futex_waiter *w = b->head; w; w = w->next)
      if (w->key_uaddr == key_word)
        w->wakes_seen++;

    struct futex_waiter **pp = &b->head;
    while (*pp && woken < val) {
      struct futex_waiter *w = *pp;
      if (w->key_pml4 == key_pml4 && w->key_uaddr == key_word) {
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
      /* Remember WHERE a wake found nobody.
       *
       * "The sleeper was never signalled" and "the signal arrived a moment too
       * early" look identical from the waiter's side, and they need different
       * fixes. If the address a thread is parked on turns up in this list, the
       * wake happened and was lost; if it never does, nobody ever sent one. */
      g_futex_missed_addr[g_futex_missed_pos] = uaddr;
      g_futex_missed_pos = (g_futex_missed_pos + 1) % FUTEX_MISSED_LOG;
      /* A wake that finds nobody is ordinary — an uncontended unlock wakes
       * unconditionally. A wake that finds nobody while somebody IS parked on
       * that very address is not: it means the two sides computed different
       * keys for the same futex, and the sleeper is never coming back. Report
       * only that case, and only a few times. */
      static u64 reported;
      u64 f2;
      /* Scan EVERY bucket, not just the one this wake hashed into. A waiter
       * that computed a different key is, by definition, in a different
       * bucket — looking only here could never have found the mismatch it was
       * written to catch. The comparison is on the address the waiter sees in
       * its own address space, which is the one thing both sides always agree
       * on. */
      int same_addr = 0;
      u64 other_key = 0;

      for (unsigned hh = 0; hh < FUTEX_BUCKETS && !same_addr; hh++) {
        struct futex_bucket *ob = &g_futex[hh];

        spin_lock_irqsave(&ob->lock, &f2);
        for (struct futex_waiter *w = ob->head; w; w = w->next) {
          if (w->diag_vaddr == uaddr &&
              (w->key_pml4 != key_pml4 || w->key_uaddr != key_word)) {
            same_addr = 1;
            other_key = w->key_pml4;
            break;
          }
        }
        spin_unlock_irqrestore(&ob->lock, f2);
      }
      if (same_addr && ++reported <= 8) {
        console_write("futex: wake missed a waiter on the same address —"
                      " uaddr=0x");
        console_write_hex64(uaddr);
        console_write(" wake key=0x");
        console_write_hex64(key_pml4);
        console_write(" waiter key=0x");
        console_write_hex64(other_key);
        console_write("\n");
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
/* Per-operation tally of what userspace asked of the futex code, and whether
 * this kernel served it. An op counted as refused is a wake or a wait that
 * silently did nothing. */
#define FUTEX_OP_MAX 16
static u64 g_futex_op_seen[FUTEX_OP_MAX];
static u64 g_futex_op_refused[FUTEX_OP_MAX];

static u64 g_futex_bad_timespec;

/* A FUTEX_WAIT whose timeout could not be read out of user memory. */
void futex_note_bad_timespec(void) { g_futex_bad_timespec++; }

void futex_note_op(int base_op, int served) {
  if (base_op < 0 || base_op >= FUTEX_OP_MAX)
    return;
  g_futex_op_seen[base_op]++;
  if (!served)
    g_futex_op_refused[base_op]++;
}


/* ── Watchpoint on a futex word ─────────────────────────────────────────────
 *
 * The wedge under investigation is a thread parked on a word that the rest of
 * the process has already released, with no wake ever sent for it. Every
 * counter says the same thing and none of them says WHO released it, which is
 * the one fact that separates "the kernel dropped a wake" from "userspace
 * decided none was needed".
 *
 * So the kernel watches the word: the page holding it is made read-only when
 * the first such waiter parks, and the write that follows faults. The fault
 * handler prints the thread and the instruction that wrote, restores the page
 * and gets out of the way. One page, one fault, one line — and the arming
 * happens at most once a boot, under `b1nix.futex-watch`. */
static volatile int g_fwatch_armed;
static u64 g_fwatch_pml4;
static u64 g_fwatch_page;
static u64 g_fwatch_word;

void futex_watch_arm(u64 uaddr) {
  extern void paging_mprotect_page_in_space(u64 pml4_phys, u64 vaddr, u64 flags);
  extern void tlb_shootdown_page(u64 vaddr);
  struct task *t = current_task;

  if (g_fwatch_armed || !t)
    return;
  if (!bootinfo_has_flag("b1nix.futex-watch"))
    return;
  g_fwatch_pml4 = t->pml4_phys;
  g_fwatch_page = uaddr & ~(u64)(PAGE_SIZE - 1);
  g_fwatch_word = uaddr;
  g_fwatch_armed = 1;
  /* Read-only, still user-accessible: the next store to this page traps. */
  paging_mprotect_page_in_space(t->pml4_phys, g_fwatch_page, VMM_USER);
  tlb_shootdown_page(g_fwatch_page);
  console_write("futex-watch: armed on 0x");
  console_write_hex64(uaddr);
  console_write("\n");
}

/* Called from the page-fault entry, which has the faulting RIP. Returns 1 when
 * this fault is the watched write, having already disarmed the watch. */
int futex_watch_hit(u64 fault_addr, u64 pml4_phys, u64 rip, usize task_id) {
  extern void paging_mprotect_page_in_space(u64 pml4_phys, u64 vaddr, u64 flags);
  extern void tlb_shootdown_page(u64 vaddr);

  if (!g_fwatch_armed)
    return 0;
  if ((fault_addr & ~(u64)(PAGE_SIZE - 1)) != g_fwatch_page ||
      pml4_phys != g_fwatch_pml4)
    return 0;
  g_fwatch_armed = 0;
  paging_mprotect_page_in_space(g_fwatch_pml4, g_fwatch_page,
                                VMM_USER | VMM_WRITABLE);
  tlb_shootdown_page(g_fwatch_page);
  console_write("futex-watch: task ");
  console_write_dec(task_id);
  console_write(" wrote 0x");
  console_write_hex64(fault_addr);
  console_write(" from rip 0x");
  console_write_hex64(rip);
  console_write(" (watched word 0x");
  console_write_hex64(g_fwatch_word);
  console_write(")\n");
  return 1;
}

/* FUTEX_REQUEUE / FUTEX_CMP_REQUEUE: wake `nr_wake` waiters on uaddr1 and MOVE
 * up to `nr_requeue` of the rest to uaddr2's queue.
 *
 * Serving this as "wake some here, wake some there" — which is what this
 * kernel did — is not an approximation of requeue, it is the opposite of it.
 * musl's pthread_cond_signal asks to wake NOBODY and requeue ONE: the sleeper
 * is supposed to end up queued on the mutex, to be released later by whoever
 * unlocks it. Waking zero and then waking a queue that is empty left the
 * sleeper parked on the condition variable with nothing in the system able to
 * reach it — the condvar handoff simply never completed, and the thread waited
 * until an unrelated signal happened to disturb it.
 *
 * Buckets are locked in index order so two concurrent requeues in opposite
 * directions cannot deadlock. */
int scheduler_futex_requeue(u64 uaddr1, u64 uaddr2, int nr_wake, int nr_requeue,
                            int priv) {
  if ((uaddr1 & 0x3) || (uaddr2 & 0x3))
    return -EINVAL;
  if (nr_wake < 0)
    nr_wake = 0;
  if (nr_requeue < 0)
    nr_requeue = 0;

  u64 key1 = futex_key_pml4_for(uaddr1, priv);
  u64 word1 = futex_key_word_for(uaddr1, priv);
  u64 key2 = futex_key_pml4_for(uaddr2, priv);
  u64 word2 = futex_key_word_for(uaddr2, priv);
  unsigned h1 = futex_hash(key1, word1);
  unsigned h2 = futex_hash(key2, word2);
  struct futex_bucket *b1 = &g_futex[h1];
  struct futex_bucket *b2 = &g_futex[h2];
  int woken = 0, moved = 0;
  usize wake_ids[8];
  int nwake_ids = 0;

  u64 flags;
  if (h1 == h2) {
    spin_lock_irqsave(&b1->lock, &flags);
  } else if (h1 < h2) {
    spin_lock_irqsave(&b1->lock, &flags);
    spin_lock(&b2->lock);
  } else {
    spin_lock_irqsave(&b2->lock, &flags);
    spin_lock(&b1->lock);
  }

  struct futex_waiter **pp = &b1->head;

  while (*pp) {
    struct futex_waiter *w = *pp;

    if (w->key_pml4 != key1 || w->key_uaddr != word1) {
      pp = &w->next;
      continue;
    }
    if (woken < nr_wake) {
      *pp = w->next;
      __atomic_store_n(&w->woken, 1, __ATOMIC_RELEASE);
      if (nwake_ids < 8)
        wake_ids[nwake_ids++] = w->task_id;
      woken++;
      continue;
    }
    if (moved < nr_requeue) {
      *pp = w->next;
      w->key_pml4 = key2;
      w->key_uaddr = word2;
      w->diag_vaddr = uaddr2;
      w->bucket = h2;
      w->next = b2->head;
      b2->head = w;
      moved++;
      continue;
    }
    break;
  }

  if (h1 == h2) {
    spin_unlock_irqrestore(&b1->lock, flags);
  } else if (h1 < h2) {
    spin_unlock(&b2->lock);
    spin_unlock_irqrestore(&b1->lock, flags);
  } else {
    spin_unlock(&b1->lock);
    spin_unlock_irqrestore(&b2->lock, flags);
  }

  /* Waking runs the scheduler's own locking, so it happens after both bucket
   * locks are gone. */
  for (int i = 0; i < nwake_ids; i++)
    scheduler_wake_task(wake_ids[i]);

  return woken;
}

void futex_dump_waiters(void) {
  console_write("futex ops (op:seen/refused):");
  for (int o = 0; o < FUTEX_OP_MAX; o++) {
    if (!g_futex_op_seen[o])
      continue;
    console_write(" ");
    console_write_dec((u64)o);
    console_write(":");
    console_write_dec(g_futex_op_seen[o]);
    console_write("/");
    console_write_dec(g_futex_op_refused[o]);
  }
  console_write("\n");
  console_write("futex missed-wake addrs:");
  for (unsigned m = 0; m < FUTEX_MISSED_LOG; m++) {
    u64 a = g_futex_missed_addr[(g_futex_missed_pos + m) % FUTEX_MISSED_LOG];

    if (!a)
      continue;
    console_write(" 0x");
    console_write_hex64(a);
  }
  console_write("\n");
  {
    extern void cow_shootdown_stats(u64 *done, u64 *skipped);
    u64 done = 0, skipped = 0;

    cow_shootdown_stats(&done, &skipped);
    console_write("cow shootdowns: done ");
    console_write_dec(done);
    console_write(" skipped ");
    console_write_dec(skipped);
    console_write("\n");
  }
  console_write("futex bad-timespec: ");
  console_write_dec(g_futex_bad_timespec);
  console_write("\n");
  console_write("futex wakes: hit ");
  console_write_dec(g_futex_wake_hit);
  console_write(" missed ");
  console_write_dec(g_futex_wake_missed);
  console_write("\nfutex waiters:\n");
  for (unsigned h = 0; h < FUTEX_BUCKETS; h++) {
    struct futex_bucket *b = &g_futex[h];
    u64 flags;
    spin_lock_irqsave(&b->lock, &flags);
    usize rescue_id = 0;
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
        u64 frame = paging_user_frame(w->diag_pml4,
                                      w->diag_vaddr & ~(u64)(PAGE_SIZE - 1));
        if (frame) {
          u32 cur = *(volatile u32 *)(usize)(frame + vmm_direct_map_base() +
                                             (w->diag_vaddr & (PAGE_SIZE - 1)));
          console_write(" cur=");
          console_write_dec((u64)cur);
          /* Diagnostic release, off unless asked for.
           *
           * A waiter whose word no longer holds the value it slept on, with no
           * wake ever attempted against its address, is a lost wake — and the
           * question that matters next is whether it is the ONLY thing holding
           * the process up. `b1nix.futex-rescue` answers it by unblocking such
           * a waiter here: if the run then completes, the wedge is this and
           * nothing else. It is an instrument, not a fix; the fix belongs
           * wherever the wake was dropped. */
          if (cur != (u32)w->expect && !w->woken && w->wakes_seen == 0 &&
              bootinfo_has_flag("b1nix.futex-rescue")) {
            console_write(" RESCUED");
            __atomic_store_n(&w->woken, 1, __ATOMIC_RELEASE);
            rescue_id = w->task_id;
          }
        } else {
          console_write(" cur=<unmapped>");
        }
      }
      console_write("\n");
    }
    spin_unlock_irqrestore(&b->lock, flags);
    /* Outside the bucket lock: waking runs the scheduler's wake path, which
     * takes locks of its own. */
    if (rescue_id)
      scheduler_wake_task(rescue_id);
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
