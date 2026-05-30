#include <b1nix/bkl.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/ipi.h>
#include <b1nix/lapic.h>
#include <b1nix/mm.h>
#include <b1nix/panic.h>
#include <b1nix/posix.h>
#include <b1nix/runqueue.h>
#include <b1nix/sched.h>
#include <b1nix/uidgid.h>
#include <b1nix/user.h>
#include <b1nix/vfs.h>
#include <b1nix/arch_x86.h>
#include <b1nix/aio.h>
#include <string.h>

/* syscall_entry.S reads the per-CPU current task as %gs:0x10 — keep that in
 * sync with struct percpu's cur_task member. */
_Static_assert(__builtin_offsetof(struct percpu, cur_task) == 0x10,
               "cur_task must be at offset 0x10 (see syscall_entry.S)");

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
 * and the VFS/ext4 call chain they exceed 16 KB. Keep this at >= 32 KB. */
#define KERNEL_STACK_SIZE (32 * 1024)
#define TASK_ENV_MAX 16
#define TASK_ENV_VALUE_MAX 64

extern void arch_context_switch(struct cpu_context *old_context,
                                struct cpu_context *new_context);
extern char x86_syscall_stack_top[];

/* x86 FPU/SSE save/restore (kernel/arch/x86/fpu.S). The kernel is -mno-sse, so
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

  /* 1) Fast path: reuse a slot already in [0, g_task_hwm). */
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->state == TASK_UNUSED) {
      memset(T(i), 0, sizeof(struct task));
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
    /* F4 (M28 #7): atomic CAS SLEEPING -> READY so only one CPU wins when
     * two timer ticks (or a tick + an explicit wake) race for the same
     * sleeper. Without this, the loser also enqueues the task — same task
     * in the runqueue twice → corruption. Under M24b BKL the race couldn't
     * happen; T1+ depends on this fix. */
    if (T(i)->wake_tick <= scheduler_ticks) {
      enum task_state expected = TASK_SLEEPING;
      if (__atomic_compare_exchange_n(&T(i)->state, &expected, TASK_READY,
                                      0, __ATOMIC_ACQUIRE,
                                      __ATOMIC_RELAXED)) {
        T(i)->wake_tick = 0;
        sched_rq_enqueue_current(T(i));
        woken++;
      }
    }
  }
  /* M28 #6: if we promoted at least one task, kick the other CPUs out of
   * `sti; hlt` so they re-poll the global runqueue immediately instead of
   * waiting for their next LAPIC tick (up to 10 ms). The IPI is cheap and
   * the bound it removes matters most when this task's wake-up is the only
   * event in flight. */
  if (woken > 0) ipi_reschedule_all();
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

  /* The boot task now holds the Big Kernel Lock at depth 1. From here on the
   * BSP runs all kernel code holding the BKL; it is handed off (still depth 1)
   * across every context switch and only released when a task enters userspace
   * or the idle loop parks. Uncontended on a single CPU, so this is a no-op for
   * the non-SMP path — it just establishes the depth==1-at-context-switch
   * invariant before the first scheduler_yield. */
  bkl_lock();

  console_write("sched: initialized\n");
  console_write("scheduler offsets: fd_table=");
  console_write_dec((usize)&((struct task *)0)->fd_table);
  console_write(" fd_lock=");
  console_write_dec((usize)&((struct task *)0)->fd_lock);
  console_write(" pml4_phys=");
  console_write_dec((usize)&((struct task *)0)->pml4_phys);
  console_write(" vma_list=");
  console_write_dec((usize)&((struct task *)0)->vma_list);
  console_write("\n");
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
 * kernel_thread_trampoline; defined in kernel/arch/x86/lapic.c. */
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
  u64 initial_rsp = stack_top - 16;
  *(u64 *)(usize)initial_rsp = (u64)(usize)trampoline;
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
#else
  task->context.rsp = initial_rsp;
  task->context.rbp = 0;
  task->context.rbx = 0;
  task->context.r12 = 0;
  task->context.r13 = 0;
  task->context.r14 = 0;
  task->context.r15 = 0;
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

  extern void paging_swap_in_all_swapped(u64 pml4_phys);
  paging_swap_in_all_swapped(parent->pml4_phys);

  void *child_stack = kmalloc(KERNEL_STACK_SIZE);
  if (!child_stack) {
    return -1;
  }

  interrupts_disable();
  struct task *child = find_unused_task();
  interrupts_enable();
  if (!child) {
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
    return -1;
  }

  if (child->user_image) {
    ((struct user_loaded_image *)child->user_image)->refcount++;
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

  int is_user = (parent->user_image != NULL && ((struct user_loaded_image *)parent->user_image)->kind == USER_IMAGE_ELF64);

  if (is_user) {
    struct interrupt_frame *child_iframe = (struct interrupt_frame *)(usize)(child->kernel_stack_ptr - sizeof(struct interrupt_frame));
    child_iframe->rax = 0;

    console_write("FORK-USER-DEBUG: child_iframe->rip = 0x");
    console_write_hex64(child_iframe->rip);
    console_write(" child_iframe->rsp = 0x");
    console_write_hex64(child_iframe->rsp);
    console_write("\n");

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

  // 3. Clone address space with interrupts disabled
  child->pml4_phys = paging_clone_address_space(parent->pml4_phys);

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
  child->state = TASK_READY;
  sched_rq_enqueue_current(child);
  interrupts_enable();
  console_write("scheduler_fork_current: done, child=");
  console_write_dec(child_id);
  console_write("\n");
  return child_id;
}



int scheduler_yield(void) {
  interrupts_disable();
  wake_sleepers();

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

    interrupts_enable();
    return 0; /* nothing runnable — caller's idle loop may drop the BKL */
  }

  if (old_task->state == TASK_RUNNING) {
    old_task->state = TASK_READY;
  }

  new_task->state = TASK_RUNNING;
  current_task = new_task;

  paging_switch_address_space(new_task->pml4_phys);
  arch_set_kernel_stack(new_task->kernel_stack_ptr);

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

  arch_context_switch(&old_task->context, &new_task->context);
  interrupts_enable();
  return 1; /* we switched out and have since been resumed (BKL held, depth 1) */
}

void scheduler_block_current(void) {
  interrupts_disable();

  if (current_task == 0 || current_task->state != TASK_RUNNING) {
    panic("scheduler_block_current without running task");
  }

  current_task->state = TASK_BLOCKED;
  scheduler_yield();
  interrupts_enable();
}

void scheduler_wake_task(usize task_id) {
  interrupts_disable();

  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->id == task_id && T(i)->state == TASK_BLOCKED) {
      T(i)->state = TASK_READY;
      T(i)->wait_chan = 0;
      sched_rq_enqueue_current(T(i));
      break;
    }
  }

  interrupts_enable();
}

void scheduler_block_on(void *chan) {
  interrupts_disable();

  if (current_task == 0 || current_task->state != TASK_RUNNING) {
    panic("scheduler_block_on without running task");
  }

  current_task->wait_chan = chan;
  current_task->state = TASK_BLOCKED;
  scheduler_yield();
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

  if (current_task == 0 || current_task->state != TASK_RUNNING) {
    panic("scheduler_sleep_ticks without running task");
  }

  current_task->wake_tick = scheduler_ticks + ticks;
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

  /* Cooperative scheduling: do NOT yield from the timer interrupt.
   * Several VFS walkers (find_child, vfs_get_mount_for_node, add_node)
   * traverse vfs_node parent/sibling chains without holding the parent's
   * inode lock. Preemptive yields here interleave those walks with concurrent
   * unlink/rmdir paths and let the walker dereference a freed sibling.
   * Tasks still yield voluntarily on syscall blocks (scheduler_block_on,
   * sleep, I/O), which is enough for smoke-test progress without exposing
   * the unsynchronized chain walks. Re-enable once VFS locking is audited. */
}

u64 scheduler_get_uptime_ticks(void) { return scheduler_ticks; }

void scheduler_exit_current(int exit_code) {
  if (current_task == 0) {
    panic("scheduler_exit_current without current task");
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
  current_task->state = TASK_DEAD;

  // Wake up parent if it is blocked waiting for us
  int woke_parent = 0;
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->id == current_task->parent_id) {
      /* F6 (M28 #7): atomic CAS BLOCKED -> READY so concurrent waitpid
       * paths on multiple CPUs can't double-promote and double-enqueue
       * the parent. The wake here doesn't enqueue at all (pick_next_task
       * scan handles the unenqueued READY tasks separately) so the CAS
       * just claims the transition; whoever loses the CAS observed a
       * parent that was already woken by someone else. */
      enum task_state expected = TASK_BLOCKED;
      if (__atomic_compare_exchange_n(&T(i)->state, &expected,
                                      TASK_READY, 0,
                                      __ATOMIC_RELEASE,
                                      __ATOMIC_RELAXED)) {
        woke_parent = 1;
      }
      break;  /* flat parent model: only one task matches parent_id */
    }
  }

  /* F6 (M28 #7): kick the BSP (or whichever CPU runs the parent kthread)
   * out of sti;hlt so it picks the parent immediately instead of waiting
   * for the next 10 ms LAPIC tick. Particularly important for the
   * test-driver init kthread on the BSP, which sits in sti;hlt during
   * userspace test runs. */
  if (woke_parent) ipi_reschedule_all();

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

  while (1) {
    interrupts_disable();
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
            int code = T(i)->exit_code;
            int child_id = T(i)->id;
            if (T(i)->user_image) {
              user_image_free(T(i)->user_image);
              T(i)->user_image = 0;
            }
            user_address_space_cleanup(T(i));
            paging_free_address_space(T(i)->pml4_phys);
            T(i)->pml4_phys = 0;
            if (T(i)->name && strcmp(T(i)->name, "boot") != 0) {
              kfree((void *)T(i)->name);
              T(i)->name = 0;
            }
            kfree(T(i)->stack);
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
              interrupts_enable();
              return child_id;
            }
            if ((options & B1NIX_WCONTINUED) &&
                T(i)->continued_report_pending) {
              if (status)
                *status = 0xFFFF;
              T(i)->continued_report_pending = 0;
              interrupts_enable();
              return child_id;
            }
          }
        }
      }
    }

    if (!has_children) {
      interrupts_enable();
      return -ECHILD;
    }

    if (options & B1NIX_WNOHANG) {
      interrupts_enable();
      return 0;
    }

    current_task->state = TASK_BLOCKED;
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
      /* SIGKILL and SIGSTOP cannot be blocked/ignored */
      T(i)->pending_signals |= (1ULL << (sig - 1));

      /* Wake blocked task so it can handle signal */
      if (sig == SIGCONT && T(i)->state == TASK_STOPPED) {
        T(i)->continued_report_pending = 1;
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
      T(i)->pending_signals |= (1ULL << (sig - 1));

      /* Wake blocked task so it can handle signal */
      if (sig == SIGCONT && T(i)->state == TASK_STOPPED) {
        T(i)->continued_report_pending = 1;
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
    console_write("scheduler_set_user_image: task=");
    console_write(current_task->name);
    console_write(" pml4_phys before=0x");
    console_write_hex64(current_task->pml4_phys);
    if (current_task->pml4_phys == 0) {
      current_task->pml4_phys = paging_create_address_space();
      paging_switch_address_space(current_task->pml4_phys);
    }
    console_write(" after=0x");
    console_write_hex64(current_task->pml4_phys);
    console_write("\n");
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
  u64 end = 0x7FFFFFFFFFFFULL;

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

  u64 pending = current_task->pending_signals;
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
      current_task->state = TASK_DEAD;
      for (usize i = 0; i < g_task_hwm; i++) {
        if (T(i)->id == current_task->parent_id && T(i)->state == TASK_BLOCKED)
          T(i)->state = TASK_READY;
      }
      return;
      /* unreachable */
    }

    if (handler == SIG_IGN) {
      /* Ignored — just clear */
      current_task->pending_signals &= ~(1ULL << (sig - 1));
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
        current_task->state = TASK_DEAD;
        for (usize i = 0; i < g_task_hwm; i++) {
          if (T(i)->id == current_task->parent_id && T(i)->state == TASK_BLOCKED)
            T(i)->state = TASK_READY;
        }
        return;
        /* unreachable */
      case SIGCONT:
        current_task->pending_signals &= ~(1ULL << (sig - 1));
        if (current_task->state == TASK_STOPPED) {
          current_task->state = TASK_READY;
          current_task->continued_report_pending = 1;
        }
        continue;
      case SIGSTOP:
      case SIGTSTP:
      case SIGTTIN:
      case SIGTTOU:
        current_task->state = TASK_STOPPED;
        current_task->last_stop_signal = sig;
        current_task->stop_report_pending = 1;
        current_task->pending_signals &= ~(1ULL << (sig - 1));
        continue;
      case SIGCHLD:
      case SIGURG:
      case SIGWINCH:
      default:
        /* Ignore by default */
        current_task->pending_signals &= ~(1ULL << (sig - 1));
        continue;
      }
    }

    /* Custom handlers need an architecture frame; leave them pending for the
     * syscall/interrupt return path instead of consuming the signal here. */
    return;
  }
}
