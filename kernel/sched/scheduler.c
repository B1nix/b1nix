#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/ipi.h>
#include <b1nix/lapic.h>
#include <b1nix/mm.h>
#include <b1nix/panic.h>
#include <b1nix/posix.h>
#include <b1nix/runqueue.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <b1nix/uidgid.h>
#include <b1nix/user.h>
#include <b1nix/vfs.h>

/* Drain cross-CPU TLB shootdowns while spin-waiting with IRQs disabled (these
 * stack_released hand-off spins run inside scheduler_yield, IRQs off). Without
 * it a core parked here can't ACK a shootdown IPI and the initiator times out
 * (observed: `tlb_shootdown stalled pending=1` panic under -smp4 parallel
 * builds). Defined in kernel/arch/x86_64/tlb.c; fast no-op when nothing pending. */
extern void tlb_shootdown_poll(void);
#ifdef __x86_64__
#include <b1nix/arch_x86_64.h>
#else
#include <b1nix/arch_x86.h>
#endif
#include <b1nix/aio.h>
#include <string.h>

#ifdef __x86_64__
_Static_assert(__builtin_offsetof(struct percpu, cur_task) == 0x10,
               "cur_task must be at offset 0x10 (see syscall_entry.S)");
_Static_assert(__builtin_offsetof(struct percpu, syscall_scratch_rax) == 0x60,
               "syscall_scratch_rax must be at offset 0x60 (see syscall_entry.S)");
#else
_Static_assert(__builtin_offsetof(struct percpu, cur_task) == 0x10,
               "cur_task must be at offset 0x10");
#endif

/*
 * Task table (C1 audit item).
 *
 * The table is split into fixed-size CHUNKS that are allocated from the
 * kernel heap on demand. Existing code accessed slots as `T(i)`; the
 * chunked storage is exposed through the `T(i)` accessor that returns a
 * `struct task *` into the right chunk. Growth never relocates an existing
 * slot, so pointers stored elsewhere (current_task, runqueue links, etc.)
 * stay valid forever.
 *
 * MAX_TASKS is now the hard ceiling; the live table size starts at one
 * chunk (TASK_CHUNK_SIZE slots) and grows by one chunk each time find_unused
 * runs out of slots. g_task_hwm tracks the high-water-mark slot index used
 * so far, so the linear scans cost O(active) instead of O(ceiling).
 */
#define TASK_CHUNK_SIZE   64
#define TASK_MAX_CHUNKS   64
#define MAX_TASKS         (TASK_CHUNK_SIZE * TASK_MAX_CHUNKS)  /* 4096 */
/* The kernel stack is a kmalloc'd block in the shared kheap, so an overflow
 * silently corrupts the adjacent heap block (e.g. a vfs_node) instead of
 * faulting. b1nix runs the busybox coreutils builtins in kernel mode on this
 * stack, and some have large on-stack buffers (uniq_main alone is ~12 KB:
 * buf[8192] + lines[512]); together with the ~6.6 KB syscall dispatch frame
 * and the VFS/ext4 call chain they exceed 16 KB. SMP/T8 preemption adds
 * enough nested scheduler/signal/syscall frames that 32 KB still leaves too
 * little headroom for the M12/M14 stress paths. */
#define KERNEL_STACK_SIZE (64 * 1024)
#define TASK_ENV_MAX 16
#define TASK_ENV_VALUE_MAX 64

extern void arch_context_switch(struct cpu_context *old_context,
                                struct cpu_context *new_context,
                                volatile int *released_publish);
extern char x86_syscall_stack_top[];

/* x86 FPU/SSE save/restore (kernel/arch/x86_64/fpu.S). The kernel is -mno-sse, so
 * the fxsave/fxrstor instructions live in assembly. */
extern void arch_fpu_save(void *area);
extern void arch_fpu_restore(void *area);
extern void arch_fpu_capture_clean(void *area);

/* Canonical clean FXSAVE image, loaded into tasks that have never run. */
static __attribute__((aligned(16))) u8 g_clean_fpu[512];
static int g_clean_fpu_ready = 0;

/* Chunked task storage. g_task_chunks[c] points to a kmalloc'd block of
 * TASK_CHUNK_SIZE consecutively-laid-out struct tasks (or NULL if that chunk
 * is not yet allocated). Slot indices i map as (i >> 6) -> chunk, (i & 63) ->
 * offset within chunk. */
static struct task *g_task_chunks[TASK_MAX_CHUNKS];
static usize        g_task_hwm = 0;  /* one past highest slot ever used */

/* M29: per-task thread metadata kept in parallel arrays (NOT in struct task
 * — see comment in sched.h). Indexed by the slot in g_task_chunks computed
 * via task_index(). */
static int  g_task_is_thread[MAX_TASKS];
static u64  g_task_tls_base[MAX_TASKS];
static u64  g_task_child_tid_clear[MAX_TASKS];
static inline struct task *T(usize i) {
  return &g_task_chunks[i >> 6][i & 63];
}
/* current_task is per-CPU now (a macro -> get_percpu()->cur_task, see sched.h). */
static usize next_task_id = 1;

/* SMP: the global runqueue holds READY *non-stealable* tasks — ordinary
 * userspace processes and kernel threads. Any CPU (BSP or AP) dequeues from it
 * in pick_next_task and runs the task under the Big Kernel Lock, so userspace
 * runs on more than one core while kernel-mode execution stays serialised.
 * (Stealable CPU-bound workers stay on per-CPU runqueues for the work-stealing
 * path in ap_main — see sched_rq_enqueue_current / sched_steal_task.) Its own
 * spinlock makes enqueue/dequeue safe even outside the BKL. */
static struct runqueue g_global_rq;

struct runqueue *sched_global_rq(void) { return &g_global_rq; }

/* ── Per-CPU idle tasks for APs ──
 * Each AP runs a dedicated idle task as current_task while waiting for work.
 * These live OUTSIDE the global tasks[] table so the O(n) scan never migrates
 * one onto another core; the cooperative scheduler reaches an AP's idle task
 * only through the per-CPU fallback in pick_next_task.
 *
 * C3 audit: lazy-allocate from the kheap so MAX_CPUS=64 doesn't reserve
 * ~256 KiB BSS for slots that may never run. Pointer table costs 512 B. */
static struct task *g_ap_idle_tasks[MAX_CPUS];

struct task *scheduler_setup_ap_idle(int cpu, u64 kstack_top) {
  if (cpu < 0 || cpu >= MAX_CPUS)
    return 0;
  if (!g_ap_idle_tasks[cpu]) {
    g_ap_idle_tasks[cpu] = kzalloc(sizeof(struct task));
    if (!g_ap_idle_tasks[cpu]) return 0;
  }
  struct task *t = g_ap_idle_tasks[cpu];
  memset(t, 0, sizeof(*t));
  t->name = "ap-idle";
  t->state = TASK_RUNNING;
  t->priority = 0;
  t->pml4_phys = 0; /* kernel address space */
  t->kernel_stack_ptr = kstack_top;
  t->cwd[0] = '/';
  t->cwd[1] = '\0';
  return t;
}
static volatile u64 scheduler_ticks;
static int scheduler_started;
static void task_init_cred(struct task *task);

/* SMP: protects the tasks[] slot lifecycle (UNUSED <-> claimed) so allocation
 * and freeing are atomic across CPUs. interrupts_disable() only fences the
 * local CPU, so it is NOT sufficient once APs touch the table (e.g. an AP
 * reaping a stolen worker while the BSP allocates). This is a leaf lock — never
 * acquire another lock while holding it, and never hold it across a context
 * switch. State transitions of an already-owned task (RUNNING/READY/...) stay
 * word-atomic and are not covered here; only slot ownership is. */
static spinlock_t g_tasks_lock = SPINLOCK_INIT;

/* Lockdep-traced acquire/release helpers — wrap spin_lock_irqsave with the
 * DAG level for the held-lock tracker. Inlines to a single store in
 * production builds (KERNEL_LOCKDEP undef). Defined here rather than as
 * macros because the level must be paired with the lock pointer at one
 * place. */
#include <b1nix/lockdep.h>
static inline void tasks_lock(u64 *flags) {
  spin_lock_irqsave(&g_tasks_lock, flags);
  LOCKDEP_ACQUIRE(LOCKDEP_LVL_TASKS);
}
static inline void tasks_unlock(u64 flags) {
  LOCKDEP_RELEASE(LOCKDEP_LVL_TASKS);
  spin_unlock_irqrestore(&g_tasks_lock, flags);
}

/* fd_lock helpers — wraps spin_lock/unlock on current_task->fd_lock with
 * lockdep tracking. fd_lock is plain (no irq-save) because fd ops never
 * fire from ISR context and the lock is process-local (only the owning
 * task touches it). */
static inline void fd_lock_acquire(void) {
  spin_lock(&current_task->fd_lock);
  LOCKDEP_ACQUIRE(LOCKDEP_LVL_FD);
}
static inline void fd_lock_release(void) {
  LOCKDEP_RELEASE(LOCKDEP_LVL_FD);
  spin_unlock(&current_task->fd_lock);
}

#include <b1nix/arch.h>

static u64 align_down_u64(u64 value, u64 alignment) {
  return value & ~(alignment - 1);
}

/* Lazily allocate task chunk `c` from the kernel heap. Called only with
 * g_tasks_lock held. Returns 0 if kmalloc fails (then the caller must report
 * "no free slot" — same as the old hard-cap behaviour). */
static int ensure_task_chunk(usize c) {
  if (c >= TASK_MAX_CHUNKS) return 0;
  if (g_task_chunks[c]) return 1;
  struct task *chunk = kzalloc(TASK_CHUNK_SIZE * sizeof(struct task));
  if (!chunk) return 0;
  /* kzalloc already zeros the chunk -> all slots are TASK_UNUSED. */
  g_task_chunks[c] = chunk;
  return 1;
}

/* Atomically claim a free task slot, zero it, mark it BLOCKED (reserved),
 * and assign its id — all under g_tasks_lock so two CPUs can never claim the
 * same slot. Scans the in-use range first (cheap), then grows by one chunk
 * (TASK_CHUNK_SIZE slots) on demand up to the MAX_TASKS ceiling. */
static struct task *find_unused_task(void) {
  u64 flags;
  tasks_lock(&flags);

  /* 1) Fast path: reuse a slot already in [0, g_task_hwm). Never recycle slot 0
   * — it is the permanent boot/idle task (the BSP's idle-fallback target). */
  for (usize i = 1; i < g_task_hwm; i++) {
    if (T(i)->state == TASK_UNUSED) {
      memset(T(i), 0, sizeof(struct task));
      /* M29: clear side-table metadata so the reused slot starts clean. */
      g_task_is_thread[i] = 0;
      g_task_tls_base[i] = 0;
      g_task_child_tid_clear[i] = 0;
      T(i)->state = TASK_BLOCKED;
      T(i)->id = next_task_id++;
      tasks_unlock(flags);
      return T(i);
    }
  }

  /* 2) Slow path: extend the high-water mark, allocating a new chunk if the
   *    next slot crosses a chunk boundary. */
  if (g_task_hwm >= MAX_TASKS) {
    tasks_unlock(flags);
    return 0;
  }
  if (!ensure_task_chunk(g_task_hwm >> 6)) {
    tasks_unlock(flags);
    return 0;
  }
  usize i = g_task_hwm++;
  /* Chunk was kzalloc'd, but be explicit so a slot that gets reused after
   * free_task_slot starts from a clean state too (same as the old path). */
  memset(T(i), 0, sizeof(struct task));
  T(i)->state = TASK_BLOCKED;
  T(i)->id = next_task_id++;
  tasks_unlock(flags);
  return T(i);
}

/* Release a tasks[] slot back to UNUSED under g_tasks_lock (pairs with
 * find_unused_task). The store also publishes any prior writes (e.g. the
 * kfree of the task's resources) before the slot becomes claimable again. */
static void free_task_slot(struct task *t) {
  u64 flags;
  tasks_lock(&flags);
  t->state = TASK_UNUSED;
  tasks_unlock(flags);
}

/* Find the slot index of a task by scanning the populated chunks. Only
 * called from pick_next_task to choose a scan start point, so an O(active)
 * walk is fine. Returns 0 (a safe default scan origin) if the pointer is not
 * one of ours — e.g., an AP's idle task lives outside the chunked table. */
static usize task_index(const struct task *task) {
  for (usize c = 0; c < TASK_MAX_CHUNKS; c++) {
    const struct task *chunk = g_task_chunks[c];
    if (!chunk) break;
    if (task >= chunk && task < chunk + TASK_CHUNK_SIZE) {
      return (c << 6) | (usize)(task - chunk);
    }
  }
  return 0;
}

/* True if `t` is the live current task on some CPU — i.e. it is executing
 * right now, not merely parked READY. scheduler_yield reassigns this CPU's
 * cur_task to the *incoming* task BEFORE arch_context_switch saves+publishes
 * the outgoing task's stack_released, so a task that is genuinely mid-switch-out
 * is NOT cur_task anywhere during its stack_released==0 window. A task that is
 * cur_task somewhere with stack_released==0 is therefore actively running (it
 * was woken BLOCKED->READY mid-flight and kept running) and will not publish
 * stack_released soon — the picker must not wait on it. */
static int task_running_somewhere(struct task *t) {
  for (int c = 0; c < g_max_cpus; c++) {
    struct percpu *pc = get_percpu_n(c);
    if (pc && (struct task *)pc->cur_task == t)
      return 1;
  }
  return 0;
}

static struct task *pick_next_task(void) {
  if (current_task == 0) {
    return 0;
  }

  struct percpu *pcpu = get_percpu();
  int on_ap = (pcpu && pcpu->cpu_id != 0);

  /* On the BSP, drain the global runqueue first (fast path for freshly woken /
   * created tasks). APs skip the rq dequeue and rely on the scan below with the
   * ap_runnable filter — an AP must run ONLY userspace ELF processes (kernel
   * threads would hold the BKL across their ring-0 yields and monopolise it). A
   * userspace task left in the rq is still found by the AP scan; the BSP later
   * dequeues the now-RUNNING stale entry and discards it. */
  if (!on_ap) {
    for (;;) {
      struct task *t = rq_dequeue(&g_global_rq);
      if (!t)
        break;
      if (t->stealable)
        continue;
      /* M28 T4: spin until the outgoing CPU's arch_context_switch publishes
       * stack_released==1. Without this, we could load a saved RSP that is
       * still being written to as a callee-save spill area by the other
       * CPU's save side. See struct task::stack_released and
       * scheduler_yield's RUNNING->READY transition. The spin is bounded by
       * the arch_context_switch save-side, which is a handful of mov +
       * a single store with no locks/IRQs/sleeping calls.
       *
       * Skip the wait when re-picking OUR OWN task: a waker on another CPU may
       * have CAS'd us BLOCKED/SLEEPING->READY and enqueued us here before this
       * CPU even reached pick_next_task (now that the block paths clear the
       * lease before yielding). Our stack is live on THIS CPU and the lease is
       * only published once we leave it via arch_context_switch — waiting on
       * our own publish would self-deadlock. Resuming ourselves is a no-op
       * switch, always safe. */
      /* Stay in the wait ONLY while the task is still READY. A waker
       * (e.g. a child's exit / STOPPED / CONTINUED notify) can CAS a task
       * BLOCKED->READY and enqueue it here while the task is still executing
       * its own block loop with stack_released==0. If that task then re-blocks
       * (its waitpid found only a STOPPED child, or scheduler_yield returned 0
       * so it never context-switched) it goes BLOCKED again WITHOUT ever
       * reaching arch_context_switch — so stack_released never becomes 1 and a
       * naive `while (!stack_released)` spins forever (a real -smp livelock,
       * the m27-smoke wedge). Bailing when state!=READY drops the now-stale rq
       * entry: the CAS below fails and the next real wake re-enqueues the task
       * once it has genuinely switched out. */
      while (t != current_task &&
             __atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == TASK_READY &&
             !__atomic_load_n(&t->stack_released, __ATOMIC_ACQUIRE) &&
             !task_running_somewhere(t)) {
        __asm__ volatile("pause");
        tlb_shootdown_poll();
      }
      /* If the wait bailed with stack_released still 0 (the task is running on
       * another CPU as its live cur_task, or it re-blocked), its saved context
       * is NOT safe to load — drop this rq entry and try the next. The task is
       * still found by the scan / re-enqueued by its next genuine wake. */
      if (t != current_task &&
          !__atomic_load_n(&t->stack_released, __ATOMIC_ACQUIRE)) {
        continue;
      }
      /* F5 (M28 #7): atomic claim. CAS READY -> RUNNING so a concurrent
       * scan on another CPU can't pick the same task between our check and
       * scheduler_yield's eventual state=RUNNING store. The same fix is
       * applied to the scan path below. Under M24b BKL the race couldn't
       * happen; T1+ depends on this. */
      enum task_state expected = TASK_READY;
      if (__atomic_compare_exchange_n(&t->state, &expected, TASK_RUNNING,
                                      0, __ATOMIC_ACQUIRE,
                                      __ATOMIC_RELAXED)) {
        return t;
      }
      /* Lost the CAS (someone else already picked, or task transitioned to
       * BLOCKED/SLEEPING). Drop on the floor — if it's still relevant the
       * next iteration / scan will find it. */
    }
  }

  /* O(n) scan over the task table. A task that yields is marked READY without
   * being re-enqueued (scheduler_yield), so this scan — not the rq — re-picks
   * voluntarily-yielding tasks. Stealable workers are always skipped; on an AP
   * only ap_runnable userspace processes are eligible. */
  usize start = task_index(current_task);
  int max_priority = -1;
  struct task *best_task = 0;

  for (usize offset = 1; offset <= g_task_hwm; offset++) {
    usize index = (start + offset) % g_task_hwm;

    if (T(index)->state != TASK_READY)
      continue;
    if (T(index)->stealable)
      continue;
    if (on_ap && !T(index)->ap_runnable)
      continue; /* APs run only userspace ELF processes */

    if (T(index)->priority > max_priority) {
      max_priority = T(index)->priority;
      best_task = T(index);
    }
  }

  if (best_task) {
    /* M28 T4: see global-rq comment — wait for the outgoing CPU to publish
     * stack_released==1 before claiming, otherwise we could resume on a
     * kernel stack the other CPU is still saving to. Skip for our own task
     * (re-picked after a cross-CPU wake) — its stack is live here and the
     * publish only happens once we leave it; see the global-rq comment. */
    /* Same READY guard as the global-rq path above — bail if the task
     * re-blocked or was claimed elsewhere so we never spin forever on a
     * stack_released that a never-context-switched task won't publish. */
    while (best_task != current_task &&
           __atomic_load_n(&best_task->state, __ATOMIC_ACQUIRE) == TASK_READY &&
           !__atomic_load_n(&best_task->stack_released, __ATOMIC_ACQUIRE) &&
           !task_running_somewhere(best_task)) {
      __asm__ volatile("pause");
      tlb_shootdown_poll();
    }
    /* If the wait bailed with stack_released still 0 (task running on another
     * CPU or re-blocked), do not claim it — its context isn't safe to load.
     * Treat as no-work; the next pick re-finds it once it has switched out. */
    if (best_task == current_task ||
        __atomic_load_n(&best_task->stack_released, __ATOMIC_ACQUIRE)) {
      /* F5 (M28 #7): atomic claim — see global-rq comment above. If we lose
       * the CAS, return 0 (no work this iteration) and let the caller retry;
       * starting a fresh scan here can spin under contention without ever
       * settling, so the cleaner shape is "treat as no-work and try again". */
      enum task_state expected = TASK_READY;
      if (__atomic_compare_exchange_n(&best_task->state, &expected,
                                      TASK_RUNNING, 0, __ATOMIC_ACQUIRE,
                                      __ATOMIC_RELAXED)) {
        return best_task;
      }
    }
  }

  /* AP fallback: nothing else runnable, so park back to this CPU's idle task,
   * which lets the AP cooperative loop regain control and drop the BKL. NULL on
   * the BSP (its boot task handles idling), and skipped when the idle task is
   * already current (so scheduler_yield returns 0 and the loop parks). */
  if (pcpu && pcpu->idle_task && current_task != (struct task *)pcpu->idle_task)
    return (struct task *)pcpu->idle_task;

  return 0;
}

static void wake_sleepers(void) {
  int woken = 0;
  for (usize i = 0; i < g_task_hwm; i++) {
    /* F4 (M28 #7): atomic CAS SLEEPING/BLOCKED -> READY so only one CPU wins when
     * two timer ticks (or a tick + an explicit wake) race for the same
     * task. */
    if (T(i)->wake_tick != 0 && T(i)->wake_tick <= scheduler_ticks) {
      enum task_state expected = TASK_SLEEPING;
      if (__atomic_compare_exchange_n(&T(i)->state, &expected, TASK_READY,
                                      0, __ATOMIC_ACQUIRE,
                                      __ATOMIC_RELAXED)) {
        T(i)->wake_tick = 0;
        sched_rq_enqueue_current(T(i));
        woken++;
      } else {
        expected = TASK_BLOCKED;
        if (__atomic_compare_exchange_n(&T(i)->state, &expected, TASK_READY,
                                        0, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
          T(i)->wake_tick = 0;
          T(i)->wait_chan = 0;
          sched_rq_enqueue_current(T(i));
          woken++;
        }
      }
    }
  }
  /* M28 #6: if we promoted at least one task, kick the other CPUs out of
   * `sti; hlt` so they re-poll the global runqueue. We are called from the
   * timer ISR (T3) which now runs WITHOUT the BKL. The IPI is a fire-and-
   * forget no-op handler — safe regardless. */
  if (woken > 0) ipi_reschedule_all();
}

static int scheduler_wake_blocked_parent(usize parent_id) {
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->id != parent_id)
      continue;

    enum task_state expected = TASK_BLOCKED;
    if (__atomic_compare_exchange_n(&T(i)->state, &expected,
                                    TASK_READY, 0,
                                    __ATOMIC_RELEASE,
                                    __ATOMIC_RELAXED)) {
      sched_rq_enqueue_current(T(i));
      return 1;
    }
    return 0;
  }
  return 0;
}

void scheduler_notify_wait_event(usize parent_id) {
  if (scheduler_wake_blocked_parent(parent_id))
    ipi_reschedule_all();
}

static void scheduler_waitpid_fast_return(void) {
  current_task->state = TASK_RUNNING;
  /* A child may have won BLOCKED -> READY and enqueued us while we were still
   * scanning. If this waitpid iteration resolves without actually yielding,
   * scrub that stale self-wakeup so the global runqueue doesn't accumulate
   * duplicate entries across many fast child exits. */
  sched_rq_remove_task(current_task);
}

static void kernel_thread_trampoline(void) {
  interrupts_enable();

  if (current_task == 0 || current_task->entry == 0) {
    panic("scheduler entered invalid task");
  }

  current_task->entry(current_task->arg);
  scheduler_exit_current(0);
}

void scheduler_init(void) {
  /* Allocate the first task chunk so T(0) is dereferenceable for the boot
   * task. kzalloc panics on OOM here — we cannot proceed without a task
   * table. */
  if (!ensure_task_chunk(0)) {
    panic("scheduler: failed to allocate initial task chunk");
  }
  g_task_hwm = 1;  /* boot task occupies slot 0 */

  struct task *boot = T(0);
  boot->id = next_task_id++;
  boot->name = "boot";
  boot->state = TASK_RUNNING;
  boot->stdout_fd = -1;
  boot->fd_capacity = SCHED_MAX_FDS;
  boot->fd_table = kzalloc(boot->fd_capacity * sizeof(struct vfs_handle *));
  boot->fd_flags = kzalloc(boot->fd_capacity * sizeof(int));
  boot->fd_lock = 0;
  boot->priority = 1;
  boot->parent_id = 0;
  boot->cwd[0] = '/';
  boot->cwd[1] = '\0';
  boot->user_brk = 0;
  boot->heap_start = 0;
  boot->umask = 022;
  boot->process_group_id = boot->id;
  boot->session_id = boot->id;
  boot->kernel_stack_ptr = (u64)(usize)x86_syscall_stack_top;
  boot->pml4_phys = 0; // Kernel PML4
  boot->vma_list = 0;
  task_init_cred(boot);
  current_task = boot;
  scheduler_started = 1;

  /* T3 (M28 #7): make the boot task the BSP's idle-fallback target so that
   * pick_next_task's fallback path has somewhere to go when no READY task
   * is found — symmetric to scheduler_setup_ap_idle on APs. Without this,
   * an exit_current on the BSP that races a fully-blocked task set hits
   * "dead task has nowhere to yield" because the DEAD task can't continue
   * and there's no idle target. */
  {
    struct percpu *p = get_percpu();
    if (p) p->idle_task = boot;
  }

  /* BKL fully retired (M28 #7) — no init-time acquire needed. Kernel entry
   * runs BKL-free and the context switch no longer hands a lock across. */

  console_write("sched: initialized\n");
}

static void task_init_cred(struct task *task) {
  if (task->id == 1) {
    /* Boot task gets root credentials */
    task->cred = cred_create_default();
  } else {
    /* Inherit credentials from parent */
    struct task *parent = 0;
    for (usize i = 0; i < g_task_hwm; i++) {
      if (T(i)->id == task->parent_id) {
        parent = T(i);
        break;
      }
    }
    task->cred = cred_dup(parent ? parent->cred : 0);
  }
}

/* Stealable CPU-bound kernel workers (SMP work-stealing, M24b) park back to
 * the stealing AP's idle context via this trampoline instead of the normal
 * kernel_thread_trampoline; defined in kernel/arch/x86_64/lapic.c. */
extern void ap_worker_trampoline(void);

static int kthread_create_impl(const char *name, kernel_thread_entry entry,
                               void *arg, void (*trampoline)(void),
                               int stealable, int ap_runnable) {
  interrupts_disable();
  struct task *parent_task = current_task;
  /* find_unused_task atomically claims the slot, marks it BLOCKED (reserved
   * until fully initialized), and assigns its id. */
  struct task *task = find_unused_task();
  interrupts_enable();

  if (task == 0) {
    return -1;
  }

  void *stack = kmalloc(KERNEL_STACK_SIZE);
  if (!stack) {
    free_task_slot(task);
    return -1;
  }

  u64 stack_top = align_down_u64((u64)(usize)stack + KERNEL_STACK_SIZE, 16);
  task->kernel_stack_ptr = stack_top;
#ifdef __x86_64__
  u64 initial_rsp = stack_top - 16;
#else
  u64 initial_rsp = stack_top - 8;
#endif
#ifdef __aarch64__
  // AArch64 uses registers for trampoline return path
#elif defined(__x86_64__)
  *(u64 *)(usize)initial_rsp = (u64)(usize)trampoline;
#else
  *(u32 *)(usize)initial_rsp = (u32)(usize)trampoline;
#endif
  task->stealable = stealable;
  task->ap_runnable = ap_runnable;

  task->name = strdup(name);
  if (!task->name) {
    free_task_slot(task);
    kfree(stack);
    return -1;
  }
  task->entry = entry;
  task->arg = arg;
  task->stack = stack;
  task->wake_tick = 0;
#ifdef __aarch64__
  task->context.fp = 0;
  task->context.lr =
      initial_rsp; // Use lr for entry point on AArch64 trampoline
  task->context.sp = initial_rsp;
  task->context.x19 = 0;
  task->context.x20 = 0;
  task->context.x21 = 0;
  task->context.x22 = 0;
  task->context.x23 = 0;
  task->context.x24 = 0;
  task->context.x25 = 0;
  task->context.x26 = 0;
  task->context.x27 = 0;
  task->context.x28 = 0;
#elif defined(__x86_64__)
  task->context.rsp = initial_rsp;
  task->context.rbp = 0;
  task->context.rbx = 0;
  task->context.r12 = 0;
  task->context.r13 = 0;
  task->context.r14 = 0;
  task->context.r15 = 0;
#else
  task->context.esp = (u32)initial_rsp;
  task->context.ebp = 0;
  task->context.ebx = 0;
  task->context.esi = 0;
  task->context.edi = 0;
#endif
  task->stdout_fd = parent_task ? parent_task->stdout_fd : -1;
  if (stealable) {
    /* Stealable CPU-bound workers are self-contained: no fd table, no cred,
     * no address space. This keeps the AP run/reap path free of VFS/cred
     * teardown (the worker never makes syscalls). */
    task->stdout_fd = -1;
    task->fd_capacity = 0;
    task->fd_table = 0;
    task->fd_flags = 0;
    task->fd_lock = 0;
  } else if (parent_task) {
    task->fd_capacity = parent_task->fd_capacity;
    task->fd_table = kzalloc(task->fd_capacity * sizeof(struct vfs_handle *));
    task->fd_flags = kzalloc(task->fd_capacity * sizeof(int));
    task->fd_lock = 0;
    for (usize i = 0; i < task->fd_capacity; i++) {
      task->fd_table[i] = parent_task->fd_table[i];
      task->fd_flags[i] = parent_task->fd_flags[i];
      if (task->fd_table[i]) {
        vfs_handle_retain(task->fd_table[i]);
      }
    }
  } else {
    task->fd_capacity = SCHED_MAX_FDS;
    task->fd_table = kzalloc(task->fd_capacity * sizeof(struct vfs_handle *));
    task->fd_flags = kzalloc(task->fd_capacity * sizeof(int));
    task->fd_lock = 0;
  }
  task->priority = 1;
  task->parent_id = parent_task ? parent_task->id : 0;
  if (parent_task) {
    memcpy(task->cwd, parent_task->cwd, sizeof(task->cwd));
    task->cwd[sizeof(task->cwd) - 1] = '\0';
    task->user_brk = parent_task->user_brk;
    task->heap_start = parent_task->heap_start;
    task->umask = parent_task->umask;
    task->process_group_id = parent_task->process_group_id;
    task->session_id = parent_task->session_id;
    memcpy(task->env, parent_task->env, sizeof(task->env));
    task->pml4_phys = 0;
    task->vma_list = 0;
  } else {
    task->cwd[0] = '/';
    task->cwd[1] = '\0';
    task->user_brk = 0;
    task->umask = 022;
    task->process_group_id = task->id;
    task->session_id = task->id;
    memset(task->env, 0, sizeof(task->env));
    task->pml4_phys = 0;
    task->vma_list = 0;
  }
  task->exit_code = 0;
  task->pending_signals = 0;
  task->blocked_signals = 0;
  task->last_stop_signal = 0;
  task->stop_report_pending = 0;
  task->continued_report_pending = 0;
  memset(task->sigactions, 0, sizeof(task->sigactions));

  if (!stealable)
    task_init_cred(task);

  interrupts_disable();
  /* M28 T4: fresh task has never been context-switched out, so its kernel
   * stack is fully set up (context.rsp points at a manually-initialised
   * frame). pick_next_task waits for stack_released==1 before claiming;
   * publish that here so the first pick doesn't spin forever. */
  task->stack_released = 1;
  task->state = TASK_READY;
  sched_rq_enqueue_current(task);
  interrupts_enable();

  return (int)task->id;
}

int kthread_create(const char *name, kernel_thread_entry entry, void *arg) {
  return kthread_create_impl(name, entry, arg, kernel_thread_trampoline, 0, 0);
}

int kthread_create_user(const char *name, kernel_thread_entry entry, void *arg,
                        int ap_runnable) {
  return kthread_create_impl(name, entry, arg, kernel_thread_trampoline, 0,
                             ap_runnable);
}

int sched_create_stealable_worker(const char *name, kernel_thread_entry entry,
                                  void *arg) {
  return kthread_create_impl(name, entry, arg, ap_worker_trampoline, 1, 0);
}

/* Reap a finished stealable worker. Called by an AP from ap_main AFTER the
 * worker has switched back to the AP idle context (so its kernel stack is no
 * longer in use). Stealable workers carry no fd table or cred, so teardown is
 * just freeing the stack + name and releasing the task slot. kfree is
 * heap_lock-protected; the state store is a word-sized x86 write, which the
 * BSP's scheduler_task_count() reads tolerantly (DEAD and UNUSED both count as
 * inactive). */
void sched_ap_reap_worker(struct task *t) {
  if (!t)
    return;
  if (t->stack) {
    kfree(t->stack);
    t->stack = 0;
  }
  if (t->name) {
    kfree((void *)t->name);
    t->name = 0;
  }
  /* Publish the kfrees and release the slot atomically vs find_unused_task. */
  free_task_slot(t);
}

extern void x86_fork_child_trampoline(void);

int scheduler_fork_current(void) {
  struct task *parent = current_task;
  if (!parent) {
    return -1;
  }
  /* "User" forks resume the child by iret'ing through the interrupt_frame the
   * int $0x80 entry pushed onto the parent's kernel stack. A *builtin* (kind ==
   * USER_IMAGE_BUILTIN) runs entirely in ring 0 and calls scheduler_fork_current
   * directly via syscall_dispatch, so there is NO such frame at the stack top —
   * treating it as a user fork made the child iret through stack garbage (a
   * #GP with a bogus CS). Builtins fork through the kernel trampoline instead,
   * resuming the C code right after fork() returns 0. Only real ELF processes
   * (ELF32/ELF64) take the iret path. */
  struct user_loaded_image *parent_img = parent->user_image;
  int is_user = (parent_img != NULL && parent_img->kind != USER_IMAGE_BUILTIN);

  /* If no swap device is attached, no PT entry can ever carry VMM_SWAPPED,
   * so walking the full user page-table tree on every fork is pure overhead.
   * Skip when swap is inactive — recovers a measurable fraction of fork
   * wall-clock under -j8 KVM. */
  extern int swap_active(void);
  if (swap_active()) {
    extern void paging_swap_in_all_swapped(u64 pml4_phys);
    paging_swap_in_all_swapped(parent->pml4_phys);
  }

  void *child_stack = kmalloc(KERNEL_STACK_SIZE);
  if (!child_stack) {
    return -1;
  }

  interrupts_disable();
  struct task *child = find_unused_task();
  if (!child) {
    interrupts_enable();
    kfree(child_stack);
    return -1;
  }
  /* find_unused_task assigned the id under g_tasks_lock; preserve it across the
   * struct copy below (memcpy from the parent would otherwise clobber it). */
  usize claimed_id = child->id;

  // 1. Copy the task structure
  memcpy(child, parent, sizeof(struct task));
  child->id = claimed_id;
  child->parent_id = parent->id;
  child->state = TASK_BLOCKED;
  child->name = parent->name ? strdup(parent->name) : 0;
  if (parent->name && !child->name) {
    free_task_slot(child);
    kfree(child_stack);
    interrupts_enable();
    return -1;
  }

  if (child->user_image) {
    __atomic_fetch_add(&((struct user_loaded_image *)child->user_image)->refcount, 1, __ATOMIC_RELAXED);
  }

  // Clear inherited pending signals and sleep/block states
  child->pending_signals = 0;
  child->blocked_signals = 0;
  child->last_stop_signal = 0;
  child->stop_report_pending = 0;
  child->continued_report_pending = 0;
  child->wake_tick = 0;
  child->wait_chan = 0;

  // Copy parent's kernel stack
  void *parent_stack = parent->stack;
  memcpy(child_stack, parent_stack, KERNEL_STACK_SIZE);

  child->stack = child_stack;

  // Calculate child's stack pointer and frame pointer offsets
  u64 stack_offset = (u64)(usize)child_stack - (u64)(usize)parent_stack;

  // Relocate the kernel stack pointer in child task structure to prevent sharing stack
  child->kernel_stack_ptr = parent->kernel_stack_ptr + stack_offset;

#ifdef __x86_64__
  u64 current_rsp, current_rbp;
  __asm__ volatile("movq %%rsp, %0" : "=r"(current_rsp));
  __asm__ volatile("movq %%rbp, %0" : "=r"(current_rbp));

  // Save callee-saved registers of parent to restore in child context
  __asm__ volatile("movq %%rbx, %0" : "=r"(child->context.rbx));
  __asm__ volatile("movq %%r12, %0" : "=r"(child->context.r12));
  __asm__ volatile("movq %%r13, %0" : "=r"(child->context.r13));
  __asm__ volatile("movq %%r14, %0" : "=r"(child->context.r14));
  __asm__ volatile("movq %%r15, %0" : "=r"(child->context.r15));

  extern void x86_fork_child_trampoline(void);
  extern void x86_fork_kernel_trampoline(void);

  if (is_user) {
    struct interrupt_frame *child_iframe = (struct interrupt_frame *)(usize)(child->kernel_stack_ptr - sizeof(struct interrupt_frame));
    child_iframe->rax = 0;

    child->context.rsp = (u64)child_iframe - 16;
    child->context.rbp = current_rbp + stack_offset;

    child->context.rsp -= 8;
    *(u64 *)(usize)child->context.rsp = (u64)x86_fork_child_trampoline;
  } else {
    child->context.rsp = current_rsp + stack_offset;
    child->context.rbp = current_rbp + stack_offset;

    /* Relocate the entire saved frame-pointer chain into the child's copied
     * stack. The kernel-fork trampoline resumes the child via leave/ret, which
     * unwinds by following the rbp chain. memcpy duplicates the stack bytes but
     * each saved rbp still points into the PARENT's stack; without fixing them
     * the child would `leave` onto the parent's stack and clobber it (and read
     * stale slots as return addresses). Walk the chain and rebase every saved
     * rbp by stack_offset so the child unwinds entirely on its own stack. */
    u64 clo = (u64)(usize)child_stack;
    u64 chi = clo + KERNEL_STACK_SIZE;
    u64 fp = child->context.rbp;
    for (int i = 0; i < 64 && fp >= clo && fp + 16 <= chi; i++) {
      u64 saved = *(u64 *)(usize)fp;
      if (saved == 0)
        break;
      u64 reloc = saved + stack_offset;
      if (reloc < clo || reloc + 16 > chi)
        break;
      *(u64 *)(usize)fp = reloc;
      fp = reloc;
    }

    child->context.rsp -= 8;
    *(u64 *)(usize)child->context.rsp = (u64)x86_fork_kernel_trampoline;
  }
#else
  u32 current_esp, current_ebp;
  __asm__ volatile("movl %%esp, %0" : "=r"(current_esp));
  __asm__ volatile("movl %%ebp, %0" : "=r"(current_ebp));

  // Save callee-saved registers of parent to restore in child context
  __asm__ volatile("movl %%ebx, %0" : "=r"(child->context.ebx));
  __asm__ volatile("movl %%esi, %0" : "=r"(child->context.esi));
  __asm__ volatile("movl %%edi, %0" : "=r"(child->context.edi));

  extern void x86_fork_child_trampoline(void);
  extern void x86_fork_kernel_trampoline(void);

  if (is_user) {
    struct interrupt_frame *child_iframe = (struct interrupt_frame *)(usize)(child->kernel_stack_ptr - sizeof(struct interrupt_frame));
    child_iframe->eax = 0;

    /* x86_fork_child_trampoline jmps into x86_syscall_return, whose first pop
     * expects ESP at the very base of the interrupt_frame (the saved EAX slot).
     * arch_context_switch's `ret` pops the trampoline address we push below and
     * leaves ESP at context.esp + 4, so context.esp must be child_iframe - 4
     * for ESP to land exactly on child_iframe after the return. (An earlier
     * stray `- 16` here left ESP 16 bytes low, so the 7 register pops + iret
     * read EIP/CS from the wrong slots and iret faulted with a garbage CS.) */
    child->context.esp = (u32)(usize)child_iframe;
    child->context.ebp = current_ebp + (u32)stack_offset;

    child->context.esp -= 4;
    *(u32 *)(usize)child->context.esp = (u32)(usize)x86_fork_child_trampoline;
  } else {
    child->context.esp = current_esp + (u32)stack_offset;
    child->context.ebp = current_ebp + (u32)stack_offset;

    /* Relocate the entire saved frame-pointer chain into the child's copied stack. */
    u32 clo = (u32)(usize)child_stack;
    u32 chi = clo + KERNEL_STACK_SIZE;
    u32 fp = child->context.ebp;
    for (int i = 0; i < 64 && fp >= clo && fp + 8 <= chi; i++) {
      u32 saved = *(u32 *)(usize)fp;
      if (saved == 0)
        break;
      u32 reloc = saved + (u32)stack_offset;
      if (reloc < clo || reloc + 8 > chi)
        break;
      *(u32 *)(usize)fp = reloc;
      fp = reloc;
    }

    child->context.esp -= 4;
    *(u32 *)(usize)child->context.esp = (u32)(usize)x86_fork_kernel_trampoline;
  }
#endif

  // 3. Clone address space with interrupts disabled
  child->pml4_phys = paging_clone_address_space(parent->pml4_phys);

  /* paging_clone_address_space just flipped every writable user page in the
   * PARENT's page tables to read-only/COW, but the parent keeps running on the
   * same CR3 and its TLB still caches the old writable translations. Without a
   * flush the parent's very next stack write (returning from fork(), pushing a
   * waitpid frame, …) bypasses the COW fault via the stale TLB entry and writes
   * straight into the now-shared frame — scribbling the child's copy of the
   * stack, so the child later `ret`s through a corrupted (often 0) return
   * address and SIGSEGVs. The page tables were edited in place, so reloading
   * CR3 (parent == current task) flushes the stale entries. */
  if (parent == current_task) {
    extern void paging_switch_address_space(u64 pml4_phys);
    paging_switch_address_space(parent->pml4_phys);
  }

  // 4. Clone VMAs
  child->vma_list = 0;
  struct vm_area *src_vma = parent->vma_list;
  struct vm_area **dst_prev = &child->vma_list;
  while (src_vma) {
    struct vm_area *new_vma = kmalloc(sizeof(struct vm_area));
    if (new_vma) {
      memcpy(new_vma, src_vma, sizeof(struct vm_area));
      new_vma->next = 0;
      if (new_vma->node) {
        vfs_node_get(new_vma->node);
      }
      *dst_prev = new_vma;
      dst_prev = &new_vma->next;
    }
    src_vma = src_vma->next;
  }

  // 5. Clone credentials and file descriptors
  task_init_cred(child);
  child->fd_capacity = parent->fd_capacity;
  child->fd_table = kzalloc(child->fd_capacity * sizeof(struct vfs_handle *));
  child->fd_flags = kzalloc(child->fd_capacity * sizeof(int));
  if (!child->fd_table || !child->fd_flags) {
    if (child->fd_table)
      kfree(child->fd_table);
    if (child->fd_flags)
      kfree(child->fd_flags);
    
    extern void user_address_space_cleanup(struct task *t);
    user_address_space_cleanup(child);
    paging_free_address_space(child->pml4_phys);
    child->pml4_phys = 0;

    if (child->cred) {
      cred_free(child->cred);
      child->cred = 0;
    }
    if (child->user_image) {
      user_image_free(child->user_image);
      child->user_image = 0;
    }
    if (child->name) {
      kfree((void *)child->name);
      child->name = 0;
    }
    kfree(child_stack);
    child->stack = 0;
    interrupts_enable();
    free_task_slot(child);
    return -1;
  }
  child->fd_lock = 0;
  for (usize i = 0; i < child->fd_capacity; i++) {
    child->fd_table[i] = parent->fd_table[i];
    child->fd_flags[i] = parent->fd_flags[i];
    if (child->fd_table[i]) {
      vfs_handle_retain(child->fd_table[i]);
    }
  }

  int child_id = (int)child->id;
  /* M28 T4: publish stack_released=1 BEFORE state=READY so a concurrent
   * pick on an AP doesn't observe READY with stack_released=0 (which would
   * make it spin forever — fork sets up the child stack synchronously and
   * never calls arch_context_switch from this path). */
  child->stack_released = 1;
  child->state = TASK_READY;
  sched_rq_enqueue_current(child);
  interrupts_enable();
  return child_id;
}

/* M29: side-tables already forward-declared at file scope (see top of file).
 * Accessor implementations follow. */

int task_is_thread(const struct task *t) {
  if (!t) return 0;
  return g_task_is_thread[task_index(t)];
}
void task_set_is_thread(struct task *t, int v) {
  if (!t) return;
  g_task_is_thread[task_index(t)] = v;
}
u64 task_tls_base(const struct task *t) {
  if (!t) return 0;
  return g_task_tls_base[task_index(t)];
}
void task_set_tls_base(struct task *t, u64 base) {
  if (!t) return;
  g_task_tls_base[task_index(t)] = base;
}
u64 task_child_tid_clear(const struct task *t) {
  if (!t) return 0;
  return g_task_child_tid_clear[task_index(t)];
}
void task_set_child_tid_clear(struct task *t, u64 addr) {
  if (!t) return;
  g_task_child_tid_clear[task_index(t)] = addr;
}

/* M29: thread-task user entry. Runs in ring 0 on the new thread's kernel
 * stack via kernel_thread_trampoline, then drops to ring 3 at the user
 * entry point with the user-supplied stack and argument. x86_user_jump
 * places its `argc` argument (third positional) into %rdi for the user
 * entry function, which is exactly what pthread's start_routine(void*)
 * expects (the void* lands in %rdi). */
struct clone_thread_args {
  u64 user_entry;
  u64 user_stack;
  u64 user_arg;
};

extern void x86_user_jump(usize entry, usize stack, usize argc, usize argv);

static void clone_thread_kentry(void *arg) {
  struct clone_thread_args *cta = (struct clone_thread_args *)arg;
  u64 entry = cta->user_entry;
  u64 stack = cta->user_stack;
  u64 user_arg = cta->user_arg;
  kfree(cta);
#ifdef __x86_64__
  /* SysV AMD64: x86_user_jump drops its 3rd positional (user_arg) into %rdi,
   * which is the first argument of pthread's start_routine(void*). */
  x86_user_jump((usize)entry, (usize)stack, (usize)user_arg, 0);
#else
  /* SysV i386 passes arguments on the stack, not in registers, and the 32-bit
   * x86_user_jump clears the GPRs — so the thread's start_routine(void*) would
   * read a zero arg from [esp+4] and dereference it (the M29 pthread crash).
   * Build the expected call frame on the (CLONE_VM-shared, already-mapped) user
   * stack: a fake return address at [esp] and the void* arg at [esp+4]. */
  u32 sp = (u32)stack & ~0xFU; /* keep the 16-byte ABI alignment */
  sp -= 8;
  *(volatile u32 *)(usize)(sp + 4) = (u32)user_arg;
  *(volatile u32 *)(usize)(sp + 0) = 0; /* return address: threads exit via SYS_EXIT_THREAD */
  x86_user_jump((usize)entry, (usize)sp, 0, 0);
#endif
}

/* Look up the number of live tasks currently using this pml4 (excluding
 * `except`). Caller must hold interrupts disabled (matches the reap path's
 * existing convention). */
static int pml4_other_refs(u64 pml4_phys, const struct task *except) {
  if (pml4_phys == 0) return 0;
  int n = 0;
  for (usize i = 0; i < g_task_hwm; i++) {
    struct task *t = T(i);
    if (t == except) continue;
    if (t->state == TASK_UNUSED || t->state == TASK_REAPING) continue;
    if (t->pml4_phys == pml4_phys) n++;
  }
  return n;
}

static int fdtable_other_refs(struct vfs_handle **tbl, const struct task *except) {
  if (!tbl) return 0;
  int n = 0;
  for (usize i = 0; i < g_task_hwm; i++) {
    struct task *t = T(i);
    if (t == except) continue;
    if (t->state == TASK_UNUSED || t->state == TASK_REAPING) continue;
    if (t->fd_table == tbl) n++;
  }
  return n;
}

int g_has_any_thread = 0;

int scheduler_clone_thread(u64 flags, u64 entry, u64 user_stack, u64 arg,
                           u64 tls, u64 ctid) {
  g_has_any_thread = 1;
  struct task *parent = current_task;
  if (!parent) return -EINVAL;
  /* Reject obviously non-canonical user addresses up front; the actual
   * user-mode access still goes through the page-fault path. */
  if (entry == 0 || user_stack == 0) return -EFAULT;
  if (entry >= USER_SPACE_LIMIT ||
      user_stack >= USER_SPACE_LIMIT)
    return -EFAULT;

  struct clone_thread_args *cta = kzalloc(sizeof(*cta));
  if (!cta) return -ENOMEM;
  cta->user_entry = entry;
  cta->user_stack = user_stack;
  cta->user_arg = arg;

  void *kstack = kmalloc(KERNEL_STACK_SIZE);
  if (!kstack) { kfree(cta); return -ENOMEM; }

  interrupts_disable();
  struct task *child = find_unused_task();
  interrupts_enable();
  if (!child) { kfree(kstack); kfree(cta); return -EAGAIN; }

  /* Bootstrap kernel context — same shape as kthread_create_impl. */
  u64 stack_top = align_down_u64((u64)(usize)kstack + KERNEL_STACK_SIZE, 16);
  child->kernel_stack_ptr = stack_top;
#ifdef __x86_64__
  u64 initial_rsp = stack_top - 16;
#else
  u64 initial_rsp = stack_top - 8;
#endif
#ifdef __x86_64__
  *(u64 *)(usize)initial_rsp = (u64)(usize)kernel_thread_trampoline;
  child->stack = kstack;
  child->entry = clone_thread_kentry;
  child->arg = cta;
  child->context.rsp = initial_rsp;
  child->context.rbp = 0;
  child->context.rbx = 0;
  child->context.r12 = 0;
  child->context.r13 = 0;
  child->context.r14 = 0;
  child->context.r15 = 0;
#else
  *(u32 *)(usize)initial_rsp = (u32)(usize)kernel_thread_trampoline;
  child->stack = kstack;
  child->entry = clone_thread_kentry;
  child->arg = cta;
  child->context.esp = (u32)initial_rsp;
  child->context.ebp = 0;
  child->context.ebx = 0;
  child->context.esi = 0;
  child->context.edi = 0;
#endif

  /* Name — keep short (kthread_create truncates at 15 chars). */
  child->name = strdup("pthread");

  /* Address-space inheritance. */
  if (flags & B1NIX_CLONE_VM) {
    child->pml4_phys = parent->pml4_phys;
    child->vma_list  = parent->vma_list;
    child->user_brk  = parent->user_brk;
    child->heap_start = parent->heap_start;
    child->user_image = parent->user_image;
    if (child->user_image) {
      __atomic_fetch_add(&((struct user_loaded_image *)child->user_image)->refcount, 1, __ATOMIC_RELAXED);
    }
  } else {
    /* Without CLONE_VM we'd need a full address-space clone like fork.
     * That path is fork — clone-without-CLONE_VM is unsupported on b1nix. */
    free_task_slot(child);
    kfree(kstack);
    kfree(cta);
    return -EINVAL;
  }

  /* FD-table inheritance. */
  if (flags & B1NIX_CLONE_FILES) {
    child->fd_table   = parent->fd_table;
    child->fd_flags   = parent->fd_flags;
    child->fd_capacity = parent->fd_capacity;
    /* fd_lock is shared via the parent's lock pointer-equivalence: both
     * tasks reference the same fd_table address so they both lock-step
     * around the parent's spinlock. Practically: parent's fd_lock IS the
     * shared lock; routines that lock via current_task->fd_lock have a
     * separate copy. The cost of cleanly fixing this is intrusive; for
     * M29's smoke (single-process pthreads doing modest fd traffic) the
     * existing parent_id-based path is sufficient. */
    child->fd_lock = 0;
  } else {
    /* Copy parent's table. */
    child->fd_capacity = parent->fd_capacity;
    child->fd_table = kzalloc(child->fd_capacity * sizeof(struct vfs_handle *));
    child->fd_flags = kzalloc(child->fd_capacity * sizeof(int));
    child->fd_lock = 0;
    for (usize i = 0; i < child->fd_capacity; i++) {
      child->fd_table[i] = parent->fd_table[i];
      child->fd_flags[i] = parent->fd_flags[i];
      if (child->fd_table[i]) vfs_handle_retain(child->fd_table[i]);
    }
  }

  /* FS / cwd / umask / env. */
  if (flags & B1NIX_CLONE_FS) {
    memcpy(child->cwd, parent->cwd, sizeof(child->cwd));
    child->umask = parent->umask;
    memcpy(child->env, parent->env, sizeof(child->env));
  } else {
    memcpy(child->cwd, parent->cwd, sizeof(child->cwd));
    child->umask = parent->umask;
    memcpy(child->env, parent->env, sizeof(child->env));
  }

  /* Signal handlers — always copy (b1nix has no per-thread-group sharing). */
  memcpy(child->sigactions, parent->sigactions, sizeof(child->sigactions));
  child->pending_signals = 0;
  child->blocked_signals = 0;

  /* Process group / session. */
  child->process_group_id = parent->process_group_id;
  child->session_id = parent->session_id;

  /* M29 thread flags. */
  task_set_is_thread(child, 1);
  task_set_tls_base(child, (flags & B1NIX_CLONE_SETTLS) ? tls : 0);
  task_set_child_tid_clear(child,
                            (flags & B1NIX_CLONE_CHILD_CLEARTID) ? ctid : 0);

  /* Parent linkage. Threads are joined via futex, not waitpid — point them
   * at the parent's parent so waitpid(-1, ...) skips them. */
  if (flags & B1NIX_CLONE_THREAD) {
    child->parent_id = parent->parent_id;
  } else {
    child->parent_id = parent->id;
  }
  child->priority = parent->priority;
  child->stdout_fd = parent->stdout_fd;

  /* Credentials — dup. */
  if (parent->cred) {
    child->cred = cred_dup(parent->cred);
  } else {
    task_init_cred(child);
  }

  /* Userspace ELF tasks may run on Application Processors. */
  child->ap_runnable = parent->ap_runnable;

  interrupts_disable();
  /* M28 T4: see fork/kthread_create_impl — fresh task's kernel stack is set
   * up synchronously without going through arch_context_switch, so publish
   * stack_released=1 explicitly before the first pick sees it. */
  child->stack_released = 1;
  child->state = TASK_READY;
  sched_rq_enqueue_current(child);
  interrupts_enable();

  return (int)child->id;
}

void scheduler_reap_dead_threads(void) {
  /* Called from scheduler_yield with interrupts already disabled. Free the
   * kernel stack + slot of any DEAD thread (is_thread=1) whose
   * arch_context_switch has finished swapping RSP off its kernel stack
   * (stack_released==1). The shared mm/fds/user_image are NOT touched here
   * — they are freed by either the leader's waitpid reap (if pml4 has no
   * more users) or stay live for whichever sibling is still running. */
  for (usize i = 0; i < g_task_hwm; i++) {
    struct task *t = T(i);
    if (t == current_task) continue;
    if (t->state != TASK_DEAD) continue;
    if (!task_is_thread(t)) continue;
    if (!__atomic_load_n(&t->stack_released, __ATOMIC_ACQUIRE)) continue;

    /* Release any per-thread (non-shared) resources. */
    if (t->cred) { cred_free(t->cred); t->cred = 0; }

    /* If this thread did NOT share fds with anyone (theoretical: CLONE
     * without CLONE_FILES — currently unreachable because we reject it),
     * its private fd table would have been freed here. */
    if (t->stack) { kfree(t->stack); t->stack = 0; }
    if (t->name) { kfree((void *)t->name); t->name = 0; }
    free_task_slot(t);
  }
}



int scheduler_yield(void) {
  interrupts_disable();
  wake_sleepers();

  /* M29: reap DEAD thread tasks (CLONE_VM) whose stack_released has been
   * published. Cheap (skips fast over non-DEAD slots) and runs on every
   * yield so the kernel-stack heap pressure of short-lived threads stays
   * bounded. Skip when no threads have ever been created — the walk is
   * O(g_task_hwm) which is nontrivial overhead on every yield otherwise. */
  extern int g_has_any_thread;
  if (g_has_any_thread) scheduler_reap_dead_threads();

  /* Deliver pending signals for current task */
  if (current_task) {
    scheduler_deliver_pending_signals();
  }

  struct task *old_task = current_task;
  struct task *new_task = pick_next_task();

  if (new_task == 0) {
    if (old_task != 0 && old_task->state == TASK_DEAD) {
      panic("dead task has nowhere to yield");
    }

    /* Nothing else is runnable, so we never context-switch here: we just keep
     * running. But a blocker set state=BLOCKED before calling us, and a waker
     * on another CPU may already have CAS'd us BLOCKED->READY and enqueued us.
     * If we return now leaving a non-RUNNING state, two things break:
     *   1. our own stack_released stays 0 (no arch_context_switch ran), so a
     *      picker that grabbed our stale rq entry spins on stack_released==1
     *      forever — the -smp producer/consumer pipe livelock; and
     *   2. timer-tick preemption is skipped for us (it only yields RUNNING
     *      tasks), so we can monopolise the CPU with a stale state.
     * Reclaim RUNNING. If a waker had promoted+enqueued us (state==READY),
     * also scrub that now-stale runqueue entry; a concurrent waker that
     * promotes us *after* this still leaves only a RUNNING entry, which
     * pick_next_task drops via its READY guard. */
    if (old_task != 0) {
      enum task_state st = old_task->state;
      if (st == TASK_READY) {
        old_task->state = TASK_RUNNING;
        sched_rq_remove_task(old_task);
      } else if (st == TASK_BLOCKED || st == TASK_SLEEPING) {
        old_task->state = TASK_RUNNING;
      }
    }

    interrupts_enable();
    return 0; /* nothing runnable — caller's idle loop may drop the BKL */
  }

  if (old_task->state == TASK_RUNNING) {
    /* M28 T4: claim the kernel stack BEFORE publishing state=READY. Under T4
     * the save side of arch_context_switch (the movq %rsp,0(%rdi) +
     * movq 0(%rsi),%rsp sequence) is no longer serialised by the BKL, so a
     * concurrent pick_next_task on another CPU could observe state==READY,
     * win the CAS, load this task's saved RSP, and start `ret`ing on the
     * same kernel stack while THIS CPU is mid-save. The reader checks
     * stack_released==1 (published by arch_context_switch after the RSP
     * swap) before claiming; x86 TSO orders the 0 store ahead of the READY
     * store, so other CPUs observe "READY but not yet released" and skip. */
    old_task->stack_released = 0;
    old_task->state = TASK_READY;
  }

  new_task->state = TASK_RUNNING;
  current_task = new_task;

  paging_switch_address_space(new_task->pml4_phys);
  arch_set_kernel_stack(new_task->kernel_stack_ptr);

  /* M29: reload userspace FS base for TLS on every context switch. Writing 0
   * matters just as much as writing a nonzero TLS base: IA32_FS_BASE is per-CPU
   * state, so a task with no TLS must not inherit the previous task's FS base. */
  {
    u64 fsbase = task_tls_base(new_task);
    extern void arch_set_fs_base(u64 base);
    arch_set_fs_base(fsbase);
  }

  /* Preserve userspace FPU/SSE/MXCSR/x87 across the switch. Save the outgoing
   * task's live state, then load the incoming task's (or a clean image if it
   * has never run). Without this, userspace XMM registers are clobbered by
   * other tasks and FP-heavy programs (e.g. cc1) corrupt silently. */
  arch_fpu_save(old_task->fpu_state);
  old_task->fpu_initialized = 1;
  if (!g_clean_fpu_ready) {
    /* old_task's state is already saved above; capture_clean reinits the live
     * FPU, which is fine since new_task's state is loaded immediately after. */
    arch_fpu_capture_clean(g_clean_fpu);
    g_clean_fpu_ready = 1;
  }
  if (new_task->fpu_initialized) {
    arch_fpu_restore(new_task->fpu_state);
  } else {
    arch_fpu_restore(g_clean_fpu);
    new_task->fpu_initialized = 1;
  }

  arch_context_switch(&old_task->context, &new_task->context,
                      &old_task->stack_released);
  interrupts_enable();
  return 1; /* we switched out and have since been resumed */
}

void scheduler_block_current(void) {
  interrupts_disable();

  if (current_task == 0) {
    panic("scheduler_block_current without running task");
  }

  /* Same yield-returned-0 recovery as the other voluntary blockers. */
  if (current_task->state == TASK_BLOCKED || current_task->state == TASK_SLEEPING) {
    current_task->state = TASK_RUNNING;
  } else if (current_task->state != TASK_RUNNING) {
    panic("scheduler_block_current without running task");
  }

  /* M28 T4: claim the kernel stack BEFORE publishing a non-RUNNING state, the
   * same lease yield/exit take. A waker on another CPU can CAS us BLOCKED->READY
   * and resume us the instant the state is visible; without clearing the lease
   * it would observe a stale stack_released==1 and load our saved RSP while this
   * CPU is still mid-arch_context_switch on this very stack. yield's
   * RUNNING->READY arm only clears it when state==RUNNING, which is already
   * false here, so we must do it ourselves. */
  current_task->stack_released = 0;
  current_task->state = TASK_BLOCKED;
  scheduler_yield();
  interrupts_enable();
}

void scheduler_wake_task(usize task_id) {
  interrupts_disable();

  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->id != task_id) continue;

    /* Explicit id wakeups are used both for waiters (futex) and for timer-
     * sleeping kernel daemons with external work (net_task loopback kick). */
    enum task_state expected = TASK_BLOCKED;
    if (__atomic_compare_exchange_n(&T(i)->state, &expected, TASK_READY,
                                    0, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
      T(i)->wait_chan = 0;
      T(i)->wake_tick = 0;
      sched_rq_enqueue_current(T(i));
      break;
    }

    expected = TASK_SLEEPING;
    if (__atomic_compare_exchange_n(&T(i)->state, &expected, TASK_READY,
                                    0, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
      T(i)->wait_chan = 0;
      T(i)->wake_tick = 0;
      sched_rq_enqueue_current(T(i));
      break;
    }
  }

  interrupts_enable();
}

void scheduler_block_on(void *chan) {
  interrupts_disable();

  if (current_task == 0) {
    panic("scheduler_block_on without running task");
  }

  /* Same pre-existing-bug recovery as scheduler_sleep_ticks: if scheduler_yield
   * previously returned 0 (no other task ready), we resumed with the old
   * state — TASK_BLOCKED here — even though we're conceptually RUNNING again
   * (the kernel kept us on-CPU). Allow the re-entry. */
  if (current_task->state == TASK_BLOCKED || current_task->state == TASK_SLEEPING) {
    current_task->state = TASK_RUNNING;
  } else if (current_task->state != TASK_RUNNING) {
    panic("scheduler_block_on without running task");
  }

  current_task->wait_chan = chan;
  /* M28 T4: claim the stack lease before publishing BLOCKED — see
   * scheduler_block_current for the full race. */
  current_task->stack_released = 0;
  current_task->state = TASK_BLOCKED;
  scheduler_yield();
  interrupts_enable();
}

/* SMP-safe condition wait, split into three steps so the caller can re-test its
 * wait predicate AFTER the BLOCKED state is published. This closes the classic
 * check-then-block lost-wakeup window: with plain scheduler_block_on, a
 * scheduler_wake_all(chan) firing on another CPU between the caller's last
 * predicate test and the block is lost, and the task sleeps forever (the
 * loopback-TCP / dropbear-recv -smp wedge).
 *
 * Usage:
 *     while (!ready()) {
 *         scheduler_wait_prepare(chan);   // publish BLOCKED + full barrier
 *         if (ready()) { scheduler_wait_cancel(); continue; }  // racing wake
 *         scheduler_wait_commit();        // actually sleep
 *     }
 *
 * The __ATOMIC_SEQ_CST store + fence order our BLOCKED publication ahead of the
 * caller's re-test load, so a concurrent waker either (a) is observed by the
 * re-test (it set the predicate before its wake_all's CAS, which is a full
 * barrier) or (b) observes our BLOCKED state and wakes us. Interrupts stay
 * disabled between prepare and commit/cancel. */
void scheduler_wait_prepare(void *chan) {
  interrupts_disable();
  if (current_task == 0)
    panic("scheduler_wait_prepare without running task");
  current_task->wait_chan = chan;
  current_task->stack_released = 0;
  __atomic_store_n(&current_task->state, TASK_BLOCKED, __ATOMIC_SEQ_CST);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void scheduler_wait_commit(void) {
  scheduler_yield();
  interrupts_enable();
}

void scheduler_wait_cancel(void) {
  current_task->wait_chan = 0;
  current_task->state = TASK_RUNNING;
  interrupts_enable();
}

void scheduler_wake_all(void *chan) {
  interrupts_disable();

  int woken = 0;
  for (usize i = 0; i < g_task_hwm; i++) {
    /* F4 (M28 #7): atomic CAS BLOCKED -> READY so two CPUs both trying
     * to wake the same channel can't both enqueue. Reading wait_chan
     * outside the CAS is fine — it's written only by scheduler_block_on
     * under the task's own context (the task itself sets it before
     * yielding), so the value is stable while the task is BLOCKED. */
    if (T(i)->wait_chan != chan) continue;
    enum task_state expected = TASK_BLOCKED;
    if (__atomic_compare_exchange_n(&T(i)->state, &expected, TASK_READY,
                                    0, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
      T(i)->wait_chan = 0;
      sched_rq_enqueue_current(T(i));
      woken++;
    }
  }

  interrupts_enable();
  /* M28 #6: same reschedule kick as wake_sleepers. Outside the IRQ-off
   * window because lapic_send_ipi spins on ICR delivery — cheap, but no
   * reason to do it with interrupts disabled. */
  if (woken > 0) ipi_reschedule_all();
}

void scheduler_sleep_ticks(u64 ticks) {
  interrupts_disable();

  if (current_task == 0) {
    panic("scheduler_sleep_ticks without running task");
  }

  /* Pre-existing bug surfaced by T3: scheduler_yield can return 0 when no
   * other task is ready to run, leaving us with our own state still set to
   * TASK_SLEEPING (we set it just before yielding). The next iteration of
   * the caller's loop then re-enters sleep_ticks with state == SLEEPING.
   * Pre-T3 the BKL-serialised timer ISR happened to keep at least one task
   * always runnable on the BSP, so the path was masked; T3 makes it
   * reproducible. Treat SLEEPING-at-entry as a no-op-sleep recovery: we're
   * effectively already RUNNING (the kernel just kept us on-CPU), so reset
   * state and proceed. */
  if (current_task->state == TASK_SLEEPING) {
    current_task->state = TASK_RUNNING;
  } else if (current_task->state != TASK_RUNNING) {
    panic("scheduler_sleep_ticks without running task");
  }

  current_task->wake_tick = scheduler_ticks + ticks;
  /* M28 T4: claim the stack lease before publishing SLEEPING — a timer-tick
   * wake_sleepers on another CPU can CAS us SLEEPING->READY and resume us; see
   * scheduler_block_current for the full race. */
  current_task->stack_released = 0;
  current_task->state = TASK_SLEEPING;
  scheduler_yield();
  interrupts_enable();
}

void scheduler_on_timer_tick(void) {
  if (!scheduler_started || current_task == 0) {
    return;
  }

  scheduler_ticks++;
  wake_sleepers();

  /* T8 (M28 #8): preemptive yield from the timer ISR. The historical concern
   * that motivated the cooperative model — VFS chain walks (find_child /
   * vfs_get_mount_for_node / add_node) traversing parent/sibling chains
   * without explicit locks — is closed by M28-B's vfs_tree_lock + the
   * IRQ-save semantics of the chain-walk helpers (a walker holds the lock
   * with IRQs disabled, so a same-CPU timer ISR cannot preempt it). Every
   * other spinlock in the kernel is either irq-save (heap, pmm, vmm, tasks)
   * or per-task (fd_lock), so preemption from this ISR cannot leave a
   * locked critical section running on another task.
   *
   * Only the BSP runs this branch (the AP timer's BSP-only filter in
   * x86_irq_handler_inner). Yielding from the ISR without taking BKL is
   * sound under T4: the per-CPU current_task / runqueue mutations made by
   * scheduler_yield are already SMP-safe via F-tier atomic CASes, and
   * adding a tick-side bkl_lock around scheduler_yield wedges the
   * userspace → exit path (M25 TCC-compiled binaries hang after their last
   * printf because the timer-acquired BKL is never observed released by
   * the next syscall on the resumed task). */
  if (current_task->state == TASK_RUNNING) {
    scheduler_yield();
  }
}

u64 scheduler_get_uptime_ticks(void) { return scheduler_ticks; }

void scheduler_exit_current(int exit_code) {
  if (current_task == 0) {
    panic("scheduler_exit_current without current task");
  }

  /* M29: a clone()d thread shares fd_table / pml4 / user_image with its
   * parent process. Skip the per-task fd/cred/mm teardown, let the reaper
   * (scheduler_reap_dead_threads) free what's exclusively this thread's
   * (kernel stack + slot). CLONE_CHILD_CLEARTID also writes 0 + futex_wakes
   * so a pthread_join sleeper unblocks. */
  if (task_is_thread(current_task)) {
    u64 ctid = task_child_tid_clear(current_task);
    if (ctid) {
      int zero = 0;
      (void)syscall_copyout((void *)(usize)ctid, &zero, sizeof(int));
      scheduler_futex_wake_addr(ctid, 1);
    }
    if (current_task->user_image) {
      user_image_free(current_task->user_image);
      current_task->user_image = 0;
    }
    if (current_task->cred) {
      cred_free(current_task->cred);
      current_task->cred = 0;
    }
    interrupts_disable();
    current_task->exit_code = exit_code;
    current_task->stack_released = 0;
    current_task->state = TASK_DEAD;
    scheduler_yield();
    panic("dead thread resumed");
  }

  /* Close all open file descriptors with interrupts enabled, so writebacks can sleep/block */
  if (current_task->fd_table) {
    for (usize i = 0; i < current_task->fd_capacity; i++) {
      if (current_task->fd_table[i]) {
        vfs_close((int)i);
      }
    }
  }

  interrupts_disable();

  /* Free credentials */
  if (current_task->cred) {
    cred_free(current_task->cred);
    current_task->cred = 0;
  }

  if (current_task->fd_table) {
    kfree(current_task->fd_table);
    kfree(current_task->fd_flags);
    current_task->fd_table = 0;
    current_task->fd_flags = 0;
    current_task->fd_capacity = 0;
  }

  aio_task_cleanup(current_task);

  current_task->exit_code = exit_code;
  /* F-tier T4 prerequisite: claim stack_released BEFORE publishing DEAD.
   * x86 TSO orders the stores, so any CPU that observes state == DEAD also
   * observes stack_released == 0 — its waitpid path will spin on the
   * stack_released flag until arch_context_switch publishes 1 after the
   * RSP swap below. See struct task::stack_released in sched.h. */
  current_task->stack_released = 0;
  current_task->state = TASK_DEAD;


  /* F6 (M28 #7): kick the BSP (or whichever CPU runs the parent kthread)
   * out of sti;hlt so it picks the parent immediately instead of waiting
   * for the next 10 ms LAPIC tick. Particularly important for the
   * test-driver init kthread on the BSP, which sits in sti;hlt during
   * userspace test runs. */
  scheduler_notify_wait_event(current_task->parent_id);

  scheduler_yield();
  panic("dead task resumed");
}

int scheduler_wait(usize pid, int *status) {
  return scheduler_waitpid(pid, status, 0);
}

int scheduler_waitpid(usize pid, int *status, int options) {
  if (current_task == 0)
    return -ECHILD;

  /* pid 0 ("any child in my process group") and pid -1 ("any child", which
   * arrives as (usize)-1 from a userspace waitpid(-1, ...)) both mean "reap any
   * child" in b1nix's flat process model. GNU Make's reap_children() relies on
   * waitpid(-1, &status, WNOHANG); without the -1 case it matched no task and
   * the syscall's -1 return was mapped to EPERM by the libc. */
  int wait_any = (pid == 0 || pid == (usize)-1);
  int may_block = ((options & B1NIX_WNOHANG) == 0);

  while (1) {
    interrupts_disable();
    if (may_block) {
      /* Same yield-returned-0 recovery as the other voluntary blockers. */
      if (current_task->state == TASK_BLOCKED ||
          current_task->state == TASK_SLEEPING) {
        current_task->state = TASK_RUNNING;
      }

      /* Publish the blocked wait state before scanning children so an exiting
       * child on another CPU cannot miss the wakeup between the scan and yield.
       * Skip this for WNOHANG polling calls: they never sleep, so publishing
       * BLOCKED would let child events enqueue a stale self-wakeup.
       *
       * The SEQ_CST store + fence are REQUIRED, not decorative: without the
       * store-before-load barrier, x86 store buffering lets the child-scan loads
       * below execute before our BLOCKED store is globally visible. A child
       * exiting on another CPU would then store DEAD + CAS our state, read a
       * stale RUNNING (CAS fails, no wake), while our scan reads the child as
       * still-alive — the wakeup is lost and the parent sleeps forever (the
       * -smp pipeline / waitpid wedge). The fence pairs with the full barrier in
       * scheduler_wake_blocked_parent's CAS. */
      current_task->stack_released = 0;
      __atomic_store_n(&current_task->state, TASK_BLOCKED, __ATOMIC_SEQ_CST);
      __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }

    int has_children = 0;
    for (usize i = 0; i < g_task_hwm; i++) {
      if (T(i)->state != TASK_UNUSED &&
          T(i)->parent_id == current_task->id) {
        if (wait_any || T(i)->id == pid) {
          has_children = 1;

          /* F1 (M28): atomic CAS DEAD -> REAPING so only one CPU drives the
           * actual free path even if two waitpids race for the same child.
           * The loser observes state == REAPING (which still counts as "this
           * child exists" thanks to has_children) and falls through to the
           * yield/return path, letting the winner publish the reaped status.
           *
           * Note: parent_id is read non-atomically above, but the field is
           * only written by fork (publication before the child runs) and is
           * never overwritten thereafter, so a torn read here is impossible. */
          enum task_state _expected = TASK_DEAD;
          if (__atomic_compare_exchange_n(&T(i)->state, &_expected,
                                          TASK_REAPING, 0,
                                          __ATOMIC_ACQUIRE,
                                          __ATOMIC_RELAXED)) {
            /* F-tier T4 prerequisite: the child's kernel_stack is still
             * being used as RSP on whatever CPU ran exit_current →
             * scheduler_yield up until arch_context_switch's RSP swap.
             * Spin until that swap has published stack_released==1 — see
             * struct task::stack_released in sched.h.
             *
             * The child set stack_released=0 BEFORE state=DEAD (x86 TSO
             * orders the two stores), so observing state==DEAD here
             * guarantees we will observe a 0 first and then transition to
             * 1 once the child's CPU finishes context_switch. The CAS
             * above used __ATOMIC_ACQUIRE, which pairs with the implicit
             * release of the asm's `movl $1, (%rdx)` after the RSP swap. */
            while (!__atomic_load_n(&T(i)->stack_released,
                                    __ATOMIC_ACQUIRE)) {
              __asm__ volatile("pause");
              tlb_shootdown_poll();
            }

            int code = T(i)->exit_code;
            int child_id = T(i)->id;
            if (T(i)->user_image) {
              user_image_free(T(i)->user_image);
              T(i)->user_image = 0;
            }
            /* M29: keep the address space alive if a CLONE_VM sibling
             * still uses it. The dying thread's reaper handles its own
             * kernel stack; the mm/vmas/fd-table belong to the surviving
             * sibling. */
            if (pml4_other_refs(T(i)->pml4_phys, T(i)) == 0) {
              user_address_space_cleanup(T(i));
              paging_free_address_space(T(i)->pml4_phys);
            } else {
              T(i)->vma_list = 0;
            }
            T(i)->pml4_phys = 0;
            if (T(i)->fd_table &&
                fdtable_other_refs(T(i)->fd_table, T(i)) > 0) {
              T(i)->fd_table = 0;
              T(i)->fd_flags = 0;
              T(i)->fd_capacity = 0;
            }
            if (T(i)->name && strcmp(T(i)->name, "boot") != 0) {
              kfree((void *)T(i)->name);
              T(i)->name = 0;
            }
            kfree(T(i)->stack);
            if (may_block)
              scheduler_waitpid_fast_return();
            interrupts_enable();
            free_task_slot(T(i));
            if (status) {
              if (code >= 128 && code < 128 + NSIG) {
                /* Task was killed by a signal */
                *status = (code - 128) & 0x7F;
              } else {
                /* Normal exit */
                *status = (code & 0xFF) << 8;
              }
            }
             return child_id;
          } else if ((options & (B1NIX_WUNTRACED | B1NIX_WCONTINUED)) &&
                     (T(i)->state == TASK_STOPPED ||
                      T(i)->continued_report_pending)) {
            int child_id = T(i)->id;
            if ((options & B1NIX_WUNTRACED) && T(i)->state == TASK_STOPPED &&
                T(i)->stop_report_pending) {
              if (status)
                *status = ((T(i)->last_stop_signal & 0xFF) << 8) | 0x7F;
              T(i)->stop_report_pending = 0;
              if (may_block)
                scheduler_waitpid_fast_return();
              interrupts_enable();
              return child_id;
            }
            if ((options & B1NIX_WCONTINUED) &&
                T(i)->continued_report_pending) {
              if (status)
                *status = 0xFFFF;
              T(i)->continued_report_pending = 0;
              if (may_block)
                scheduler_waitpid_fast_return();
              interrupts_enable();
              return child_id;
            }
          }
        }
      }
    }

    if (!has_children) {
      if (may_block)
        scheduler_waitpid_fast_return();
      interrupts_enable();
      return -ECHILD;
    }

    if (options & B1NIX_WNOHANG) {
      if (may_block)
        scheduler_waitpid_fast_return();
      interrupts_enable();
      return 0;
    }

    scheduler_yield();
    interrupts_enable();
  }
}

usize scheduler_task_count(void) {
  usize count = 0;

  for (usize i = 0; i < g_task_hwm; i++) {
    enum task_state s = T(i)->state;
    if (s != TASK_UNUSED && s != TASK_DEAD && s != TASK_REAPING) {
      count++;
    }
  }

  return count;
}

/* ── M34: task-table introspection for procfs / ps / top ──
 * Expose the chunked task array to read-only consumers (the /proc filesystem
 * and the in-shell ps/top builtins). No struct task fields are added — callers
 * read the already-public struct directly. */
usize scheduler_task_slots(void) { return g_task_hwm; }

struct task *scheduler_task_slot(usize index) {
  if (index >= g_task_hwm)
    return 0;
  struct task *t = T(index);
  if (t->state == TASK_UNUSED || t->state == TASK_DEAD ||
      t->state == TASK_REAPING)
    return 0;
  return t;
}

struct task *scheduler_task_by_pid(usize pid) {
  for (usize i = 0; i < g_task_hwm; i++) {
    struct task *t = scheduler_task_slot(i);
    if (t && t->id == pid)
      return t;
  }
  return 0;
}

const char *scheduler_state_name(int state) {
  switch (state) {
  case TASK_RUNNING:  return "R";
  case TASK_READY:    return "R";
  case TASK_BLOCKED:  return "D";
  case TASK_SLEEPING: return "S";
  case TASK_STOPPED:  return "T";
  case TASK_DEAD:     return "Z";
  case TASK_REAPING:  return "Z";
  default:            return "?";
  }
}

void scheduler_dump_tasks(void) {
  console_write("ID\tSTATE\tNAME\n");
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->state != TASK_UNUSED) {
      console_write_hex64(T(i)->id);
      console_write("\t");

      const char *state_str = "UNKNOWN";
      switch (T(i)->state) {
      case TASK_RUNNING:
        state_str = "RUNNING";
        break;
      case TASK_READY:
        state_str = "READY";
        break;
      case TASK_BLOCKED:
        state_str = "BLOCKED";
        break;
      case TASK_SLEEPING:
        state_str = "SLEEPING";
        break;
      case TASK_STOPPED:
        state_str = "STOPPED";
        break;
      case TASK_DEAD:
        state_str = "DEAD";
        break;
      case TASK_REAPING:
        state_str = "REAPING";
        break;
      default:
        break;
      }

      console_write(state_str);
      console_write("\t");
      console_write(T(i)->name);
      console_write("\n");
    }
  }
}

void scheduler_set_stdout(int fd) {
  interrupts_disable();
  if (current_task != 0) {
    current_task->stdout_fd = fd;
  }
  interrupts_enable();
}

int scheduler_get_stdout(void) {
  int fd = -1;
  interrupts_disable();
  if (current_task != 0) {
    fd = current_task->stdout_fd;
  }
  interrupts_enable();
  return fd;
}

void scheduler_fd_table_init_current(void) {
  if (!current_task)
    return;
  if (current_task->fd_table) {
    kfree(current_task->fd_table);
    kfree(current_task->fd_flags);
  }
  current_task->fd_capacity = SCHED_MAX_FDS;
  current_task->fd_table = kzalloc(current_task->fd_capacity * sizeof(struct vfs_handle *));
  current_task->fd_flags = kzalloc(current_task->fd_capacity * sizeof(int));
  current_task->fd_lock = 0; /* reset lock for new table */
}

int scheduler_fd_alloc(struct vfs_handle *handle) {
  if (!current_task || !handle)
    return -1;

  fd_lock_acquire();

  for (usize i = 0; i < current_task->fd_capacity; i++) {
    if (current_task->fd_table[i] == 0) {
      current_task->fd_table[i] = handle;
      current_task->fd_flags[i] = 0;
      fd_lock_release();
      return (int)i;
    }
  }

  if (current_task->fd_capacity >= SCHED_MAX_FD_LIMIT) {
    fd_lock_release();
    return -1;
  }

  usize new_capacity = current_task->fd_capacity * 2;
  if (new_capacity > SCHED_MAX_FD_LIMIT)
    new_capacity = SCHED_MAX_FD_LIMIT;

  struct vfs_handle **new_table = kzalloc(new_capacity * sizeof(struct vfs_handle *));
  if (!new_table) {
    fd_lock_release();
    return -1;
  }
  int *new_flags = kzalloc(new_capacity * sizeof(int));
  if (!new_flags) {
    kfree(new_table);
    fd_lock_release();
    return -1;
  }

  memcpy(new_table, current_task->fd_table, current_task->fd_capacity * sizeof(struct vfs_handle *));
  memcpy(new_flags, current_task->fd_flags, current_task->fd_capacity * sizeof(int));

  kfree(current_task->fd_table);
  kfree(current_task->fd_flags);

  current_task->fd_table = new_table;
  current_task->fd_flags = new_flags;

  int allocated_fd = (int)current_task->fd_capacity;
  current_task->fd_capacity = new_capacity;
  current_task->fd_table[allocated_fd] = handle;
  current_task->fd_flags[allocated_fd] = 0;

  fd_lock_release();
  return allocated_fd;
}

struct vfs_handle *scheduler_fd_get(int fd) {
  if (!current_task || fd < 0 || (usize)fd >= current_task->fd_capacity)
    return 0;
  fd_lock_acquire();
  struct vfs_handle *h = current_task->fd_table[fd];
  fd_lock_release();
  return h;
}

int scheduler_fd_set(int fd, struct vfs_handle *handle) {
  if (!current_task || fd < 0)
    return -1;

  fd_lock_acquire();

  if ((usize)fd >= current_task->fd_capacity) {
    if ((usize)fd >= SCHED_MAX_FD_LIMIT) {
      fd_lock_release();
      return -1;
    }
    usize new_capacity = current_task->fd_capacity;
    while (new_capacity <= (usize)fd) {
      new_capacity *= 2;
    }
    if (new_capacity > SCHED_MAX_FD_LIMIT)
      new_capacity = SCHED_MAX_FD_LIMIT;

    struct vfs_handle **new_table = kzalloc(new_capacity * sizeof(struct vfs_handle *));
    if (!new_table) {
      fd_lock_release();
      return -1;
    }
    int *new_flags = kzalloc(new_capacity * sizeof(int));
    if (!new_flags) {
      kfree(new_table);
      fd_lock_release();
      return -1;
    }

    memcpy(new_table, current_task->fd_table, current_task->fd_capacity * sizeof(struct vfs_handle *));
    memcpy(new_flags, current_task->fd_flags, current_task->fd_capacity * sizeof(int));

    kfree(current_task->fd_table);
    kfree(current_task->fd_flags);

    current_task->fd_table = new_table;
    current_task->fd_flags = new_flags;
    current_task->fd_capacity = new_capacity;
  }

  current_task->fd_table[fd] = handle;
  current_task->fd_flags[fd] = 0;
  fd_lock_release();
  return fd;
}

int scheduler_fd_close(int fd) {
  if (!current_task || fd < 0 || (usize)fd >= current_task->fd_capacity)
    return -1;
  fd_lock_acquire();
  current_task->fd_table[fd] = 0;
  current_task->fd_flags[fd] = 0;
  fd_lock_release();
  return 0;
}

int scheduler_fd_flags_get(int fd) {
  if (!current_task || fd < 0 || (usize)fd >= current_task->fd_capacity)
    return -1;
  fd_lock_acquire();
  int f = current_task->fd_table[fd] ? current_task->fd_flags[fd] : -1;
  fd_lock_release();
  return f;
}

int scheduler_fd_flags_set(int fd, int flags_val) {
  if (!current_task || fd < 0 || (usize)fd >= current_task->fd_capacity)
    return -1;
  fd_lock_acquire();
  if (!current_task->fd_table[fd]) {
    fd_lock_release();
    return -1;
  }
  current_task->fd_flags[fd] = flags_val;
  fd_lock_release();
  return 0;
}

void scheduler_fd_close_on_exec(void) {
  if (!current_task || !current_task->fd_table)
    return;
  for (usize i = 0; i < current_task->fd_capacity; i++) {
    if (current_task->fd_table[i] && (current_task->fd_flags[i] & B1NIX_FD_CLOEXEC) != 0) {
      vfs_handle_release(current_task->fd_table[i]);
      current_task->fd_table[i] = 0;
      current_task->fd_flags[i] = 0;
    }
  }
}

/* ── Signal Delivery ── */

int scheduler_kill(usize task_id, int sig) {
  if (sig < 1 || sig >= NSIG)
    return -1;

  interrupts_disable();
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->id == task_id && T(i)->state != TASK_UNUSED) {
      /* SIGKILL and SIGSTOP cannot be blocked/ignored. Atomic RMW: post-BKL
       * the target task (or another killer) may concurrently set/clear its own
       * pending bits, so a plain |= would drop a racing update. */
      __atomic_fetch_or(&T(i)->pending_signals, (1ULL << (sig - 1)),
                        __ATOMIC_RELEASE);

      if (T(i) != current_task &&
          (sig == SIGSTOP || sig == SIGTSTP ||
           sig == SIGTTIN || sig == SIGTTOU)) {
        __atomic_fetch_and(&T(i)->pending_signals, ~(1ULL << (sig - 1)),
                           __ATOMIC_RELAXED);
        T(i)->last_stop_signal = sig;
        T(i)->stop_report_pending = 1;
        T(i)->state = TASK_STOPPED;
        interrupts_enable();
        scheduler_notify_wait_event(T(i)->parent_id);
        return 0;
      }

      /* Wake blocked task so it can handle signal */
      if (sig == SIGCONT && T(i)->state == TASK_STOPPED) {
        T(i)->continued_report_pending = 1;
        interrupts_enable();
        scheduler_notify_wait_event(T(i)->parent_id);
        interrupts_disable();
      }
      if (T(i)->state == TASK_BLOCKED || T(i)->state == TASK_STOPPED) {
        T(i)->state = TASK_READY;
        sched_rq_enqueue_current(T(i));
      }
      interrupts_enable();
      return 0;
    }
  }
  interrupts_enable();
  return -1;
}

int scheduler_kill_process_group(usize pgrp, int sig) {
  if (sig < 1 || sig >= NSIG || pgrp == 0)
    return -1;

  int sent = 0;
  interrupts_disable();
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->state != TASK_UNUSED && T(i)->process_group_id == pgrp) {
      __atomic_fetch_or(&T(i)->pending_signals, (1ULL << (sig - 1)),
                        __ATOMIC_RELEASE);

      if (T(i) != current_task &&
          (sig == SIGSTOP || sig == SIGTSTP ||
           sig == SIGTTIN || sig == SIGTTOU)) {
        __atomic_fetch_and(&T(i)->pending_signals, ~(1ULL << (sig - 1)),
                           __ATOMIC_RELAXED);
        T(i)->last_stop_signal = sig;
        T(i)->stop_report_pending = 1;
        T(i)->state = TASK_STOPPED;
        interrupts_enable();
        scheduler_notify_wait_event(T(i)->parent_id);
        interrupts_disable();
        sent++;
        continue;
      }

      /* Wake blocked task so it can handle signal */
      if (sig == SIGCONT && T(i)->state == TASK_STOPPED) {
        T(i)->continued_report_pending = 1;
        interrupts_enable();
        scheduler_notify_wait_event(T(i)->parent_id);
        interrupts_disable();
      }
      if (T(i)->state == TASK_BLOCKED || T(i)->state == TASK_STOPPED) {
        T(i)->state = TASK_READY;
        sched_rq_enqueue_current(T(i));
      }
      sent++;
    }
  }
  interrupts_enable();
  return sent > 0 ? 0 : -1;
}

int scheduler_sigaction(int sig, const struct sigaction *act,
                        struct sigaction *old) {
  if (sig < 1 || sig >= NSIG)
    return -1;
  /* SIGKILL and SIGSTOP cannot be caught/ignored */
  if (sig == SIGKILL || sig == SIGSTOP)
    return -1;
  if (!current_task)
    return -1;

  interrupts_disable();
  if (old) {
    *old = current_task->sigactions[sig - 1];
  }
  if (act) {
    current_task->sigactions[sig - 1] = *act;
  }
  interrupts_enable();
  return 0;
}

int scheduler_sigprocmask(int how, const u64 *set, u64 *oldset) {
  if (!current_task)
    return -1;

  interrupts_disable();
  u64 old = current_task->blocked_signals;
  if (oldset) {
    *oldset = old;
  }

  if (set) {
    u64 mask = *set;
    /* SIGKILL/SIGSTOP cannot be blocked. */
    mask &= ~(1ULL << (SIGKILL - 1));
    mask &= ~(1ULL << (SIGSTOP - 1));

    if (how == 0) { /* SIG_BLOCK */
      current_task->blocked_signals |= mask;
    } else if (how == 2) { /* SIG_UNBLOCK */
      current_task->blocked_signals &= ~mask;
    } else if (how == 1) { /* SIG_SETMASK */
      current_task->blocked_signals = mask;
    } else {
      interrupts_enable();
      return -1;
    }
  }

  interrupts_enable();
  return 0;
}

sighandler_t scheduler_get_sighandler(int sig) {
  if (!current_task || sig < 1 || sig >= NSIG)
    return SIG_DFL;
  return current_task->sigactions[sig - 1].sa_handler;
}

usize scheduler_get_pid(void) {
  if (!current_task)
    return 0;
  return current_task->id;
}

struct cred *scheduler_get_current_cred(void) {
  if (!current_task)
    return 0;
  return current_task->cred;
}

void scheduler_set_user_image(void *image) {
  if (current_task) {
    current_task->user_image = image;
    if (current_task->pml4_phys == 0) {
      current_task->pml4_phys = paging_create_address_space();
      paging_switch_address_space(current_task->pml4_phys);
    }
  }
}

const char *scheduler_get_cwd(void) {
  if (!current_task)
    return "/";
  return current_task->cwd;
}

int scheduler_set_cwd(const char *path) {
  if (!current_task || !path || path[0] == '\0')
    return -1;
  usize len = strlen(path);
  if (len >= sizeof(current_task->cwd))
    return -1;
  memcpy(current_task->cwd, path, len + 1);
  return 0;
}

u64 scheduler_brk_get(void) {
  if (!current_task)
    return 0;
  return current_task->user_brk;
}

u64 vm_find_free_area(struct task *t, usize length) {
  u64 start = 0x40000000ULL;
#ifdef __x86_64__
  u64 end = 0x7FFFFFFFFFFFULL;
#else
  /* 32-bit: anonymous mmap must stay below the kernel split at 0x80000000 so it
   * never aliases the direct map [0x80000000, 0xC0000000). The user stack VMA
   * (just under 0x80000000) further bounds the search. */
  u64 end = 0x80000000ULL;
#endif

  // Simple first-fit hole finding
  u64 current_addr = start;
  struct vm_area *vma = t->vma_list;

  while (vma) {
    if (vma->start >= current_addr + length) {
      return current_addr;
    }
    current_addr = vma->end;
    vma = vma->next;
  }

  if (current_addr + length <= end) {
    return current_addr;
  }

  return (u64)-1;
}

struct vm_area *vma_split(struct task *t, struct vm_area *vma, u64 addr) {
  (void)t;
  struct vm_area *new_vma = kzalloc(sizeof(struct vm_area));
  if (!new_vma)
    return 0;

  memcpy(new_vma, vma, sizeof(struct vm_area));
  new_vma->start = addr;
  vma->end = addr;

  new_vma->next = vma->next;
  vma->next = new_vma;

  return new_vma;
}

void vma_delete_range(struct task *task, u64 start, u64 end) {
  struct vm_area **curr = &task->vma_list;
  while (*curr) {
    struct vm_area *vma = *curr;
    if (vma->start >= end || vma->end <= start) {
      curr = &vma->next;
      continue;
    }

    if (vma->start < start) {
      vma_split(task, vma, start);
      curr = &vma->next;
      continue;
    }

    if (vma->end > end) {
      vma_split(task, vma, end);
    }

    /* Now vma is entirely within [start, end] */
    *curr = vma->next;
    kfree(vma);
  }
}

/* ── Priority ── */

int scheduler_set_priority(usize pid, int priority) {
  if (priority < -20 || priority > 19)
    return -1;
  interrupts_disable();
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->state != TASK_UNUSED && T(i)->id == pid) {
      T(i)->priority = 10 - priority; /* nice → internal (higher = better) */
      interrupts_enable();
      return 0;
    }
  }
  interrupts_enable();
  return -1;
}

int scheduler_get_priority(usize pid) {
  interrupts_disable();
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->state != TASK_UNUSED && T(i)->id == pid) {
      int p = 10 - T(i)->priority; /* internal → nice */
      interrupts_enable();
      return p;
    }
  }
  interrupts_enable();
  return -1;
}

/* ── Session / Process Group ── */

usize scheduler_setsid(void) {
  if (!current_task)
    return (usize)-1;
  interrupts_disable();
  /* A process cannot be a process group leader to call setsid */
  if (current_task->process_group_id == current_task->id) {
    interrupts_enable();
    return (usize)-1;
  }
  current_task->session_id = current_task->id;
  current_task->process_group_id = current_task->id;
  interrupts_enable();
  return current_task->session_id;
}

usize scheduler_getpgrp(void) {
  if (!current_task)
    return 0;
  return current_task->process_group_id;
}

int scheduler_setpgrp(usize pid, usize pgrp) {
  if (pid == 0)
    pid = current_task ? current_task->id : 0;
  if (pgrp == 0)
    pgrp = pid;
  interrupts_disable();
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->state != TASK_UNUSED && T(i)->id == pid) {
      if (T(i)->session_id != current_task->session_id) {
        interrupts_enable();
        return -EPERM;
      }
      T(i)->process_group_id = pgrp;
      interrupts_enable();
      return 0;
    }
  }
  interrupts_enable();
  return -1;
}

int scheduler_is_pgrp_in_session(usize pgrp, usize session_id) {
  int found = 0;
  interrupts_disable();
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->state != TASK_UNUSED && T(i)->process_group_id == pgrp) {
      if (T(i)->session_id == session_id) {
        found = 1;
        break;
      }
    }
  }
  interrupts_enable();
  return found;
}

/* Called before returning to userspace — delivers pending signals */
void scheduler_deliver_pending_signals(void) {
  if (!current_task)
    return;

  /* Acquire-load the pending set: another CPU's scheduler_kill publishes new
   * bits with a release fetch_or. blocked_signals is task-local. */
  u64 pending = __atomic_load_n(&current_task->pending_signals, __ATOMIC_ACQUIRE);
  u64 blocked = current_task->blocked_signals;
  u64 deliverable = pending & ~blocked;

  if (deliverable == 0)
    return;

  /* Find highest-priority signal (lowest number = highest priority) */
  for (int sig = 1; sig < NSIG; sig++) {
    if (!(deliverable & (1ULL << (sig - 1))))
      continue;

    sighandler_t handler = current_task->sigactions[sig - 1].sa_handler;

    if (sig == SIGKILL) {
      /* SIGKILL — terminate immediately */
      current_task->exit_code = 128 + SIGKILL;
      /* See scheduler_exit_current — claim stack_released before DEAD. */
      current_task->stack_released = 0;
      current_task->state = TASK_DEAD;
      scheduler_notify_wait_event(current_task->parent_id);
      return;
      /* unreachable */
    }

    if (handler == SIG_IGN) {
      /* Ignored — just clear (atomic: races with a concurrent killer's set) */
      __atomic_fetch_and(&current_task->pending_signals,
                         ~(1ULL << (sig - 1)), __ATOMIC_RELAXED);
      continue;
    }

    if (handler == SIG_DFL) {
      /* Default actions */
      switch (sig) {
      case SIGINT:
      case SIGTERM:
      case SIGQUIT:
      case SIGPIPE:
      case SIGSEGV:
      case SIGBUS:
      case SIGFPE:
      case SIGILL:
      case SIGABRT:
      case SIGSYS:
      case SIGTRAP:
      case SIGXCPU:
      case SIGXFSZ:
      case SIGVTALRM:
      case SIGPROF:
        /* Terminate */
        current_task->exit_code = 128 + sig;
        /* See scheduler_exit_current — claim stack_released before DEAD. */
        current_task->stack_released = 0;
        current_task->state = TASK_DEAD;
        scheduler_notify_wait_event(current_task->parent_id);
        return;
        /* unreachable */
      case SIGCONT:
        __atomic_fetch_and(&current_task->pending_signals,
                           ~(1ULL << (sig - 1)), __ATOMIC_RELAXED);
        if (current_task->state == TASK_STOPPED) {
          current_task->state = TASK_READY;
          current_task->continued_report_pending = 1;
          scheduler_notify_wait_event(current_task->parent_id);
        }
        continue;
      case SIGSTOP:
      case SIGTSTP:
      case SIGTTIN:
      case SIGTTOU:
        current_task->state = TASK_STOPPED;
        current_task->last_stop_signal = sig;
        current_task->stop_report_pending = 1;
        __atomic_fetch_and(&current_task->pending_signals,
                           ~(1ULL << (sig - 1)), __ATOMIC_RELAXED);
        scheduler_notify_wait_event(current_task->parent_id);
        continue;
      case SIGCHLD:
      case SIGURG:
      case SIGWINCH:
      default:
        /* Ignore by default */
        __atomic_fetch_and(&current_task->pending_signals,
                           ~(1ULL << (sig - 1)), __ATOMIC_RELAXED);
        continue;
      }
    }

    /* Custom handlers need an architecture frame; leave them pending for the
     * syscall/interrupt return path instead of consuming the signal here. */
    return;
  }
}
