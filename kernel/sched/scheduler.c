#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/ipi.h>
#include <b1nix/lapic.h>
#include <b1nix/mm.h>
#include <b1nix/panic.h>
#include <b1nix/posix.h>
#include <b1nix/runqueue.h>
#include <b1nix/ptrace.h>
#include <b1nix/rseq.h>
#include <b1nix/sysv_ipc.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <b1nix/uidgid.h>
#include <b1nix/user.h>
#include <b1nix/vfs.h>
#include <b1nix/serial_tty.h>

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
/* x86_syscall_entry hardcodes these two offsets: it stores the user RSP at
 * 112(%rax) and loads the kernel RSP from 104(%rax). Adding or reordering a
 * field above them silently makes every syscall save/restore the WRONG words
 * of struct task — the task then "returns" onto a bogus stack. Pin them. */
_Static_assert(__builtin_offsetof(struct task, kernel_stack_ptr) == 104,
               "kernel_stack_ptr must be at offset 104 (see syscall_entry.S)");
_Static_assert(__builtin_offsetof(struct task, saved_user_rsp) == 112,
               "saved_user_rsp must be at offset 112 (see syscall_entry.S)");
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

/* M80: per-task XSAVE area (x87+SSE+AVX). Allocated lazily, the first time a
 * task is about to run userspace code, and then CACHED ON THE SLOT: a slot is
 * recycled by the next task that lands in it, so keeping the buffer costs one
 * allocation per concurrently-live slot and removes any chance of freeing an
 * area a context switch is still using. Kernel threads never touch the FPU and
 * so never get one — they keep using the 512-byte FXSAVE area in struct task. */
static void *g_task_xsave[MAX_TASKS];
static void *g_task_xsave_raw[MAX_TASKS];

static usize task_index(const struct task *task);

/* Save/restore whichever representation this task uses. A task with an XSAVE
 * area uses it exclusively; one without keeps to FXSAVE. The two never mix for
 * a given task, because task_fpu_alloc seeds a fresh area from the task's
 * existing FXSAVE image. */
static void task_fpu_save(struct task *t) {
  void *area = g_task_xsave[task_index(t)];
  if (area)
    arch_xsave(area, arch_xsave_mask());
  else
    arch_fpu_save(t->fpu_state);
}

static void task_fpu_restore(struct task *t) {
  void *area = g_task_xsave[task_index(t)];
  if (area)
    arch_xrstor(area, arch_xsave_mask());
  else
    arch_fpu_restore(t->fpu_state);
}

void *task_xsave_area(const struct task *t) {
  if (!t || !arch_xsave_enabled())
    return 0;
  return g_task_xsave[task_index(t)];
}

int task_fpu_alloc(struct task *t) {
  if (!t || !arch_xsave_enabled())
    return 0;
  usize idx = task_index(t);
  if (g_task_xsave[idx])
    return 1;
  usize size = arch_xsave_area_size();
  void *raw = kzalloc(size + 64);
  if (!raw)
    return 0; /* stay on FXSAVE: correct, just without the AVX half */
  usize aligned = ((usize)raw + 63) & ~(usize)63;
  void *area = (void *)aligned;
  memset(area, 0, size);
  /* Seed the area from whatever FXSAVE image this task already carries (a
   * fork inherits one), and declare x87+SSE present so xrstor loads it. The
   * AVX half starts in its init state, which is what a fresh thread has. */
  memcpy(area, t->fpu_state, 512);
  u64 bv = arch_xsave_mask() & 0x3; /* x87+SSE seeded; AVX starts in init state */
  memcpy((u8 *)area + 512, &bv, sizeof(bv));
  u64 flags = interrupts_save();
  g_task_xsave_raw[idx] = raw;
  g_task_xsave[idx] = area;
  interrupts_restore(flags);
  return 1;
}

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

/* Non-preemptible depth, per CPU. Imported drivers ask for this around short
 * register polls; see lkpi_preempt_disable. Interrupts stay on, so the only
 * effect is that the timer tick does not yield here. */
static volatile u32 g_preempt_depth[MAX_CPUS];

void scheduler_preempt_disable(void) {
  u64 flags = interrupts_save();
  u32 cpu = (u32)percpu_read(cpu_id);

  if (cpu < MAX_CPUS)
    g_preempt_depth[cpu]++;
  interrupts_restore(flags);
}

void scheduler_preempt_enable(void) {
  u64 flags = interrupts_save();
  u32 cpu = (u32)percpu_read(cpu_id);

  if (cpu < MAX_CPUS && g_preempt_depth[cpu] > 0)
    g_preempt_depth[cpu]--;
  interrupts_restore(flags);
}
static u64  g_task_child_tid_clear[MAX_TASKS];
static u64  g_task_saved_sigmask[MAX_TASKS];
static int  g_task_has_saved_sigmask[MAX_TASKS];
/* sigaltstack side-table (per-task alternate signal stack). Kept here, NOT in
 * struct task, to preserve the M29 paging invariant. ss_size == 0 means no alt
 * stack is registered. */
static u64  g_task_altstack_sp[MAX_TASKS];
static u64  g_task_altstack_size[MAX_TASKS];

/* M74 RT signals (SIGRTMIN..SIGRTMAX). Per-task RT sigactions + a FIFO of queued
 * (signo, code, value) payloads, lazily allocated (most tasks never use RT
 * signals; a static 2D table over MAX_TASKS would waste megabytes of BSS and
 * struct task cannot grow — the M29 invariant). Freed when the task slot is
 * recycled and on execve (handlers reset to default). */
#define RT_QUEUE_MAX 64
struct rt_queued {
  int signo;
  int si_code;
  union sigval value;
};
struct rt_state {
  struct sigaction action[SIGRTMAX - SIGRTMIN + 1];
  struct rt_queued queue[RT_QUEUE_MAX];
  int qhead;  /* index of oldest entry */
  int qcount; /* live entries */
};
static struct rt_state *g_rt_state[MAX_TASKS];
/* SMP: with the BKL gone, two CPUs can sigqueue to the same task (or the timer
 * ISR can fire) concurrently. interrupts_save() only masks the local CPU, so it
 * does NOT serialise across cores. g_rt_lock guards every RT-queue / RT-sigaction
 * / rt_state (de)allocation; g_timer_lock guards the POSIX timer table. Both are
 * always taken with interrupts disabled (callers either run in the timer ISR or
 * disable interrupts first), so a plain spin_lock cannot self-deadlock against
 * the ISR on the same CPU. Lock order: g_timer_lock -> g_rt_lock (the tick),
 * then -> the heap lock (lazy rt_state alloc) — never the reverse. */
static spinlock_t g_rt_lock = SPINLOCK_INIT;
static spinlock_t g_timer_lock = SPINLOCK_INIT;

static void rt_state_free(usize i) {
  if (g_rt_state[i]) {
    kfree(g_rt_state[i]);
    g_rt_state[i] = 0;
  }
}
static struct rt_state *rt_state_get(usize i, int alloc) {
  if (!g_rt_state[i] && alloc)
    g_rt_state[i] = kzalloc(sizeof(struct rt_state));
  return g_rt_state[i];
}

/* M74 POSIX per-process interval timers (timer_create). A small global table —
 * timers are few system-wide — each owning a task. timer_t is the table index.
 * The timer tick scans armed timers and queues an RT signal (SI_TIMER) on
 * expiry, so this builds on the RT-signal machinery above. */
#define MAX_POSIX_TIMERS 64
struct posix_timer {
  int used;
  usize owner_id;       /* task id that created it */
  int signo;            /* SIGEV_SIGNAL target signal */
  union sigval value;   /* sigev_value, delivered as si_value */
  u64 interval_ticks;   /* 0 = one-shot */
  u64 next_ticks;       /* absolute fire tick; 0 = disarmed */
};
static struct posix_timer g_posix_timers[MAX_POSIX_TIMERS];
static void posix_timers_tick(void); /* fire expired timers (defined below) */

static u64  g_task_alarm_ticks[MAX_TASKS];
/* setitimer(ITIMER_REAL) repeat period; 0 = one-shot (plain alarm(2)). */
static u64  g_task_alarm_interval_ticks[MAX_TASKS];
/* Set once the task has successfully execve()d. POSIX setpgid: changing a
 * CHILD's process group after it exec'd is EACCES. Deliberately NOT copied
 * by fork — the fresh child has not exec'd yet. */
static int  g_task_execed[MAX_TASKS];
/* POSIX nice value (-20..19, 0 default) — see scheduler_set_priority for why
 * this is NOT task->priority. Inherited across fork. */
static int  g_task_nice[MAX_TASKS];
static struct rlimit g_task_rlimits[MAX_TASKS][16];
static usize g_task_tgid[MAX_TASKS];
/* Set once a task has committed to scheduler_exit_current() with a chosen exit
 * code. A sibling that processes its own SIGKILL re-posts SIGKILL to the whole
 * thread group (terminate_group_siblings); without this guard the already-exiting
 * leader would process that pending SIGKILL in scheduler_deliver_pending_signals
 * and overwrite its exit_group(0) code with SIGNALED|SIGKILL, so waitpid reports
 * a spurious signalled death (M29 stress-exit-code). */
static u8    g_task_exiting[MAX_TASKS];

/*
 * Depth of kernel critical sections a task is inside — sections it must be
 * allowed to finish even when a fatal signal arrives.
 *
 * A SIGKILL is acted on wherever the task happens to be, and the handler below
 * marks it dead and returns rather than unwinding. That is fine in most places
 * and catastrophic in a few: a task killed between virtio_blk_lock and
 * virtio_blk_unlock never releases the device, and every later request — from
 * every process — spins on a lock whose owner no longer exists. A browser that
 * routinely kills its own helpers hits this window often enough that the whole
 * system stops with the disk held by a dead thread.
 *
 * Linux avoids the problem by acting on fatal signals only at well-defined
 * points (the return to user mode). This is the same idea, expressed as a
 * counter: while it is non-zero the kill is left pending, and the section's own
 * exit path delivers it.
 */
static u8    g_task_kcrit[MAX_TASKS];

void scheduler_kcrit_enter(void) {
  if (!current_task)
    return;
  usize idx = task_index(current_task);
  if (idx < MAX_TASKS && g_task_kcrit[idx] < 255)
    g_task_kcrit[idx]++;
}

void scheduler_kcrit_leave(void) {
  if (!current_task)
    return;
  usize idx = task_index(current_task);
  if (idx < MAX_TASKS && g_task_kcrit[idx])
    g_task_kcrit[idx]--;
}

static int task_in_kcrit(struct task *t) {
  usize idx = task_index(t);
  return idx < MAX_TASKS && g_task_kcrit[idx] != 0;
}
static int   g_task_ctty_type[MAX_TASKS];
static int   g_task_ctty_index[MAX_TASKS];
/* M86: per-thread CPU accounting, in NANOSECONDS. The counters are advanced by
 * acct_flush() from the four boundaries where a CPU changes what it is running
 * or which mode it runs in (ring-3 entry/exit and context switch), so they are
 * exact TSC deltas rather than the 100 Hz sample the tick charger produced —
 * a thread that always blocks before the tick used to account as 0. Per-task
 * counters are only ever written by the CPU currently running that task, so
 * plain adds are SMP-safe. */
static u64   g_task_utime_ns[MAX_TASKS];
static u64   g_task_stime_ns[MAX_TASKS];
static u64   g_task_cutime_ns[MAX_TASKS];
static u64   g_task_cstime_ns[MAX_TASKS];
/* Scheduler tick at which the slot's current occupant was created — /proc's
 * starttime field, and the base for a task's wall-clock age. */
static u64   g_task_start_tick[MAX_TASKS];
/* Context-switch counts: voluntary (the task blocked/slept/exited of its own
 * accord) and involuntary (it was still runnable when preempted). getrusage's
 * ru_nvcsw/ru_nivcsw. */
static u64   g_task_nvcsw[MAX_TASKS];
static u64   g_task_nivcsw[MAX_TASKS];
/* CPU time of threads that have already exited, folded into their thread-group
 * leader's slot. Without it a process's own CPU time would SHRINK as its
 * threads finish — times(2), getrusage(RUSAGE_SELF) and
 * CLOCK_PROCESS_CPUTIME_ID all have to keep counting work that is done. */
static u64   g_task_gone_utime_ns[MAX_TASKS];
static u64   g_task_gone_stime_ns[MAX_TASKS];
/* M86: set while a thread-group leader that called exit(2) waits for its
 * remaining threads (scheduler_exit_thread). Such a leader is already dead as
 * far as userspace is concerned, so a group teardown must not SIGKILL it — it
 * is handed the group's exit code through g_task_parked_code instead, and
 * reports that as an ordinary exit, the way Linux reports the exit_group() of
 * whichever thread ran it. */
/* M86: peak resident set (ru_maxrss). The value itself is measured exactly, by
 * walking the task's page tables — the same walk /proc/<pid>/statm does — so it
 * cannot drift the way an incrementally maintained counter would (fork's COW
 * table copy, exec teardown and swap-out all move pages without passing through
 * a single choke point). What is maintained here is only the MAXIMUM of those
 * measurements. Resident memory falls only at munmap, brk-shrink, exec teardown
 * and swap-out, so sampling immediately before each of those, plus on every
 * read, sees every peak; the sample is rate-limited to one per scheduler tick
 * so a munmap-heavy process does not pay for a page-table walk per call. */
static u64   g_task_maxrss_pages[MAX_TASKS];
static u64   g_task_rss_sample_tick[MAX_TASKS];
static u8    g_task_parked_leader[MAX_TASKS];
static u8    g_task_parked_override[MAX_TASKS];
static int   g_task_parked_code[MAX_TASKS];
/* Set on a thread pulled into a group stop by a signal sent to its leader: it
 * parks without posting its own SIGCHLD/stop report (see the delivery path). */
static u8    g_task_stop_quiet[MAX_TASKS];
static u64   g_task_pass[MAX_TASKS];
static u64   g_min_pass = 0;
/* Last userspace RIP of each task, captured by the LAPIC timer tick when it
 * preempts a ring-3 task (see task_set_user_rip). Consumed by the silence
 * watchdog task dump to name the user function a wedged thread group spins
 * in. */
static u64   g_task_user_rip[MAX_TASKS];
/* Scratch: which address the dump above decided to show for the current
 * thread — the sampled one or the syscall entry — so the mapping lookup that
 * follows describes the address actually printed. */
static u64   g_task_user_rip_shown;

/* The full argument vector a task was exec'd with, NUL-separated, as
 * /proc/<pid>/cmdline is defined to present it.
 *
 * Side table for the usual reason (struct task cannot grow — see the M29
 * LAPIC-PT note). Until this existed, cmdline reported the executable path and
 * nothing else, so every process looked like it had been started with no
 * arguments at all: `ps` showed a column of identical paths, and a browser's
 * helper processes — which are the same binary as the browser and differ only
 * in their flags — were indistinguishable from it and from each other. */
static char *g_task_cmdline[MAX_TASKS];
static usize g_task_cmdline_len[MAX_TASKS];
/* M63: seccomp-bpf per-task state (side-tables — struct task cannot grow, see
 * the M29 LAPIC-PT note). g_task_seccomp holds the installed filter chain
 * (opaque to the scheduler; defined in seccomp.c); g_task_nnp is no_new_privs. */
static void *g_task_seccomp[MAX_TASKS];
static int   g_task_nnp[MAX_TASKS];
static inline struct task *T(usize i) {
  return &g_task_chunks[i >> 6][i & 63];
}
/* current_task is per-CPU now (a macro -> get_percpu()->cur_task, see sched.h). */
/* PID numbering follows the Unix model: the boot/idle task is PID 0 (Linux's
 * "swapper"), PID 1 is reserved for the userspace init process, and everything
 * else is numbered from 2 up. Reserving 1 matters because real init systems
 * (OpenRC's openrc-init, sysvinit, ...) do `if (getpid() != 1) return 1;` and
 * exit immediately otherwise. See scheduler_reserve_init_pid(). */
static usize next_task_id = 2;

static int g_task_vfork_pending[MAX_TASKS];
static usize g_task_vfork_id[MAX_TASKS];

static int g_reserve_init_pid = 0;

/* Claim the next task id. Caller must hold g_tasks_lock. */
static usize claim_task_id(void) {
  if (g_reserve_init_pid) {
    g_reserve_init_pid = 0;
    return 1;
  }
  return next_task_id++;
}

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

/* Arm the PID-1 reservation: the next task created gets id 1. kernel_main calls
 * this immediately before spawning the init binary, at a point where no other
 * task creation is in flight. */
void scheduler_reserve_init_pid(void) {
  u64 flags;
  tasks_lock(&flags);
  g_reserve_init_pid = 1;
  tasks_unlock(flags);
}

/* fd_lock helpers — wraps spin_lock/unlock on the fd table owner's fd_lock
 * with lockdep tracking. fd_lock is plain (no irq-save) because fd ops never
 * fire from ISR context.
 *
 * A CLONE_FILES thread shares the parent's fd TABLE, so it must take the
 * parent's fd LOCK too: the per-task lock copies the old code used serialized
 * nothing across siblings (two threads could claim the same free slot or torn-
 * update a shared entry). g_task_fdlock_owner points at the lock-owning task
 * (the thread-group leader); NULL means "use your own lock". Task slot memory
 * is never freed, so a stale owner pointer after the leader is reaped degrades
 * to harmless lock sharing with whatever reuses the slot, not a dangling ref. */
static usize task_index(const struct task *task);
static struct task *g_task_fdlock_owner[MAX_TASKS];
/* Where each task last cleared its kernel-stack lease (stack_released = 0).
 * A task found READY with the lease still cleared and running on no CPU is
 * unschedulable forever; this names the code path that stranded it. */
static const char *g_task_lease_site[MAX_TASKS];
static usize task_index(const struct task *task);
static inline void task_lease_clear(struct task *t, const char *site) {
  g_task_lease_site[task_index(t)] = site;
  t->stack_released = 0;
}
static inline struct task *fd_lock_task(void) {
  struct task *owner = g_task_fdlock_owner[task_index(current_task)];
  return owner ? owner : current_task;
}
/* Who holds each task's fd_lock, and from which function. A lockup on fd_lock
 * names only the SPINNER in the panic dump; without this the holder (possibly
 * a CLONE_FILES sibling or a task blocked mid-critical-section) is invisible. */
static usize g_task_fdlock_holder[MAX_TASKS];
static const char *g_task_fdlock_site[MAX_TASKS];
/* Observed failure this hardens against: a task span-locked on an fd_lock that
 * the dump attributed to ITSELF (holder id == spinner id, acquired in
 * scheduler_fd_get) — i.e. some earlier critical section released a different
 * lock than it took and left this one held. Two changes make that impossible:
 *
 *  - release unlocks the object recorded at acquire time, instead of
 *    re-resolving fd_lock_task() (which reads g_task_fdlock_owner[] and can
 *    therefore answer differently than it did at acquire);
 *  - the section runs with IRQs off, removing the window where a timer tick
 *    preempts or migrates a task mid-section and every other CPU spins on an
 *    fd_lock whose holder is not currently running.
 *
 * The sections are a handful of array accesses (plus one kzalloc on table
 * growth), so the IRQ-off cost is negligible. */
static struct task *g_task_fdlock_on[MAX_TASKS];
static u64 g_task_fdlock_flags[MAX_TASKS];
static inline void fd_lock_acquire_at(const char *site) {
  struct task *owner = fd_lock_task();
  u64 flags = interrupts_save();
  /* Self-deadlock guard: spinning on a lock this task already holds can only
   * end in the 30-second lockup detector, by which point the holder's identity
   * is guesswork. Fail immediately, naming both critical sections. */
  {
    usize self_idx = task_index(current_task);
    if (g_task_fdlock_on[self_idx]) {
      console_write("fd_lock: recursive acquire in ");
      console_write(site);
      console_write(", already held since ");
      console_write(g_task_fdlock_site[task_index(g_task_fdlock_on[self_idx])]
                        ? g_task_fdlock_site[task_index(g_task_fdlock_on[self_idx])]
                        : "?");
      console_write("\n");
      panic("fd_lock recursive acquire");
    }
  }
  spin_lock(&owner->fd_lock);
  LOCKDEP_ACQUIRE(LOCKDEP_LVL_FD);
  usize self = task_index(current_task);
  g_task_fdlock_on[self] = owner;
  g_task_fdlock_flags[self] = flags;
  usize oi = task_index(owner);
  g_task_fdlock_holder[oi] = current_task->id;
  g_task_fdlock_site[oi] = site;
}
#define fd_lock_acquire() fd_lock_acquire_at(__func__)
static inline void fd_lock_release(void) {
  usize self = task_index(current_task);
  struct task *owner = g_task_fdlock_on[self];
  u64 flags = g_task_fdlock_flags[self];
  if (!owner) {
    /* Never observed in a balanced path; keep the old behaviour rather than
     * dereferencing NULL if some future caller releases without acquiring. */
    owner = fd_lock_task();
    flags = interrupts_save();
  }
  usize oi = task_index(owner);
  g_task_fdlock_holder[oi] = 0;
  g_task_fdlock_site[oi] = 0;
  g_task_fdlock_on[self] = 0;
  LOCKDEP_RELEASE(LOCKDEP_LVL_FD);
  spin_unlock(&owner->fd_lock);
  interrupts_restore(flags);
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
      g_task_saved_sigmask[i] = 0;
      g_task_has_saved_sigmask[i] = 0;
      g_task_altstack_sp[i] = 0;
      g_task_altstack_size[i] = 0;
      rt_state_free(i); /* M74: drop any RT-signal state from the prior occupant */
      g_task_alarm_ticks[i] = 0;
      g_task_alarm_interval_ticks[i] = 0;
      g_task_execed[i] = 0;
      g_task_nice[i] = 0;
      g_task_fdlock_owner[i] = 0;
      g_task_exiting[i] = 0;
      g_task_tgid[i] = 0;
      if (g_task_cmdline[i]) {
        kfree(g_task_cmdline[i]);
        g_task_cmdline[i] = 0;
      }
      g_task_cmdline_len[i] = 0;
      g_task_ctty_type[i] = 0;
      g_task_ctty_index[i] = 0;
      g_task_utime_ns[i] = 0;
      g_task_stime_ns[i] = 0;
      g_task_cutime_ns[i] = 0;
      g_task_cstime_ns[i] = 0;
      g_task_start_tick[i] = scheduler_ticks;
      g_task_nvcsw[i] = 0;
      g_task_nivcsw[i] = 0;
      g_task_gone_utime_ns[i] = 0;
      g_task_gone_stime_ns[i] = 0;
      g_task_maxrss_pages[i] = 0;
      g_task_rss_sample_tick[i] = 0;
      g_task_parked_leader[i] = 0;
      g_task_parked_override[i] = 0;
      g_task_stop_quiet[i] = 0;
      g_task_pass[i] = g_min_pass;
      g_task_vfork_pending[i] = 0;
      for (int r = 0; r < 16; r++) {
        g_task_rlimits[i][r].rlim_cur = RLIM_INFINITY;
        g_task_rlimits[i][r].rlim_max = RLIM_INFINITY;
      }
      g_task_rlimits[i][RLIMIT_NOFILE].rlim_cur = 1024;
      g_task_rlimits[i][RLIMIT_NOFILE].rlim_max = 1024;
      /* M77: default core-dump soft cap of 1 MiB (the kernel's coredump path
       * further clamps to the global /proc/sys/kernel/coredump-max). */
      g_task_rlimits[i][RLIMIT_CORE].rlim_cur = 1024 * 1024;
      T(i)->state = TASK_BLOCKED;
      T(i)->id = claim_task_id();
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
  g_task_is_thread[i] = 0;
  g_task_tls_base[i] = 0;
  g_task_child_tid_clear[i] = 0;
  g_task_saved_sigmask[i] = 0;
  g_task_has_saved_sigmask[i] = 0;
  g_task_altstack_sp[i] = 0;
  g_task_altstack_size[i] = 0;
  rt_state_free(i); /* M74: drop any RT-signal state from the prior occupant */
  g_task_alarm_ticks[i] = 0;
      g_task_alarm_interval_ticks[i] = 0;
  g_task_utime_ns[i] = 0;
  g_task_stime_ns[i] = 0;
  g_task_cutime_ns[i] = 0;
  g_task_cstime_ns[i] = 0;
  g_task_start_tick[i] = scheduler_ticks;
  g_task_nvcsw[i] = 0;
  g_task_nivcsw[i] = 0;
  g_task_gone_utime_ns[i] = 0;
  g_task_gone_stime_ns[i] = 0;
  g_task_maxrss_pages[i] = 0;
  g_task_rss_sample_tick[i] = 0;
  g_task_parked_leader[i] = 0;
  g_task_parked_override[i] = 0;
  g_task_stop_quiet[i] = 0;
  for (int r = 0; r < 16; r++) {
    g_task_rlimits[i][r].rlim_cur = RLIM_INFINITY;
    g_task_rlimits[i][r].rlim_max = RLIM_INFINITY;
  }
  g_task_rlimits[i][RLIMIT_NOFILE].rlim_cur = 1024;
  g_task_rlimits[i][RLIMIT_NOFILE].rlim_max = 1024;
  /* M77: default core-dump soft cap of 1 MiB. */
  g_task_rlimits[i][RLIMIT_CORE].rlim_cur = 1024 * 1024;
  T(i)->state = TASK_BLOCKED;
  T(i)->id = claim_task_id();
  tasks_unlock(flags);
  return T(i);
}

/* Release a tasks[] slot back to UNUSED under g_tasks_lock (pairs with
 * find_unused_task). The store also publishes any prior writes (e.g. the
 * kfree of the task's resources) before the slot becomes claimable again. */
static void free_task_slot(struct task *t) {
  /* M63: drop the task's seccomp filter chain (unref; frees at zero) BEFORE
   * taking tasks_lock — filter_unref calls kfree and tasks_lock is a leaf lock
   * that must not nest the heap lock. Idempotent: clears the side-table slot. */
  {
    extern void seccomp_release(struct task * t);
    seccomp_release(t);
  }
  /* Same reason: the slot is about to be claimable by a new task that may
   * reuse this pid, and it must not inherit a parent-death signal. */
  scheduler_clear_pdeathsig(t->id);
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


/* Spins to wait for a task to publish its kernel-stack lease before the lease
 * is treated as STALE rather than in-flight. The genuine hand-off is a handful
 * of instructions inside arch_context_switch, so anything past this bound is
 * not a switch in progress.
 *
 * A stale lease is a real, reachable state: a waker CASes a task
 * BLOCKED -> READY inside its prepare/commit window, the task never reaches
 * arch_context_switch, and stack_released stays 0 while the task is no longer
 * current on any CPU. Both pick paths then refuse to claim it FOREVER — the
 * task is lost, and anything it holds (its fd_lock, in the case caught here)
 * wedges every other CPU spinning for that lock with interrupts off, i.e. a
 * silent whole-machine hang. A task that is not current anywhere is provably
 * not mid-save, so once the grace period expires its saved context is safe to
 * load: publish the lease ourselves and schedule it. */
#define SCHED_HANDOFF_GRACE_SPINS 5000000ULL

static void sched_handoff_recover(struct task *t, const char *where) {
  static int reported;
  if (!reported) {
    reported = 1;
    console_write("sched: stale kernel-stack lease (");
    console_write(where);
    console_write(") on pid ");
    console_write_dec(t ? (u64)t->id : 0);
    console_write(" name=");
    console_write(t && t->name ? t->name : "(none)");
    console_write(" — claiming it\n");
  }
  if (t)
    __atomic_store_n(&t->stack_released, 1, __ATOMIC_RELEASE);
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
      /* sched_setaffinity: this CPU may not be allowed to run the task. Drop
       * the rq entry; the scan below (on a permitted CPU) still finds it. */
      if (pcpu && !sched_task_allowed_on_cpu(t, pcpu->cpu_id))
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
      u64 handoff_spins = 0;
      while (t != current_task &&
             __atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == TASK_READY &&
             !__atomic_load_n(&t->stack_released, __ATOMIC_ACQUIRE) &&
             !task_running_somewhere(t)) {
        __asm__ volatile("pause");
        tlb_shootdown_poll();
        /* A hand-off that never completes is a deadlock, not contention: name
         * the task instead of spinning here with IRQs off forever (a silent,
         * markerless hang). */
        if (++handoff_spins > SCHED_HANDOFF_GRACE_SPINS) {
          sched_handoff_recover(t, "global-rq");
          break;
        }
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
        g_min_pass = g_task_pass[task_index(t)];
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
   * only ap_runnable userspace processes are eligible.
   *
   * Nice biases otherwise-equal tasks with stride scheduling. Kernel/internal
   * priorities remain the primary ordering so adding nice support does not
   * change the scheduler's existing priority contract. */
  usize start = task_index(current_task);
  int max_priority = -1;
  u64 min_pass = -1ULL;
  struct task *best_task = 0;

  for (usize offset = 1; offset <= g_task_hwm; offset++) {
    usize index = (start + offset) % g_task_hwm;

    if (T(index)->state != TASK_READY)
      continue;
    if (T(index)->stealable)
      continue;
    if (on_ap && !T(index)->ap_runnable)
      continue; /* APs run only userspace ELF processes */
    if (pcpu && !sched_task_allowed_on_cpu(T(index), pcpu->cpu_id))
      continue; /* pinned elsewhere by sched_setaffinity */

    int priority = T(index)->priority;
    u64 pass = g_task_pass[index];
    if (priority > max_priority ||
        (priority == max_priority && pass < min_pass)) {
      max_priority = priority;
      min_pass = pass;
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
    u64 handoff_spins = 0;
    while (best_task != current_task &&
           __atomic_load_n(&best_task->state, __ATOMIC_ACQUIRE) == TASK_READY &&
           !__atomic_load_n(&best_task->stack_released, __ATOMIC_ACQUIRE) &&
           !task_running_somewhere(best_task)) {
      __asm__ volatile("pause");
      tlb_shootdown_poll();
      if (++handoff_spins > SCHED_HANDOFF_GRACE_SPINS) {
        sched_handoff_recover(best_task, "scan");
        break;
      }
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
        g_min_pass = g_task_pass[task_index(best_task)];
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
  boot->id = 0; /* PID 0: the boot/idle task (Linux calls it swapper) */
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
  g_task_is_thread[0] = 0;
  g_task_tls_base[0] = 0;
  g_task_child_tid_clear[0] = 0;
  g_task_saved_sigmask[0] = 0;
  g_task_has_saved_sigmask[0] = 0;
  g_task_altstack_sp[0] = 0;
  g_task_altstack_size[0] = 0;
  g_task_alarm_ticks[0] = 0;
      g_task_alarm_interval_ticks[0] = 0;
  g_task_tgid[0] = boot->id;
  g_task_ctty_type[0] = 1; /* 1 = console */
  g_task_ctty_index[0] = 0;
  g_task_utime_ns[0] = 0;
  g_task_stime_ns[0] = 0;
  g_task_cutime_ns[0] = 0;
  g_task_cstime_ns[0] = 0;
  g_task_start_tick[0] = 0;
  g_task_nvcsw[0] = 0;
  g_task_nivcsw[0] = 0;
  g_task_gone_utime_ns[0] = 0;
  g_task_gone_stime_ns[0] = 0;
  g_task_maxrss_pages[0] = 0;
  g_task_rss_sample_tick[0] = 0;
  g_task_parked_leader[0] = 0;
  g_task_parked_override[0] = 0;
  g_task_stop_quiet[0] = 0;
  g_task_pass[0] = 0;
  for (int r = 0; r < 16; r++) {
    g_task_rlimits[0][r].rlim_cur = RLIM_INFINITY;
    g_task_rlimits[0][r].rlim_max = RLIM_INFINITY;
  }
  g_task_rlimits[0][RLIMIT_NOFILE].rlim_cur = 1024;
  g_task_rlimits[0][RLIMIT_NOFILE].rlim_max = 1024;
  /* M77: default core-dump soft cap of 1 MiB (boot task). */
  g_task_rlimits[0][RLIMIT_CORE].rlim_cur = 1024 * 1024;
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
  if (task->id == 0) {
    /* Boot task (PID 0) gets root credentials */
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
  /* M74: reset RT handlers + drop queued RT, under g_rt_lock so it can't race a
   * concurrent sigqueue from another CPU into the freed rt_state. */
  {
    u64 rtf;
    spin_lock_irqsave(&g_rt_lock, &rtf);
    rt_state_free(task_index(task));
    spin_unlock_irqrestore(&g_rt_lock, rtf);
  }

  g_task_tgid[task_index(task)] = task->id;
  g_task_exiting[task_index(task)] = 0;
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

static void scheduler_root_inherit(struct task *child, struct task *parent);
static void scheduler_root_clear(struct task *t);
/* Defined with the other sched_setaffinity state below; fork needs it here. */
static u64 g_task_affinity[MAX_TASKS];

int scheduler_fork_current(void) {
  struct task *parent = current_task;
  if (!parent) {
    return -1;
  }
  /* "User" forks resume the child by iret'ing through the interrupt_frame.
   * All user tasks are real ELF processes (ELF32/ELF64) in Ring 3. */
  struct user_loaded_image *parent_img = parent->user_image;
  int is_user = (parent_img != NULL);

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
  /* The memcpy copied the parent's fpu_state SAVE-AREA, which is only refreshed
   * on a context switch-out and may lag the parent's live FPU. Capture the
   * parent's current (live) FPU directly so the child inherits the real FP
   * environment — POSIX fork semantics, and it guarantees a masked control word
   * rather than a stale/zero one (see the exec-time flush in process.c). */
  arch_fpu_save(child->fpu_state);
  child->fpu_initialized = 1;
  /* The child inherits the parent's full FPU state, AVX included: allocate its
   * area (seeded from the FXSAVE image just taken) and refresh it from the live
   * registers, which are still the parent's. */
  if (task_fpu_alloc(child)) {
    void *area = g_task_xsave[task_index(child)];
    if (area)
      arch_xsave(area, arch_xsave_mask());
  }
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

  // Clear inherited pending signals and sleep/block states.
  // POSIX fork: the child starts with an EMPTY pending set but INHERITS the
  // parent's blocked-signal mask.
  /* rseq(2) areas are per-thread and are not inherited across fork. */
  rseq_fork_clear(child);
  /* A chroot IS inherited — the child stays inside the parent's root. */
  scheduler_root_inherit(child, parent);
  /* So is the CPU affinity mask (Linux inherits it across fork and exec). */
  g_task_affinity[task_index(child)] = g_task_affinity[task_index(parent)];
  child->pending_signals = 0;
  child->blocked_signals = parent->blocked_signals;
  child->last_stop_signal = 0;
  child->stop_report_pending = 0;
  child->continued_report_pending = 0;
  child->wake_tick = 0;
  child->wait_chan = 0;

  // Copy side-table metadata from parent to child
  usize p_idx = task_index(parent);
  usize c_idx = task_index(child);
  g_task_saved_sigmask[c_idx] = 0;
  g_task_has_saved_sigmask[c_idx] = 0;
  /* POSIX: the alternate signal stack is inherited across fork (the child's
   * address space is a copy, so the same user range is valid). */
  g_task_altstack_sp[c_idx] = g_task_altstack_sp[p_idx];
  g_task_altstack_size[c_idx] = g_task_altstack_size[p_idx];
  g_task_alarm_ticks[c_idx] = 0;
      g_task_alarm_interval_ticks[c_idx] = 0;
  g_task_nice[c_idx] = g_task_nice[p_idx]; /* POSIX: nice survives fork */
  g_task_tgid[c_idx] = child->id;          /* child is its own thread group leader */
  /* M63: the child inherits the parent's seccomp filter chain (shared,
   * refcounted) and no_new_privs — a fork can only ever be as restricted. */
  {
    extern void seccomp_inherit(struct task * parent, struct task * child);
    seccomp_inherit(parent, child);
  }
  g_task_ctty_type[c_idx] = g_task_ctty_type[p_idx];
  g_task_ctty_index[c_idx] = g_task_ctty_index[p_idx];
  for (int r = 0; r < 16; r++) {
    g_task_rlimits[c_idx][r] = g_task_rlimits[p_idx][r];
  }
  /* M92: inherit the parent's TLS (FS segment) base so the child can access
   * thread-local storage immediately after fork. Without this, the child's
   * FS base is 0 and any TLS access (e.g. musl's _Fork cleanup) crashes. */
  g_task_tls_base[c_idx] = g_task_tls_base[p_idx];

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
   * CR3 (parent == current task) flushes the stale entries. Reload explicitly:
   * switching to the address space already loaded no longer writes CR3. */
  if (parent == current_task) {
    extern void paging_reload_cr3(void);
    paging_reload_cr3();
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
        if (new_vma->node->inode && new_vma->node->inode->mmap_open_cb)
          new_vma->node->inode->mmap_open_cb(new_vma->node);
        if (new_vma->node->inode && new_vma->node->inode->mmap_range_open_cb)
          new_vma->node->inode->mmap_range_open_cb(
              new_vma->node, (u64)new_vma->offset,
              (usize)(new_vma->end - new_vma->start));
      }
      *dst_prev = new_vma;
      dst_prev = &new_vma->next;
    }
    src_vma = src_vma->next;
  }

  /* Account the shm segments the child inherited via the cloned VMM_SHARED
   * VMAs so shm_nattch counts both processes (otherwise IPC_RMID could free a
   * segment the child still maps) and the child's exit cleans up its slot. */
  {
    extern void shm_fork_inherit(usize parent_pid, usize child_pid);
    shm_fork_inherit(parent->id, child->id);
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
  /* M80: PTRACE_O_TRACEFORK — attach the child to the same tracer and stop it
   * before it runs, then report the event on the parent. */
  ptrace_event_child(parent, child, PTRACE_EVENT_FORK);
  /* M28 T4: publish stack_released=1 BEFORE state=READY so a concurrent
   * pick on an AP doesn't observe READY with stack_released=0 (which would
   * make it spin forever — fork sets up the child stack synchronously and
   * never calls arch_context_switch from this path). */
  child->stack_released = 1;
  child->state = TASK_READY;
  sched_rq_enqueue_current(child);
  interrupts_enable();
  /* Kick the other CPUs out of `sti; hlt` so an idle AP (or the BSP, once the
   * forking task blocks in waitpid) picks the freshly runnable child now rather
   * than after the next 10 ms LAPIC tick. Every other make-runnable enqueue
   * (scheduler_wake_all / wake_sleepers / notify_wait_event) already issues this
   * kick; fork was the lone exception, which left a window where a child could
   * sit in the runqueue while all CPUs idled. */
  ipi_reschedule_all();
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
/* M63: seccomp filter chain + no_new_privs accessors (side-table backed). */
void *task_seccomp_filter(const struct task *t) {
  if (!t) return 0;
  return g_task_seccomp[task_index(t)];
}
void task_set_seccomp_filter(struct task *t, void *f) {
  if (!t) return;
  g_task_seccomp[task_index(t)] = f;
}
int task_no_new_privs(const struct task *t) {
  if (!t) return 0;
  return g_task_nnp[task_index(t)];
}
void task_set_no_new_privs(struct task *t, int v) {
  if (!t) return;
  g_task_nnp[task_index(t)] = v;
}
u64 task_child_tid_clear(const struct task *t) {
  if (!t) return 0;
  return g_task_child_tid_clear[task_index(t)];
}
void task_set_child_tid_clear(struct task *t, u64 addr) {
  if (!t) return;
  g_task_child_tid_clear[task_index(t)] = addr;
}
u64 task_saved_sigmask(const struct task *t) {
  if (!t) return 0;
  return g_task_saved_sigmask[task_index(t)];
}
int task_has_saved_sigmask(const struct task *t) {
  if (!t) return 0;
  return g_task_has_saved_sigmask[task_index(t)];
}
void task_set_saved_sigmask(struct task *t, u64 mask, int has_saved) {
  if (!t) return;
  usize idx = task_index(t);
  g_task_saved_sigmask[idx] = mask;
  g_task_has_saved_sigmask[idx] = has_saved;
}
void task_clear_saved_sigmask(struct task *t) {
  if (!t) return;
  g_task_has_saved_sigmask[task_index(t)] = 0;
}

/* sigaltstack side-table accessors. ss_size == 0 means "no alt stack". When a
 * task is currently executing on its alt stack, sigaltstack() reports
 * SS_ONSTACK and changing it is forbidden — but b1nix delivers one signal frame
 * at a time and the alt stack is consulted only at delivery, so the
 * "executing on the stack" state is derived from the live user SP rather than
 * stored. */
void task_get_altstack(const struct task *t, kstack_t *out) {
  if (!out) return;
  if (!t || g_task_altstack_size[task_index(t)] == 0) {
    out->ss_sp = 0;
    out->ss_size = 0;
    out->ss_flags = SS_DISABLE;
    return;
  }
  usize i = task_index(t);
  out->ss_sp = g_task_altstack_sp[i];
  out->ss_size = g_task_altstack_size[i];
  out->ss_flags = 0;
}

int task_set_altstack(struct task *t, const kstack_t *ss) {
  if (!t || !ss) return -1;
  usize i = task_index(t);
  if (ss->ss_flags & SS_DISABLE) {
    g_task_altstack_sp[i] = 0;
    g_task_altstack_size[i] = 0;
    return 0;
  }
  if (ss->ss_size < MINSIGSTKSZ)
    return -1; /* caller maps this to ENOMEM */
  g_task_altstack_sp[i] = ss->ss_sp;
  g_task_altstack_size[i] = ss->ss_size;
  return 0;
}

u64 task_altstack_top(const struct task *t) {
  if (!t) return 0;
  usize i = task_index(t);
  if (g_task_altstack_size[i] == 0) return 0;
  return g_task_altstack_sp[i] + g_task_altstack_size[i];
}

int task_on_altstack(const struct task *t, u64 sp) {
  if (!t) return 0;
  usize i = task_index(t);
  if (g_task_altstack_size[i] == 0) return 0;
  return sp >= g_task_altstack_sp[i] &&
         sp < g_task_altstack_sp[i] + g_task_altstack_size[i];
}

u64 task_alarm_ticks(const struct task *t) {
  if (!t) return 0;
  return g_task_alarm_ticks[task_index(t)];
}
u64 task_alarm_interval_ticks(const struct task *t) {
  if (!t) return 0;
  return g_task_alarm_interval_ticks[task_index(t)];
}
void task_set_alarm_interval_ticks(struct task *t, u64 ticks) {
  if (!t) return;
  g_task_alarm_interval_ticks[task_index(t)] = ticks;
}
void task_set_alarm_ticks(struct task *t, u64 ticks) {
  if (!t) return;
  g_task_alarm_ticks[task_index(t)] = ticks;
}
usize task_tgid(const struct task *t) {
  if (!t) return 0;
  return g_task_tgid[task_index(t)];
}
/* Last userspace RIP captured at timer-tick preemption (side-table; struct
 * task cannot grow — see the M29 LAPIC-PT note). Lets the silence watchdog's
 * task dump name the exact user function a wedged thread group is spinning
 * in, and lets the kernel tell a CPU-bound but progressing thread group from
 * one stuck in the same user address forever. */
u64 task_user_rip(const struct task *t) {
  if (!t) return 0;
  return g_task_user_rip[task_index(t)];
}

/* Record the argument vector for /proc/<pid>/cmdline. Called from execve once
 * the new image is committed; replaces whatever the previous image left. */
void task_set_cmdline(struct task *t, const char *const *argv) {
  if (!t)
    return;

  usize idx = task_index(t);
  usize need = 0;
  int argc = 0;

  for (; argv && argv[argc]; argc++)
    need += strlen(argv[argc]) + 1;

  char *buf = need ? (char *)kmalloc(need) : 0;
  if (need && !buf)
    return; /* keep the old vector rather than losing it for a short alloc */

  usize off = 0;
  for (int i = 0; i < argc; i++) {
    usize n = strlen(argv[i]) + 1;
    memcpy(buf + off, argv[i], n);
    off += n;
  }

  char *old = g_task_cmdline[idx];
  g_task_cmdline[idx] = buf;
  g_task_cmdline_len[idx] = off;
  if (old)
    kfree(old);
}

/* The recorded vector and its length in bytes, or NULL. The bytes include the
 * terminating NUL of every argument, which is what a reader expects to split
 * on. */
const char *task_cmdline(const struct task *t, usize *len_out) {
  if (!t) {
    if (len_out) *len_out = 0;
    return 0;
  }
  usize idx = task_index(t);
  if (len_out) *len_out = g_task_cmdline_len[idx];
  return g_task_cmdline[idx];
}

/* Release a slot's vector when the task is reaped. */
void task_clear_cmdline(struct task *t) {
  if (!t)
    return;
  usize idx = task_index(t);
  char *old = g_task_cmdline[idx];
  g_task_cmdline[idx] = 0;
  g_task_cmdline_len[idx] = 0;
  if (old)
    kfree(old);
}
void task_set_user_rip(struct task *t, u64 rip) {
  if (!t) return;
  g_task_user_rip[task_index(t)] = rip;
}

/* Where a task was in userspace when it entered the kernel, and the frame
 * pointer it left behind.
 *
 * A thread parked in a syscall has no fault to report and no timer tick to be
 * caught by, so the only record of what it was doing is the frame it came in
 * on. Keeping the caller's rip and rbp here makes a blocked thread's user
 * stack walkable from the watchdog dump — which is the difference between
 * "everything is waiting" and knowing what it waits for. */
static u64 g_task_entry_rip[MAX_TASKS];
static u64 g_task_entry_rbp[MAX_TASKS];

void task_set_syscall_entry(struct task *t, u64 rip, u64 rbp) {
  if (!t) return;
  usize i = task_index(t);
  g_task_entry_rip[i] = rip;
  g_task_entry_rbp[i] = rbp;
}
u64 task_syscall_entry_rip(const struct task *t) {
  return t ? g_task_entry_rip[task_index(t)] : 0;
}
u64 task_syscall_entry_rbp(const struct task *t) {
  return t ? g_task_entry_rbp[task_index(t)] : 0;
}

/* Print " (module+offset)" for a userspace address, or nothing when it falls
 * outside every mapping. A bare address says little when everything is loaded
 * above 0x500000000000; the module name is the part that identifies a layer. */
static void sched_name_user_addr(struct task *t, u64 addr) {
  if (!t || !addr)
    return;
  for (struct vm_area *v = t->vma_list; v; v = v->next) {
    if (addr >= v->start && addr < v->end) {
      console_write(" (");
      console_write(v->node && v->node->name[0] ? v->node->name : "anon");
      console_write("+0x");
      console_write_hex64(addr - v->start);
      console_write(")");
      return;
    }
  }
}
/* The system call a task entered last, kept beside its user RIP and for the
 * same reason: a thread that is RUNNING with an unchanging RIP is either
 * looping in one call or making the same call over and over, and the dump
 * cannot tell those apart without the number. */
static u64 g_task_syscall[MAX_TASKS];
void task_note_syscall(u64 number) {
  if (current_task)
    g_task_syscall[task_index(current_task)] = number;
}
/* ── M86: per-thread CPU time ─────────────────────────────────────────────── */
/* Nanosecond accessors are the primitives; the tick accessors (clock_t at the
 * 100 Hz USER_HZ times(2)/procfs report in) are derived from them. */
u64 task_utime_ns(const struct task *t) {
  if (!t) return 0;
  return g_task_utime_ns[task_index(t)];
}
u64 task_stime_ns(const struct task *t) {
  if (!t) return 0;
  return g_task_stime_ns[task_index(t)];
}
u64 task_cutime_ns(const struct task *t) {
  if (!t) return 0;
  return g_task_cutime_ns[task_index(t)];
}
u64 task_cstime_ns(const struct task *t) {
  if (!t) return 0;
  return g_task_cstime_ns[task_index(t)];
}
/* Thread-group totals: POSIX times(2) and getrusage(RUSAGE_SELF) report the
 * whole process, so a thread's own counters are not the answer for them — only
 * getrusage(RUSAGE_THREAD) and CLOCK_THREAD_CPUTIME_ID use the per-task values.
 * The walk is over live slots and takes no lock: each counter is a u64 read of
 * a value only its own CPU writes, so a racing thread can make the sum a few
 * hundred nanoseconds stale, never torn. */
void task_group_cputime_ns(const struct task *t, u64 *utime_ns, u64 *stime_ns) {
  u64 u = 0, s = 0;
  if (t) {
    usize tgid = g_task_tgid[task_index(t)];
    if (tgid == 0) tgid = t->id;
    for (usize i = 0; i < g_task_hwm; i++) {
      if (T(i)->state == TASK_UNUSED)
        continue;
      if (g_task_tgid[i] != tgid && T(i)->id != tgid)
        continue;
      u += g_task_utime_ns[i];
      s += g_task_stime_ns[i];
      if (T(i)->id == tgid) {
        u += g_task_gone_utime_ns[i];
        s += g_task_gone_stime_ns[i];
      }
    }
  }
  if (utime_ns) *utime_ns = u;
  if (stime_ns) *stime_ns = s;
}
u64 task_utime(const struct task *t) {
  return task_utime_ns(t) / NS_PER_USER_TICK;
}
u64 task_stime(const struct task *t) {
  return task_stime_ns(t) / NS_PER_USER_TICK;
}
u64 task_cutime(const struct task *t) {
  return task_cutime_ns(t) / NS_PER_USER_TICK;
}
u64 task_cstime(const struct task *t) {
  return task_cstime_ns(t) / NS_PER_USER_TICK;
}
u64 task_start_ticks(const struct task *t) {
  if (!t) return 0;
  return g_task_start_tick[task_index(t)];
}
/* Measure this task's resident set and fold it into the peak. `force` skips the
 * per-tick rate limit — used on the paths that are about to drop pages, where
 * the sample IS the peak. Returns the peak in pages. */
/* How often an unforced sample may walk the address space, in ticks.
 *
 * The walk visits every page of every mapping and asks the tables about each
 * one. For a browser that is millions of lookups, and the unforced callers are
 * mmap/munmap/mremap — so a program allocating steadily paid that walk up to a
 * hundred times a second, to maintain a peak figure nobody reads until
 * getrusage asks. Once a second is plenty for a high-water mark; getrusage and
 * the procfs reader still force an exact one. */
#define RSS_SAMPLE_INTERVAL_TICKS 100

u64 task_rss_sample(struct task *t, int force) {
  if (!t)
    return 0;
  usize idx = task_index(t);
  u64 now = scheduler_ticks;
  if (!force && g_task_rss_sample_tick[idx] &&
      now - g_task_rss_sample_tick[idx] < RSS_SAMPLE_INTERVAL_TICKS)
    return g_task_maxrss_pages[idx];
  g_task_rss_sample_tick[idx] = now;
  u64 resident = 0;
  if (t->pml4_phys) {
    for (struct vm_area *v = t->vma_list; v; v = v->next)
      for (u64 va = v->start; va < v->end; va += PAGE_SIZE)
        if (paging_user_frame(t->pml4_phys, va))
          resident++;
  }
  if (resident > g_task_maxrss_pages[idx])
    g_task_maxrss_pages[idx] = resident;
  return g_task_maxrss_pages[idx];
}

u64 task_maxrss_pages(const struct task *t) {
  if (!t)
    return 0;
  return g_task_maxrss_pages[task_index(t)];
}

u64 task_nvcsw(const struct task *t) {
  if (!t) return 0;
  return g_task_nvcsw[task_index(t)];
}
u64 task_nivcsw(const struct task *t) {
  if (!t) return 0;
  return g_task_nivcsw[task_index(t)];
}
usize scheduler_max_tasks(void) { return MAX_TASKS; }

/* Post a signal WITHOUT the stop/continue side effects scheduler_kill applies.
 * ptrace uses this for SIGSTOP: forcing TASK_STOPPED on a task that is running
 * on another CPU parks it before its context has been saved, and the next
 * picker then resumes from a half-written frame. Leaving the signal pending
 * lets the tracee stop itself on its next return to ring 3, which is where its
 * registers are complete — the same thing Linux does. */
int scheduler_post_signal(usize pid, int sig) {
  if (sig <= 0 || sig > NSIG)
    return -EINVAL;
  u64 flags = interrupts_save();
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->id != pid || T(i)->state == TASK_UNUSED ||
        T(i)->state == TASK_DEAD || T(i)->state == TASK_REAPING)
      continue;
    __atomic_fetch_or(&T(i)->pending_signals, (1ULL << (sig - 1)),
                      __ATOMIC_RELEASE);
    /* A blocked task must run to notice it; a running one will see it on its
     * next return to userspace. */
    enum task_state expected = TASK_BLOCKED;
    if (__atomic_compare_exchange_n(&T(i)->state, &expected, TASK_READY, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
      sched_rq_enqueue_current(T(i));
    interrupts_restore(flags);
    return 0;
  }
  interrupts_restore(flags);
  return -ESRCH;
}

/* ── ioprio(2) ───────────────────────────────────────────────────────────────
 * Per-task I/O scheduling class + level. b1nix issues one device command at a
 * time, and the block layer's admission gate (kernel/dev/blk.c) hands a busy
 * device to the waiting request with the best priority — so this value really
 * does decide who goes first, with ageing so nothing starves. */
static u16 g_task_ioprio[MAX_TASKS];

int scheduler_set_ioprio(usize pid, int ioprio) {
  struct task *t = pid ? scheduler_task_by_pid(pid) : current_task;
  if (!t)
    return -ESRCH;
  g_task_ioprio[task_index(t)] = (u16)ioprio;
  return 0;
}

int scheduler_get_ioprio(usize pid) {
  struct task *t = pid ? scheduler_task_by_pid(pid) : current_task;
  if (!t)
    return -ESRCH;
  return (int)g_task_ioprio[task_index(t)];
}

/* ── sched_setaffinity(2) ────────────────────────────────────────────────────
 * A per-task CPU mask, honoured by every path that can put a task on a CPU:
 * the global-runqueue pick, the O(n) scan, and work stealing. 0 means "no
 * restriction", which is the default and costs a single compare. */
int sched_task_allowed_on_cpu(const struct task *t, int cpu) {
  if (!t || cpu < 0 || cpu > 63)
    return 1;
  u64 mask = g_task_affinity[task_index(t)];
  return mask == 0 || (mask & (1ULL << cpu)) != 0;
}

int scheduler_set_affinity(usize pid, u64 mask) {
  struct task *t = pid ? scheduler_task_by_pid(pid) : current_task;
  if (!t)
    return -ESRCH;
  extern int get_online_cpu_count(void);
  int online = get_online_cpu_count();
  if (online < 1)
    online = 1;
  u64 online_mask = (online >= 64) ? ~0ULL : ((1ULL << online) - 1);
  /* A mask that permits no online CPU would make the task unschedulable. */
  if ((mask & online_mask) == 0)
    return -EINVAL;
  g_task_affinity[task_index(t)] = mask & online_mask;
  return 0;
}

u64 scheduler_get_affinity(usize pid) {
  struct task *t = pid ? scheduler_task_by_pid(pid) : current_task;
  if (!t)
    return 0;
  u64 mask = g_task_affinity[task_index(t)];
  if (mask)
    return mask;
  extern int get_online_cpu_count(void);
  int online = get_online_cpu_count();
  if (online < 1)
    online = 1;
  return (online >= 64) ? ~0ULL : ((1ULL << online) - 1);
}

/* ── chroot(2) ───────────────────────────────────────────────────────────────
 * A task's filesystem root. NULL means the real root, so the common case costs
 * nothing. The node is ref-held for as long as a task names it (inherited by
 * fork, released at exit), which is what keeps the directory alive under the
 * chrooted task even if it is unlinked from the parent tree. Path resolution
 * starts here and clamps ".." at it — see vfs_find_node_internal. */
static struct vfs_node *g_task_root_node[MAX_TASKS];
static char g_task_root_path[MAX_TASKS][VFS_MAX_PATH];

struct vfs_node *scheduler_get_root_node(void) {
  if (!current_task)
    return 0;
  return g_task_root_node[task_index(current_task)];
}

const char *scheduler_get_root_path(void) {
  if (!current_task)
    return "/";
  const char *p = g_task_root_path[task_index(current_task)];
  return p[0] ? p : "/";
}

int scheduler_set_root(struct vfs_node *node, const char *path) {
  if (!current_task)
    return -EINVAL;
  usize idx = task_index(current_task);
  struct vfs_node *old = g_task_root_node[idx];
  g_task_root_node[idx] = node; /* caller passes an already-referenced node */
  if (path) {
    strncpy(g_task_root_path[idx], path, VFS_MAX_PATH - 1);
    g_task_root_path[idx][VFS_MAX_PATH - 1] = '\0';
  } else {
    g_task_root_path[idx][0] = '\0';
  }
  if (old)
    vfs_node_put(old);
  return 0;
}

static void scheduler_root_clear(struct task *t) {
  if (!t)
    return;
  usize idx = task_index(t);
  if (g_task_root_node[idx]) {
    vfs_node_put(g_task_root_node[idx]);
    g_task_root_node[idx] = 0;
  }
  g_task_root_path[idx][0] = '\0';
}

static void scheduler_root_inherit(struct task *child, struct task *parent) {
  if (!child || !parent)
    return;
  usize ci = task_index(child), pi = task_index(parent);
  g_task_root_node[ci] = g_task_root_node[pi];
  if (g_task_root_node[ci])
    vfs_node_get(g_task_root_node[ci]);
  memcpy(g_task_root_path[ci], g_task_root_path[pi], VFS_MAX_PATH);
}

/* swapoff(2): page every swapped-out page of every live address space back
 * into RAM, so no slot on the device is still referenced by a page table.
 * Address spaces are shared by threads, so each distinct pml4 is walked once.
 * Returns the number of address spaces processed. */
usize scheduler_swapin_all_tasks(void) {
  usize done = 0;
  for (usize i = 0; i < g_task_hwm; i++) {
    struct task *t = T(i);
    if (t->state == TASK_UNUSED || t->state == TASK_REAPING || !t->pml4_phys)
      continue;
    int seen = 0;
    for (usize j = 0; j < i; j++) {
      struct task *p = T(j);
      if (p->state != TASK_UNUSED && p->pml4_phys == t->pml4_phys) {
        seen = 1;
        break;
      }
    }
    if (seen)
      continue;
    paging_swap_in_all_swapped(t->pml4_phys);
    done++;
  }
  return done;
}

/* Per-task variant of scheduler_getrlimit, for /proc/<pid>/limits. */
int scheduler_getrlimit_task(const struct task *t, int resource,
                             struct rlimit *rlim) {
  if (resource < 0 || resource >= 16 || !rlim || !t)
    return -EINVAL;
  *rlim = g_task_rlimits[task_index(t)][resource];
  return 0;
}

int scheduler_getrlimit(int resource, struct rlimit *rlim) {
  if (resource < 0 || resource >= 16 || !rlim || !current_task)
    return -EINVAL;
  usize idx = task_index(current_task);
  *rlim = g_task_rlimits[idx][resource];
  return 0;
}
int scheduler_setrlimit(int resource, const struct rlimit *rlim) {
  if (resource < 0 || resource >= 16 || !rlim || !current_task)
    return -EINVAL;
  if (rlim->rlim_cur > rlim->rlim_max)
    return -EINVAL;
  const struct cred *cred = scheduler_get_current_cred();
  usize idx = task_index(current_task);
  if (cred && cred->euid != ROOT_UID) {
    if (rlim->rlim_max > g_task_rlimits[idx][resource].rlim_max) {
      return -EPERM;
    }
  }
  g_task_rlimits[idx][resource] = *rlim;
  if (current_task->pml4_phys != 0) {
    for (usize i = 0; i < g_task_hwm; i++) {
      struct task *t = T(i);
      if (t->state == TASK_UNUSED || t->state == TASK_REAPING)
        continue;
      if (t->pml4_phys == current_task->pml4_phys)
        g_task_rlimits[i][resource] = *rlim;
    }
  }
  return 0;
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
  u64 start_func;  /* musl pthread: start_routine pointer for r9 */
};

extern void x86_user_jump(usize entry, usize stack, usize argc, usize argv);

static void clone_thread_kentry(void *arg) {
  struct clone_thread_args *cta = (struct clone_thread_args *)arg;
  u64 entry = cta->user_entry;
  u64 stack = cta->user_stack;
  u64 user_arg = cta->user_arg;
  u64 start_func = cta->start_func;
  kfree(cta);
  if (start_func) {
    /* M92 musl pthread: child continues at parent's RIP (after syscall in
     * musl __clone), with the new user stack and r9 = start_routine. The
     * x86_clone_thread_jump sets rax=0 (clone return value) and r9 =
     * start_func, then iretqs to ring 3 at the parent's RIP. musl's child
     * code then pops the arg from the stack and calls *r9. */
    extern void x86_clone_thread_jump(u64 entry, u64 stack, u64 start_func);
    x86_clone_thread_jump(entry, stack, start_func);
  }
#ifdef __x86_64__
  /* SysV AMD64: a function entered via `call` sees %rsp ≡ 8 (mod 16) — the
   * 8-byte return address sits just below a 16-byte-aligned call site, and the
   * callee's prologue realigns to 16. The kernel transfers control directly
   * (no call), so we must simulate that frame: 16-align the stack, then reserve
   * a dummy return-address slot. Without the `-8`, the thread entry runs with
   * %rsp ≡ 0 (mod 16), and the first `movaps %xmm,off(%rsp)` with a 16-aligned
   * `off` #GPs — e.g. Rust std's stack_overflow::make_handler zeroes a stack
   * struct that way (the M67 std::thread crash). The i386 branch below already
   * builds this frame; x86_64 must too. x86_user_jump drops its 3rd positional
   * (user_arg) into %rdi = start_routine's void* argument. */
  u64 sp = ((u64)stack & ~0xFUL) - 8;
  *(volatile u64 *)(usize)sp = 0; /* return address: threads exit via SYS_EXIT_THREAD, never ret */
  sched_acct_leave_kernel(); /* M86: kernel-time interval ends at the ring-3 jump */
  x86_user_jump((usize)entry, (usize)sp, (usize)user_arg, 0);
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

/* Serializes the "last user of this mm" decision across CPUs. Leaf lock:
 * nothing else is acquired while it is held, and the critical section is a
 * bounded table scan. */
static spinlock_t g_mm_release_lock = SPINLOCK_INIT;

/* Atomically drop t's reference to its shared address space. Clears
 * t->pml4_phys under g_mm_release_lock and counts the remaining holders;
 * returns the pml4 root iff t was the LAST user (the caller must then tear
 * the mm down), 0 otherwise. The lock is what makes the decision sound:
 * with an unlocked refs scan, two reapers on different CPUs could each
 * claim a different "last" sibling (each excluding itself and skipping the
 * other's REAPING state), both observe zero refs, and both free the same
 * page tables — a pmm double-free that resurfaced as poison in whatever
 * process reused the frames (the M29 no-join stress hang under -smp 4).
 * Serializing drop-then-count means exactly one caller — the one whose
 * drop empties the holder set — wins the teardown. REAPING holders still
 * count as refs here: their own mm_release_user call hasn't dropped them
 * yet, and it will see the updated holder set when it runs. */
static u64 mm_release_user(struct task *t) {
  u64 flags;
  spin_lock_irqsave(&g_mm_release_lock, &flags);
  u64 p = t->pml4_phys;
  t->pml4_phys = 0;
  int refs = 0;
  if (p) {
    for (usize i = 0; i < g_task_hwm; i++) {
      struct task *o = T(i);
      if (o == t) continue;
      if (o->state == TASK_UNUSED) continue;
      if (o->pml4_phys == p) refs++;
    }
  }
  spin_unlock_irqrestore(&g_mm_release_lock, flags);
  return (p && refs == 0) ? p : 0;
}

/* Atomically drop t's reference to its (possibly CLONE_FILES-shared) fd
 * table. Mirrors mm_release_user: clear our own pointer under
 * g_mm_release_lock, then count the remaining holders — exactly one releaser
 * observes zero and gets the table back to close + free. Any non-UNUSED
 * holder counts (DEAD/REAPING included): its own release call hasn't dropped
 * it yet and will see the updated holder set when it runs. Returns the table
 * iff t was the last user (caller must vfs_close_handle each entry and kfree
 * both arrays), 0 otherwise. */
static struct vfs_handle **fdtable_release(struct task *t, int **flags_out,
                                           usize *cap_out) {
  u64 flags;
  spin_lock_irqsave(&g_mm_release_lock, &flags);
  struct vfs_handle **tbl = t->fd_table;
  int *fl = t->fd_flags;
  usize cap = t->fd_capacity;
  t->fd_table = 0;
  t->fd_flags = 0;
  t->fd_capacity = 0;
  int holders = 0;
  if (tbl) {
    for (usize i = 0; i < g_task_hwm; i++) {
      struct task *o = T(i);
      if (o == t) continue;
      if (o->state == TASK_UNUSED) continue;
      if (o->fd_table == tbl) holders++;
    }
  }
  spin_unlock_irqrestore(&g_mm_release_lock, flags);
  if (!tbl || holders > 0)
    return 0;
  *flags_out = fl;
  *cap_out = cap;
  return tbl;
}

int g_has_any_thread = 0;

/* CLONE_VFORK: the parent must not run until the child has execve()d or
 * exited. musl's posix_spawn (and therefore popen/system) relies on this — its
 * child runs on a buffer that lives on the PARENT's stack, so a parent that
 * returns early reuses that memory underneath the running child. Indexed by the
 * child's task slot; the id is stored alongside so a recycled slot cannot
 * satisfy a stale wait. */
/* Called by the child at the two points that end a vfork: a successful execve
 * (it has its own address space and stack now) and task exit. */
void scheduler_vfork_release(void) {
  struct task *t = current_task;
  if (!t)
    return;
  usize idx = task_index(t);
  if (!g_task_vfork_pending[idx] || g_task_vfork_id[idx] != t->id)
    return;
  g_task_vfork_pending[idx] = 0;
  scheduler_wake_all(&g_task_vfork_pending[idx]);
}

int scheduler_clone_thread(u64 flags, u64 entry, u64 user_stack, u64 arg,
                           u64 tls, u64 ctid,
                           u64 parent_tid_addr, u64 child_tid_addr,
                           u64 start_func) {
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
  cta->start_func = start_func;

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

  usize p_idx = task_index(parent);
  usize c_idx = task_index(child);
  if (flags & B1NIX_CLONE_THREAD) {
    g_task_tgid[c_idx] = g_task_tgid[p_idx];
  } else {
    g_task_tgid[c_idx] = child->id;
  }
  g_task_ctty_type[c_idx] = g_task_ctty_type[p_idx];
  g_task_ctty_index[c_idx] = g_task_ctty_index[p_idx];
  for (int r = 0; r < 16; r++)
    g_task_rlimits[c_idx][r] = g_task_rlimits[p_idx][r];

  /* Address-space inheritance. */
  if ((flags & B1NIX_CLONE_VM) && !(flags & B1NIX_CLONE_THREAD)) {
    /* vfork / posix_spawn: a process of its own that Linux would run in the
     * parent's mm until execve. b1nix gives it a COW copy instead. musl's spawn
     * child only reads the shared data and reports failures through a pipe, so
     * it does not need writes to be visible to the parent — and a private mm
     * removes the whole class of hazards where the child's COW fault or its
     * execve teardown rewrites page-table entries the parent is still using.
     * CLONE_VFORK still suspends the parent until exec/exit, so the ordering
     * userspace relies on is preserved. */
    extern u64 paging_clone_address_space(u64 pml4_phys);
    extern void paging_reload_cr3(void);
    child->pml4_phys = paging_clone_address_space(parent->pml4_phys);
    /* The clone flipped the parent's writable user pages to COW in place; the
     * parent keeps running on the same CR3, so reload it to drop the stale
     * writable TLB entries (see the identical note in scheduler_fork_current). */
    if (parent == current_task)
      paging_reload_cr3();

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
          if (new_vma->node->inode && new_vma->node->inode->mmap_open_cb)
            new_vma->node->inode->mmap_open_cb(new_vma->node);
          if (new_vma->node->inode && new_vma->node->inode->mmap_range_open_cb)
            new_vma->node->inode->mmap_range_open_cb(
                new_vma->node, (u64)new_vma->offset,
                (usize)(new_vma->end - new_vma->start));
        }
        *dst_prev = new_vma;
        dst_prev = &new_vma->next;
      }
      src_vma = src_vma->next;
    }
    {
      extern void shm_fork_inherit(usize parent_pid, usize child_pid);
      shm_fork_inherit(parent->id, child->id);
    }
    child->user_brk = parent->user_brk;
    child->heap_start = parent->heap_start;
    child->user_image = parent->user_image;
    if (child->user_image) {
      __atomic_fetch_add(&((struct user_loaded_image *)child->user_image)->refcount, 1, __ATOMIC_RELAXED);
    }
  } else if (flags & B1NIX_CLONE_VM) {
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
    child->fd_lock = 0;
    /* Share the table owner's fd_lock: per-task lock copies would let two
     * siblings race the same table (see fd_lock_task). If the parent is
     * itself a thread, point at ITS owner so the whole group converges on
     * one lock. */
    {
      struct task *owner = g_task_fdlock_owner[task_index(parent)];
      g_task_fdlock_owner[task_index(child)] = owner ? owner : parent;
    }
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

  /* M29 thread flags. A CLONE_THREAD child is a thread of the caller's process
   * (joined via futex, invisible to waitpid, no SIGCHLD). A CLONE_VM child
   * WITHOUT CLONE_THREAD is the vfork/posix_spawn shape: a real child process
   * that merely borrows the address space until it execs, so it must be
   * waitpid-able and must raise SIGCHLD — marking it a thread left openrc-run
   * polling forever for a child exit it never heard about. */
  task_set_is_thread(child, (flags & B1NIX_CLONE_THREAD) ? 1 : 0);
  /* For fork (no CLONE_SETTLS), inherit the parent's TLS base so the child
   * can access thread-local data (FS segment). Only set an explicit TLS base
   * when CLONE_SETTLS is provided (pthread_create path). */
  task_set_tls_base(child, (flags & B1NIX_CLONE_SETTLS) ? tls : task_tls_base(current_task));
  task_set_child_tid_clear(child,
                            (flags & B1NIX_CLONE_CHILD_CLEARTID) ? ctid : 0);
  /* M63: a cloned thread shares its creator's seccomp filter (same process). */
  {
    extern void seccomp_inherit(struct task * parent, struct task * child);
    seccomp_inherit(parent, child);
  }

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

  /* M92: CLONE_PARENT_SETTID — write child TID to parent's location.
   * CLONE_CHILD_SETTID — write child TID to child's location (in its address
   * space). Both are needed by musl pthread_create for pthread_join.
   *
   * These MUST land before the child is made runnable. A thread that starts,
   * finishes and exits promptly has its CLONE_CHILD_CLEARTID word zeroed by
   * the kernel — and if this write ran afterwards it put the dead thread's tid
   * back, leaving pthread_join parked forever on a thread that no longer
   * exists (the M29 stress wedge on -smp 2). Linux writes the tids in
   * copy_process for the same reason, before wake_up_new_task. */
  if ((flags & B1NIX_CLONE_PARENT_SETTID) && parent_tid_addr) {
    /* parent_tid_addr is in the parent's address space (same mm for threads). */
    syscall_copyout((void *)(usize)parent_tid_addr, &child->id, sizeof(u32));
  }
  if ((flags & B1NIX_CLONE_CHILD_SETTID) && child_tid_addr) {
    /* child_tid_addr is in the child's address space (same mm for threads). */
    syscall_copyout((void *)(usize)child_tid_addr, &child->id, sizeof(u32));
  }

  /* M80: a new thread runs userspace code too, so give it its own XSAVE area
   * before it can be scheduled. */
  task_fpu_alloc(child);
  /* M80: PTRACE_O_TRACECLONE / TRACEVFORK — same as fork, with the event that
   * matches how this child was created. */
  ptrace_event_child(parent, child,
                     (flags & B1NIX_CLONE_VFORK)  ? PTRACE_EVENT_VFORK
                     : (flags & B1NIX_CLONE_THREAD) ? PTRACE_EVENT_CLONE
                                                    : PTRACE_EVENT_CLONE);
  interrupts_disable();
  /* M28 T4: see fork/kthread_create_impl — fresh task's kernel stack is set
   * up synchronously without going through arch_context_switch, so publish
   * stack_released=1 explicitly before the first pick sees it. */
  child->stack_released = 1;
  child->state = TASK_READY;
  sched_rq_enqueue_current(child);
  interrupts_enable();

  /* CLONE_VFORK: suspend here until the child execs or exits. */
  if (flags & B1NIX_CLONE_VFORK) {
    usize c_slot = task_index(child);
    usize c_id = child->id;
    g_task_vfork_id[c_slot] = c_id;
    g_task_vfork_pending[c_slot] = 1;
    while (g_task_vfork_pending[c_slot] && g_task_vfork_id[c_slot] == c_id) {
      interrupts_disable();
      if (!g_task_vfork_pending[c_slot] || g_task_vfork_id[c_slot] != c_id) {
        interrupts_enable();
        break;
      }
      current_task->wait_chan = &g_task_vfork_pending[c_slot];
      scheduler_lease_clear_here(__func__);
      current_task->state = TASK_BLOCKED;
      scheduler_yield();
      interrupts_enable();
    }
  }

  return (int)child->id;
}

void scheduler_reap_dead_threads(void) {
  /* Called from scheduler_yield with interrupts already disabled. Free the
   * kernel stack + slot of any DEAD thread (is_thread=1) whose
   * arch_context_switch has finished swapping RSP off its kernel stack
   * (stack_released==1). The thread's shareable resources (fd table,
   * user_image, cred) were already released on its exit path; the shared
   * mm however is freed HERE when this thread is its LAST user. A leader
   * that exit()s without joining is waitpid-reaped while its threads still
   * hold pml4 refs (mm_release_user returns 0 for it), so without this
   * last-user teardown the whole address space — pml4, page tables, vmas —
   * leaked on every unjoined multithreaded exit until PMM OOM. */
  for (usize i = 0; i < g_task_hwm; i++) {
    struct task *t = T(i);
    if (t == current_task) continue;
    if (t->state != TASK_DEAD) continue;
    if (!task_is_thread(t)) continue;
    if (!__atomic_load_n(&t->stack_released, __ATOMIC_ACQUIRE)) continue;

    /* Claim DEAD->REAPING first. Load-bearing twice over: (a) a racing
     * reaper/waitpid on another CPU must not double-free this slot, and
     * (b) user_address_space_cleanup below re-enables IRQs mid-walk, so a
     * timer tick can nest another scheduler_yield -> reap sweep on this
     * same CPU — the claim makes the nested sweep skip this slot instead
     * of tearing the same mm down twice. */
    enum task_state expected = TASK_DEAD;
    if (!__atomic_compare_exchange_n(&t->state, &expected, TASK_REAPING, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
      continue;

    u64 last_pml4 = mm_release_user(t);
    if (last_pml4) {
      /* Last user of the shared mm — tear it down exactly like the
       * waitpid reap path. cleanup reads t->pml4_phys, so put the value
       * back while we work: no other task can observe this mm anymore
       * (mm_release_user counted zero holders), so republishing it cannot
       * be seen by another release scan. cleanup also returns with IRQs
       * enabled; restore the yield invariant (IRQs off) before continuing
       * the sweep. */
      t->pml4_phys = last_pml4;
      user_address_space_cleanup(t);
      paging_free_address_space(last_pml4);
      t->pml4_phys = 0;
      interrupts_disable();
    } else {
      t->vma_list = 0; /* shared list lives on in the surviving users */
    }

    /* Release any per-thread (non-shared) resources. */
    if (t->cred) { cred_free(t->cred); t->cred = 0; }

    /* Drop this thread's reference to the shared fd table; the LAST user
     * closes the handles and frees it (a leader that exit()ed earlier
     * detached its own reference the same way). Closing can sleep on
     * writeback, so run it with IRQs enabled and restore the sweep
     * invariant after — same pattern as user_address_space_cleanup above. */
    {
      int *fl = 0;
      usize cap = 0;
      struct vfs_handle **tbl = fdtable_release(t, &fl, &cap);
      if (tbl) {
        interrupts_enable();
        for (usize j = 0; j < cap; j++) {
          if (tbl[j])
            vfs_close_handle(tbl[j], (int)task_tgid(t));
        }
        kfree(tbl);
        kfree(fl);
        interrupts_disable();
      }
    }
    if (t->stack) { kfree(t->stack); t->stack = 0; }
    if (t->name) { kfree((void *)t->name); t->name = 0; }
    free_task_slot(t);
  }
}

/* Set when a process (non-thread) becomes a zombie; gates the orphan sweep
 * below so scheduler_yield only walks the table while zombies may exist. The
 * sweep clears it again once no process zombie remains. */
static int g_have_proc_zombies = 0;

/* Reap process zombies that can never be collected by waitpid because they have
 * no living parent — e.g. a daemon backgrounded by a shell that has since
 * exited (dropbear under /etc/init.d/sshd). Without this such a zombie keeps
 * its slot + address space forever, and kill(pid,0) keeps reporting it alive.
 * A zombie WITH a live parent is left untouched for that parent's waitpid (so
 * init's child-respawn accounting is unaffected). Monotonic pids guarantee a
 * dead parent's id never aliases a live task, so the orphan test is exact.
 * Called from scheduler_yield with interrupts disabled, mirroring
 * scheduler_reap_dead_threads. */
void scheduler_reap_orphan_zombies(void) {
  int still_have = 0;
  for (usize i = 0; i < g_task_hwm; i++) {
    struct task *t = T(i);
    if (t == current_task) continue;
    if (t->state != TASK_DEAD) continue;
    if (task_is_thread(t)) continue; /* threads: scheduler_reap_dead_threads */
    still_have = 1;
    if (!__atomic_load_n(&t->stack_released, __ATOMIC_ACQUIRE)) continue;

    int has_live_parent = 0;
    for (usize p = 0; p < g_task_hwm; p++) {
      struct task *pt = T(p);
      if (pt->id == t->parent_id && pt->state != TASK_UNUSED &&
          pt->state != TASK_DEAD && pt->state != TASK_REAPING) {
        has_live_parent = 1;
        break;
      }
    }
    if (has_live_parent) continue; /* its parent will waitpid it */

    /* Claim DEAD->REAPING so a racing waitpid on another CPU can't double-free. */
    enum task_state expected = TASK_DEAD;
    if (!__atomic_compare_exchange_n(&t->state, &expected, TASK_REAPING, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
      continue;

    if (t->user_image) { user_image_free(t->user_image); t->user_image = 0; }
    u64 last_pml4 = mm_release_user(t);
    if (last_pml4) {
      t->pml4_phys = last_pml4; /* see scheduler_reap_dead_threads */
      user_address_space_cleanup(t);
      paging_free_address_space(last_pml4);
      t->pml4_phys = 0;
      interrupts_disable(); /* cleanup exits IRQs-on; restore yield invariant */
    } else {
      t->vma_list = 0;
    }
    if (t->cred) { cred_free(t->cred); t->cred = 0; }
    if (t->fd_table) {
      kfree(t->fd_table);
      kfree(t->fd_flags);
      t->fd_table = 0;
      t->fd_flags = 0;
      t->fd_capacity = 0;
    }
    if (t->name && strcmp(t->name, "boot") != 0) {
      kfree((void *)t->name);
      t->name = 0;
    }
    if (t->stack) { kfree(t->stack); t->stack = 0; }
    free_task_slot(t);
  }
  g_have_proc_zombies = still_have;
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

  /* Reap orphaned process zombies (no living parent) so daemons whose shell
   * parent has exited don't leak their slot/address space forever. Gated so
   * the table walk only runs while a process zombie may exist. */
  if (g_have_proc_zombies) scheduler_reap_orphan_zombies();

  /* Deliver pending signals for current task */
  if (current_task) {
    scheduler_deliver_pending_signals();
  }

  struct task *old_task = current_task;
  struct task *new_task = pick_next_task();

  if (new_task == 0) {
    if (old_task != 0 && old_task->state == TASK_DEAD) {
      struct percpu *p = get_percpu();
      console_write("PANIC INFO: pcpu=0x");
      console_write_hex64((u64)p);
      if (p) {
        console_write(" cpu_id=");
        console_write_dec(p->cpu_id);
        console_write(" idle_task=0x");
        console_write_hex64((u64)p->idle_task);
      }
      console_write("\n");
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
    /* Stride Scheduler: increment pass of yielding task by its stride */
    usize old_idx = task_index(old_task);
    int nice = g_task_nice[old_idx];
    if (nice < -20) nice = -20;
    if (nice > 19) nice = 19;
    int tickets = 20 - nice;
    int stride = 1000 / tickets;
    g_task_pass[old_idx] += stride;

    /* M28 T4: claim the kernel stack BEFORE publishing state=READY. Under T4
     * the save side of arch_context_switch (the movq %rsp,0(%rdi) +
     * movq 0(%rsi),%rsp sequence) is no longer serialised by the BKL, so a
     * concurrent pick_next_task on another CPU could observe state==READY,
     * win the CAS, load this task's saved RSP, and start `ret`ing on the
     * same kernel stack while THIS CPU is mid-save. The reader checks
     * stack_released==1 (published by arch_context_switch after the RSP
     * swap) before claiming; x86 TSO orders the 0 store ahead of the READY
     * store, so other CPUs observe "READY but not yet released" and skip. */
    task_lease_clear(old_task, __func__);
    old_task->state = TASK_READY;
  }

  /* Catch a corrupted kernel-stack pointer AT the switch instead of letting
   * arch_context_switch `ret` off a bogus stack (which surfaces as a kernel
   * fault at a wild rip with no usable backtrace). Both the saved RSP and the
   * syscall-entry stack top must lie inside the task's own kernel stack. */
  if (new_task->stack) {
    u64 lo = (u64)(usize)new_task->stack;
    u64 hi = lo + KERNEL_STACK_SIZE;
    u64 ksp = new_task->kernel_stack_ptr;
    u64 crsp = new_task->context.rsp;
    if (ksp < lo || ksp > hi || (crsp != 0 && (crsp < lo || crsp > hi))) {
      console_write("sched: corrupt kernel stack pointer for pid ");
      console_write_dec((u64)new_task->id);
      console_write(" name=");
      console_write(new_task->name ? new_task->name : "(none)");
      console_write("\n  stack=0x");
      console_write_hex64(lo);
      console_write(" kernel_stack_ptr=0x");
      console_write_hex64(ksp);
      console_write(" context.rsp=0x");
      console_write_hex64(crsp);
      console_write("\n");
      panic("corrupt task kernel stack pointer");
    }
  }

  /* M86: close the outgoing task's CPU-time interval before `current_task`
   * moves on. Reads old_task->state, so it must run before the state of the
   * outgoing task is touched any further. */
  sched_acct_on_switch(old_task);

  new_task->state = TASK_RUNNING;
  current_task = new_task;

  arch_set_kernel_stack(new_task->kernel_stack_ptr);
  paging_switch_address_space(new_task->pml4_phys);

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
  task_fpu_save(old_task);
  old_task->fpu_initialized = 1;
  if (!g_clean_fpu_ready) {
    /* old_task's state is already saved above; capture_clean reinits the live
     * FPU, which is fine since new_task's state is loaded immediately after. */
    arch_fpu_capture_clean(g_clean_fpu);
    g_clean_fpu_ready = 1;
  }
  if (new_task->fpu_initialized) {
    task_fpu_restore(new_task);
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
  task_lease_clear(current_task, __func__);
  current_task->state = TASK_BLOCKED;
  scheduler_yield();
  interrupts_enable();
}

void scheduler_wake_task(usize task_id) {
  /* Interrupt-state-preserving for the same reason as scheduler_wake_all: this
   * may be called from an IRQs-off context (futex/loopback kicks), and a
   * force-enable there would drop interrupt masking a caller is relying on. */
  u64 flags = interrupts_save();

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

  interrupts_restore(flags);
}

void scheduler_block_on(void *chan) {
  interrupts_disable();

  if (current_task == 0) {
    panic("scheduler_block_on without running task");
  }

  if (current_task->state == TASK_READY) {
    current_task->state = TASK_RUNNING;
    current_task->wait_chan = 0;
    interrupts_enable();
    return;
  }

  if (current_task->state == TASK_BLOCKED || current_task->state == TASK_SLEEPING) {
    current_task->state = TASK_RUNNING;
  } else if (current_task->state != TASK_RUNNING) {
    panic("scheduler_block_on without running task");
  }

  current_task->wait_chan = chan;
  /* M28 T4: claim the stack lease before publishing BLOCKED — see
   * scheduler_block_current for the full race. */
  task_lease_clear(current_task, __func__);
  current_task->state = TASK_BLOCKED;
  scheduler_yield();
  interrupts_enable();
}

/* Like scheduler_block_on but arms a timer deadline: wake_sleepers() (run from
 * the timer ISR) CAS-promotes a BLOCKED task whose wake_tick has elapsed back
 * to READY even when no explicit wake arrives. This is what backs the futex
 * timed-wait path (pthread_cond_timedwait / sem_timedwait / mutex_timedlock).
 * timeout_ticks == 0 means "no deadline" and behaves exactly like
 * scheduler_block_on. */
u64 scheduler_get_ticks(void) {
  return scheduler_ticks;
}

void scheduler_block_on_timeout(void *chan, u64 timeout_ticks) {
  interrupts_disable();

  if (current_task == 0) {
    panic("scheduler_block_on_timeout without running task");
  }

  if (current_task->state == TASK_READY) {
    current_task->state = TASK_RUNNING;
    current_task->wait_chan = 0;
    current_task->wake_tick = 0;
    interrupts_enable();
    return;
  }

  if (current_task->state == TASK_BLOCKED || current_task->state == TASK_SLEEPING) {
    current_task->state = TASK_RUNNING;
  } else if (current_task->state != TASK_RUNNING) {
    panic("scheduler_block_on_timeout without running task");
  }

  current_task->wait_chan = chan;
  if (timeout_ticks)
    current_task->wake_tick = scheduler_ticks + timeout_ticks;
  /* M28 T4: claim the stack lease before publishing BLOCKED — see
   * scheduler_block_current for the full race. */
  task_lease_clear(current_task, __func__);
  current_task->state = TASK_BLOCKED;
  scheduler_yield();
  /* Drop any unfired deadline: an explicit wake may have resumed us early. */
  current_task->wake_tick = 0;
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
  task_lease_clear(current_task, __func__);
  __atomic_store_n(&current_task->state, TASK_BLOCKED, __ATOMIC_SEQ_CST);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void scheduler_wait_commit(void) {
  scheduler_yield();
  /* Drop any unfired deadline armed by scheduler_wait_prepare_timeout: an
   * explicit wake_all may have resumed us before it elapsed, and a stale
   * wake_tick would otherwise leak into a later untimed block (scheduler_block_on
   * never re-arms it). Mirrors scheduler_block_on_timeout's post-yield clear. */
  current_task->wake_tick = 0;
  interrupts_enable();
}

/*
 * One break per address space, not one per thread.
 *
 * Threads share their mappings — the page tables and the VMA list are the same
 * objects — but each carried its own copy of where the break ended. libc keeps
 * its own idea of the break in memory all the threads share, so a thread that
 * had not itself grown the heap asked to extend it from a value the kernel
 * believed was still current, and the kernel dutifully mapped fresh zeroed
 * frames over pages another thread was already using. That is how a
 * multithreaded program's allocator metadata came to be full of zeros while
 * every mapping looked correct.
 */
/*
 * The head of the mapping list, likewise.
 *
 * Threads share the VMA list itself, but each holds its own pointer to its
 * first element. A thread that inserts or removes at the front leaves its
 * siblings pointing at a node that is no longer first — or, after a removal, at
 * freed memory. They then place new mappings on top of live ones and walk a
 * list that no longer exists.
 */
void scheduler_sync_vma_head(u64 pml4_phys, struct vm_area *head) {
  u64 flags;

  if (!pml4_phys)
    return;
  tasks_lock(&flags);
  for (usize i = 0; i < g_task_hwm; i++) {
    struct task *t = T(i);

    if (t->state == TASK_UNUSED || t->pml4_phys != pml4_phys)
      continue;
    t->vma_list = head;
  }
  tasks_unlock(flags);
}

void scheduler_sync_brk(u64 pml4_phys, u64 heap_start, u64 brk) {
  u64 flags;

  if (!pml4_phys)
    return;
  tasks_lock(&flags);
  for (usize i = 0; i < g_task_hwm; i++) {
    struct task *t = T(i);

    if (t->state == TASK_UNUSED || t->pml4_phys != pml4_phys)
      continue;
    t->heap_start = heap_start;
    t->user_brk = brk;
  }
  tasks_unlock(flags);
}

usize scheduler_address_space_users(u64 pml4_phys, usize *first_id,
                                    const char **first_name) {
  u64 flags;
  usize count = 0;

  if (!pml4_phys)
    return 0;
  tasks_lock(&flags);
  for (usize i = 0; i < g_task_hwm; i++) {
    struct task *t = T(i);

    if (t == current_task || t->pml4_phys != pml4_phys)
      continue;
    /* A task past TASK_DEAD no longer executes: its address space is exactly
     * what the caller is here to release. Only a task that can still be
     * scheduled counts as a user. */
    if (t->state == TASK_UNUSED || t->state == TASK_DEAD ||
        t->state == TASK_REAPING)
      continue;
    if (count == 0) {
      if (first_id)
        *first_id = t->id;
      if (first_name)
        *first_name = t->name;
    }
    count++;
  }
  tasks_unlock(flags);
  return count;
}

int scheduler_wait_armed(void) {
  return current_task &&
         __atomic_load_n(&current_task->state, __ATOMIC_SEQ_CST) == TASK_BLOCKED;
}

void scheduler_wait_cancel(void) {
  current_task->wait_chan = 0;
  current_task->wake_tick = 0;
  current_task->state = TASK_RUNNING;
  interrupts_enable();
}

/* Like scheduler_wait_prepare but also arms a timer deadline, so wake_sleepers()
 * (run from the timer ISR) promotes the task back to READY even if the explicit
 * wake never arrives. This combines the race-free check-then-block window of the
 * prepare/commit pattern with the lost-wakeup safety net of a timeout — exactly
 * what an interrupt-driven device-completion wait wants: the IRQ handler's
 * scheduler_wake_all(chan) resumes us immediately on the common path, while the
 * deadline guarantees forward progress if an interrupt is ever genuinely lost
 * (or the controller never raises one). timeout_ticks == 0 means no deadline. */
void scheduler_wait_prepare_timeout(void *chan, u64 timeout_ticks) {
  interrupts_disable();
  if (current_task == 0)
    panic("scheduler_wait_prepare_timeout without running task");
  current_task->wait_chan = chan;
  if (timeout_ticks)
    current_task->wake_tick = scheduler_ticks + timeout_ticks;
  task_lease_clear(current_task, __func__);
  __atomic_store_n(&current_task->state, TASK_BLOCKED, __ATOMIC_SEQ_CST);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* True when the current context can safely park on a wait channel: the scheduler
 * is live, we are running inside a real task (not the early single-stack boot
 * path before the first context switch), and interrupts are enabled so a device
 * IRQ can actually wake us. Drivers that block on I/O completion fall back to a
 * cooperative poll loop when this is false (early boot, IRQs-off callers). */
int scheduler_can_block(void) {
  return scheduler_started && current_task != 0 && interrupts_enabled();
}

void scheduler_wake_all(void *chan) {
  /* Preserve the caller's interrupt state instead of force-enabling. This is
   * called from inside IRQs-off spinlock critical sections (e.g. tcp_input
   * holds tcp_queue_lock via irq_save() when it wakes pollers on vfs_poll_chan).
   * A plain interrupts_enable() there re-enabled IRQs while the spinlock was
   * still held, letting a timer tick preempt and deschedule the lock holder —
   * after which any other BSP context spinning on that lock with IRQs disabled
   * waits forever (the holder, a BSP-only kernel thread, can never be rescheduled
   * because the spinner monopolises the BSP and the APs don't run kernel threads).
   * That was the intermittent -smp wedge. interrupts_save()/restore() keeps a
   * caller that had IRQs off off. */
  u64 flags = interrupts_save();

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

  interrupts_restore(flags);
  /* M28 #6: same reschedule kick as wake_sleepers. lapic_send_ipi spins on ICR
   * delivery (no IRQs required), so running it after restoring the caller's
   * state — even if that leaves IRQs disabled — is safe. */
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
    /* A different fault from "no task at all", and it used to say the same
     * thing: this one is a task that is on a wait channel or already dying and
     * asked to sleep anyway. Naming the state is the difference between one
     * look at the log and a rebuild with printf. */
    klog_warn("scheduler_sleep_ticks: task is not running");
    panic("scheduler_sleep_ticks with a task that is not running");
  }

  current_task->wake_tick = scheduler_ticks + ticks;
  /* M28 T4: claim the stack lease before publishing SLEEPING — a timer-tick
   * wake_sleepers on another CPU can CAS us SLEEPING->READY and resume us; see
   * scheduler_block_current for the full race. */
  task_lease_clear(current_task, __func__);
  current_task->state = TASK_SLEEPING;
  scheduler_yield();
  interrupts_enable();

  /*
   * Finish the sleep even when nothing else wanted the CPU.
   *
   * scheduler_yield returns without switching when no other task is ready, and
   * the sleep above is then over the moment it began — a caller asking for a
   * second gets no delay at all. That is invisible in a busy system and total
   * in an idle one: a boot that ends in a timed hold ran through the whole hold
   * in a few microseconds and powered the machine off, which cost a night of
   * display measurements before the loop was found to be the thing at fault.
   *
   * So wait out the remainder against the timer here. The scheduler check keeps
   * this honest before the timer runs — there the tick can never arrive, and
   * spinning for it would hang the boot rather than shorten a sleep.
   */
  if (scheduler_started) {
    while ((i64)(current_task->wake_tick - scheduler_ticks) > 0) {
      /* A sleep ends when a signal arrives, not only when the clock says so.
       *
       * Programs sleep "forever" on purpose — a thread parks with a timeout of
       * years and expects a signal to be what wakes it. Without this check the
       * sleep really did last years: a thread that had been sent SIGKILL sat
       * here with the bit set and never died, and the process it belonged to
       * waited for it in exit_group and never finished. From outside, the
       * process looked alive; it was unable to die. */
      if (__atomic_load_n(&current_task->pending_signals, __ATOMIC_ACQUIRE) &
          ~current_task->blocked_signals)
        break;
      /* Someone woke us, or the task is being torn down. terminate_group_
       * siblings and the ordinary wake paths promote a SLEEPING task to READY;
       * this loop used to keep spinning on the clock regardless, so being woken
       * changed nothing.
       *
       * Named states, not "anything but SLEEPING". The obvious form of this
       * test is wrong in a way that is easy to miss: scheduler_yield above
       * returns with the task switched back in and its state already RUNNING,
       * so "not SLEEPING" is true on the very first pass and the remaining
       * ticks are never waited out at all. That silently restores the no-op
       * sleep this whole loop exists to prevent — an idle machine runs a timed
       * hold to completion in microseconds. */
      {
        int st = __atomic_load_n(&current_task->state, __ATOMIC_ACQUIRE);
        if (st == TASK_READY || st == TASK_DEAD || st == TASK_REAPING)
          break;
      }
      __asm__ volatile("sti; hlt");
    }
  }
  current_task->state = TASK_RUNNING;
}

/* Silence watchdog (test mode only). A wedged instance prints nothing, so the
 * host harness can only report "stalled" and every later marker in that run
 * reads as missing. Dump the task table from inside the guest instead: state,
 * wait channel, fd-lock holder and lease site for every task is usually enough
 * to name the deadlock without a second, instrumented run. Dumps at most three
 * times so the host's own stall timer still gets to kill a hopeless instance. */
#define SILENCE_WATCHDOG_TICKS (60 * 100) /* 60s at the 100 Hz timer tick */
#define SILENCE_WATCHDOG_MAX_DUMPS 3
extern volatile u64 g_console_write_seq;
static void serial_silence_watchdog(void) {
  static u64 last_seq;
  static u64 last_change_tick;
  static int dumps;
  static int test_mode = -1;
  if (test_mode < 0)
    test_mode = bootinfo_has_flag("b1nix.test=1") ? 1 : 0;
  if (!test_mode || dumps >= SILENCE_WATCHDOG_MAX_DUMPS)
    return;
  u64 seq = g_console_write_seq;
  if (seq != last_seq) {
    last_seq = seq;
    last_change_tick = scheduler_ticks;
    return;
  }
  if (scheduler_ticks - last_change_tick < SILENCE_WATCHDOG_TICKS)
    return;
  last_change_tick = scheduler_ticks;
  dumps++;
  console_write("\nSMOKE-GUEST-WATCHDOG: no console output for 60s — task dump:\n");
  scheduler_dump_tasks();
  last_seq = g_console_write_seq;
}

void scheduler_on_timer_tick(void) {
  if (!scheduler_started || current_task == 0) {
    return;
  }

  scheduler_ticks++;

  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->state != TASK_UNUSED && g_task_alarm_ticks[i] != 0 && g_task_alarm_ticks[i] <= scheduler_ticks) {
      /* Interval timer (setitimer ITIMER_REAL) re-arms itself; a plain
       * alarm(2) is one-shot. */
      g_task_alarm_ticks[i] = g_task_alarm_interval_ticks[i]
                                  ? scheduler_ticks + g_task_alarm_interval_ticks[i]
                                  : 0;
      scheduler_kill(T(i)->id, SIGALRM);
    }
  }

  serial_silence_watchdog();

  posix_timers_tick(); /* M74: fire expired POSIX interval timers */

  wake_sleepers();

  /* M56 timerfd: if any timerfd is armed, wake everyone blocked in
   * poll/select/epoll_wait so they re-scan and notice a fired timer. The hook
   * is a no-op (single atomic load) when no timerfds are armed, so it costs
   * nothing on the common path. */
  {
    extern void eventpoll_timer_tick(void);
    eventpoll_timer_tick();
  }

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
  /* A task inside a non-preemptible region keeps the CPU. The tick still ran:
   * time advances, accounting is done, and a driver polling a register with a
   * timeout can still observe that timeout expire. */
  if (current_task->state == TASK_RUNNING && g_preempt_depth[(u32)percpu_read(cpu_id) % MAX_CPUS] == 0) {
    scheduler_yield();
  }
}

/* ── M86: CPU-time accounting boundaries ──────────────────────────────────────
 *
 * A CPU is always running exactly one task in exactly one mode, so the whole
 * accounting problem is: stamp the TSC every time either of those two facts
 * changes, and credit the elapsed interval to whoever was running. The three
 * boundaries are ring-3 → ring-0 entry (syscall, IRQ, exception from user),
 * ring-0 → ring-3 return, and a context switch. Everything in between is one
 * homogeneous interval.
 *
 * The stamp is per-CPU (a task can only run on one CPU at a time, so its
 * counters have a single writer), and the mode of each interval is known at
 * the call site rather than tracked in a variable: an interval ended by a
 * kernel entry from ring 3 was spent in user mode, and every other interval
 * ended in the kernel. */
static u64 g_acct_stamp[MAX_CPUS];

static inline u64 acct_rdtsc(void) {
  u32 lo, hi;
  __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));
  return ((u64)hi << 32) | lo;
}

static inline u64 acct_cycles_to_ns(u64 cycles) {
  u32 khz = arch_cpu_khz();
  if (!khz)
    return 0; /* TSC frequency unknown (calibration skipped) — no accounting */
  /* Clamp so `cycles * 1000000` cannot overflow a u64. 2^43 cycles is ~48
   * minutes of a 3 GHz core, far beyond any single accounting interval; the
   * clamp only ever fires on a garbage delta (e.g. a TSC that went backwards
   * across a suspend). */
  if (cycles > (1ULL << 43))
    cycles = 1ULL << 43;
  return (cycles * 1000000ULL) / (u64)khz;
}

static void acct_flush(struct task *t, int as_user) {
  struct percpu *pcpu = get_percpu();
  if (!pcpu)
    return;
  u32 cpu = pcpu->cpu_id;
  if (cpu >= MAX_CPUS)
    return;
  u64 now = acct_rdtsc();
  u64 prev = g_acct_stamp[cpu];
  g_acct_stamp[cpu] = now;
  if (!t || now <= prev)
    return;
  u64 ns = acct_cycles_to_ns(now - prev);
  if (!ns)
    return;
  usize idx = task_index(t);
  if (as_user)
    g_task_utime_ns[idx] += ns;
  else
    g_task_stime_ns[idx] += ns;
}

void sched_acct_enter_kernel(void) {
  if (!scheduler_started)
    return;
  acct_flush(current_task, 1);
}

void sched_acct_leave_kernel(void) {
  if (!scheduler_started)
    return;
  acct_flush(current_task, 0);
}

void sched_acct_on_switch(struct task *prev) {
  if (!scheduler_started)
    return;
  /* A switch always happens with the CPU in ring 0, so the interval that just
   * ended is `prev`'s system time. */
  acct_flush(prev, 0);
  if (prev) {
    usize idx = task_index(prev);
    /* Still runnable at switch-out ⇒ it was preempted (involuntary); anything
     * else means it gave the CPU up itself. */
    if (prev->state == TASK_RUNNING || prev->state == TASK_READY)
      g_task_nivcsw[idx]++;
    else
      g_task_nvcsw[idx]++;
  }
}

u64 scheduler_get_uptime_ticks(void) { return scheduler_ticks; }

static void post_sigchld_to_parent(usize parent_id, int job_control_event);
static int is_pgrp_orphaned(usize pgid, const struct task *exiting);

/* Reparent every child of `exiting` to init (PID 1) and, for each process group
 * that thereby becomes orphaned and still has stopped members, deliver the
 * POSIX SIGHUP+SIGCONT. Shared by normal exit and the in-scheduler signal-death
 * paths so both reparent consistently (M46-3). Caller holds interrupts. */
/* prctl(PR_SET_PDEATHSIG): the signal a task asked to receive when its parent
 * dies. Kept in a side table rather than a struct task field, the same way the
 * declared ptracer is — struct task is deliberately not grown. A pid holds at
 * most one entry; signo 0 clears it. */
#define PDEATHSIG_MAX 64
static struct {
  usize pid;
  int signo;
} g_pdeathsig[PDEATHSIG_MAX];

int scheduler_set_pdeathsig(usize pid, int signo) {
  if (signo < 0 || signo > NSIG_MAX)
    return -EINVAL;
  int free_slot = -1;
  for (int i = 0; i < PDEATHSIG_MAX; i++) {
    if (g_pdeathsig[i].pid == pid) {
      g_pdeathsig[i].signo = signo;
      if (signo == 0)
        g_pdeathsig[i].pid = 0;
      return 0;
    }
    if (free_slot < 0 && g_pdeathsig[i].pid == 0)
      free_slot = i;
  }
  if (signo == 0)
    return 0; /* nothing registered; clearing is a no-op */
  if (free_slot < 0)
    return -ENOMEM;
  g_pdeathsig[free_slot].pid = pid;
  g_pdeathsig[free_slot].signo = signo;
  return 0;
}

void scheduler_clear_pdeathsig(usize pid) {
  for (int i = 0; i < PDEATHSIG_MAX; i++) {
    if (g_pdeathsig[i].pid == pid) {
      g_pdeathsig[i].pid = 0;
      g_pdeathsig[i].signo = 0;
      return;
    }
  }
}

static int pdeathsig_of(usize pid) {
  for (int i = 0; i < PDEATHSIG_MAX; i++)
    if (g_pdeathsig[i].pid == pid)
      return g_pdeathsig[i].signo;
  return 0;
}

static void reparent_children_and_signal_orphans(struct task *exiting) {
  /* is_pgrp_orphaned ignores `exiting` as a parent, so a group with several of
   * our children is judged orphaned even though some have not been reparented
   * yet in this loop (M46-2). */
  for (usize i = 0; i < g_task_hwm; i++) {
    struct task *child = T(i);
    if (child->state != TASK_UNUSED && child->parent_id == exiting->id) {
      child->parent_id = 1;
      /* Deliver the parent-death signal before the child is reparented to
       * init, which is the only moment at which "my parent died" is still a
       * fact rather than history. */
      int pds = pdeathsig_of(child->id);
      if (pds > 0)
        scheduler_kill(child->id, pds);
      usize pgid = child->process_group_id;
      if (is_pgrp_orphaned(pgid, exiting)) {
        int has_stopped = 0;
        for (usize j = 0; j < g_task_hwm; j++) {
          if (T(j)->state == TASK_STOPPED && T(j)->process_group_id == pgid) {
            has_stopped = 1;
            break;
          }
        }
        if (has_stopped) {
          scheduler_kill_process_group(pgid, SIGHUP);
          scheduler_kill_process_group(pgid, SIGCONT);
        }
      }
    }
  }
}

static void terminate_group_siblings(struct task *t) {
  usize tgid = g_task_tgid[task_index(t)];
  if (tgid == 0) return;
  for (usize i = 0; i < g_task_hwm; i++) {
    struct task *sibling = T(i);
    if (sibling->state != TASK_UNUSED && sibling != t && g_task_tgid[i] == tgid) {
      /* Skip a sibling that has already committed to exiting with its own code
       * (scheduler_exit_group/exit_current). Re-posting SIGKILL to it would let
       * its own signal-delivery pass overwrite that code with SIGNALED|SIGKILL,
       * so waitpid reports a spurious signalled death (M29 stress-exit-code). */
      if (g_task_exiting[i])
        continue;
      /* A parked leader that has been handed the group's exit status exits on
       * its own; SIGKILLing it would report a signalled death instead (M86). */
      if (g_task_parked_leader[i] && g_task_parked_override[i])
        continue;
      /* Post SIGKILL to sibling */
      __atomic_fetch_or(&sibling->pending_signals, (1ULL << (SIGKILL - 1)), __ATOMIC_RELEASE);
      /* Wake them if they are blocked/sleeping/stopped. Use a per-state CAS,
       * NOT a plain store: a sibling running on another AP can transition to
       * DEAD/REAPING between the read and the write, and a bare
       * `state = READY` would resurrect it and re-queue a task whose stack/slot
       * is being freed (UAF). Mirror wake_sleepers — only a state we still
       * observe as BLOCKED/SLEEPING/STOPPED is promoted. */
      enum task_state expected = TASK_BLOCKED;
      int woke = __atomic_compare_exchange_n(&sibling->state, &expected,
                                             TASK_READY, 0, __ATOMIC_ACQUIRE,
                                             __ATOMIC_RELAXED);
      if (!woke) {
        expected = TASK_SLEEPING;
        woke = __atomic_compare_exchange_n(&sibling->state, &expected,
                                           TASK_READY, 0, __ATOMIC_ACQUIRE,
                                           __ATOMIC_RELAXED);
      }
      if (!woke) {
        expected = TASK_STOPPED;
        woke = __atomic_compare_exchange_n(&sibling->state, &expected,
                                           TASK_READY, 0, __ATOMIC_ACQUIRE,
                                           __ATOMIC_RELAXED);
      }
      if (woke) {
        sibling->wait_chan = 0;
        sched_rq_enqueue_current(sibling);
      }
    }
  }
}

static int is_pgrp_orphaned(usize pgid, const struct task *exiting) {
  for (usize i = 0; i < g_task_hwm; i++) {
    struct task *t = T(i);
    if (t != exiting && t->state != TASK_UNUSED &&
        t->process_group_id == pgid) {
      usize parent_id = t->parent_id;
      for (usize j = 0; j < g_task_hwm; j++) {
        struct task *parent = T(j);
        if (parent->state != TASK_UNUSED && parent->id == parent_id) {
          /* The exiting task is about to reparent all its children to init, so
           * it must NOT count as an outside-pgrp parent here — otherwise a group
           * whose only outside parent is us reads as "not orphaned" and misses
           * its SIGHUP+SIGCONT (M46-2). init (PID 1) never counts either. */
          if (parent != exiting && parent->id != 1 &&
              parent->process_group_id != pgid &&
              parent->session_id == t->session_id) {
            return 0; /* not orphaned */
          }
        }
      }
    }
  }
  return 1; /* orphaned */
}

void scheduler_exit_group(int exit_code) {
  if (current_task == 0) {
    panic("scheduler_exit_group without current task");
  }
  /* Mark exiting BEFORE killing siblings: a sibling that processes its SIGKILL
   * calls terminate_group_siblings() and would otherwise re-post SIGKILL back to
   * this (already-exiting) leader, clobbering our exit_group() code with a
   * spurious SIGNALED status (M29 stress-exit-code). terminate_group_siblings
   * skips tasks whose g_task_exiting flag is set. */
  g_task_exiting[task_index(current_task)] = 1;
  u64 flags = interrupts_save();
  /* M86: if this group's leader already left through exit(2) and is waiting for
   * us, give it this exit status — a parked leader must not be turned into a
   * SIGKILL death by the teardown below. */
  {
    usize tgid = g_task_tgid[task_index(current_task)];
    if (tgid && tgid != current_task->id) {
      for (usize i = 0; i < g_task_hwm; i++) {
        if (T(i)->id == tgid && g_task_parked_leader[i]) {
          g_task_parked_code[i] = exit_code;
          g_task_parked_override[i] = 1;
          break;
        }
      }
    }
  }
  terminate_group_siblings(current_task);
  interrupts_restore(flags);
  scheduler_exit_current(exit_code);
}

/* M86: exit(2) — as opposed to exit_group(2) — ends ONE thread, even when that
 * thread is the group leader. musl's pthread_exit() from main relies on exactly
 * this: it leaves the main thread and the process must keep running until the
 * last remaining thread finishes. b1nix used to route both syscalls into
 * scheduler_exit_current, whose leader branch calls terminate_group_siblings —
 * so pthread_exit() in main killed the whole process.
 *
 * The leader cannot simply die either: b1nix reports a process's death (and
 * frees the mm/fd table) when its task dies, and the parent's waitpid must see
 * the process only once every thread is gone. So a leader with live siblings
 * parks in the kernel until the group empties, then exits with its own code —
 * the observable contract of a Linux group leader zombie, without a second
 * task-lifecycle state to keep consistent across CPUs.
 *
 * The park is a 1-tick poll rather than a wait channel: the last thread out may
 * be on another CPU, may die from a signal rather than a syscall, and may exit
 * before this loop is even entered. Polling is correct for every one of those
 * orders and costs a leader that is exiting anyway one tick per iteration. */
static void thread_release_ctid(struct task *t);

void scheduler_exit_thread(int exit_code) {
  if (current_task == 0)
    panic("scheduler_exit_thread without current task");
  usize me = current_task->id;
  usize idx = task_index(current_task);
  usize tgid = g_task_tgid[idx];
  if (tgid == me) {
    /* Release the tid word (and wake its futex) BEFORE parking: the surviving
     * threads need it to make progress, and progress is exactly what this loop
     * is waiting for. */
    thread_release_ctid(current_task);
    g_task_parked_leader[idx] = 1;
    for (;;) {
      /* A sibling ran exit_group(): adopt its status and stop waiting. */
      if (g_task_parked_override[idx]) {
        exit_code = g_task_parked_code[idx];
        break;
      }
      /* Killed outright (a sibling's fatal fault, or an outside SIGKILL): the
       * process dies by signal, so report that rather than the exit code. */
      if (__atomic_load_n(&current_task->pending_signals, __ATOMIC_ACQUIRE) &
          (1ULL << (SIGKILL - 1))) {
        exit_code = TASK_EXIT_SIGNALED | SIGKILL;
        break;
      }
      int live = 0;
      u64 flags = interrupts_save();
      for (usize i = 0; i < g_task_hwm; i++) {
        struct task *o = T(i);
        if (o->id == me || o->state == TASK_UNUSED || o->state == TASK_DEAD ||
            o->state == TASK_REAPING)
          continue;
        /* A sibling that has committed to exiting no longer counts: it wakes
         * us on its way out (see scheduler_exit_current), and waiting for its
         * slot to actually reach DEAD would race that wake-up. */
        if (g_task_tgid[i] == tgid && !g_task_exiting[i]) {
          live = 1;
          break;
        }
      }
      interrupts_restore(flags);
      if (!live)
        break;
      /* Sleep on the channel the last thread out signals, with a timeout as the
       * backstop: a thread can die on a path that never reaches the wake (a
       * fatal fault handled in the scheduler), and a lost wake-up here would
       * wedge the process forever rather than merely delay it. */
      scheduler_block_on_timeout(&g_task_parked_leader[idx], 20);
    }
    g_task_parked_leader[idx] = 0;
  }
  scheduler_exit_current(exit_code);
}

static usize g_init_pid;

void scheduler_set_init_pid(usize pid) { g_init_pid = pid; }

usize scheduler_get_init_pid(void) { return g_init_pid; }

/* Ring of the last thread deaths, dumped by scheduler_dump_tasks when the
 * watchdog fires: (tid, ctid) tells at a glance whether a joined-on thread ever
 * reached the tid-clearing path. */
#define THREAD_EXIT_TRACE 16
static struct {
  usize tid;
  u64 ctid;
  u64 frame;   /* physical frame the clearing write landed in */
  u32 readback;/* value read back right after the write */
} g_thread_exit_trace[THREAD_EXIT_TRACE];
static u32 g_thread_exit_trace_idx;

static void thread_exit_trace(usize tid, u64 ctid, u64 frame, u32 readback) {
  u32 i = __atomic_fetch_add(&g_thread_exit_trace_idx, 1, __ATOMIC_RELAXED) %
          THREAD_EXIT_TRACE;
  g_thread_exit_trace[i].tid = tid;
  g_thread_exit_trace[i].ctid = ctid;
  g_thread_exit_trace[i].frame = frame;
  g_thread_exit_trace[i].readback = readback;
}

void scheduler_dump_thread_exits(void) {
  console_write("thread exits (tid:ctid):");
  for (u32 i = 0; i < THREAD_EXIT_TRACE; i++) {
    if (!g_thread_exit_trace[i].tid)
      continue;
    console_write(" ");
    console_write_dec(g_thread_exit_trace[i].tid);
    console_write(":");
    console_write_hex64(g_thread_exit_trace[i].ctid);
    console_write("@");
    console_write_hex64(g_thread_exit_trace[i].frame);
    console_write("=");
    console_write_dec((u64)g_thread_exit_trace[i].readback);
  }
  console_write("\n");
}

/* CLONE_CHILD_CLEARTID: a dying thread must zero the tid word its joiner is
 * parked on and wake that futex. It has to happen on EVERY path a thread can
 * die on — a thread killed by a signal inside the scheduler used to skip it,
 * and pthread_join then slept forever on a word that stayed at the dead
 * thread's tid (the M29 stress wedge). Idempotent: the address is cleared as it
 * is consumed, so a second call does nothing. */
static void thread_release_ctid(struct task *t) {
  /* Not gated on task_is_thread: a MAIN thread leaving through pthread_exit()
   * registers the very same word (musl points set_tid_address at its
   * __thread_list_lock before calling exit(2)), and a leader that skipped this
   * clear left that lock owned by a thread that no longer runs — every
   * surviving thread then waits on it forever (M86). */
  if (!t)
    return;
  u64 ctid = task_child_tid_clear(t);
  /* Trace every thread death into a ring the watchdog dumps. Printing here
   * instead would change the timing enough to hide the very race this is
   * meant to catch (a few stores do not). */
  if (!ctid) {
    thread_exit_trace(t->id, 0, 0, 0);
    return;
  }
  task_set_child_tid_clear(t, 0);
  /* The write goes into the dying thread's own address space, which is still
   * the live one (threads share the process mm).
   *
   * It MUST land. musl hands this word to the kernel precisely because it is
   * its thread-list lock: the dying thread holds that lock across its final
   * unmap and exit, and this clear is what releases it. A write that silently
   * fails — the page was reclaimed or swapped out while the thread was on its
   * way out — leaves the lock owned by a thread that no longer exists, and
   * every other thread then waits on it forever. So: write, verify, and if the
   * page was not there, fault it in and write through the physical mapping. */
  extern u64 paging_user_frame(u64 pml4_phys, u64 vaddr);
  extern u64 vmm_direct_map_base(void);
  int zero = 0;
  (void)syscall_copyout((void *)(usize)ctid, &zero, sizeof(int));
  u64 page = ctid & ~(u64)(PAGE_SIZE - 1);
  u64 fr = paging_user_frame(t->pml4_phys, page);
  if (!fr) {
    (void)vmm_handle_page_fault(ctid, PF_USER | PF_WRITE);
    fr = paging_user_frame(t->pml4_phys, page);
  }
  if (fr) {
    volatile u32 *word =
        (volatile u32 *)(usize)(fr + vmm_direct_map_base() +
                                (ctid & (PAGE_SIZE - 1)));
    if (*word != 0)
      *word = 0;
    thread_exit_trace(t->id, ctid, fr, *word);
  } else {
    thread_exit_trace(t->id, ctid, 0, 0xffffffffu);
  }
  scheduler_futex_wake_addr(ctid, 1);
}

/* M86: hand a dying thread's CPU time to its thread-group leader, so the
 * process totals keep counting work whose thread has finished. The leader keeps
 * its own counters (it IS the group's anchor), and a group whose leader has
 * already gone simply drops the remainder — there is nothing left to report it
 * to, since the parent's RUSAGE_CHILDREN was settled when the leader was
 * reaped. */
static void acct_release_to_group(struct task *t) {
  usize idx = task_index(t);
  usize tgid = g_task_tgid[idx];
  if (!tgid || tgid == t->id)
    return;
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->id == tgid && T(i)->state != TASK_UNUSED) {
      g_task_gone_utime_ns[i] += g_task_utime_ns[idx];
      g_task_gone_stime_ns[i] += g_task_stime_ns[idx];
      return;
    }
  }
}

void scheduler_exit_current(int exit_code) {
  /* A vfork parent waiting on this task must be released before teardown. */
  scheduler_vfork_release();
  if (current_task == 0) {
    panic("scheduler_exit_current without current task");
  }
  {
    /* Close this thread's last CPU interval before handing its time over. */
    sched_acct_leave_kernel();
    u64 acct_flags = interrupts_save();
    acct_release_to_group(current_task);
    interrupts_restore(acct_flags);
  }

  /* M80: PTRACE_O_TRACEEXIT — hold the task here, still whole, so its tracer
   * can read the exit status and its final registers. No-op unless a tracer
   * asked for the event. */
  ptrace_exit_stop(current_task, exit_code);

  /* /bin/init dying takes the whole run with it: nothing else spawns the
   * remaining tests, so the log simply stops. A default-action signal death is
   * otherwise silent (arch signal delivery logs only fault signals), which made
   * this look like a hang rather than a kill. Always announce it. */
  if (g_init_pid && current_task->id == g_init_pid) {
    console_write("INIT-EXIT: init exited, pid=");
    console_write_dec(current_task->id);
    console_write(" code=0x");
    console_write_hex64((u64)(unsigned)exit_code);
    console_write("\n");
  }

  /* Commit to exiting with this code: block scheduler_deliver_pending_signals
   * from later re-routing us through the SIGKILL path and clobbering exit_code
   * (M29 stress-exit-code — a sibling's SIGKILL-back races our exit_group(0)). */
  g_task_exiting[task_index(current_task)] = 1;

  /* M86: a leader parked in scheduler_exit_thread is waiting for exactly this
   * — the last of its threads leaving. Wake it now that we are committed to
   * exiting, so it does not sit out the backstop timeout. */
  {
    usize my_tgid = g_task_tgid[task_index(current_task)];
    if (my_tgid && my_tgid != current_task->id) {
      u64 wflags = interrupts_save();
      for (usize i = 0; i < g_task_hwm; i++) {
        if (T(i)->id == my_tgid && g_task_parked_leader[i]) {
          interrupts_restore(wflags);
          scheduler_wake_all(&g_task_parked_leader[i]);
          wflags = interrupts_save();
          break;
        }
      }
      interrupts_restore(wflags);
    }
  }

  /* If process leader exits, or crash/signal exit, terminate siblings first */
  if (g_task_tgid[task_index(current_task)] == current_task->id ||
      (exit_code & TASK_EXIT_SIGNALED) || (exit_code < 0)) {
    u64 flags = interrupts_save();
    terminate_group_siblings(current_task);
    interrupts_restore(flags);
  }

  /* If session leader exits, detach ctty and send SIGHUP/SIGCONT to fg pgrp */
  if (current_task->session_id == current_task->id) {
    usize idx = task_index(current_task);
    int type = g_task_ctty_type[idx];
    int ctty_idx = g_task_ctty_index[idx];
    g_task_ctty_type[idx] = 0;
    g_task_ctty_index[idx] = 0;

    usize fg = 0;
    if (type == 1) {
      fg = console.fg_pgrp;
    } else if (type == 2) {
      fg = serial_tty_fg_pgrp(ctty_idx);
    } else if (type == 3) {
      fg = pty_fg_pgrp(ctty_idx);
    }

    /* fg <= 1 is the boot/kernel group, which the console carries as its
     * placeholder foreground group (console.fg_pgrp starts at 1) rather than as
     * a real session's group. Hanging it up would signal system tasks that never
     * joined the session — /bin/init among them, which dies silently on SIGHUP
     * and takes the rest of the run with it. */
    if (fg > 1) {
      scheduler_kill_process_group(fg, SIGHUP);
      scheduler_kill_process_group(fg, SIGCONT);
    }
  }

  /* Drop any futex waiter this task still owns (e.g. it was signalled out of
   * FUTEX_WAIT and terminated before self-detaching) so a later wake on the
   * same key cannot target this slot once it is recycled. */
  scheduler_futex_cleanup_task(current_task->id);
  scheduler_timer_cleanup_task(current_task->id); /* M74: free POSIX timers */

  /* M29: a clone()d thread shares fd_table / pml4 / user_image with its
   * parent process. Skip the per-task fd/cred/mm teardown, let the reaper
   * (scheduler_reap_dead_threads) free what's exclusively this thread's
   * (kernel stack + slot). CLONE_CHILD_CLEARTID also writes 0 + futex_wakes
   * so a pthread_join sleeper unblocks. */
  if (task_is_thread(current_task)) {
    thread_release_ctid(current_task);
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
    task_lease_clear(current_task, __func__);
    current_task->state = TASK_DEAD;
    scheduler_yield();
    panic("dead thread resumed");
  }

  /* fd-table teardown, with interrupts enabled so writebacks can sleep.
   * Drop our reference first: with CLONE_FILES threads still alive the table
   * must survive — only the LAST user closes the handles and frees the
   * arrays. The old unconditional close+kfree here freed the shared table
   * out from under surviving sibling threads (use-after-free). */
  {
    int *fl = 0;
    usize cap = 0;
    struct vfs_handle **tbl = fdtable_release(current_task, &fl, &cap);
    if (tbl) {
      for (usize i = 0; i < cap; i++) {
        if (tbl[i])
          vfs_close_handle(tbl[i], (int)task_tgid(current_task));
      }
      kfree(tbl);
      kfree(fl);
    }
  }

  interrupts_disable();

  reparent_children_and_signal_orphans(current_task);

  /* Free credentials */
  if (current_task->cred) {
    cred_free(current_task->cred);
    current_task->cred = 0;
  }

  aio_task_cleanup(current_task);
  rseq_task_cleanup(current_task); /* rseq(2) area dies with the task */
  /* SysV SEM_UNDO: give back every adjustment this task still owes, so a
   * process that dies holding a semaphore does not wedge the set forever. */
  sysv_sem_task_cleanup(current_task->id);
  ptrace_task_cleanup(current_task); /* drop tracer/tracee links */
  g_task_affinity[task_index(current_task)] = 0;
  scheduler_root_clear(current_task); /* release a chroot's root reference */

  current_task->exit_code = exit_code;
  /* F-tier T4 prerequisite: claim stack_released BEFORE publishing DEAD.
   * x86 TSO orders the stores, so any CPU that observes state == DEAD also
   * observes stack_released == 0 — its waitpid path will spin on the
   * stack_released flag until arch_context_switch publishes 1 after the
   * RSP swap below. See struct task::stack_released in sched.h. */
  task_lease_clear(current_task, __func__);
  current_task->state = TASK_DEAD;
  g_have_proc_zombies = 1; /* arm the orphan sweep in scheduler_yield */

  post_sigchld_to_parent(current_task->parent_id, 0);

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

/* True if a caught signal (one with an installed user handler) that must
 * interrupt a blocking wait is deliverable for the current task. Only caught
 * signals interrupt the wait — this is the POSIX EINTR contract and matches the
 * historical behaviour for default-action signals (a waitpid-blocked task was
 * never interrupted by an uncaught signal, so we don't start now). SIGCHLD is
 * deliberately excluded: it is the child-reaping signal, and interrupting
 * waitpid on it would spuriously abort a parent waiting on one child while
 * another child changes state (breaks dropbear/wget child reaping). */
static int wait_interrupted_by_signal(void) {
  u64 pending = __atomic_load_n(&current_task->pending_signals,
                                __ATOMIC_ACQUIRE) & ~current_task->blocked_signals;
  if (pending == 0)
    return 0;
  for (int sig = 1; sig < NSIG; sig++) {
    if (sig == SIGCHLD)
      continue;
    if (!(pending & (1ULL << (sig - 1))))
      continue;
    sighandler_t h = current_task->sigactions[sig - 1].sa_handler;
    if (h != SIG_IGN && h != SIG_DFL)
      return 1;
  }
  return 0;
}

int scheduler_waitpid(usize pid, int *status, int options) {
  if (current_task == 0)
    return -ECHILD;

  /* POSIX pid decoding: -1 = any child (GNU Make's reap_children() relies on
   * waitpid(-1, ..., WNOHANG)); 0 = any child in the CALLER's process group;
   * < -1 = any child of process group |pid|; > 0 = that specific child. */
  int wait_any = (pid == (usize)-1);
  usize wait_pgid = 0;
  if (pid == 0)
    wait_pgid = current_task->process_group_id;
  else if ((isize)pid < -1)
    wait_pgid = (usize)(-(isize)pid);
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
      task_lease_clear(current_task, __func__);
      __atomic_store_n(&current_task->state, TASK_BLOCKED, __ATOMIC_SEQ_CST);
      __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }

    int has_children = 0;
    for (usize i = 0; i < g_task_hwm; i++) {
      /* ptrace(2): a tracer waits for its tracee's stops even when it is not
       * the tracee's parent — without that it could attach but never drive it.
       * Exit status still belongs to the real parent (see the reap path
       * below), so a tracer only ever consumes stop reports. */
      int is_child = (T(i)->parent_id == current_task->id);
      int is_tracee = !is_child &&
                      ptrace_tracer_pid(T(i)) == current_task->id;
      if (T(i)->state != TASK_UNUSED && (is_child || is_tracee)) {
        int match;
        if (wait_any)
          match = 1;
        else if (wait_pgid)
          match = (T(i)->process_group_id == wait_pgid);
        else
          match = (T(i)->id == pid);
        if (match) {
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
          if (is_child &&
              __atomic_compare_exchange_n(&T(i)->state, &_expected,
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
            /* Accumulate child times in parent */
            usize p_idx = task_index(current_task);
            g_task_cutime_ns[p_idx] +=
                g_task_utime_ns[i] + g_task_cutime_ns[i] + g_task_gone_utime_ns[i];
            g_task_cstime_ns[p_idx] +=
                g_task_stime_ns[i] + g_task_cstime_ns[i] + g_task_gone_stime_ns[i];
            if (T(i)->user_image) {
              user_image_free(T(i)->user_image);
              T(i)->user_image = 0;
            }
            /* M29: keep the address space alive if a CLONE_VM sibling
             * still uses it. The dying thread's reaper handles its own
             * kernel stack; the mm/vmas/fd-table belong to the surviving
             * sibling. mm_release_user serializes the last-user decision
             * against the thread reaper on another CPU. */
            u64 last_pml4 = mm_release_user(T(i));
            if (last_pml4) {
              T(i)->pml4_phys = last_pml4; /* see scheduler_reap_dead_threads */
              user_address_space_cleanup(T(i));
              paging_free_address_space(last_pml4);
              T(i)->pml4_phys = 0;
            } else {
              T(i)->vma_list = 0;
            }
            /* Normally a no-op: the child's exit path already closed and
             * detached its table. A live table here means we reaped a
             * CLONE_FILES sibling that was the last user of the shared
             * table — close + free it on its behalf (IRQs back on: the
             * writeback in vfs_close_handle can sleep). */
            {
              int *fl = 0;
              usize cap = 0;
              struct vfs_handle **tbl = fdtable_release(T(i), &fl, &cap);
              if (tbl) {
                interrupts_enable();
                for (usize j = 0; j < cap; j++) {
                  if (tbl[j])
                    vfs_close_handle(tbl[j], (int)task_tgid(T(i)));
                }
                kfree(tbl);
                kfree(fl);
                interrupts_disable();
              }
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
              if (code & TASK_EXIT_SIGNALED) {
                /* Task was killed by a signal */
                *status = code & 0x7F;
              } else {
                /* Normal exit — exit(128..158) must NOT read as a signal
                 * death, so the encoding is a flag bit, not a value range. */
                *status = (code & 0xFF) << 8;
              }
            }
             return child_id;
          } else if (((options & (B1NIX_WUNTRACED | B1NIX_WCONTINUED)) ||
                      ptrace_tracer_pid(T(i)) == current_task->id) &&
                     (T(i)->state == TASK_STOPPED ||
                      T(i)->continued_report_pending)) {
            int child_id = T(i)->id;
            /* A tracer always sees its tracee's ptrace stops, whether or not it
             * passed WUNTRACED — that flag governs ordinary job-control stops.
             * Crashpad's handler waits with __WALL alone, and without this it
             * would sleep forever next to a tracee that is already stopped. */
            int is_tracer = (ptrace_tracer_pid(T(i)) == current_task->id);
            if (((options & B1NIX_WUNTRACED) || is_tracer) &&
                T(i)->state == TASK_STOPPED && T(i)->stop_report_pending) {
              if (status)
                {
                  /* A ptrace event stop reports (SIGTRAP | event << 8) in the
                   * high half of the status word, which is how a tracer tells
                   * a fork/exec event from a plain signal stop. */
                  int ss = T(i)->last_stop_signal & 0xFF;
                  int ev = ptrace_stop_event(T(i));
                  *status = (((ss | (ev << 8)) & 0xFFFF) << 8) | 0x7F;
                }
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

    /* Woken — possibly by a signal rather than a child state change. If a
     * signal that must interrupt the wait is now deliverable, abort with
     * -ERESTARTSYS; the syscall tail converts it to -EINTR (or restarts the
     * call when the handler used SA_RESTART). Without this, a caught signal
     * (e.g. SIGALRM) wakes the task but the loop just re-blocks forever. */
    if (wait_interrupted_by_signal()) {
      current_task->state = TASK_RUNNING;
      interrupts_enable();
      return -ERESTARTSYS;
    }

    interrupts_enable();
  }
}

int scheduler_waitid(idtype_t idtype, usize id, siginfo_t *infop, int options) {
  if (current_task == 0)
    return -ECHILD;
  if (!infop)
    return -EFAULT;
  if (idtype != P_ALL && idtype != P_PID && idtype != P_PGID)
    return -EINVAL;
  if (idtype == P_PID && id == 0)
    return -EINVAL;

  const int allowed = B1NIX_WNOHANG | B1NIX_WSTOPPED | B1NIX_WEXITED |
                      B1NIX_WCONTINUED | B1NIX_WNOWAIT;
  if ((options & ~allowed) != 0 ||
      (options & (B1NIX_WSTOPPED | B1NIX_WEXITED |
                  B1NIX_WCONTINUED)) == 0)
    return -EINVAL;
  if (idtype == P_PGID && id == 0)
    id = current_task->process_group_id;

  int may_block = ((options & B1NIX_WNOHANG) == 0);

  while (1) {
    interrupts_disable();
    if (may_block) {
      if (current_task->state == TASK_BLOCKED || current_task->state == TASK_SLEEPING) {
        current_task->state = TASK_RUNNING;
      }
      task_lease_clear(current_task, __func__);
      __atomic_store_n(&current_task->state, TASK_BLOCKED, __ATOMIC_SEQ_CST);
      __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }

    int has_children = 0;
    for (usize i = 0; i < g_task_hwm; i++) {
      struct task *child = T(i);
      if (child->state != TASK_UNUSED && child->parent_id == current_task->id) {
        int match = 0;
        if (idtype == P_ALL) {
          match = 1;
        } else if (idtype == P_PID) {
          match = (child->id == id);
        } else if (idtype == P_PGID) {
          match = (child->process_group_id == id);
        }

        if (match) {
          has_children = 1;

          /* Check exited child */
          if ((options & B1NIX_WEXITED) && child->state == TASK_DEAD) {
            siginfo_t info;
            memset(&info, 0, sizeof(info));
            info.si_signo = SIGCHLD;
            info.si_pid = (int)child->id;
            info.si_uid = child->cred ? (int)child->cred->uid : 0;
            int code = child->exit_code;
            if (code & TASK_EXIT_SIGNALED) {
              info.si_code = 2; /* CLD_KILLED */
              info.si_status = code & 0x7F;
            } else {
              info.si_code = 1; /* CLD_EXITED */
              info.si_status = code & 0xFF;
            }

            if (!(options & B1NIX_WNOWAIT)) {
              /* Perform actual reap */
              enum task_state _expected = TASK_DEAD;
              if (__atomic_compare_exchange_n(&child->state, &_expected,
                                              TASK_REAPING, 0,
                                              __ATOMIC_ACQUIRE,
                                              __ATOMIC_RELAXED)) {
                while (!__atomic_load_n(&child->stack_released, __ATOMIC_ACQUIRE)) {
                  __asm__ volatile("pause");
                  tlb_shootdown_poll();
                }

                usize p_idx = task_index(current_task);
                g_task_cutime_ns[p_idx] += g_task_utime_ns[i] +
                                           g_task_cutime_ns[i] +
                                           g_task_gone_utime_ns[i];
                g_task_cstime_ns[p_idx] += g_task_stime_ns[i] +
                                           g_task_cstime_ns[i] +
                                           g_task_gone_stime_ns[i];

                if (child->user_image) {
                  user_image_free(child->user_image);
                  child->user_image = 0;
                }
                u64 last_pml4 = mm_release_user(child);
                if (last_pml4) {
                  child->pml4_phys = last_pml4;
                  user_address_space_cleanup(child);
                  paging_free_address_space(last_pml4);
                  child->pml4_phys = 0;
                } else {
                  child->vma_list = 0;
                }
                {
                  int *fl = 0;
                  usize cap = 0;
                  struct vfs_handle **tbl = fdtable_release(child, &fl, &cap);
                  if (tbl) {
                    interrupts_enable();
                    for (usize j = 0; j < cap; j++) {
                      if (tbl[j])
                        vfs_close_handle(tbl[j], (int)task_tgid(child));
                    }
                    kfree(tbl);
                    kfree(fl);
                    interrupts_disable();
                  }
                }
                if (child->name && strcmp(child->name, "boot") != 0) {
                  kfree((void *)child->name);
                  child->name = 0;
                }
                kfree(child->stack);
                if (may_block)
                  scheduler_waitpid_fast_return();
                interrupts_enable();
                free_task_slot(child);

                if (infop) {
                  if (syscall_copyout(infop, &info, sizeof(siginfo_t)) < 0) {
                    return -EFAULT;
                  }
                }
                return 0;
              } else {
                continue;
              }
            } else {
              if (may_block)
                scheduler_waitpid_fast_return();
              interrupts_enable();
              if (infop) {
                if (syscall_copyout(infop, &info, sizeof(siginfo_t)) < 0) {
                  return -EFAULT;
                }
              }
              return 0;
            }
          }

          /* Check stopped child */
          if ((options & B1NIX_WSTOPPED) && child->state == TASK_STOPPED && child->stop_report_pending) {
            siginfo_t info;
            memset(&info, 0, sizeof(info));
            info.si_signo = SIGCHLD;
            info.si_code = 5; /* CLD_STOPPED */
            info.si_pid = (int)child->id;
            info.si_uid = child->cred ? (int)child->cred->uid : 0;
            info.si_status = child->last_stop_signal;

            if (!(options & B1NIX_WNOWAIT)) {
              child->stop_report_pending = 0;
            }
            if (may_block)
              scheduler_waitpid_fast_return();
            interrupts_enable();
            if (infop) {
              if (syscall_copyout(infop, &info, sizeof(siginfo_t)) < 0) {
                return -EFAULT;
              }
            }
            return 0;
          }

          /* Check continued child */
          if ((options & B1NIX_WCONTINUED) && child->continued_report_pending) {
            siginfo_t info;
            memset(&info, 0, sizeof(info));
            info.si_signo = SIGCHLD;
            info.si_code = 6; /* CLD_CONTINUED */
            info.si_pid = (int)child->id;
            info.si_uid = child->cred ? (int)child->cred->uid : 0;
            info.si_status = SIGCONT;

            if (!(options & B1NIX_WNOWAIT)) {
              child->continued_report_pending = 0;
            }
            if (may_block)
              scheduler_waitpid_fast_return();
            interrupts_enable();
            if (infop) {
              if (syscall_copyout(infop, &info, sizeof(siginfo_t)) < 0) {
                return -EFAULT;
              }
            }
            return 0;
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
      if (infop) {
        siginfo_t info;
        memset(&info, 0, sizeof(info));
        if (syscall_copyout(infop, &info, sizeof(siginfo_t)) < 0) {
          return -EFAULT;
        }
      }
      return 0;
    }

    scheduler_yield();

    if (wait_interrupted_by_signal()) {
      current_task->state = TASK_RUNNING;
      interrupts_enable();
      return -ERESTARTSYS;
    }
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

/* Out-of-file lease clear (pipe/futex block loops): same bookkeeping as the
 * in-file task_lease_clear so the dump can name their sites too. */
void scheduler_lease_clear_here(const char *site) {
  if (current_task)
    task_lease_clear(current_task, site);
}

void scheduler_dump_tasks(void) {
  extern void futex_dump_waiters(void);
  futex_dump_waiters();
  {
    /* Which fcntl a spinning thread repeats. See vfs_fcntl_dump_counts. */
    extern void vfs_fcntl_dump_counts(void);
    vfs_fcntl_dump_counts();
  }
  scheduler_dump_thread_exits();
  console_write("tick=");
  console_write_dec(scheduler_ticks);
  console_write("\nID\tSTATE\tNAME\n");
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
      console_write("\tppid=0x");
      console_write_hex64(T(i)->parent_id);
      console_write(" chan=0x");
      console_write_hex64((u64)(usize)T(i)->wait_chan);
      /* What that address actually is. A wait channel is whatever object the
       * sleeper picked — an inode, a pipe, a socket — and a bare heap address
       * names none of them. The heap knows the block it falls in and whether
       * it is still live, which is the difference between "waiting on
       * something" and "waiting on something that was freed". */
      if (T(i)->wait_chan)
        kheap_describe((u64)(usize)T(i)->wait_chan, " chan->");
      console_write(" rel=");
      console_write_dec(T(i)->stack_released);
      console_write(" ap=");
      console_write_dec(T(i)->ap_runnable);
      console_write(" steal=");
      console_write_dec(T(i)->stealable);
      /* Saved kernel RSP + fd_lock state: a task stuck holding its fd_lock
       * (the lock every other CPU then spins on) is only identifiable here. */
      console_write(" rsp=0x");
      console_write_hex64(T(i)->context.rsp);
      console_write(" kstack=0x");
      console_write_hex64((u64)(usize)T(i)->stack);
      console_write(" fdlock=");
      console_write_dec((u64)(unsigned)T(i)->fd_lock);
      /* A SLEEPING task that never wakes is only explicable with its wake
       * deadline next to the current tick; nice/pass name a stride-scheduling
       * imbalance the same way. */
      console_write(" wake_tick=");
      console_write_dec(T(i)->wake_tick);
      console_write(" nice=");
      console_write_dec((u64)(u32)g_task_nice[i]);
      console_write(" pass=");
      console_write_dec(g_task_pass[i]);
      console_write(" fdheld_by=");
      console_write_dec(g_task_fdlock_holder[i]);
      console_write("@");
      console_write(g_task_fdlock_site[i] ? g_task_fdlock_site[i] : "-");
      console_write(" lease_site=");
      console_write(g_task_lease_site[i] ? g_task_lease_site[i] : "-");
      /* User RIP at last timer-tick preemption: names the user function a
       * wedged (spinning) thread group is stuck in. 0 for kernel tasks. */
      /* Peak resident pages, as already sampled — printed, never recomputed:
       * the walk behind it is expensive and this dump runs on a timer. It is
       * what says whether a run that fills memory is filling it inside one
       * process or spreading it across many. */
      /* What is waiting to be delivered to this task.
       *
       * A thread that will not die during exit_group is either not being
       * signalled at all or is being signalled and not acting on it, and those
       * need opposite fixes. The pending mask says which. */
      if (T(i)->pending_signals) {
        console_write(" pending=0x");
        console_write_hex64(T(i)->pending_signals);
      }
      /* Thread group and exit state.
       *
       * A leader in exit_group waits for every sibling that is not already
       * exiting, and the kill it sends goes to exactly the same set. When a
       * process will not die, the question is which thread is in that set and
       * why it is still there — and neither the group nor the flag was
       * printed, so the set could not be reconstructed from the dump. */
      console_write(" tgid=");
      console_write_dec(g_task_tgid[i]);
      if (g_task_exiting[i])
        console_write(" exiting");
      if (g_task_parked_leader[i])
        console_write(" parked-leader");
      console_write(" rsskb=");
      console_write_dec(g_task_maxrss_pages[i] * 4);
      console_write(" sys=");
      console_write_dec(g_task_syscall[i]);
      /* The last user address seen for this thread.
       *
       * g_task_user_rip is only written when the timer tick catches a thread
       * running in userspace, so a thread that blocked in a syscall has none —
       * and a dump of a stalled process is almost entirely such threads, which
       * is why most entries read user_rip=0 and said nothing. The syscall entry
       * records where the call was made from; fall back to that, and mark which
       * of the two is being shown. */
      {
        u64 rip = g_task_user_rip[i];
        const char *what = " user_rip=0x";
        if (!rip) {
          rip = task_syscall_entry_rip(T(i));
          what = " call_rip=0x";
        }
        console_write(what);
        console_write_hex64(rip);
        g_task_user_rip_shown = rip;
      }
      /* Which mapping that address is in, and how far into it. A bare address
       * says nothing when everything is loaded above 0x700000000000 — the same
       * number could be the loader, the C library or a 30 MB renderer. */
      if (g_task_user_rip_shown) {
        for (struct vm_area *v = T(i)->vma_list; v; v = v->next) {
          if (g_task_user_rip_shown >= v->start && g_task_user_rip_shown < v->end) {
            console_write(" in=");
            console_write(v->node && v->node->name[0] ? v->node->name
                                                      : "<anonymous>");
            console_write("+0x");
            console_write_hex64(g_task_user_rip_shown - v->start);
            break;
          }
        }
      }
      /* The call chain a blocked thread came in on, followed rather than
       * scanned.
       *
       * The scan below prints every stack slot that looks like code, which
       * over-reports by design. This walks the frame pointer chain from the
       * rbp userspace entered the kernel with, so each line is a real return
       * address — and for a thread parked in a syscall that is the only exact
       * account of what it is waiting for. Read through the task's own page
       * tables: this is not the running address space. */
      {
        u64 erip = task_syscall_entry_rip(T(i));
        u64 fp = task_syscall_entry_rbp(T(i));

        if (erip && fp && T(i)->pml4_phys && T(i)->vma_list) {
          console_write("\n    called from 0x");
          console_write_hex64(erip);
          sched_name_user_addr(T(i), erip);
          console_write(" chain:");
          for (unsigned depth = 0; depth < 10 && fp; depth++) {
            u64 page = fp & ~(u64)(PAGE_SIZE - 1);
            u64 pf = paging_user_frame(T(i)->pml4_phys, page);

            if (!pf || (fp & 7) || (fp & (PAGE_SIZE - 1)) > PAGE_SIZE - 16)
              break;
            const volatile u64 *f =
                (const volatile u64 *)(usize)(pf + vmm_direct_map_base() +
                                              (fp & (PAGE_SIZE - 1)));
            u64 next = f[0];
            u64 ret = f[1];

            if (!ret)
              break;
            console_write(" 0x");
            console_write_hex64(ret);
            sched_name_user_addr(T(i), ret);
            if (next <= fp) /* frames grow upward; anything else is broken */
              break;
            fp = next;
          }
        }
      }

      /* The USER stack of a wedged thread, when asked for.
       *
       * Everything above says where a thread is in the kernel; none of it says
       * what the program was doing when it stopped. Chromium parks every thread
       * on a mutex and creates no window, and from the kernel side the two
       * cases — "waiting for work" and "waiting for a lock nobody will release"
       * — are identical. The user stack tells them apart.
       *
       * The stack is read through the task's own page tables (it is not the
       * running address space) and every value that lands inside one of its
       * mappings is printed with that mapping's name and offset. Without frame
       * pointers this over-reports — a dead slot looks like a return address —
       * but a plausible-but-dead frame is easy to discount, and the module
       * names alone say which layer is stuck. */
      if (bootinfo_has_flag("b1nix.user-stack") && T(i)->saved_user_rsp &&
          T(i)->pml4_phys && T(i)->vma_list) {
        u64 sp = T(i)->saved_user_rsp;
        unsigned shown = 0;

        console_write("\n    user stack (rsp=0x");
        console_write_hex64(sp);
        console_write("):");
        for (unsigned k = 0; k < 1024 && shown < 24; k++) {
          u64 addr = sp + (u64)k * 8;
          u64 page = addr & ~(u64)(PAGE_SIZE - 1);
          u64 frame = paging_user_frame(T(i)->pml4_phys, page);
          u64 value;

          /* Skip, do not stop: a thread's stack is lazily backed, so the
           * first slots above rsp are often not mapped yet and stopping there
           * printed nothing at all. */
          if (!frame)
            continue;
          value = *(volatile u64 *)(usize)(frame + vmm_direct_map_base() +
                                           (addr & (PAGE_SIZE - 1)));
          for (struct vm_area *v = T(i)->vma_list; v; v = v->next) {
            /* In a mapping AND on an executable page. The VMA's prot cannot
             * be trusted for this — the loader maps a library's text and the
             * flag is not mirrored back, so every VMA reads non-executable —
             * but the page table knows. Without this the walk printed pointers
             * to data structures at tiny offsets and called them frames. */
            /* A file mapping, and a page that is actually there.
             *
             * Executable code always comes from a file; anonymous memory is
             * heap, thread stacks and JIT, and printing values that land there
             * filled the walk with pointers to data. The page table cannot help
             * — this kernel does not mark data pages non-executable — so the
             * mapping's origin is the best available test. */
            if (value >= v->start && value < v->end && v->node &&
                v->node->name[0] &&
                ({ u64 _pte = paging_user_pte(T(i)->pml4_phys,
                                              value & ~(u64)(PAGE_SIZE - 1));
                   (_pte & VMM_PRESENT) != 0; })) {
              console_write("\n      0x");
              console_write_hex64(value);
              console_write(" ");
              console_write(v->node && v->node->name[0] ? v->node->name
                                                        : "<anonymous>");
              console_write("+0x");
              console_write_hex64(value - v->start);
              console_write((v->prot & PROT_EXEC) ? " x" : " -");
              shown++;
              break;
            }
          }
        }
        console_write("\n");
      }

      /* The kernel return addresses still on the task's stack. A blocked task
       * left them there, so they name the path that put it to sleep — which is
       * the one thing state and wait channel together still do not say. */
      if (T(i) != current_task && T(i)->context.rsp) {
        extern char __kernel_text_start[], __kernel_text_end[];
        u64 lo = (u64)(usize)__kernel_text_start;
        u64 hi = (u64)(usize)__kernel_text_end;
        const u64 *stack = (const u64 *)(usize)T(i)->context.rsp;
        unsigned printed = 0;

        console_write(" callers=");
        for (unsigned w = 0; w < 96 && printed < 5; w++) {
          u64 v = stack[w];

          if (v >= lo && v < hi) {
            console_write("0x");
            console_write_hex64(v);
            console_write(",");
            printed++;
          }
        }
      }
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

/* Publish a grown fd table to current_task AND every CLONE_FILES sibling that
 * aliases the old allocation, then free the old arrays. The old code updated
 * only current_task, leaving every sibling thread's fd_table pointing at the
 * freed old arrays (use-after-free on their next fd op). Caller holds the
 * shared fd_lock; the swap additionally takes g_mm_release_lock so an exiting
 * sibling's fdtable_release cannot snapshot the old pointer mid-swap and read
 * it after the kfree. */
static void fdtable_publish_grown(struct vfs_handle **new_table, int *new_flags,
                                  usize new_capacity) {
  struct vfs_handle **old_table = current_task->fd_table;
  int *old_flags = current_task->fd_flags;
  u64 mmflags;
  spin_lock_irqsave(&g_mm_release_lock, &mmflags);
  for (usize ti = 0; ti < g_task_hwm; ti++) {
    struct task *o = T(ti);
    if (o == current_task || o->state == TASK_UNUSED)
      continue;
    if (o->fd_table == old_table) {
      o->fd_table = new_table;
      o->fd_flags = new_flags;
      o->fd_capacity = new_capacity;
    }
  }
  current_task->fd_table = new_table;
  current_task->fd_flags = new_flags;
  current_task->fd_capacity = new_capacity;
  spin_unlock_irqrestore(&g_mm_release_lock, mmflags);
  kfree(old_table);
  kfree(old_flags);
}

int scheduler_fd_alloc(struct vfs_handle *handle) {
  if (!current_task || !handle)
    return -1;

  fd_lock_acquire();

  usize idx = task_index(current_task);
  usize nofile_limit = g_task_rlimits[idx][RLIMIT_NOFILE].rlim_cur;

  for (usize i = 0; i < current_task->fd_capacity; i++) {
    if (i >= nofile_limit) {
      fd_lock_release();
      return -EMFILE;
    }
    if (current_task->fd_table[i] == 0) {
      current_task->fd_table[i] = handle;
      current_task->fd_flags[i] = 0;
      fd_lock_release();
      return (int)i;
    }
  }

  if (current_task->fd_capacity >= SCHED_MAX_FD_LIMIT || current_task->fd_capacity >= nofile_limit) {
    fd_lock_release();
    return -EMFILE;
  }

  usize new_capacity = current_task->fd_capacity * 2;
  if (new_capacity > SCHED_MAX_FD_LIMIT)
    new_capacity = SCHED_MAX_FD_LIMIT;
  if (new_capacity > nofile_limit)
    new_capacity = nofile_limit;
  if (new_capacity <= current_task->fd_capacity) {
    fd_lock_release();
    return -EMFILE;
  }

  struct vfs_handle **new_table = kzalloc(new_capacity * sizeof(struct vfs_handle *));
  if (!new_table) {
    fd_lock_release();
    return -ENOMEM;
  }
  int *new_flags = kzalloc(new_capacity * sizeof(int));
  if (!new_flags) {
    kfree(new_table);
    fd_lock_release();
    return -ENOMEM;
  }

  memcpy(new_table, current_task->fd_table, current_task->fd_capacity * sizeof(struct vfs_handle *));
  memcpy(new_flags, current_task->fd_flags, current_task->fd_capacity * sizeof(int));

  int allocated_fd = (int)current_task->fd_capacity;
  fdtable_publish_grown(new_table, new_flags, new_capacity);
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

struct vfs_handle *scheduler_fd_get_retain(int fd) {
  if (!current_task || fd < 0 || (usize)fd >= current_task->fd_capacity)
    return 0;
  fd_lock_acquire();
  struct vfs_handle *h = current_task->fd_table[fd];
  if (h)
    vfs_handle_retain(h);
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

    fdtable_publish_grown(new_table, new_flags, new_capacity);
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

/* Atomically fetch AND clear an fd slot. vfs_close needs the two as one
 * critical section: a peek-then-clear pair lets two threads closing the same
 * fd both retrieve the handle and both run the release path (double release /
 * touch-after-free on the recycled handle). Exactly one caller gets non-NULL. */
struct vfs_handle *scheduler_fd_take(int fd) {
  if (!current_task || fd < 0 || (usize)fd >= current_task->fd_capacity)
    return 0;
  fd_lock_acquire();
  struct vfs_handle *h = current_task->fd_table[fd];
  current_task->fd_table[fd] = 0;
  current_task->fd_flags[fd] = 0;
  fd_lock_release();
  return h;
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

/* Post SIGCHLD to a parent task when one of its children stops or continues, so
 * a parent that installed a SIGCHLD handler is notified of job-control state
 * transitions (POSIX delivers SIGCHLD on child stop/continue, not just exit).
 * The bit is just set pending; it is delivered to the handler on the parent's
 * next return to userspace. Caller must already hold IRQs disabled. SIGCHLD is
 * excluded from waitpid interruption, so this never spuriously aborts a parent
 * blocked reaping another child. */
static void post_sigchld_to_parent(usize parent_id, int job_control_event) {
  if (parent_id == 0)
    return;
  for (usize p = 0; p < g_task_hwm; p++) {
    if (T(p)->id == parent_id && T(p)->state != TASK_UNUSED) {
      if (job_control_event &&
          (T(p)->sigactions[SIGCHLD - 1].sa_flags & SA_NOCLDSTOP))
        return;
      __atomic_fetch_or(&T(p)->pending_signals, (1ULL << (SIGCHLD - 1)),
                        __ATOMIC_RELEASE);
      return;
    }
  }
}

/* OOM reclaim: SIGKILL the current userspace task — the one whose allocation
 * the PMM cannot satisfy — so its address space is torn down and its memory
 * reclaimed, instead of returning ENOMEM forever (which a JS engine or any
 * allocator turns into a console-flooding retry storm). The kill is async: the
 * task dies at its next return-to-user, then exit teardown frees its frames.
 * Never targets kernel threads (pml4_phys==0) or init (pid 1). Returns 1 if a
 * victim was signalled. */
int scheduler_oom_kill_current(void) {
  struct task *v = current_task;
  if (!v || v->pml4_phys == 0 || v->id == 1)
    return 0;
  if (v->pending_signals & (1ULL << (SIGKILL - 1)))
    return 0; /* already condemned */
  console_write("[OOM-KILL] killing '");
  console_write(v->name ? v->name : "?");
  console_write("' pid ");
  console_write_dec(v->id);
  console_write(" to reclaim memory\n");
  scheduler_kill(v->id, SIGKILL);
  return 1;
}

int scheduler_kill(usize task_id, int sig) {
  if (sig < 0 || sig > NSIG_MAX)
    return -EINVAL;
  /* M74: kill(2)/raise(3) of an RT signal QUEUES one instance with a zero
   * payload (si_code SI_USER == 0), rather than coalescing into a single bit. */
  if (SIG_IS_RT(sig)) {
    union sigval v;
    v.sival_ptr = 0;
    return scheduler_sigqueue(task_id, sig, v, 0 /* SI_USER */);
  }

  u64 flags = interrupts_save();
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->id == task_id && T(i)->state != TASK_UNUSED &&
        T(i)->state != TASK_DEAD && T(i)->state != TASK_REAPING) {
      /* Signal 0 is the POSIX existence/permission probe: the target was just
       * found alive, so report success without posting any signal. */
      if (sig == 0) {
        interrupts_restore(flags);
        return 0;
      }
      /* SIGKILL and SIGSTOP cannot be blocked/ignored. Atomic RMW: post-BKL
       * the target task (or another killer) may concurrently set/clear its own
       * pending bits, so a plain |= would drop a racing update. */
      __atomic_fetch_or(&T(i)->pending_signals, (1ULL << (sig - 1)),
                        __ATOMIC_RELEASE);

      if (T(i) != current_task && !ptrace_is_traced(T(i)) &&
          (sig == SIGSTOP || sig == SIGTSTP ||
           sig == SIGTTIN || sig == SIGTTOU)) {
        __atomic_fetch_and(&T(i)->pending_signals, ~(1ULL << (sig - 1)),
                           __ATOMIC_RELAXED);
        T(i)->last_stop_signal = sig;
        T(i)->stop_report_pending = 1;
        T(i)->state = TASK_STOPPED;
        post_sigchld_to_parent(T(i)->parent_id, 1);
        interrupts_restore(flags);
        scheduler_notify_wait_event(T(i)->parent_id);
        return 0;
      }

      /* Wake blocked task so it can handle signal */
      if (sig == SIGCONT && T(i)->state == TASK_STOPPED) {
        T(i)->continued_report_pending = 1;
        post_sigchld_to_parent(T(i)->parent_id, 1);
        interrupts_restore(flags);
        scheduler_notify_wait_event(T(i)->parent_id);
        flags = interrupts_save();
      }
      /* CAS BLOCKED/STOPPED -> READY so a kill racing the target's exit on
       * another CPU cannot resurrect a DEAD/REAPING task into the runqueue
       * (R3-12, mirrors wake_sleepers). */
      {
        enum task_state expected = TASK_BLOCKED;
        if (__atomic_compare_exchange_n(&T(i)->state, &expected, TASK_READY, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
          sched_rq_enqueue_current(T(i));
        } else {
          expected = TASK_STOPPED;
          if (__atomic_compare_exchange_n(&T(i)->state, &expected, TASK_READY,
                                          0, __ATOMIC_ACQUIRE,
                                          __ATOMIC_RELAXED)) {
            sched_rq_enqueue_current(T(i));
          }
        }
      }
      interrupts_restore(flags);
      return 0;
    }
  }
  interrupts_restore(flags);
  return -ESRCH;
}

/* ── M86: thread-directed signal targeting ────────────────────────────────── */

/* Resolve a task id to its live slot. Caller holds interrupts. */
static struct task *find_live_task(usize id) {
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->id == id && T(i)->state != TASK_UNUSED &&
        T(i)->state != TASK_DEAD && T(i)->state != TASK_REAPING)
      return T(i);
  }
  return 0;
}

static inline usize task_tgid_of(const struct task *t) {
  usize tgid = g_task_tgid[task_index(t)];
  return tgid ? tgid : t->id;
}

/* tkill(2)/tgkill(2): deliver `sig` to exactly one thread, never to whichever
 * sibling happens to have it unblocked. `tgid` of 0 means "don't check" (that
 * is tkill; tgkill passes the caller's expected thread-group id, and a tid that
 * has since been recycled into another process must NOT be signalled — which is
 * the entire reason tgkill exists). */
int scheduler_tkill(usize tgid, usize tid, int sig) {
  if (tid == 0 || (isize)tid < 0 || (isize)tgid < 0)
    return -EINVAL;
  if (sig < 0 || sig > NSIG_MAX)
    return -EINVAL;

  u64 flags = interrupts_save();
  struct task *t = find_live_task(tid);
  usize found_tgid = t ? task_tgid_of(t) : 0;
  interrupts_restore(flags);
  if (!t)
    return -ESRCH;
  if (tgid && found_tgid != tgid)
    return -ESRCH;
  return scheduler_kill(tid, sig);
}

/* Stop/continue a thread-group sibling without the parent SIGCHLD report — the
 * group leader's own scheduler_kill does that once for the whole group. Caller
 * holds interrupts. */
static void group_stop_sibling(struct task *t, int sig) {
  if (sig == SIGCONT) {
    __atomic_fetch_and(&t->pending_signals,
                       ~((1ULL << (SIGSTOP - 1)) | (1ULL << (SIGTSTP - 1)) |
                         (1ULL << (SIGTTIN - 1)) | (1ULL << (SIGTTOU - 1))),
                       __ATOMIC_RELAXED);
    enum task_state expected = TASK_STOPPED;
    if (__atomic_compare_exchange_n(&t->state, &expected, TASK_READY, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
      sched_rq_enqueue_current(t);
    return;
  }
  /* Post the stop and let the sibling park itself on its next return to ring 3.
   * Writing TASK_STOPPED into a task running on another CPU parks it before its
   * context has been saved, and the next picker then resumes it from a
   * half-written frame (the M40 lesson). */
  g_task_stop_quiet[task_index(t)] = 1;
  __atomic_fetch_or(&t->pending_signals, (1ULL << (sig - 1)), __ATOMIC_RELEASE);
  /* A sibling asleep in an interruptible wait would otherwise ignore the group
   * stop until its syscall happened to finish — a thread blocked on a read that
   * never completes would keep the job "running" forever. Wake it so it takes
   * the signal now; the interrupted syscall restarts (SA_RESTART) or returns
   * EINTR through the usual path. Same per-state CAS scheduler_kill uses, so a
   * task that is concurrently dying on another CPU cannot be resurrected. */
  enum task_state expected = TASK_BLOCKED;
  if (__atomic_compare_exchange_n(&t->state, &expected, TASK_READY, 0,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    sched_rq_enqueue_current(t);
}

/* kill(2) with a positive pid targets a PROCESS, not a thread: POSIX lets the
 * kernel pick any thread in the group that does not block the signal, and only
 * if every thread blocks it does the signal stay pending on the group. b1nix
 * used to post to the leader unconditionally, so a signal the leader had
 * blocked (musl's own SIGCANCEL handling, or any thread that runs a handler on
 * behalf of the process) was never delivered even though a sibling was ready to
 * take it. Stop and continue signals act on the whole group, as on Linux. */
int scheduler_kill_thread_group(usize pid, int sig) {
  if (sig < 0 || sig > NSIG_MAX)
    return -EINVAL;

  u64 flags = interrupts_save();
  struct task *leader = find_live_task(pid);
  if (!leader) {
    interrupts_restore(flags);
    return -ESRCH;
  }
  usize tgid = task_tgid_of(leader);
  /* Signalling a non-leader tid keeps the historical thread-directed meaning
   * (b1nix tids and pids share one number space, and in-kernel callers rely on
   * it); only a real thread-group leader gets process-wide semantics. */
  if (tgid != pid || sig == 0) {
    interrupts_restore(flags);
    return scheduler_kill(pid, sig);
  }

  int is_stop = (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN ||
                 sig == SIGTTOU || sig == SIGCONT);
  if (is_stop) {
    for (usize i = 0; i < g_task_hwm; i++) {
      struct task *o = T(i);
      if (o == leader || o->state == TASK_UNUSED || o->state == TASK_DEAD ||
          o->state == TASK_REAPING)
        continue;
      if (task_tgid_of(o) != tgid)
        continue;
      group_stop_sibling(o, sig);
    }
    interrupts_restore(flags);
    return scheduler_kill(pid, sig);
  }

  /* SIGKILL is never blockable and its delivery already tears the group down
   * (terminate_group_siblings), so it goes to the leader. */
  usize target = pid;
  if (sig != SIGKILL) {
    u64 bit = 1ULL << (sig - 1);
    if (leader->blocked_signals & bit) {
      for (usize i = 0; i < g_task_hwm; i++) {
        struct task *o = T(i);
        if (o == leader || o->state == TASK_UNUSED || o->state == TASK_DEAD ||
            o->state == TASK_REAPING)
          continue;
        if (task_tgid_of(o) != tgid)
          continue;
        if (!(o->blocked_signals & bit)) {
          target = o->id;
          break;
        }
      }
      /* Every thread blocks it: leave it pending on the leader, which is where
       * the group's shared pending set lives. */
    }
  }
  interrupts_restore(flags);
  return scheduler_kill(target, sig);
}

/* M74: enqueue one RT-signal instance (with payload) to a task and wake it. The
 * pending bit (signo-1) means "≥1 queued"; delivery dequeues one entry FIFO and
 * clears the bit only when the queue for that signo drains. */
int scheduler_sigqueue(usize task_id, int sig, union sigval value, int si_code) {
  if (!SIG_IS_RT(sig))
    return -EINVAL;
  u64 flags = interrupts_save();
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->id == task_id && T(i)->state != TASK_UNUSED &&
        T(i)->state != TASK_DEAD && T(i)->state != TASK_REAPING) {
      /* g_rt_lock serialises the RT queue + lazy rt_state alloc across CPUs and
       * vs the timer ISR (interrupts are already off here). The enqueue and the
       * pending-bit set are done together under the lock so they cannot
       * interleave with the locked dequeue's "queue empty -> clear bit" — that
       * is what closes the signal-loss race (a send between dequeue and a
       * separate bit-clear used to drop the bit while an entry was queued). */
      spin_lock(&g_rt_lock);
      struct rt_state *rs = rt_state_get(i, 1);
      if (!rs || rs->qcount >= RT_QUEUE_MAX) {
        spin_unlock(&g_rt_lock);
        interrupts_restore(flags);
        return -EAGAIN; /* OOM or RLIMIT_SIGPENDING analogue */
      }
      int slot = (rs->qhead + rs->qcount) % RT_QUEUE_MAX;
      rs->queue[slot].signo = sig;
      rs->queue[slot].si_code = si_code;
      rs->queue[slot].value = value;
      rs->qcount++;
      __atomic_fetch_or(&T(i)->pending_signals, (1ULL << (sig - 1)),
                        __ATOMIC_RELEASE);
      spin_unlock(&g_rt_lock);
      /* Wake a blocked/stopped target so it re-checks pending signals. Mirrors
       * scheduler_kill's CAS so a kill racing the target's exit cannot resurrect
       * a DEAD/REAPING task. Done outside g_rt_lock to avoid nesting it under the
       * runqueue lock. */
      enum task_state expected = TASK_BLOCKED;
      if (__atomic_compare_exchange_n(&T(i)->state, &expected, TASK_READY, 0,
                                      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        sched_rq_enqueue_current(T(i));
      }
      interrupts_restore(flags);
      return 0;
    }
  }
  interrupts_restore(flags);
  return -ESRCH;
}

/* Dequeue the oldest queued entry for `sig` (SIGRTMIN..SIGRTMAX) on the current
 * task, returning its si_code and payload. Returns 1 if one was dequeued (and,
 * via *more, whether the queue for that signo still has entries), 0 if none.
 * Called from the arch signal-delivery path with interrupts off. */
int scheduler_rt_dequeue_current(int sig, int *si_code, union sigval *value,
                                 int *more) {
  if (!current_task)
    return 0;
  int ret = 0;
  spin_lock(&g_rt_lock); /* interrupts already off in the delivery path */
  struct rt_state *rs = g_rt_state[task_index(current_task)];
  if (!rs || rs->qcount == 0) {
    spin_unlock(&g_rt_lock);
    return 0;
  }
  for (int n = 0; n < rs->qcount; n++) {
    int idx = (rs->qhead + n) % RT_QUEUE_MAX;
    if (rs->queue[idx].signo == sig) {
      if (si_code)
        *si_code = rs->queue[idx].si_code;
      if (value)
        *value = rs->queue[idx].value;
      /* Shift the entries before idx forward by one to fill the gap, keeping
       * FIFO order for the remaining signals. */
      for (int k = n; k > 0; k--) {
        int dst = (rs->qhead + k) % RT_QUEUE_MAX;
        int src = (rs->qhead + k - 1) % RT_QUEUE_MAX;
        rs->queue[dst] = rs->queue[src];
      }
      rs->qhead = (rs->qhead + 1) % RT_QUEUE_MAX;
      rs->qcount--;
      int rem = 0;
      for (int m = 0; m < rs->qcount; m++)
        if (rs->queue[(rs->qhead + m) % RT_QUEUE_MAX].signo == sig) {
          rem = 1;
          break;
        }
      if (more)
        *more = rem;
      /* Clear the pending bit HERE, under the same lock that the enqueue's
       * bit-set holds — so a concurrent sigqueue cannot slip an entry in between
       * "queue drained" and "bit cleared" and have its bit wiped (signal loss). */
      if (!rem)
        __atomic_fetch_and(&current_task->pending_signals,
                           ~(1ULL << (sig - 1)), __ATOMIC_RELAXED);
      ret = 1;
      break;
    }
  }
  spin_unlock(&g_rt_lock);
  return ret;
}

/* The current task's RT handler for `sig`, or NULL if none registered. */
struct sigaction *scheduler_rt_action_current(int sig) {
  if (!current_task || !SIG_IS_RT(sig))
    return 0;
  struct rt_state *rs = g_rt_state[task_index(current_task)];
  if (!rs)
    return 0;
  return &rs->action[sig - SIGRTMIN];
}

/* ── M74 POSIX timers ── */

int scheduler_timer_create(int signo, union sigval value) {
  if (!current_task || signo < 1 || signo > NSIG_MAX)
    return -EINVAL;
  u64 flags;
  spin_lock_irqsave(&g_timer_lock, &flags);
  for (int i = 0; i < MAX_POSIX_TIMERS; i++) {
    if (!g_posix_timers[i].used) {
      g_posix_timers[i].used = 1;
      g_posix_timers[i].owner_id = current_task->id;
      g_posix_timers[i].signo = signo;
      g_posix_timers[i].value = value;
      g_posix_timers[i].interval_ticks = 0;
      g_posix_timers[i].next_ticks = 0;
      spin_unlock_irqrestore(&g_timer_lock, flags);
      return i;
    }
  }
  spin_unlock_irqrestore(&g_timer_lock, flags);
  return -EAGAIN;
}

/* Arm (first_ticks > 0) or disarm (0) timer `id`. interval_ticks == 0 makes it
 * one-shot. Pre-touches the owner's RT state so the tick path never allocates in
 * interrupt context. */
int scheduler_timer_settime(int id, u64 first_ticks, u64 interval_ticks,
                            u64 *old_remaining, u64 *old_interval) {
  if (!current_task || id < 0 || id >= MAX_POSIX_TIMERS)
    return -EINVAL;
  /* Pre-allocate the owner's RT state HERE (under g_rt_lock, before g_timer_lock
   * to avoid nesting the two) so the timer ISR never has to allocate while
   * holding g_timer_lock. */
  {
    u64 rf;
    spin_lock_irqsave(&g_rt_lock, &rf);
    rt_state_get(task_index(current_task), 1);
    spin_unlock_irqrestore(&g_rt_lock, rf);
  }
  u64 flags;
  spin_lock_irqsave(&g_timer_lock, &flags);
  struct posix_timer *t = &g_posix_timers[id];
  if (!t->used || t->owner_id != current_task->id) {
    spin_unlock_irqrestore(&g_timer_lock, flags);
    return -EINVAL;
  }
  if (old_interval)
    *old_interval = t->interval_ticks;
  if (old_remaining) {
    u64 now = scheduler_ticks;
    *old_remaining = (t->next_ticks > now) ? (t->next_ticks - now) : 0;
  }
  t->interval_ticks = interval_ticks;
  t->next_ticks = first_ticks ? scheduler_ticks + first_ticks : 0;
  spin_unlock_irqrestore(&g_timer_lock, flags);
  return 0;
}

int scheduler_timer_gettime(int id, u64 *remaining, u64 *interval) {
  if (!current_task || id < 0 || id >= MAX_POSIX_TIMERS)
    return -EINVAL;
  u64 flags;
  spin_lock_irqsave(&g_timer_lock, &flags);
  struct posix_timer *t = &g_posix_timers[id];
  if (!t->used || t->owner_id != current_task->id) {
    spin_unlock_irqrestore(&g_timer_lock, flags);
    return -EINVAL;
  }
  u64 now = scheduler_ticks;
  if (remaining)
    *remaining = (t->next_ticks > now) ? (t->next_ticks - now) : 0;
  if (interval)
    *interval = t->interval_ticks;
  spin_unlock_irqrestore(&g_timer_lock, flags);
  return 0;
}

int scheduler_timer_delete(int id) {
  if (!current_task || id < 0 || id >= MAX_POSIX_TIMERS)
    return -EINVAL;
  u64 flags;
  spin_lock_irqsave(&g_timer_lock, &flags);
  struct posix_timer *t = &g_posix_timers[id];
  if (!t->used || t->owner_id != current_task->id) {
    spin_unlock_irqrestore(&g_timer_lock, flags);
    return -EINVAL;
  }
  t->used = 0;
  t->next_ticks = 0;
  spin_unlock_irqrestore(&g_timer_lock, flags);
  return 0;
}

/* Free all timers owned by a terminating task (called from the exit path). */
void scheduler_timer_cleanup_task(usize task_id) {
  u64 flags;
  spin_lock_irqsave(&g_timer_lock, &flags);
  for (int i = 0; i < MAX_POSIX_TIMERS; i++)
    if (g_posix_timers[i].used && g_posix_timers[i].owner_id == task_id) {
      g_posix_timers[i].used = 0;
      g_posix_timers[i].next_ticks = 0;
    }
  spin_unlock_irqrestore(&g_timer_lock, flags);
}

/* Called from the timer tick (interrupts already off): snapshot every armed
 * timer whose deadline has passed (re-arming periodic ones / disarming one-shots
 * in place) UNDER g_timer_lock, then fire the signals AFTER releasing the lock —
 * so g_timer_lock is never held across scheduler_sigqueue (which takes g_rt_lock
 * and the runqueue lock), keeping the lock-hold short and the ordering simple. */
static void posix_timers_tick(void) {
  struct { usize owner; int signo; union sigval value; } fire[MAX_POSIX_TIMERS];
  int nfire = 0;
  u64 flags;
  spin_lock_irqsave(&g_timer_lock, &flags);
  for (int i = 0; i < MAX_POSIX_TIMERS; i++) {
    struct posix_timer *t = &g_posix_timers[i];
    if (!t->used || t->next_ticks == 0 || t->next_ticks > scheduler_ticks)
      continue;
    fire[nfire].owner = t->owner_id;
    fire[nfire].signo = t->signo;
    fire[nfire].value = t->value;
    nfire++;
    if (t->interval_ticks)
      t->next_ticks = scheduler_ticks + t->interval_ticks;
    else
      t->next_ticks = 0; /* one-shot: disarm */
  }
  spin_unlock_irqrestore(&g_timer_lock, flags);
  for (int i = 0; i < nfire; i++) {
    if (SIG_IS_RT(fire[i].signo))
      scheduler_sigqueue(fire[i].owner, fire[i].signo, fire[i].value,
                         B1NIX_SI_TIMER);
    else
      scheduler_kill(fire[i].owner, fire[i].signo);
  }
}

int scheduler_kill_process_group(usize pgrp, int sig) {
  if (sig < 0 || sig >= NSIG)
    return -EINVAL;
  if (pgrp == 0)
    return -ESRCH;

  int sent = 0;
  u64 flags = interrupts_save();
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->state != TASK_UNUSED && T(i)->state != TASK_DEAD &&
        T(i)->state != TASK_REAPING && T(i)->process_group_id == pgrp) {
      /* Signal 0: existence probe only — the group has a live member, so
       * count it as found without posting anything. */
      if (sig == 0) {
        sent++;
        continue;
      }
      __atomic_fetch_or(&T(i)->pending_signals, (1ULL << (sig - 1)),
                        __ATOMIC_RELEASE);

      if (T(i) != current_task && !ptrace_is_traced(T(i)) &&
          (sig == SIGSTOP || sig == SIGTSTP ||
           sig == SIGTTIN || sig == SIGTTOU)) {
        __atomic_fetch_and(&T(i)->pending_signals, ~(1ULL << (sig - 1)),
                           __ATOMIC_RELAXED);
        T(i)->last_stop_signal = sig;
        T(i)->stop_report_pending = 1;
        T(i)->state = TASK_STOPPED;
        post_sigchld_to_parent(T(i)->parent_id, 1);
        interrupts_restore(flags);
        scheduler_notify_wait_event(T(i)->parent_id);
        flags = interrupts_save();
        sent++;
        continue;
      }

      /* Wake blocked task so it can handle signal */
      if (sig == SIGCONT && T(i)->state == TASK_STOPPED) {
        T(i)->continued_report_pending = 1;
        post_sigchld_to_parent(T(i)->parent_id, 1);
        interrupts_restore(flags);
        scheduler_notify_wait_event(T(i)->parent_id);
        flags = interrupts_save();
      }
      /* CAS BLOCKED/STOPPED -> READY so a kill racing the target's exit on
       * another CPU cannot resurrect a DEAD/REAPING task into the runqueue
       * (R3-12, mirrors wake_sleepers). */
      {
        enum task_state expected = TASK_BLOCKED;
        if (__atomic_compare_exchange_n(&T(i)->state, &expected, TASK_READY, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
          sched_rq_enqueue_current(T(i));
        } else {
          expected = TASK_STOPPED;
          if (__atomic_compare_exchange_n(&T(i)->state, &expected, TASK_READY,
                                          0, __ATOMIC_ACQUIRE,
                                          __ATOMIC_RELAXED)) {
            sched_rq_enqueue_current(T(i));
          }
        }
      }
      sent++;
    }
  }
  interrupts_restore(flags);
  return sent > 0 ? 0 : -ESRCH;
}

/* POSIX kill(-1, sig): signal every process the caller may signal, except
 * the caller itself and init (pid 1). Kernel threads (no user address
 * space) are never targeted. */
/* POSIX 1003.1 kill(2) permission: the sender may signal a target if its real
 * or effective uid matches the target's real or saved uid, or it holds
 * CAP_KILL. SIGCONT to a process in the same session is always allowed.
 *
 * This was missing entirely: sys_kill dispatched straight into the delivery
 * routines, so any uid could signal any process, PID 1 included, and CAP_KILL
 * was defined but never read. The check lives on the syscall path only —
 * kernel-internal signalling (orphaned-group SIGHUP, parent-death signals)
 * must not be subject to the current task's credentials. */
static int signal_permitted(const struct task *target, int sig) {
  const struct cred *me = scheduler_get_current_cred();
  const struct cred *them = target ? target->cred : 0;
  if (!me || !them)
    return 1; /* kernel context, or a task with no credentials */
  if (cred_has_cap(me, CAP_KILL))
    return 1;
  if (me->euid == them->uid || me->euid == them->suid ||
      me->uid == them->uid || me->uid == them->suid)
    return 1;
  if (sig == SIGCONT && current_task &&
      target->session_id == current_task->session_id)
    return 1;
  return 0;
}

/* Signal one process from a syscall, enforcing the check above. */
int scheduler_kill_thread_group_user(usize pid, int sig) {
  u64 flags = interrupts_save();
  struct task *t = find_live_task(pid);
  int allowed = t ? signal_permitted(t, sig) : -1;
  interrupts_restore(flags);
  if (allowed < 0)
    return -ESRCH;
  if (!allowed)
    return -EPERM;
  return scheduler_kill_thread_group(pid, sig);
}

/* Signal a process group or (pgrp == 0) every process the caller may signal,
 * skipping members it may not. POSIX: success if the signal reached at least
 * one; EPERM if members existed but none were permitted. */
static int kill_many_user(usize pgrp, int sig, int all) {
  if (sig < 0 || sig > NSIG_MAX)
    return -EINVAL;
  int found = 0, sent = 0;
  for (usize i = 0; i < g_task_hwm; i++) {
    u64 flags = interrupts_save();
    struct task *t = T(i);
    int eligible = t->state != TASK_UNUSED && t->state != TASK_DEAD &&
                   t->state != TASK_REAPING && !task_is_thread(t) &&
                   t->pml4_phys != 0;
    if (eligible && all)
      eligible = (t != current_task && t->id != 1);
    if (eligible && !all)
      eligible = (t->process_group_id == pgrp);
    usize id = t->id;
    int allowed = eligible ? signal_permitted(t, sig) : 0;
    interrupts_restore(flags);
    if (!eligible)
      continue;
    found++;
    if (!allowed)
      continue;
    if (sig != 0)
      scheduler_kill(id, sig);
    sent++;
  }
  if (!found)
    return -ESRCH;
  if (!sent)
    return -EPERM;
  return 0;
}

int scheduler_kill_process_group_user(usize pgrp, int sig) {
  if (pgrp == 0)
    return -ESRCH;
  return kill_many_user(pgrp, sig, 0);
}

int scheduler_kill_all_user(int sig) { return kill_many_user(0, sig, 1); }

int scheduler_kill_all(int sig) {
  if (sig < 0 || sig >= NSIG)
    return -EINVAL;
  int sent = 0;
  for (usize i = 0; i < g_task_hwm; i++) {
    struct task *t = T(i);
    if (t->state == TASK_UNUSED || t->state == TASK_DEAD ||
        t->state == TASK_REAPING)
      continue;
    if (t == current_task)
      continue;
    if (t->id == 1)
      continue;
    if (t->pml4_phys == 0)
      continue; /* kernel thread */
    if (task_is_thread(t))
      continue; /* signal processes, not their CLONE_VM threads */
    if (sig != 0)
      scheduler_kill(t->id, sig);
    sent++;
  }
  return sent > 0 ? 0 : -ESRCH;
}

int scheduler_sigaction(int sig, const struct sigaction *act,
                        struct sigaction *old) {
  if (!current_task)
    return -1;
  /* M74: RT signals (SIGRTMIN..SIGRTMAX) keep their sigactions in the lazy
   * side-table rather than the in-struct sigactions[31] array. */
  if (SIG_IS_RT(sig)) {
    usize idx = task_index(current_task);
    int slot = sig - SIGRTMIN;
    /* g_rt_lock guards the lazy rt_state alloc (two CPUs racing rt_state_get
     * would otherwise leak one allocation) and the action read/write vs the
     * delivery path / a concurrent sigqueue. */
    u64 flags;
    spin_lock_irqsave(&g_rt_lock, &flags);
    struct rt_state *rs = rt_state_get(idx, act != 0);
    if (old)
      *old = rs ? rs->action[slot] : (struct sigaction){0};
    if (act) {
      if (!rs) {
        spin_unlock_irqrestore(&g_rt_lock, flags);
        return -1; /* allocation failed */
      }
      rs->action[slot] = *act;
    }
    spin_unlock_irqrestore(&g_rt_lock, flags);
    return 0;
  }
  if (sig < 1 || sig >= NSIG)
    return -1;
  /* SIGKILL and SIGSTOP cannot be caught/ignored */
  if (sig == SIGKILL || sig == SIGSTOP)
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
  /* getpid() is the process, not the thread.
   *
   * Returning the caller's own id gave every thread a different "pid", which
   * is what made tgkill fail: the sender passes getpid() as the thread group
   * and the kernel compares it against the target's real group, so from any
   * thread but the first they never matched and the signal came back ESRCH —
   * seen as a stream of "tgkill: No such process" from chromium's crash
   * handler. Programs also cache this value and check it after forking, and
   * name their IPC endpoints with it.
   *
   * gettid() remains the thread's own id; the two are separate calls. */
  usize tgid = task_tgid(current_task);

  return tgid ? tgid : current_task->id;
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

/* Link a mapping into the list, in address order.
 *
 * The list used to be in reverse-insertion order, and everything that walks it
 * paid for that: the free-area search could not trust a single pass and went
 * quadratic (see below), and every fault's lookup had to walk to the end. One
 * insertion point keeps the order, and then a walk can stop at the first
 * mapping that starts past the address it wants.
 */
/* Serialises every edit to a VMA list.
 *
 * The list is shared by all threads of an address space, and it was edited
 * with no lock at all: two threads calling mmap at once each walked to the
 * same insertion point and each wrote its own node's next pointer there, which
 * either loses a mapping or joins the list into a ring. A ring is not a subtle
 * failure — the next walk of it never ends, so a thread disappeared inside
 * mmap with its state left RUNNING and its switch count frozen, unkillable,
 * while the process it belonged to waited on it forever. chromium, which maps
 * from many threads at once, hit this every run.
 *
 * One lock for all address spaces: mmap is not a hot path, the sections here
 * are a few pointer writes long, and a per-space lock needs a home that
 * struct task deliberately does not have. */
static spinlock_t g_vma_lock = SPINLOCK_INIT;

void vma_list_lock(u64 *flags) { spin_lock_irqsave(&g_vma_lock, flags); }
void vma_list_unlock(u64 flags) { spin_unlock_irqrestore(&g_vma_lock, flags); }

void vma_insert(struct task *t, struct vm_area *vma) {
  if (!t || !vma)
    return;

  u64 vflags;
  vma_list_lock(&vflags);
  struct vm_area **link = &t->vma_list;

  while (*link && (*link)->start < vma->start)
    link = &(*link)->next;
  vma->next = *link;
  /* Publish the node only after its own next pointer is set, so a walker
   * that is not holding the lock never sees a half-linked entry. */
  __atomic_store_n(link, vma, __ATOMIC_RELEASE);
  vma_list_unlock(vflags);
  /* Only a new head has to be published to the threads sharing this address
   * space — they hold the head pointer, not the list. Publishing on every
   * insertion would walk the whole task table per mmap. */
  if (link == &t->vma_list)
    scheduler_sync_vma_head(t->pml4_phys, t->vma_list);
}

/* The mapping covering an address, or NULL.
 *
 * Sorted order means the walk stops at the first mapping that starts past the
 * address, and a one-entry cache turns the common case — repeated faults in
 * the mapping just used — into a pointer comparison. chromium runs with
 * thousands of mappings, and every fault used to walk them.
 */
static struct vm_area *g_vma_cache[MAX_TASKS];

struct vm_area *vma_lookup(struct task *t, u64 addr) {
  if (!t)
    return 0;

  usize slot = task_index(t);
  struct vm_area *hit = g_vma_cache[slot];

  /* The cached mapping is only trusted while it is still in this task's list;
   * a freed one would be a use-after-free, so the cache is cleared by every
   * path that unlinks (vma_cache_forget). */
  if (hit && addr >= hit->start && addr < hit->end)
    return hit;

  /* Deliberately NOT under the list lock.
   *
   * Taking it here looked right — this walk does race the mutators — and it
   * wedged the machine: the lock is held with interrupts off, this runs in the
   * page-fault path, and a process with thousands of mappings walks a long
   * list. Every other CPU spun behind a fault, and the guest went silent.
   *
   * What makes the lockless read safe enough is that a node is published only
   * after its own next pointer is set (see vma_insert/vma_split), so a walker
   * never lands mid-splice. The remaining exposure is a mapping freed while
   * this walk holds it — pre-existing, and not something a spinlock in the
   * fault path can pay for. */
  for (struct vm_area *v = t->vma_list; v && v->start <= addr; v = v->next) {
    if (addr < v->end) {
      g_vma_cache[slot] = v;
      return v;
    }
  }
  return 0;
}

void vma_cache_forget(struct task *t) {
  if (t)
    g_vma_cache[task_index(t)] = 0;
}

u64 vm_find_free_area(struct task *t, usize length) {
#ifdef __x86_64__
  /* The bootstrap identity map occupies the low 4 GiB with huge pages.
   * File-backed lazy mappings need ordinary leaf PTEs, so start above it. */
  u64 start = 0x100000000ULL;
  u64 end = 0x7FFFFFFFFFFFULL;
#else
  u64 start = 0x40000000ULL;
  /* 32-bit: anonymous mmap must stay below the kernel split at 0x80000000 so it
   * never aliases the direct map [0x80000000, 0xC0000000). The user stack VMA
   * (just under 0x80000000) further bounds the search. */
  u64 end = 0x80000000ULL;
#endif

  /*
   * First fit over a list kept in address order: walk the mappings once and
   * take the first gap that is big enough.
   *
   * This used to be quadratic, and deliberately so — the list was in
   * reverse-insertion order, where a single pass proves nothing (a mapping
   * placed early and linked late is invisible ahead of the candidate), and
   * accepting a candidate too early handed out an address already in use. What
   * that looked like from userspace was a compositor crashing inside malloc()
   * on metadata that had been given to someone else. With vma_insert keeping
   * the order, one pass is sound: every mapping that could overlap has already
   * been seen when the walk passes it.
   */
  u64 candidate = start;
  u64 vflags;

  /* Under the list lock: this walk is the one that hung when the list was a
   * ring, and it must not run while another thread is relinking it. */
  /* No lock across this walk either: it is the longest one in the kernel (a
   * browser has thousands of mappings) and it runs with mmap's own mutex
   * already held, so the mutators cannot be relinking underneath it. Holding
   * a spinlock with interrupts off for that long stalls every other CPU. */
  (void)vflags;
  /* Bounded, so a list that is somehow still circular reports itself instead
   * of hanging the caller in the kernel where nothing can kill it. No process
   * legitimately holds anywhere near this many mappings. */
  unsigned steps = 0;
  for (struct vm_area *vma = t->vma_list; vma; vma = vma->next) {
    if (++steps > 1000000u) {
      console_write("vma: walk did not terminate — list is circular, pid ");
      console_write_dec(t->id);
      console_write("\n");
      return (u64)-1;
    }
    if (vma->end <= candidate)
      continue; /* entirely below the candidate */
    if (vma->start >= candidate + length)
      break;    /* the gap before this mapping is big enough */
    candidate = vma->end;
    if (candidate + length > end || candidate + length < candidate)
      return (u64)-1;
  }
  if (candidate + length > end || candidate + length < candidate)
    return (u64)-1;
  return candidate;
}

struct vm_area *vma_split(struct task *t, struct vm_area *vma, u64 addr) {
  (void)t;
  struct vm_area *new_vma = kzalloc(sizeof(struct vm_area));
  if (!new_vma)
    return 0;

  u64 old_start = vma->start;
  memcpy(new_vma, vma, sizeof(struct vm_area));
  new_vma->start = addr;
  new_vma->offset += (isize)(addr - old_start);
  vma->end = addr;
  if (new_vma->node) {
    vfs_node_get(new_vma->node);
    if (new_vma->node->inode && new_vma->node->inode->mmap_open_cb)
      new_vma->node->inode->mmap_open_cb(new_vma->node);
    if (new_vma->node->inode && new_vma->node->inode->mmap_range_open_cb)
      new_vma->node->inode->mmap_range_open_cb(
          new_vma->node, (u64)new_vma->offset,
          (usize)(new_vma->end - new_vma->start));
  }

  /* Link under the list lock, and only after the new node is fully built:
   * a splice done in the open is how two concurrent edits lose a mapping or
   * close the list into a ring. */
  u64 svflags;
  vma_list_lock(&svflags);
  new_vma->next = vma->next;
  __atomic_store_n(&vma->next, new_vma, __ATOMIC_RELEASE);
  vma_list_unlock(svflags);

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

    /* Now vma is entirely within [start, end].
     *
     * Only the unlink is under the lock — vma_split takes it itself, and the
     * teardown below can call into the VFS, neither of which may run with
     * interrupts disabled. */
    {
      u64 dvflags;
      vma_list_lock(&dvflags);
      __atomic_store_n(curr, vma->next, __ATOMIC_RELEASE);
      vma_list_unlock(dvflags);
    }
    /* The lookup cache may be pointing at what is about to be freed. */
    vma_cache_forget(task);
    if (vma->node) {
      if (vma->node->inode && vma->node->inode->mmap_close_cb)
        vma->node->inode->mmap_close_cb(vma->node);
      if (vma->node->inode && vma->node->inode->mmap_range_close_cb)
        vma->node->inode->mmap_range_close_cb(
            vma->node, (u64)vma->offset, (usize)(vma->end - vma->start));
      vfs_node_put(vma->node);
    }
    kfree(vma);
  }
}

/* ── Priority ── */

/* The POSIX nice value lives in a side-table and deliberately does NOT map
 * onto task->priority: pick_next_task is a strict highest-priority scan with
 * a -1 floor and a default of 1, so writing "10 - nice" there made any task
 * with nice > 9 permanently unschedulable once preempted (negative priority
 * never beats the -1 floor — the M46 nice() hang), and any nice < 9 would
 * starve every default task. The value round-trips per POSIX; biasing the
 * cooperative scheduler with it is tracked as planned work. */
int scheduler_set_priority(usize pid, int priority) {
  if (priority < -20 || priority > 19)
    return -EINVAL;
  interrupts_disable();
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->state != TASK_UNUSED && T(i)->id == pid) {
      g_task_nice[i] = priority;
      interrupts_enable();
      return 0;
    }
  }
  interrupts_enable();
  return -ESRCH;
}

/* Returns the Linux getpriority(2) kernel encoding 20 - nice (1..40, always
 * positive) so a negative return unambiguously means an errno — a raw nice
 * of e.g. -3 would otherwise collide with -ESRCH. The libc wrapper converts
 * back to the POSIX -20..19 range. */
int scheduler_get_priority(usize pid) {
  interrupts_disable();
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->state != TASK_UNUSED && T(i)->id == pid) {
      int nice_val = g_task_nice[i];
      interrupts_enable();
      return 20 - nice_val;
    }
  }
  interrupts_enable();
  return -ESRCH;
}

/* ── Session / Process Group ── */

isize scheduler_setsid(void) {
  if (!current_task)
    return -ESRCH;
  interrupts_disable();
  /* A process cannot be a process group leader to call setsid */
  if (current_task->process_group_id == current_task->id) {
    interrupts_enable();
    return -EPERM;
  }
  current_task->session_id = current_task->id;
  current_task->process_group_id = current_task->id;
  usize idx = task_index(current_task);
  g_task_ctty_type[idx] = 0;
  g_task_ctty_index[idx] = 0;
  interrupts_enable();
  return (isize)current_task->session_id;
}

void scheduler_get_ctty(int *type, int *index) {
  if (!current_task) {
    *type = 0;
    *index = 0;
    return;
  }
  u64 flags = interrupts_save();
  usize session_id = current_task->session_id;
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->state != TASK_UNUSED && T(i)->id == session_id) {
      *type = g_task_ctty_type[i];
      *index = g_task_ctty_index[i];
      interrupts_restore(flags);
      return;
    }
  }
  *type = 0;
  *index = 0;
  interrupts_restore(flags);
}

void scheduler_set_ctty(struct task *t, int type, int index) {
  if (!t) return;
  u64 flags = interrupts_save();
  usize session_id = t->session_id;
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->state != TASK_UNUSED && T(i)->id == session_id) {
      g_task_ctty_type[i] = type;
      g_task_ctty_index[i] = index;
      interrupts_restore(flags);
      return;
    }
  }
  interrupts_restore(flags);
}

isize scheduler_getsid(usize pid) {
  if (!current_task)
    return -ESRCH;
  if (pid == 0)
    pid = current_task->id;

  interrupts_disable();
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->state != TASK_UNUSED && T(i)->id == pid) {
      isize sid = (isize)T(i)->session_id;
      interrupts_enable();
      return sid;
    }
  }
  interrupts_enable();
  return -ESRCH;
}

usize scheduler_getpgrp(void) {
  if (!current_task)
    return 0;
  return current_task->process_group_id;
}

isize scheduler_getpgid(usize pid) {
  if (!current_task)
    return -ESRCH;
  if (pid == 0)
    return (isize)current_task->process_group_id;
  isize ret = -ESRCH;
  interrupts_disable();
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->state != TASK_UNUSED && T(i)->id == pid) {
      ret = (isize)T(i)->process_group_id;
      break;
    }
  }
  interrupts_enable();
  return ret;
}

/* Called by the exec path once the new image is committed (see setpgid's
 * EACCES rule and g_task_execed above). */
void scheduler_mark_execed_current(void) {
  if (current_task)
    g_task_execed[task_index(current_task)] = 1;
}

int scheduler_setpgrp(usize pid, usize pgrp) {
  if (!current_task)
    return -ESRCH;
  if ((isize)pgrp < 0)
    return -EINVAL;
  if (pid == 0)
    pid = current_task->id;
  if (pgrp == 0)
    pgrp = pid;
  int ret = -ESRCH; /* POSIX: pid is not the caller or a child of the caller */
  interrupts_disable();
  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->state != TASK_UNUSED && T(i)->id == pid) {
      struct task *t = T(i);
      int is_self = (t == current_task);
      if (!is_self && t->parent_id != current_task->id)
        break; /* not the caller and not a child → ESRCH */
      if (t->session_id != current_task->session_id) {
        ret = -EPERM;
        break;
      }
      if (t->id == t->session_id) {
        ret = -EPERM; /* session leader's pgid is immutable */
        break;
      }
      if (!is_self && g_task_execed[task_index(t)]) {
        ret = -EACCES; /* child has already exec'd */
        break;
      }
      if (pgrp != t->id) {
        /* Joining an existing group: it must live in the caller's session. */
        int found = 0;
        for (usize j = 0; j < g_task_hwm; j++) {
          if (T(j)->state != TASK_UNUSED && T(j)->process_group_id == pgrp &&
              T(j)->session_id == current_task->session_id) {
            found = 1;
            break;
          }
        }
        if (!found) {
          ret = -EPERM;
          break;
        }
      }
      t->process_group_id = pgrp;
      ret = 0;
      break;
    }
  }
  interrupts_enable();
  return ret;
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

    /* PID 1 only receives signals it installed a handler for (Linux semantics).
     * Otherwise any stray group signal — a session's SIGHUP, a test's SIGTERM
     * to its group — silently terminates /bin/init and the whole run stops with
     * no output. init is the one process whose death nothing can recover from. */
    if (g_init_pid && current_task->id == g_init_pid &&
        (handler == SIG_DFL || sig == SIGKILL)) {
      __atomic_fetch_and(&current_task->pending_signals,
                         ~(1ULL << (sig - 1)), __ATOMIC_RELAXED);
      continue;
    }

    if (sig == SIGKILL) {
      /* Not while the task holds a driver's critical section — see g_task_kcrit.
       * The signal stays pending and is acted on once the section is left. */
      if (task_in_kcrit(current_task))
        return;
      current_task->exit_code = TASK_EXIT_SIGNALED | SIGKILL;
      terminate_group_siblings(current_task);
      /* Reparent children + signal newly-orphaned stopped pgrps and drop our
       * futex waiters — this path used to skip all of that (M46-3). It runs
       * inside the scheduler so it must NOT do the yield-based fd teardown that
       * scheduler_exit_current performs; the zombie reaper handles the rest. */
      reparent_children_and_signal_orphans(current_task);
      thread_release_ctid(current_task);
      scheduler_futex_cleanup_task(current_task->id);
  scheduler_timer_cleanup_task(current_task->id); /* M74: free POSIX timers */
      ptrace_task_cleanup(current_task); /* M80: never leave a stale tracee link */
      task_lease_clear(current_task, __func__);
      current_task->state = TASK_DEAD;
      g_have_proc_zombies = 1;
      post_sigchld_to_parent(current_task->parent_id, 0);
      scheduler_notify_wait_event(current_task->parent_id);
      return;
    }

    /* M80: a traced task must not be acted on from inside the scheduler for any
     * signal but SIGKILL — its tracer gets first refusal, and that decision
     * happens at the next return to ring 3, where ptrace_signal_stop has a
     * complete register frame. This sits ABOVE the SIG_IGN case on purpose: a
     * tracee stops for an ignored signal too, exactly as it does on Linux, so
     * a debugger can see signals the process itself discards. */
    if (ptrace_is_traced(current_task))
      continue;

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
        current_task->exit_code = TASK_EXIT_SIGNALED | sig;
        terminate_group_siblings(current_task);
        /* Reparent + orphan-pgrp signalling + futex cleanup, as for SIGKILL
         * above (M46-3). No yield-based teardown — we are inside the scheduler. */
        reparent_children_and_signal_orphans(current_task);
        thread_release_ctid(current_task);
        scheduler_futex_cleanup_task(current_task->id);
  scheduler_timer_cleanup_task(current_task->id); /* M74: free POSIX timers */
        ptrace_task_cleanup(current_task); /* M80: never leave a stale tracee link */
        task_lease_clear(current_task, __func__);
        current_task->state = TASK_DEAD;
        g_have_proc_zombies = 1;
        post_sigchld_to_parent(current_task->parent_id, 0);
        scheduler_notify_wait_event(current_task->parent_id);
        return;
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
        /* A traced task must stop through ptrace instead, on its next return
         * to ring 3 — that is the only point where its register frame exists
         * for the tracer to read, and stopping it here (possibly mid-exit,
         * with no frame) leaves it parked in a state nothing resumes. Leave
         * the signal pending; the return-to-user path picks it up. */
        if (ptrace_is_traced(current_task))
          continue;
        current_task->state = TASK_STOPPED;
        current_task->last_stop_signal = sig;
        /* M86: a group stop reports to the parent ONCE, from the thread-group
         * leader. Siblings that were pulled into the same stop park silently —
         * otherwise waitpid(WUNTRACED) reports the job stopped once per
         * thread. */
        if (g_task_stop_quiet[task_index(current_task)]) {
          g_task_stop_quiet[task_index(current_task)] = 0;
        } else {
          current_task->stop_report_pending = 1;
          scheduler_notify_wait_event(current_task->parent_id);
        }
        __atomic_fetch_and(&current_task->pending_signals,
                           ~(1ULL << (sig - 1)), __ATOMIC_RELAXED);
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

/* Any deliverable signal at all, including the ones with no handler.
 *
 * scheduler_signal_pending() deliberately answers only for signals a handler
 * will run, because that is what an interruptible wait needs to decide whether
 * userspace has something to do. A sleep cut short by SIGKILL has no handler
 * to run and still ended early, and the caller has to report that. */
int scheduler_signal_pending_any(void) {
  if (!current_task)
    return 0;
  return (__atomic_load_n(&current_task->pending_signals, __ATOMIC_ACQUIRE) &
          ~current_task->blocked_signals) != 0;
}

int scheduler_signal_pending(void) {
  if (!current_task)
    return 0;
  u64 pending = __atomic_load_n(&current_task->pending_signals,
                                __ATOMIC_ACQUIRE) & ~current_task->blocked_signals;
  if (pending == 0)
    return 0;
  for (int i = 1; i < NSIG; i++) {
    if (!(pending & (1ULL << (i - 1))))
      continue;
    sighandler_t h = current_task->sigactions[i - 1].sa_handler;
    if (h != SIG_IGN && h != SIG_DFL)
      return 1;
  }
  return 0;
}

/* M56 signalfd: return the bits of `mask` that are currently pending on the
 * calling task. Used by signalfd reads/polls to learn which masked signals are
 * waiting. The caller is expected to keep those signals blocked (so the normal
 * handler-delivery path leaves them pending) — exactly the POSIX contract. */
u64 scheduler_peek_pending_signals(u64 mask) {
  if (!current_task)
    return 0;
  u64 pending = __atomic_load_n(&current_task->pending_signals, __ATOMIC_ACQUIRE);
  return pending & mask;
}

/* M56 signalfd: atomically clear one pending signal bit on the calling task
 * (consume it on read). Returns 1 if the bit was set and is now cleared, 0
 * otherwise. */
int scheduler_consume_pending_signal(int sig) {
  if (!current_task || sig < 1 || sig >= NSIG)
    return 0;
  u64 bit = 1ULL << (sig - 1);
  u64 prev = __atomic_fetch_and(&current_task->pending_signals, ~bit,
                                __ATOMIC_ACQ_REL);
  return (prev & bit) ? 1 : 0;
}
