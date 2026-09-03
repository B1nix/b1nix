#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/ipi.h>
#include <b1nix/lapic.h>
#include <b1nix/mm.h>
#include <b1nix/cgroup.h>
#include <b1nix/namespace.h>
#include <b1nix/panic.h>
#include <b1nix/posix.h>
#include <b1nix/runqueue.h>
#include <b1nix/ptrace.h>
#include <b1nix/rseq.h>
#include <b1nix/sysv_ipc.h>
#include <b1nix/kmsg.h>
#include <b1nix/ktime.h>
#include <b1nix/sched.h>
#include <b1nix/klog.h>
#include <b1nix/syscall.h>
#include <b1nix/uidgid.h>
#include <b1nix/user.h>
#include <b1nix/vfs.h>
#include <b1nix/serial_tty.h>
#include <stdio.h>

/* Drain cross-CPU TLB shootdowns while spin-waiting with IRQs disabled (these
 * stack_released hand-off spins run inside scheduler_yield, IRQs off). Without
 * it a core parked here can't ACK a shootdown IPI and the initiator times out
 * (observed: `tlb_shootdown stalled pending=1` panic under -smp4 parallel
 * builds). Defined in kernel/arch/x86_64/tlb.c; fast no-op when nothing pending. */
extern void tlb_shootdown_poll(void);
#ifdef __x86_64__
#include <b1nix/arch_x86_64.h>
#elif defined(__aarch64__)
#include <b1nix/arch_aarch64.h>
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
/* Side tables are indexed by task_index(), and the per-CPU idle tasks are NOT
 * in the task table -- see scheduler_setup_ap_idle for why putting them there
 * was tried and reverted. task_index() used to answer 0 for anything it could
 * not find in a chunk, so every idle task aliased slot 0, the boot task, in
 * every one of these arrays at once: two CPUs sharing one task's FPU area,
 * stride pass, nice, affinity and syscall ring. Give them slots of their own
 * above the table instead. */
#define TASK_SLOTS        (MAX_TASKS + MAX_CPUS)
/* The kernel stack is a kmalloc'd block in the shared kheap, so an overflow
 * silently corrupts the adjacent heap block (e.g. a vfs_node) instead of
 * faulting. b1nix runs the busybox coreutils builtins in kernel mode on this
 * stack, and some have large on-stack buffers (uniq_main alone is ~12 KB:
 * buf[8192] + lines[512]); together with the ~6.6 KB syscall dispatch frame
 * and the VFS/ext4 call chain they exceed 16 KB. SMP/T8 preemption adds
 * enough nested scheduler/signal/syscall frames that 32 KB still leaves too
 * little headroom for the M12/M14 stress paths. */
/* aarch64 needs more headroom than x86_64's tuned 64 KB: SAVE_REGS/
 * RESTORE_REGS plus this build's unoptimized (-O0-ish, -mgeneral-regs-only)
 * codegen produce noticeably bigger per-frame stack usage for the same VFS/
 * ext4/ELF-load call chains, and openrc-init's boot sequence was observed
 * driving a task to ~64 KB of usage on a single syscall (real overflow into
 * adjacent kheap memory, not just a tight fit) before ever calling fork(). */
/* Written at the lowest address of every kernel stack and verified on each
 * context switch. A kernel stack is an ordinary heap block, so an overflow
 * silently corrupts whatever object sits below it and only surfaces later,
 * somewhere unrelated — this turns that into an immediate, named panic. */
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
static void *g_task_xsave[TASK_SLOTS];

/* Is this task inside its switch-OUT window?
 *
 * Set just before arch_context_switch and cleared by whichever pick next
 * claims the task. Between those two points the task has already stopped being
 * any CPU's cur_task (scheduler_yield publishes the incoming task first, on
 * purpose) while arch_context_switch is still writing its callee-saved state
 * into ->context and has not yet published the stack lease. Nothing else can
 * tell that state apart from a task whose lease is genuinely stale, and the
 * difference decides whether another CPU may load ->context and run on that
 * stack. See sched_handoff_recover.
 *
 * A side table, not a field: adding to struct task is its own hazard here (see
 * the M29 note on g_task_is_thread and friends), and the idle tasks have slots
 * above MAX_TASKS which TASK_SLOTS already covers. */
static volatile int g_task_switching_out[TASK_SLOTS];

/* The task this CPU was running just before the one it is switching to. Only
 * meaningful between scheduler_yield's cur_task publish and the SP load inside
 * arch_context_switch; see the note at its assignment. */
static struct task *volatile g_cpu_prev_task[MAX_CPUS];

static void sched_set_prev_task(struct task *t) {
  struct percpu *pcpu = get_percpu();

  if (pcpu && pcpu->cpu_id < MAX_CPUS)
    g_cpu_prev_task[pcpu->cpu_id] = t;
}

struct task *sched_prev_task_this_cpu(void) {
  struct percpu *pcpu = get_percpu();

  if (!pcpu || pcpu->cpu_id >= MAX_CPUS)
    return 0;
  return g_cpu_prev_task[pcpu->cpu_id];
}

/* The last calls each thread made.
 *
 * A parked thread's register state says only that it is inside futex; what it
 * did on the way in is the part that can be compared against the same program
 * traced on a working kernel. A tracer in the guest cannot supply that (ptrace
 * has no group-stop, so strace dies on PTRACE_LISTEN) and console tracing
 * changes the timing it is meant to measure. Sixteen numbers per thread costs
 * two stores per system call and 128 KB, and the watchdog prints them beside
 * the thread that stopped. */
#define SYSRING_DEPTH 64
static u16 g_sysring[TASK_SLOTS][SYSRING_DEPTH];
static u8 g_sysring_pos[TASK_SLOTS];
static void *g_task_xsave_raw[TASK_SLOTS];



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
static _Atomic usize g_task_hwm = 0; /* one past highest slot ever used */

/* Tick at which a context switch last actually happened; the stall detector
 * in scheduler_on_timer_tick measures against it. */
static u64 g_last_switch_tick;

/* What pick_next_task actually decides. Three tasks sitting READY, passing
 * every gate, never scheduled, while context switches happen every tick, is
 * only explicable if the picker keeps choosing something else -- and no dump so
 * far could say what it chose. */
static u64 g_pick_calls, g_pick_rq, g_pick_scan, g_pick_idle, g_pick_null;
static u64 g_scan_seen, g_scan_rej_lease, g_scan_rej_running, g_scan_rej_cas;
static usize g_scan_best_id = (usize)-1;
static usize g_last_pick_id = (usize)-1;

/* M29: per-task thread metadata kept in parallel arrays (NOT in struct task
 * — see comment in sched.h). Indexed by the slot in g_task_chunks computed
 * via task_index(). */
static int  g_task_is_thread[TASK_SLOTS];
static u64  g_task_tls_base[TASK_SLOTS];

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
static u64  g_task_child_tid_clear[TASK_SLOTS];
static u64  g_task_saved_sigmask[TASK_SLOTS];
static int  g_task_has_saved_sigmask[TASK_SLOTS];
/* sigaltstack side-table (per-task alternate signal stack). Kept here, NOT in
 * struct task, to preserve the M29 paging invariant. ss_size == 0 means no alt
 * stack is registered. */
static u64  g_task_altstack_sp[TASK_SLOTS];
static u64  g_task_altstack_size[TASK_SLOTS];

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
static struct rt_state *g_rt_state[TASK_SLOTS];
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

static u64  g_task_alarm_ticks[TASK_SLOTS];
/* The signal set a task is parked in sigtimedwait() for, or 0.
 *
 * A side table, not a struct task field: adding fields to that struct has a
 * history of moving something the LAPIC page tables care about (see M29). */
static u64  g_task_sigwait_set[TASK_SLOTS];
/* setitimer(ITIMER_REAL) repeat period; 0 = one-shot (plain alarm(2)). */
static u64  g_task_alarm_interval_ticks[TASK_SLOTS];
/* Set once the task has successfully execve()d. POSIX setpgid: changing a
 * CHILD's process group after it exec'd is EACCES. Deliberately NOT copied
 * by fork — the fresh child has not exec'd yet. */
static int  g_task_execed[TASK_SLOTS];
/* POSIX nice value (-20..19, 0 default) — see scheduler_set_priority for why
 * this is NOT task->priority. Inherited across fork. */
static int  g_task_nice[TASK_SLOTS];

/* Stride for one nice value: how far a task's pass advances each time it gives
 * the CPU up. The scheduler then always picks the smallest pass among equal
 * priorities, so a task with a smaller stride comes up more often — the whole
 * of what nice means here.
 *
 * Tickets are 20 - nice, so nice -20 buys 40 tickets and nice 19 buys one; the
 * stride is the reciprocal scaled by 1000. Out-of-range values clamp rather
 * than divide by zero or go negative.
 *
 * Extracted from schedule() so the contract can be tested directly. From ring 3
 * it can only be inferred from how often each of several spinning processes got
 * to run, which is a statistical argument about a loaded machine — the M46
 * userspace test makes it, and it cannot say whether a particular nice value
 * produced the stride it should. */
int sched_stride_for_nice(int nice) {
  if (nice < -20) nice = -20;
  if (nice > 19) nice = 19;

  int tickets = 20 - nice;

  return 1000 / tickets;
}

static struct rlimit g_task_rlimits[TASK_SLOTS][16];
static usize g_task_tgid[TASK_SLOTS];
/* How far into exit a task has got.
 *
 * A task observed READY with its exiting flag set is looping somewhere in the
 * teardown, and the teardown is long: fd tables that write back, address-space
 * release, reparenting, the leader's wait for its threads. A single number per
 * task, printed beside it in the watchdog dump, turns "stuck in exit" into a
 * line of code. */
static u8 g_task_exit_stage[TASK_SLOTS];
#define EXIT_STAGE(n) do { \
    if (current_task) g_task_exit_stage[task_index(current_task)] = (n); \
  } while (0)

/* Set once a task has committed to scheduler_exit_current() with a chosen exit
 * code. A sibling that processes its own SIGKILL re-posts SIGKILL to the whole
 * thread group (terminate_group_siblings); without this guard the already-exiting
 * leader would process that pending SIGKILL in scheduler_deliver_pending_signals
 * and overwrite its exit_group(0) code with SIGNALED|SIGKILL, so waitpid reports
 * a spurious signalled death (M29 stress-exit-code). */
static u8    g_task_exiting[TASK_SLOTS];

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
static u8    g_task_kcrit[TASK_SLOTS];

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
static int   g_task_ctty_type[TASK_SLOTS];
static int   g_task_ctty_index[TASK_SLOTS];
/* M86: per-thread CPU accounting, in NANOSECONDS. The counters are advanced by
 * acct_flush() from the four boundaries where a CPU changes what it is running
 * or which mode it runs in (ring-3 entry/exit and context switch), so they are
 * exact TSC deltas rather than the 100 Hz sample the tick charger produced —
 * a thread that always blocks before the tick used to account as 0. Per-task
 * counters are only ever written by the CPU currently running that task, so
 * plain adds are SMP-safe. */
static u64   g_task_utime_ns[TASK_SLOTS];
static u64   g_task_stime_ns[TASK_SLOTS];
static u64   g_task_cutime_ns[TASK_SLOTS];
static u64   g_task_cstime_ns[TASK_SLOTS];
/* Scheduler tick at which the slot's current occupant was created — /proc's
 * starttime field, and the base for a task's wall-clock age. */
static u64   g_task_start_tick[TASK_SLOTS];

/* The number a pidfd reports as its inode.
 *
 * On Linux this is a pidfs inode: it identifies the PROCESS, so two pidfds for
 * the same process share it and a recycled pid never reuses it. b1nix gave
 * each DESCRIPTOR its own number, which is a different claim — systemd takes
 * a pidfd, records the number, opens the process again later and compares, and
 * concluded every time that the descriptor now referred to something else
 * ("Failed to spawn executor: Object is remote", EREMOTE from
 * pidref_verify()). Assigned per task at creation and never reused. */
static u64   g_task_pidfs_ino[TASK_SLOTS];
static u64   g_pidfs_ino_next = 1;
/* Context-switch counts: voluntary (the task blocked/slept/exited of its own
 * accord) and involuntary (it was still runnable when preempted). getrusage's
 * ru_nvcsw/ru_nivcsw. */
static u64   g_task_nvcsw[TASK_SLOTS];
static u64   g_task_nivcsw[TASK_SLOTS];
/* CPU time of threads that have already exited, folded into their thread-group
 * leader's slot. Without it a process's own CPU time would SHRINK as its
 * threads finish — times(2), getrusage(RUSAGE_SELF) and
 * CLOCK_PROCESS_CPUTIME_ID all have to keep counting work that is done. */
static u64   g_task_gone_utime_ns[TASK_SLOTS];
static u64   g_task_gone_stime_ns[TASK_SLOTS];
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
static u64   g_task_maxrss_pages[TASK_SLOTS];
static u64   g_task_rss_sample_tick[TASK_SLOTS];
static u8    g_task_parked_leader[TASK_SLOTS];
static u8    g_task_parked_override[TASK_SLOTS];
static int   g_task_parked_code[TASK_SLOTS];
/* Set on a thread pulled into a group stop by a signal sent to its leader: it
 * parks without posting its own SIGCHLD/stop report (see the delivery path). */
static u8    g_task_stop_quiet[TASK_SLOTS];
static u64   g_task_pass[TASK_SLOTS];
/* sched_setaffinity's per-task CPU mask. Defined here, beside the other slot
 * side tables, because find_unused_task must clear it when it recycles a
 * slot. 0 means "any CPU". */
static u64 g_task_affinity[TASK_SLOTS];
static u64   g_min_pass = 0;
/* Last userspace RIP of each task, captured by the LAPIC timer tick when it
 * preempts a ring-3 task (see task_set_user_rip). Consumed by the silence
 * watchdog task dump to name the user function a wedged thread group spins
 * in. */
static u64   g_task_user_rip[TASK_SLOTS];
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
static char *g_task_cmdline[TASK_SLOTS];
static usize g_task_cmdline_len[TASK_SLOTS];
/* M63: seccomp-bpf per-task state (side-tables — struct task cannot grow, see
 * the M29 LAPIC-PT note). g_task_seccomp holds the installed filter chain
 * (opaque to the scheduler; defined in seccomp.c); g_task_nnp is no_new_privs. */
static void *g_task_seccomp[TASK_SLOTS];
static int   g_task_nnp[TASK_SLOTS];
/* Every index handed to T() is bounded before it gets here.
 *
 * This used to return a permanently-unused stand-in slot for an index it could
 * not resolve, so that a walker reading ->state on it would find TASK_UNUSED
 * and skip it. That was added after scheduler_reap_orphan_zombies took a kernel
 * data abort reading ->state with FAR 0x10 -- a NULL chunk -- and it did stop
 * the crash, but by turning a kernel bug into a task that quietly does not
 * exist, which is exactly the shape of bug that is impossible to find later.
 *
 * The cause is fixed at the source instead, and the two conditions below are
 * now unreachable:
 *   - allocate_task_slot calls ensure_task_chunk BEFORE it advances
 *     g_task_hwm, holding the task lock, and g_task_hwm is _Atomic -- so a
 *     chunk exists for every index below the mark that any walker can observe;
 *   - every caller in this file bounds its index by g_task_hwm (the walks) or
 *     range-checks it explicitly (scheduler_task_slot), and g_task_hwm never
 *     exceeds MAX_TASKS.
 * Reaching either one means one of those two invariants has broken, which is
 * worth a name and a stop rather than a task-shaped hole in a walk. */
static struct task *T(usize i);

static inline struct task *T(usize i) {
  if (i >= MAX_TASKS) {
    console_write("task: slot index ");
    console_write_dec((u64)i);
    console_write(" past MAX_TASKS, hwm=");
    console_write_dec((u64)g_task_hwm);
    console_write("\n");
    panic("scheduler: task slot index out of range");
  }
  struct task *chunk = __atomic_load_n(&g_task_chunks[i >> 6], __ATOMIC_ACQUIRE);
  if (!chunk) {
    console_write("task: slot ");
    console_write_dec((u64)i);
    console_write(" has no chunk, hwm=");
    console_write_dec((u64)g_task_hwm);
    console_write("\n");
    panic("scheduler: task slot below the high-water mark has no chunk");
  }
  return &chunk[i & 63];
}


/*
 * Is this a pointer to a real task slot?
 *
 * A runqueue holds task pointers and nothing else, so anything that is not the
 * address of a slot means the list has been corrupted. Following such a
 * pointer is not a crash at the point of corruption - the scheduler switches
 * to a context assembled from whatever that memory held, and the fault
 * surfaces later somewhere unrelated (a data abort in scheduler_yield's
 * epilogue with a FAR of -3, for instance). Checking here names the problem
 * where it can still be attributed.
 */
/* The per-CPU idle tasks, allocated outside the chunked task table. */
static struct task *g_ap_idle_tasks[MAX_CPUS];

int sched_task_ptr_valid(const struct task *t) {
  if (!t)
    return 0;
  for (usize c = 0; c < TASK_MAX_CHUNKS; c++) {
    const struct task *base =
        __atomic_load_n(&g_task_chunks[c], __ATOMIC_ACQUIRE);

    /* Chunks are allocated in order, so the first empty slot ends the search.
     * This used to `continue`, walking all 64 entries on every call -- and this
     * predicate sits on the runqueue paths. */
    if (!base)
      break;
    if (t >= base && t < base + TASK_CHUNK_SIZE) {
      usize off = (usize)((const char *)t - (const char *)base);

      return (off % sizeof(struct task)) == 0;
    }
  }
  /* The per-CPU idle tasks are real tasks that are deliberately NOT in the
   * chunked table (scheduler_setup_ap_idle explains why), so a chunk-only
   * answer called every one of them invalid.
   *
   * That is not a cosmetic gap. This function is the guard rq_enqueue and
   * rq_dequeue panic on ("runqueue list is corrupt"), and it is the check that
   * was tried at the context switch and reverted as "never fired, worst score"
   * -- of course it scored badly: on a kernel with secondaries it rejects the
   * one task every idle CPU switches through. An always-false guard cannot
   * catch anything, which is why the pointer that has been corrupting saved
   * registers went unnoticed by it. */
  for (int c = 0; c < MAX_CPUS; c++)
    if (g_ap_idle_tasks[c] == t)
      return 1;
  return 0;
}
/* current_task is per-CPU now (a macro -> get_percpu()->cur_task, see sched.h). */
/* PID numbering follows the Unix model: the boot/idle task is PID 0 (Linux's
 * "swapper"), PID 1 is reserved for the userspace init process, and everything
 * else is numbered from 2 up. Reserving 1 matters because real init systems
 * (OpenRC's openrc-init, sysvinit, ...) do `if (getpid() != 1) return 1;` and
 * exit immediately otherwise. See scheduler_reserve_init_pid(). */
static usize next_task_id = 2;

static int g_task_vfork_pending[TASK_SLOTS];
static usize g_task_vfork_id[TASK_SLOTS];

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


static struct task *find_unused_task(void);
static usize task_index(const struct task *task);
extern u64 g_task_affinity_fwd_unused;

/* A suspended task's stack pointer, wherever the arch parked it. */
#if defined(__x86_64__)
#define TASK_CTX_SP(t) ((t)->context.rsp)
#elif defined(__aarch64__)
#define TASK_CTX_SP(t) ((t)->context.sp)
#else
#define TASK_CTX_SP(t) ((t)->context.esp)
#endif

struct task *scheduler_setup_ap_idle(int cpu, u64 kstack_top) {
  if (cpu < 0 || cpu >= MAX_CPUS)
    return 0;
  /* Its own allocation, deliberately outside the task table.
   *
   * Putting these in the table was tried and reverted: a slot is visible to
   * every picker, and scheduler_yield marks a task READY when it switches away
   * from it, so an idle task became schedulable material for other CPUs. Even
   * pinned by an affinity mask the suite collapsed -- 123 passed, then 8,
   * against ~750 -- so whatever else reads the table does not tolerate them
   * either.
   *
   * Staying outside it used to cost an aliasing bug: task_index() answered 0
   * for a task it could not find in a chunk, so every idle task shared SLOT 0
   * -- the boot task's row -- in every per-task side table (g_task_pass,
   * g_task_xsave, g_task_nice, g_task_affinity, g_task_syscall). These tasks
   * now have their own index space above the table (see TASK_SLOTS and
   * task_index), so nothing is shared. Dumps still read `pid 0 name=ap-idle`,
   * because the id is genuinely 0; that is a naming artefact, not the alias. */
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
  /* The base of that stack, which every other task carries and this one did
   * not. scheduler_yield's stack-pointer guard is written as
   * `if (new_task->stack)`, so a task with none was the one thing it never
   * checked -- and this is the task that ends up running whenever a CPU has
   * nothing else to do. A switch into it with a corrupted context therefore
   * went straight through to arch_context_switch, which `ret`s to the saved
   * lr: zero, on a stack belonging to somebody else, reported as an EL1
   * instruction abort at address 0 with no backtrace worth reading. */
  t->stack = (void *)(usize)(kstack_top - KERNEL_STACK_SIZE);
  /* ...and the canary that goes with it. Every other kernel stack is painted
   * by whoever allocates it; this one is the AP's boot stack, handed over by
   * the SMP bring-up, so nothing had ever written the marker the overflow
   * check reads. Safe to write here: this is the far end of the stack, and
   * the CPU running on it is nowhere near the base. */
  *(u64 *)(usize)t->stack = KSTACK_CANARY;
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
static struct task *g_task_fdlock_owner[TASK_SLOTS];
/* Where each task last cleared its kernel-stack lease (stack_released = 0).
 * A task found READY with the lease still cleared and running on no CPU is
 * unschedulable forever; this names the code path that stranded it. */
static const char *g_task_lease_site[TASK_SLOTS];

/* What was the machine waiting for, when it was waiting?
 *
 * The aarch64 sys lane spends two thirds of its ticks in the idle task, so
 * what limits it is waiting, not computing -- and the CPU profile cannot say
 * what for.
 *
 * The first version of this asked "which timer is nearest" on each idle tick.
 * That was the wrong question and gave a confidently wrong answer: net_task
 * polls at 100 Hz, so its deadline is nearest almost always, and it collected
 * 92% of the idle time while having nothing to do with what the lane was
 * blocked on.
 *
 * So attribute causally instead. Count consecutive idle ticks, and when the
 * machine stops being idle, charge that whole stretch to the task that is now
 * running -- the one whose wake ended the wait. That names what the suite
 * actually waits for. `b1nix.waitprof` turns it on. */
#define WAITPROF_SLOTS 64
/* Keyed by lease_site only. Keying by task->name was tried and is wrong: that
 * pointer aims into the task's own argv/env pages, which are recycled, so the
 * buckets filled up with fragments of somebody's environment. The site is a
 * __func__ literal and stays put; a name is COPIED in for context. */
static struct { const char *site; char name[16]; u64 ticks; u64 spans; u64 timer; }
    g_waitprof[WAITPROF_SLOTS];
static u64 g_waitprof_idle, g_waitprof_run;
/* Set while wake_sleepers is promoting tasks, so waitprof can tell a wake that
 * a deadline caused from one an event caused. The difference is the whole
 * point: an idle stretch ended by a packet is the machine doing its job, and
 * one ended by a timer that found nothing is the waste worth removing. */
static int g_wake_from_timer;

static int waitprof_same_name(const char *a, const char *b)
{
	for (int i = 0; i < 15; i++) {
		char ca = a[i];
		char cb = b ? b[i] : 0;

		if (cb < 32 || cb >= 127)
			cb = 0;
		if (ca != cb)
			return 0;
		if (!ca)
			return 1;
	}
	return 1;
}

/* Keyed by site AND task name.
 *
 * Keying by site alone merged every task that blocks at the same place into
 * one bucket, labelled with whichever name happened to create it -- so a
 * bucket reading "net_task @scheduler_sleep_ticks_state" was really every task
 * in the machine that sleeps by ticks, and I read a beat into it that net_task
 * did not have. Fixing the key is what found the real one: a test-support
 * thread polling for a mouse open. The name has to be part of the key, not
 * decoration. */
static void waitprof_charge(const char *name, const char *site, u64 ticks)
{
	for (usize k = 0; k < WAITPROF_SLOTS; k++) {
		if (g_waitprof[k].site == site && g_waitprof[k].ticks &&
		    waitprof_same_name(g_waitprof[k].name, name)) {
			g_waitprof[k].ticks += ticks;
			g_waitprof[k].spans++;
			g_waitprof[k].timer += (u64)(g_wake_from_timer != 0);
			return;
		}
		if (!g_waitprof[k].ticks) {
			g_waitprof[k].site = site;
			g_waitprof[k].ticks = ticks;
			g_waitprof[k].spans = 1;
			g_waitprof[k].timer = (u64)(g_wake_from_timer != 0);
			for (int c = 0; c < 15; c++) {
				char ch = name ? name[c] : 0;

				g_waitprof[k].name[c] = (ch >= 32 && ch < 127) ? ch : 0;
				if (!ch)
					break;
			}
			g_waitprof[k].name[15] = 0;
			return;
		}
	}
}

/* The tick only decides WHETHER the machine is idle. Who ended the wait is
 * charged from the wake itself (sched_waitprof_wake), because a 100 Hz sampler
 * cannot see it: net_task wakes, polls and sleeps again inside a single tick,
 * so sampling caught it at the end of nearly every idle stretch and charged it
 * 22 of the lane's 44 idle seconds it had nothing to do with. */
void sched_waitprof_tick(int in_idle)
{
	if (in_idle) {
		g_waitprof_idle++;
		if (!g_waitprof_run)
			g_waitprof_run = scheduler_ticks ? scheduler_ticks : 1;
	}
}

/* A task just became runnable. If the machine was idle, this wake is what
 * ended the wait, so the whole stretch belongs to it. */
void sched_waitprof_wake(struct task *t)
{
	if (!g_waitprof_run || !t)
		return;

	u64 began = g_waitprof_run;

	g_waitprof_run = 0;
	if (scheduler_ticks > began)
		waitprof_charge(t->name, g_task_lease_site[task_index(t)],
		                scheduler_ticks - began);
}

void sched_waitprof_dump(void)
{
	if (!bootinfo_has_flag("b1nix.waitprof"))
		return;
	console_write("waitprof: idle-ticks ");
	console_write_dec(g_waitprof_idle);
	console_write(" (10 ms each), charged to the task whose wake ended each wait\n");
	for (usize k = 0; k < WAITPROF_SLOTS; k++) {
		if (!g_waitprof[k].ticks)
			continue;
		console_write("  waitprof ");
		console_write_dec(g_waitprof[k].ticks);
		console_write(" ticks over ");
		console_write_dec(g_waitprof[k].spans);
		console_write(" waits, ");
		console_write_dec(g_waitprof[k].timer);
		console_write(" by timer, ");
		console_write(g_waitprof[k].name[0] ? g_waitprof[k].name : "?");
		console_write(" @");
		console_write(g_waitprof[k].site ? g_waitprof[k].site : "-");
		console_write("\n");
	}
}
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
static usize g_task_fdlock_holder[TASK_SLOTS];
static const char *g_task_fdlock_site[TASK_SLOTS];
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
static struct task *g_task_fdlock_on[TASK_SLOTS];
static u64 g_task_fdlock_flags[TASK_SLOTS];
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
/* Poison either side of a task chunk.
 *
 * A task's saved kernel stack pointer has been seen turning into another task's
 * stack without either scheduler boundary putting it there: the EL0-entry
 * containment assert never fires, the switch-out guard never fires, and yet the
 * switch-IN guard catches a context.sp pointing into somebody else's stack. The
 * remaining way for that to happen is a write through the task struct itself,
 * and task chunks are ordinary kzalloc'd heap blocks sitting next to other
 * allocations -- kernel stacks among them. Bracket them and say so, with the
 * neighbouring block named, instead of discovering it three context switches
 * later as a `ret` into a spilled boolean. */
#define TASK_CHUNK_GUARD 0xC0DEFACEF00DBEEFULL
#define TASK_CHUNK_PAD   16 /* keeps the chunk itself 16-byte aligned */

static int ensure_task_chunk(usize c) {
  if (c >= TASK_MAX_CHUNKS) return 0;
  if (g_task_chunks[c]) return 1;
  u8 *raw = kzalloc(TASK_CHUNK_PAD + TASK_CHUNK_SIZE * sizeof(struct task) +
                    TASK_CHUNK_PAD);
  if (!raw) return 0;
  struct task *chunk = (struct task *)(raw + TASK_CHUNK_PAD);
  *(u64 *)raw = TASK_CHUNK_GUARD;
  *(u64 *)((u8 *)chunk + TASK_CHUNK_SIZE * sizeof(struct task)) =
      TASK_CHUNK_GUARD;
  /* Published with a RELEASE store, and read back with acquire everywhere.
   *
   * This was a plain store paired with plain loads, on a weakly-ordered machine
   * with real secondaries. Every reader of this array -- T(), task_index(),
   * sched_task_ptr_valid() -- could therefore observe the slot's chunk as still
   * NULL after another CPU had installed it, and each of them turns that into a
   * different disaster:
   *
   *   T()                   -> "task slot below the high-water mark has no chunk"
   *   sched_task_ptr_valid  -> a live task declared not-a-task, which
   *                            rq_enqueue/rq_dequeue panic on as a corrupt list
   *                            and the context-switch guard panics on as a bad
   *                            pointer
   *
   * Caught red-handed: the switch guard panicked, and its own report -- which
   * re-reads both pointers a few instructions later -- printed `valid=1` for
   * BOTH of them. A pointer cannot be invalid in the test and valid in the
   * message unless what the test consults is changing underneath it, and the
   * only thing it consults is this array. */
  __atomic_store_n(&g_task_chunks[c], chunk, __ATOMIC_RELEASE);
  return 1;
}

/* Verify every chunk's poison. Called from the timer tick, so the report lands
 * within a tick of the write rather than at whatever later moment the damaged
 * field is next used. */
void task_chunks_verify(const char *where) {
  for (usize c = 0; c < TASK_MAX_CHUNKS; c++) {
    struct task *chunk = g_task_chunks[c];
    if (!chunk)
      continue;
    u64 *lo = (u64 *)((u8 *)chunk - TASK_CHUNK_PAD);
    u64 *hi = (u64 *)((u8 *)chunk + TASK_CHUNK_SIZE * sizeof(struct task));
    if (*lo == TASK_CHUNK_GUARD && *hi == TASK_CHUNK_GUARD)
      continue;
    console_write("TASK-CHUNK-CORRUPT: chunk ");
    console_write_dec((u64)c);
    console_write(" at 0x");
    console_write_hex64((u64)(usize)chunk);
    console_write(" from ");
    console_write(where ? where : "?");
    console_write("\n  below=0x");
    console_write_hex64(*lo);
    console_write(" above=0x");
    console_write_hex64(*hi);
    console_write("\n");
    kheap_describe((u64)(usize)chunk - TASK_CHUNK_PAD, "  this block:");
    kheap_describe((u64)(usize)chunk - TASK_CHUNK_PAD - 1, "  block below:");
    kheap_describe((u64)(usize)hi + 8, "  block above:");
    panic("task chunk overwritten by a heap neighbour");
  }
}

/* Atomically claim a free task slot, zero it, mark it BLOCKED (reserved),
 * and assign its id — all under g_tasks_lock so two CPUs can never claim the
 * same slot. Scans the in-use range first (cheap), then grows by one chunk
 * (TASK_CHUNK_SIZE slots) on demand up to the MAX_TASKS ceiling. */
/* The stride value a brand-new task starts life with.
 *
 * This used to be g_min_pass, whose name promises a minimum but whose value is
 * whatever pick_next_task last selected -- and the runqueue fast path assigns
 * it from the dequeued task without comparing passes at all. So one freshly
 * woken userspace process with a long history was enough to leave g_min_pass
 * far above every runnable kernel thread, and the next task created inherited
 * that as its birth pass.
 *
 * A task that starts behind never catches up: pick_next_task takes the LOWEST
 * pass among the highest priority, a task only advances its pass by running,
 * and a task that is never picked never runs. The AIO worker was born at
 * 662650 while net_task and m47-input-inject sat at 65450 and climbed ~60000 a
 * minute -- ten minutes behind, against a 60-second stall watchdog. It stayed
 * READY for the entire instance, so io_getevents blocked on a completion that
 * nobody was ever going to produce and the sys lane died with ~900 checks
 * unrun.
 *
 * So take the real minimum over the tasks that can actually be picked. It is
 * one pass over the table, paid once per task creation, and it is the value
 * g_min_pass was always meant to be. Called with the task lock held. */
static u64 sched_birth_pass(void) {
  u64 min = 0;
  int seen = 0;
  for (usize i = 0; i < g_task_hwm; i++) {
    struct task *t = T(i);
    if (!t)
      continue;
    if (t->state != TASK_READY && t->state != TASK_RUNNING)
      continue;
    u64 p = g_task_pass[i];
    if (!seen || p < min) {
      min = p;
      seen = 1;
    }
  }
  return seen ? min : g_min_pass;
}

static struct task *find_unused_task(void) {
  u64 flags;
  tasks_lock(&flags);

  /* 1) Fast path: reuse a slot already in [0, g_task_hwm). Never recycle slot 0
   * — it is the permanent boot/idle task (the BSP's idle-fallback target). */
  for (usize i = 1; i < g_task_hwm; i++) {
    if (T(i)->state == TASK_UNUSED) {
      /* A slot marked UNUSED while its task is still the one executing on this
       * CPU means somebody reaped a live task; memsetting it here would turn
       * that into an unexplainable crash somewhere else entirely. Fail at the
       * point where the corruption would be created instead. */
      /* ...on ANY CPU, not just this one. The old test compared against this
       * CPU's cur_task, so a slot freed while its task was still executing on
       * another core was handed straight to a new task -- which then shares the
       * previous occupant's kernel_stack_ptr and saved context, i.e. two CPUs
       * on one stack, with the corruption surfacing far from here. */
      if (T(i) == current_task || task_running_somewhere(T(i))) {
        console_write("sched: recycling slot of a live task: pid ");
        console_write_dec((u64)T(i)->id);
        console_write(" name=");
        console_write(T(i)->name ? T(i)->name : "(none)");
        console_write("\n");
        panic("sched: reusing the slot of the task that is running");
      }
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
      /* And the rseq registration, for the same reason and one level up.
       *
       * The rseq table is keyed by the `struct task *`, and this loop is where
       * that pointer is handed to a new task -- so a registration the previous
       * occupant never had cleaned up belongs to the newcomer as far as
       * rseq_find() is concerned. Its first registration then looks like a
       * conflicting one at a different area, the kernel refuses it, and glibc
       * treats a refused rseq registration as fatal: the thread dies at
       * start-up ("Fatal glibc error: rseq registration failed").
       *
       * This was cleared on the fork path alone, so a pthread -- which does
       * not take that path -- inherited whatever the recycled slot still held.
       * Clearing it where the slot is allocated is the invariant that no exit
       * path can miss. */
      rseq_fork_clear(T(i));
      g_task_alarm_ticks[i] = 0;
      g_task_alarm_interval_ticks[i] = 0;
      g_task_execed[i] = 0;
      g_task_nice[i] = 0;
      g_task_fdlock_owner[i] = 0;
      g_task_exiting[i] = 0;
      g_task_tgid[i] = 0;
      g_task_exit_stage[i] = 0; /* a recycled slot must not inherit a stage */
      /* ...nor a CPU affinity mask. This one is cleared in
       * scheduler_exit_current, so a task that leaves by any other route --
       * killed, reaped as a thread, or dropped on a create error path --
       * hands its pinning to whoever gets the slot next.
       *
       * A kernel thread inherits it silently and is then unschedulable for
       * good: kthreads have ap_runnable == 0, so every AP skips them, and a
       * stale mask naming only an AP denies them the BSP as well. The task
       * sits READY for ever while the picker walks past it. That is what hung
       * the sys lane in m8_aio_test -- the AIO worker was created, queued and
       * never once ran its first statement, so every io_getevents blocked
       * against a completion nobody would ever produce.
       *
       * fork's own inheritance (see scheduler_fork_current) copies the parent's
       * mask after this point, so clearing here does not weaken it. */
      g_task_affinity[i] = 0;
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
      g_task_pidfs_ino[i] = g_pidfs_ino_next++;
      g_task_nvcsw[i] = 0;
      g_task_nivcsw[i] = 0;
      g_task_gone_utime_ns[i] = 0;
      g_task_gone_stime_ns[i] = 0;
      g_task_maxrss_pages[i] = 0;
      g_task_rss_sample_tick[i] = 0;
      g_task_parked_leader[i] = 0;
      g_task_parked_override[i] = 0;
      g_task_stop_quiet[i] = 0;
      g_task_pass[i] = sched_birth_pass();
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
  usize i = g_task_hwm;
  /* Chunk was kzalloc'd, but be explicit so a slot that gets reused after
   * free_task_slot starts from a clean state too (same as the old path). */
  memset(T(i), 0, sizeof(struct task));
  g_task_hwm = i + 1;
  g_task_pass[i] = sched_birth_pass();
  g_task_affinity[i] = 0;
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
  g_task_pidfs_ino[i] = g_pidfs_ino_next++;
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
  /* The slot is about to become claimable by a new task, so this is the last
   * moment the id means anything: release the pid-namespace number here rather
   * than at exit, where the task is still a zombie its parent must be able to
   * name. */
  namespace_task_reaped(t->id);
  /* And the namespace row itself, for the same reason and at the same moment.
   * namespace_task_exit() only runs on the scheduler_exit_current path, so a
   * task reaped by any other route left its row behind -- keyed by pid, which
   * is about to be handed to somebody else. The next owner then translated
   * every pid it named through a namespace it was not in and got ESRCH for
   * live processes. */
  namespace_task_exit(t->id);
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
  /* Same reason: a reused id must not inherit the previous task's chosen
   * comm. */
  {
    extern void scheduler_clear_comm_internal(usize pid);
    scheduler_clear_comm_internal(t->id);
  }
  /* Last chance to take the slot out of the run lists. Once state is
   * TASK_UNUSED the slot is claimable, and the next occupant overwrites
   * next_run in place — through a link the chain is still following. See
   * sched_rq_remove_task_all(). Done before tasks_lock: the runqueue lock is
   * the outermost level, and a leaf lock must not nest it. */
  sched_rq_remove_task_all(t);
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
    const struct task *chunk =
        __atomic_load_n(&g_task_chunks[c], __ATOMIC_ACQUIRE);
    if (!chunk) break;
    if (task >= chunk && task < chunk + TASK_CHUNK_SIZE) {
      return (c << 6) | (usize)(task - chunk);
    }
  }
  /* The per-CPU idle tasks are deliberately outside the table. They get their
   * own slots rather than all answering 0, which handed every one of them the
   * boot task's row in every side table -- a cross-CPU hazard the comment in
   * scheduler_setup_ap_idle has described for as long as it has existed. */
  for (int c = 0; c < MAX_CPUS; c++) {
    if (g_ap_idle_tasks[c] == task) {
      return (usize)MAX_TASKS + (usize)c;
    }
  }
  return 0;
}

/* Which CPU's idle task this is, or -1. The per-CPU idle tasks live outside the
 * task table on purpose (see scheduler_setup_ap_idle), so no table walk finds
 * them, and a dump that meets one otherwise has no way to name it. */
static int sched_ap_idle_cpu_of(const struct task *t) {
  if (!t)
    return -1;
  for (int c = 0; c < MAX_CPUS; c++)
    if (g_ap_idle_tasks[c] == t)
      return c;
  return -1;
}

/* True if `t` is the live current task on some CPU — i.e. it is executing
 * right now, not merely parked READY. scheduler_yield reassigns this CPU's
 * cur_task to the *incoming* task BEFORE arch_context_switch saves+publishes
 * the outgoing task's stack_released, so a task that is genuinely mid-switch-out
 * is NOT cur_task anywhere during its stack_released==0 window. A task that is
 * cur_task somewhere with stack_released==0 is therefore actively running (it
 * was woken BLOCKED->READY mid-flight and kept running) and will not publish
 * stack_released soon — the picker must not wait on it. */
int task_running_somewhere(struct task *t) {
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
/* A real hand-off is a handful of instructions, so the bound only has to
 * outlast that. Five million was chosen when expiry meant "claim it anyway"
 * and a false expiry was catastrophic; now that expiry means "do not claim it
 * this time, try again", a long spin buys nothing and is paid on every pick
 * that meets a task mid-switch. */
#define SCHED_HANDOFF_GRACE_SPINS 200000ULL

static void sched_handoff_recover(struct task *t, const char *where) {
  static int reported;
  /* Never claim a task that is genuinely mid-switch.
   *
   * The grace period above is a SPIN COUNT, and a spin count cannot measure
   * how long another CPU has had. Under round-robin TCG it measures nothing at
   * all: the spinning vCPU holds the emulator's thread for its whole slice, so
   * the vCPU being waited on does not execute a single instruction while the
   * count runs out -- the "grace period" always expires, every time, and the
   * recovery then publishes a lease for a task whose arch_context_switch is
   * still writing its callee-saved state. Another CPU loads that half-written
   * ->context and resumes on that task's kernel stack, which is exactly the
   * "secondary running with SP inside ANOTHER task's kernel stack" this port
   * has been chasing, and why userspace on secondaries is still gated off.
   *
   * The state the recovery legitimately exists for -- a waker CASing a task
   * READY inside its prepare/commit window, so it never reaches
   * arch_context_switch at all -- never sets this flag, and is still
   * recovered. So ask the fact instead of the clock. */
  if (t && __atomic_load_n(&g_task_switching_out[task_index(t)],
                           __ATOMIC_ACQUIRE))
    return;
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

/* A secondary must never end up owning the boot task: it is the BSP's idle task
 * AND slot 0 of the table, and two CPUs on its .bss stack is the failure this
 * branch keeps landing on. Two places publish this CPU's task -- the pick, and
 * the aarch64 re-publication after arch_context_switch returns -- and a dump
 * that shows `cur_task = boot` on cpu 1 cannot say which of them wrote it.
 * Say so once, from whichever one does. */

static struct task *pick_next_task(void) {
  if (current_task == 0) {
    return 0;
  }
  g_pick_calls++;

  struct percpu *pcpu = get_percpu();
  int on_ap = (pcpu && pcpu->cpu_id != 0);

#if defined(__aarch64__)
  /* `on_ap` is the whole of the filter that keeps a secondary off kernel
   * threads: get it wrong and this CPU drains the global runqueue and scans the
   * task table with no ap_runnable filter, which is the only route by which a
   * secondary can reach the BOOT task -- and the boot task is what every
   * foreign-stack report on this arch has named. It is derived from
   * pcpu->cpu_id, and pcpu comes from a table lookup on MPIDR that happens on
   * every per-CPU access, so "this CPU is reading the right block" is an
   * assumption worth checking rather than one to keep asserting.
   *
   * Reported once, not panicked: if it ever fires it explains the corruption,
   * and a report on the way past leaves the machine able to say so. */
  {
    extern int aarch64_percpu_self_mismatch(void);

    if (aarch64_percpu_self_mismatch()) {
      static volatile int reported;

      if (!__atomic_exchange_n(&reported, 1, __ATOMIC_ACQ_REL)) {
        console_write("PERCPU-MISMATCH: this CPU resolves to block cpu_id=");
        console_write_dec(pcpu ? (u64)pcpu->cpu_id : 99);
        console_write(" on_ap=");
        console_write_dec((u64)on_ap);
        console_write(" cur=");
        console_write(current_task && current_task->name ? current_task->name
                                                         : "(none)");
        console_write("\n");
      }
    }
  }
#endif

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
        cpu_relax();
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
      /* Never claim a task that is still some CPU's current task.
       *
       * stack_released stays set for the whole time a task runs — it is
       * published when the task leaves a CPU and nothing takes it back when
       * the task is resumed. A waker that CASes a live task BLOCKED->READY
       * while it is executing its own block loop therefore leaves it looking
       * exactly like a parked task with its stack handed over, and a second
       * CPU loads its saved RSP and returns through a word that is now part of
       * a live frame. The rip that results is a small integer — a system-call
       * argument, executed. The per-CPU current-task pointers are what
       * actually say who is running what. */
      if (t != current_task && task_running_somewhere(t))
        continue;
      enum task_state expected = TASK_READY;
      if (__atomic_compare_exchange_n(&t->state, &expected, TASK_RUNNING,
                                      0, __ATOMIC_ACQUIRE,
                                      __ATOMIC_RELAXED)) {
        /* The stack is this CPU's again from here on. */
        __atomic_store_n(&t->stack_released, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_task_switching_out[task_index(t)], 0,
                         __ATOMIC_RELEASE);
        g_min_pass = g_task_pass[task_index(t)];
        g_pick_rq++; g_last_pick_id = t->id;
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

    struct task *t = T(index);
    if (!t || t->state != TASK_READY)
      continue;
    if (t->stealable)
      continue;
    if (on_ap && !t->ap_runnable)
      continue; /* APs run only userspace ELF processes */
    if (pcpu && !sched_task_allowed_on_cpu(t, pcpu->cpu_id))
      continue; /* pinned elsewhere by sched_setaffinity */

    int priority = t->priority;
    u64 pass = g_task_pass[index];
    g_scan_seen++;
    if (priority > max_priority ||
        (priority == max_priority && pass < min_pass)) {
      max_priority = priority;
      min_pass = pass;
      best_task = t;
    }
  }

  if (best_task) {
    g_scan_best_id = best_task->id;
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
      cpu_relax();
      tlb_shootdown_poll();
      if (++handoff_spins > SCHED_HANDOFF_GRACE_SPINS) {
        sched_handoff_recover(best_task, "scan");
        break;
      }
    }
    /* If the wait bailed with stack_released still 0 (task running on another
     * CPU or re-blocked), do not claim it — its context isn't safe to load.
     * Treat as no-work; the next pick re-finds it once it has switched out. */
    /* Same gate as the runqueue path: a task that is some CPU's current task
     * is executing, whatever its state and lease say. */
    if (best_task != current_task &&
        !__atomic_load_n(&best_task->stack_released, __ATOMIC_ACQUIRE))
      g_scan_rej_lease++;
    else if (best_task != current_task && task_running_somewhere(best_task))
      g_scan_rej_running++;
    if ((best_task == current_task ||
         __atomic_load_n(&best_task->stack_released, __ATOMIC_ACQUIRE)) &&
        (best_task == current_task || !task_running_somewhere(best_task))) {
      /* F5 (M28 #7): atomic claim — see global-rq comment above. If we lose
       * the CAS, return 0 (no work this iteration) and let the caller retry;
       * starting a fresh scan here can spin under contention without ever
       * settling, so the cleaner shape is "treat as no-work and try again". */
      enum task_state expected = TASK_READY;
      if (__atomic_compare_exchange_n(&best_task->state, &expected,
                                      TASK_RUNNING, 0, __ATOMIC_ACQUIRE,
                                      __ATOMIC_RELAXED)) {
        __atomic_store_n(&best_task->stack_released, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_task_switching_out[task_index(best_task)], 0,
                         __ATOMIC_RELEASE);
        g_min_pass = g_task_pass[task_index(best_task)];
        g_pick_scan++; g_last_pick_id = best_task->id;
        return best_task;
      }
      g_scan_rej_cas++;
    }
  }

  /* AP fallback: nothing else runnable, so park back to this CPU's idle task,
   * which lets the AP cooperative loop regain control and drop the BKL. NULL on
   * the BSP (its boot task handles idling), and skipped when the idle task is
   * already current (so scheduler_yield returns 0 and the loop parks). */
  if (pcpu && pcpu->idle_task && current_task != (struct task *)pcpu->idle_task) {
    /* This path hands back the idle task without the CAS the two paths above
     * use, so it must clear the switch-out marker itself -- otherwise the idle
     * task carries a stale 1 from its last switch-out for the rest of the
     * boot. Nothing else can pick an AP's idle task (it is not in the table),
     * so this is hygiene rather than a live bug, and the marker is only
     * meaningful while it is true. */
    __atomic_store_n(
        &g_task_switching_out[task_index((struct task *)pcpu->idle_task)], 0,
        __ATOMIC_RELEASE);
    g_pick_idle++;
    g_last_pick_id = ((struct task *)pcpu->idle_task)->id;
    return (struct task *)pcpu->idle_task;
  }

  g_pick_null++;
  return 0;
}

static int sig_default_is_ignore(int sig);
static int sched_wake_for_signal_inner(struct task *t, int sig,
                                       int allow_stopped);

/* Will this signal make the target do anything?
 *
 * The question is only asked before cutting a TIMED sleep short. A sleeper
 * that wakes checks `pending & ~blocked` and ends its sleep on any bit it
 * finds, so waking it for a signal it ignores would turn `sleep 60` into
 * `sleep 0.1` the first time a SIGCHLD or SIGWINCH arrived. Linux never queues
 * an ignored signal at all; this is the same rule applied at the wake instead.
 */
static int sig_wakes_target(struct task *t, int sig) {
  if (sig <= 0 || sig >= NSIG)
    return 0;
  if (sig == SIGKILL || sig == SIGSTOP)
    return 1;
  if (t->blocked_signals & (1ULL << (sig - 1)))
    return 0;
  sighandler_t h = t->sigactions[sig - 1].sa_handler;
  if (h == SIG_IGN)
    return 0;
  if (h == SIG_DFL && sig_default_is_ignore(sig))
    return 0;
  return 1;
}

/* Promote a task out of an interruptible wait so it can act on a signal just
 * posted to it. Returns 1 if it was enqueued.
 *
 * Per-state CAS, never a bare store: a target running on another CPU can go
 * DEAD/REAPING between the read and the write, and resurrecting it re-queues a
 * task whose stack is being freed.
 *
 * TASK_SLEEPING is the state this existed without, and it was not a corner:
 * a task in nanosleep is asleep, not blocked, and every kill path here woke
 * only BLOCKED and STOPPED. A signal sent to it therefore sat in
 * `pending_signals` until the sleep's own deadline expired — which is
 * `timeout 3 sleep 60` returning 124 on time and taking sixty seconds, because
 * the SIGTERM that was supposed to end it reached a task nothing would run.
 */
static void sched_trace_signal_wake(const struct task *t, int sig, int state,
                                    int woke);

/* The wake, with the trace around it.
 *
 * `allow_stopped` is not a detail: promoting a STOPPED task to READY RESUMES
 * it, and only the two kill paths that always did so may. A group stop must not
 * un-stop a sibling that is already stopped, and a signal posted through ptrace
 * must not resume a tracee parked in a ptrace stop — either would be job
 * control and debugging silently undone. */
static int sched_wake_for_signal_ex(struct task *t, int sig,
                                    int allow_stopped) {
  int state_before = (int)__atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
  int woke = sched_wake_for_signal_inner(t, sig, allow_stopped);
  sched_trace_signal_wake(t, sig, state_before, woke);
  return woke;
}

static int sched_wake_for_signal(struct task *t, int sig) {
  return sched_wake_for_signal_ex(t, sig, 1);
}

/* A task that was not runnable does not bank credit for the time it was away.
 *
 * pick_next_task takes the LOWEST pass among the highest priority, and a task
 * only advances its pass by running. sched_birth_pass already applies this to
 * a NEWBORN, for the reason written above it -- a task that starts behind
 * never catches up. A task that wakes is in exactly the same position, and
 * nothing was doing it: its pass was frozen while it slept, so it came back
 * holding credit for every tick it was not competing for the CPU, and every
 * task that HAD been running was now hopelessly ahead of it.
 *
 * Measured, on the wedge this fixes: /bin/m109_smoke sat READY at pass
 * 7,691,600 while net_task, kswapd and the lkpi workers -- all of them
 * sleepers -- woke back at ~84,000 and climbed by one stride per yield. To
 * reach m109's pass each of them needed something like 150,000 turns, so the
 * lane ran, picked tasks 1.4 million times, and produced no output for two
 * minutes because the one task with work to do was behind all of them. The
 * watchdog called it a deadlock; it was starvation.
 *
 * g_min_pass is the pass of the last task the picker chose, i.e. the current
 * virtual time, and raising a waker to it is the stride form of CFS's
 * vruntime = max(vruntime, min_vruntime). Never lowers a pass, so a task that
 * is genuinely ahead keeps its position and a task that has legitimately run
 * less keeps its advantage. */
/* sigtimedwait() parks on &task->pending_signals. Nothing woke that channel, so
 * its one-tick timeout was the delivery mechanism rather than a safety net: a
 * signal took up to a tick to be seen, and an init parked there ended 2,453
 * idle stretches in a 63 s run -- the last of this kernel's heartbeats.
 *
 * The wake has to be NARROW. Waking that channel from every posting site was
 * tried and broke eight checks, because which waits a signal may interrupt is
 * policy this kernel implements deliberately (SIGCHLD must not cut waitpid
 * short). So wake only a task that asked, by name, for exactly this signal --
 * which changes no policy, because such a task is going to return this signal
 * the moment it looks. */
void sched_sigwait_arm(u64 set)
{
	if (current_task)
		__atomic_store_n(&g_task_sigwait_set[task_index(current_task)], set,
		                 __ATOMIC_RELEASE);
}

void sched_sigwait_disarm(void)
{
	if (current_task)
		__atomic_store_n(&g_task_sigwait_set[task_index(current_task)], 0,
		                 __ATOMIC_RELEASE);
}

static void sched_sigwait_notify(struct task *t, int sig)
{
	if (!t || sig < 1 || sig >= NSIG)
		return;
	u64 set = __atomic_load_n(&g_task_sigwait_set[task_index(t)],
	                          __ATOMIC_ACQUIRE);

	if (set & (1ULL << (sig - 1)))
		scheduler_wake_all(&t->pending_signals);
}

static void sched_wake_enqueue(struct task *t) {
  if (t) {
    usize i = task_index(t);

    sched_waitprof_wake(t);

    if (g_task_pass[i] < g_min_pass)
      g_task_pass[i] = g_min_pass;
  }
  sched_rq_enqueue_current(t);
}

static int sched_wake_for_signal_inner(struct task *t, int sig,
                                       int allow_stopped) {
  enum task_state expected = TASK_BLOCKED;
  if (__atomic_compare_exchange_n(&t->state, &expected, TASK_READY, 0,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    sched_wake_enqueue(t);
    return 1;
  }
  if (sig_wakes_target(t, sig)) {
    expected = TASK_SLEEPING;
    if (__atomic_compare_exchange_n(&t->state, &expected, TASK_READY, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      sched_wake_enqueue(t);
      return 1;
    }
  }
  if (allow_stopped) {
    expected = TASK_STOPPED;
    if (__atomic_compare_exchange_n(&t->state, &expected, TASK_READY, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      sched_wake_enqueue(t);
      return 1;
    }
  }
  return 0;
}

/* Why a signal did or did not move its target, for `b1nix.debug=signal`. A
 * signal that reaches a task which then does nothing is invisible from
 * userspace -- the process simply carries on -- and the difference between
 * "not posted", "posted to a task that was already running" and "posted to a
 * sleeper nothing woke" is the whole diagnosis. */
static void sched_trace_signal_wake(const struct task *t, int sig, int state,
                                    int woke) {
  if (!klog_debug_enabled("signal"))
    return;
  int in_range = (sig > 0 && sig < NSIG);
  char line[160];
  snprintf(line, sizeof(line),
           "wake id=%lu sig=%d state=%d woke=%d blocked=%d handler=%p",
           (unsigned long)t->id, sig, state, woke,
           in_range ? (int)((t->blocked_signals >> (sig - 1)) & 1ULL) : 0,
           in_range ? (void *)t->sigactions[sig - 1].sa_handler : (void *)0);
  klog_debug_category("signal", line);
}

/* The earliest armed wake_tick, as a LOWER BOUND, plus a generation counter.
 *
 * wake_sleepers() walked the whole task table looking for an elapsed deadline,
 * and it is called from every scheduler_yield(), not just from the tick -- so
 * an O(tasks) walk was paid on every context switch. Almost every one of those
 * walks found nothing: within a single tick, scheduler_ticks does not change,
 * so if the first walk of a tick found nothing due, no later one can either.
 *
 * Arming only ever LOWERS this value, so `now < g_wake_deadline` proves
 * nothing is due and the walk can be skipped. Stale-low merely costs a walk
 * that finds nothing; stale-high would lose a wakeup, which is why raising it
 * is done only from a full scan, and only if no arming raced with that scan
 * (the generation check). This is the cheap half of what a callout wheel gives
 * FreeBSD -- and it is also the "when is the next event" query that one-shot
 * timer programming needs. */
static u64 g_wake_deadline;
static u64 g_wake_deadline_gen;

/* Every place that arms task->wake_tick must call this. Missing one is a lost
 * wakeup, not a slowdown: the bound stays above the armed deadline and
 * wake_sleepers skips the scan that would have fired it. Three sites outside
 * this file (epoll_wait and two in syscall.c) were missed on the first attempt
 * and cost 78 checks. */
void sched_note_deadline(u64 tick)
{
	u64 cur = __atomic_load_n(&g_wake_deadline, __ATOMIC_RELAXED);

	if (!tick)
		return; /* 0 is the "nobody is waiting" sentinel, not a deadline */

	__atomic_fetch_add(&g_wake_deadline_gen, 1, __ATOMIC_RELEASE);
	while (cur == 0 || tick < cur) {
		if (__atomic_compare_exchange_n(&g_wake_deadline, &cur, tick, 1,
		                                __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
			return;
	}
}

/* The earliest deadline anyone is waiting on, or 0 if nobody is. */
u64 sched_next_deadline_tick(void)
{
	return __atomic_load_n(&g_wake_deadline, __ATOMIC_ACQUIRE);
}

static void wake_sleepers(void) {
  int woken = 0;
  u64 due = __atomic_load_n(&g_wake_deadline, __ATOMIC_ACQUIRE);
  u64 gen = __atomic_load_n(&g_wake_deadline_gen, __ATOMIC_ACQUIRE);
  u64 next = 0;

  if (due == 0 || scheduler_ticks < due)
    return; /* nothing can be due: see g_wake_deadline */

  g_wake_from_timer = 1;

  for (usize i = 0; i < g_task_hwm; i++) {
    struct task *t = T(i);
    if (!t) continue;
    /* F4 (M28 #7): atomic CAS SLEEPING/BLOCKED -> READY so only one CPU wins when
     * two timer ticks (or a tick + an explicit wake) race for the same
     * task. */
    /* Alarms share the bound with sleepers: this walk is already here, so
     * folding them in costs one comparison and keeps the bound exact for
     * both. */
    if (g_task_alarm_ticks[i] > scheduler_ticks &&
        (next == 0 || g_task_alarm_ticks[i] < next))
      next = g_task_alarm_ticks[i];
    if (t->wake_tick != 0 && t->wake_tick > scheduler_ticks) {
      /* Not due: it is a candidate for the next deadline. */
      if (next == 0 || t->wake_tick < next)
        next = t->wake_tick;
      continue;
    }
    if (t->wake_tick != 0 && t->wake_tick <= scheduler_ticks) {
      enum task_state expected = TASK_SLEEPING;
      if (__atomic_compare_exchange_n(&t->state, &expected, TASK_READY,
                                      0, __ATOMIC_ACQUIRE,
                                      __ATOMIC_RELAXED)) {
        t->wake_tick = 0;
        sched_wake_enqueue(t);
        woken++;
      } else {
        expected = TASK_BLOCKED;
        if (__atomic_compare_exchange_n(&t->state, &expected, TASK_READY,
                                        0, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
          sched_wake_enqueue(t);
          woken++;
        }
      }
    }
  }
  g_wake_from_timer = 0;

  /* Publish the exact minimum this scan found -- but only if nobody armed a
   * new deadline while we were scanning. An arming that raced with us may have
   * lowered the bound below `next`, and raising it over that would lose the
   * wakeup. Leaving the armed (lower) value costs one extra scan, which is the
   * safe direction. */
  if (__atomic_load_n(&g_wake_deadline_gen, __ATOMIC_ACQUIRE) == gen)
    __atomic_store_n(&g_wake_deadline, next, __ATOMIC_RELEASE);

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
      sched_wake_enqueue(T(i));
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
#if defined(__x86_64__)
  boot->kernel_stack_ptr = (u64)(usize)x86_syscall_stack_top;
#elif defined(__aarch64__)
  /* The boot task's own stack, the one boot.S set SP to. This must not be 0:
   * the scheduler publishes it into this CPU's block (arch_set_kernel_stack), and the exception
   * vectors' EL0_KSTACK_RESET treats 0 as "no task scheduled yet" and keeps
   * whatever SP_EL1 happens to hold. Entering EL0 from the boot task therefore
   * left SP_EL1 at 0, and the next SAVE_REGS turned that into -0x130 —
   * observed as `interrupted SP_EL1: 0xfffffffffffffed0` with the task's real
   * stack megabytes away. Give it the address it is actually running on. */
  {
    extern u8 stack_top[];
    extern u8 stack_bottom[];

    boot->kernel_stack_ptr = (u64)(usize)stack_top;
    /* ...and the base of it, so the switch actually validates this task.
     *
     * The guard below is written `if (new_task->stack)`, and slot 0 was the
     * one task that never carried a base -- the same gap ap-idle used to have
     * (see scheduler_setup_ap_idle). A corrupted saved context on the boot
     * stack therefore went straight through to arch_context_switch and `ret`ed
     * to whatever the stack held: observed as a data abort writing through a
     * register that a healthy path cannot leave null, from an ELR in the
     * middle of scheduler_yield's own resume sequence. Give it the range, and
     * the canary that goes with it (boot_stack_paint writes the same word;
     * either order is fine). */
    boot->stack = (void *)(usize)stack_bottom;
    *(u64 *)(usize)stack_bottom = KSTACK_CANARY;
  }
#else
  boot->kernel_stack_ptr = 0;
#endif
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

  /* Keep a slot of headroom below the end of the allocation instead of putting
   * the stack top exactly at it. Entry code briefly touches the top of the
   * frame it is building, which is harmless inside the heap but faults when
   * this stack happens to be the topmost heap block — its exclusive top is
   * then heap.end, the first unmapped address. That made the machine die at
   * exception entry/exit with a translation fault at exactly heap.end,
   * seemingly at random, since it needed the stack to land at the heap edge. */
  *(u64 *)stack = KSTACK_CANARY;
  u64 stack_top =
      align_down_u64((u64)(usize)stack + KERNEL_STACK_SIZE - 64, 16);
  task->kernel_stack_ptr = stack_top;
#ifdef __x86_64__
  u64 initial_rsp = stack_top - 16;
#elif defined(__aarch64__)
  /* No slot to reserve, and nothing to subtract.
   *
   * The `- 8` below is 32-bit x86's return-address slot. AAPCS64 keeps the
   * return address in x30 rather than on the stack, and requires SP to be
   * 16-byte aligned at every instruction that uses it — so subtracting eight
   * here left SP 8 mod 16 for the entire life of every kernel thread. That is
   * invisible under QEMU, whose SCTLR_EL1.SA comes up clear, and an immediate
   * SP-alignment fault (ESR EC 0x26) on a Cortex-A76, which has that check
   * enabled out of reset — which is how net_task died on the first real
   * arm64 hardware this kernel ran on. */
  u64 initial_rsp = stack_top;
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
  /* arch_context_switch (context_switch.S) restores x30/lr from here and
   * then `ret`s to it — lr IS the entry point on first switch-in, not a
   * stack address. (Previously set to initial_rsp: the very first switch
   * to any newly created aarch64 task `ret`ed into its own freshly
   * kzalloc'd — therefore zeroed — kernel stack and immediately UDF-trapped.) */
  task->context.lr = (u64)(usize)trampoline;
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
  int id = kthread_create_impl(name, entry, arg, ap_worker_trampoline, 1, 0);

  /* Tell the other CPUs there is something to steal. On x86_64 this is a
   * reschedule IPI and mostly redundant — an idle AP is spinning on the
   * runqueues there. On aarch64 it is the SEV that ends the idle CPU's WFE,
   * and without it a CPU that parked before the workers existed sleeps
   * through the whole test: `completed=0`, every worker still on the BSP. */
  if (id >= 0)
    ipi_reschedule_all();
  return id;
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

/* fork(2), plus the two tid side effects Linux's clone(2) can ask for.
 *
 * glibc's fork is clone(CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID|SIGCHLD, NULL,
 * NULL, NULL, &THREAD_SELF->tid) and it relies on the kernel to write the new
 * thread id into the CHILD's copy of that word — "values of TID and PID are set
 * by the kernel", as its own comment puts it. Nothing else does it, so a child
 * whose tid is never written keeps its parent's, and raise()/abort() in that
 * child then signals the parent.
 *
 * The write has to land in the child's address space only, which is why it is
 * done here rather than by the caller: the word is written in the parent BEFORE
 * the copy, so the child inherits it, and the parent's original value is put
 * back AFTER — that second write faults the now-read-only COW page and gives
 * the parent a private copy, leaving the child's untouched. */
int scheduler_fork_clone(u64 flags, u64 parent_tid_addr, u64 child_tid_addr) {
  int pid = scheduler_fork_ctid(child_tid_addr);
  if (pid < 0)
    return pid;
  if ((flags & B1NIX_CLONE_PARENT_SETTID) && parent_tid_addr) {
    u32 v = (u32)pid;
    syscall_copyout((void *)(usize)parent_tid_addr, &v, sizeof(v));
  }
  if (flags & B1NIX_CLONE_CHILD_CLEARTID) {
    struct task *child = scheduler_task_by_pid((usize)pid);
    if (child)
      task_set_child_tid_clear(child, child_tid_addr);
  }
  return pid;
}

int scheduler_fork_current(void) { return scheduler_fork_ctid(0); }

int scheduler_fork_ctid(u64 child_tid_addr) {
  struct task *parent = current_task;
  if (!parent) {
    return -1;
  }
  /* pids.max: a cgroup between the parent and the root may be full. Linux's
   * pids controller fails the fork with EAGAIN, which is what a process that
   * hits RLIMIT_NPROC also sees, so nothing needs a new error path. */
  {
    int cg_err = cgroup_fork_allowed(parent->id);
    if (cg_err < 0)
      return cg_err;
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
  /* The runqueue link is the slot's, never the parent's: copying it would
   * splice the child into the chain the parent sits in (and, when the parent
   * is one hop ahead, into itself). Preserved like the id. */
  struct task *claimed_next_run = child->next_run;

  // 1. Copy the task structure
  memcpy(child, parent, sizeof(struct task));
  child->id = claimed_id;
  child->next_run = claimed_next_run;
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
  *(u64 *)child_stack = KSTACK_CANARY;

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
#elif defined(__aarch64__)
  if (is_user) {
    /* Every EL0->EL1 entry runs EL0_KSTACK_RESET before SAVE_REGS, so the
     * frame sits at a fixed address — the task's kernel_stack_ptr, which the
     * scheduler publishes into this CPU's block on each switch — exactly the
     * way TSS.rsp0 pins it on x86_64. Take the child's copy of that frame,
     * force x0=0 (fork's child return value), and resume there. */
    struct interrupt_frame *child_frame =
        (struct interrupt_frame *)(usize)(child->kernel_stack_ptr -
                                          sizeof(struct interrupt_frame));
    child_frame->x0 = 0;

    extern void aarch64_fork_child_trampoline(void);
    child->context.sp = (u64)(usize)child_frame;
    child->context.fp = 0;
    child->context.lr = (u64)(usize)aarch64_fork_child_trampoline;
  } else {
    /* Aligned explicitly: SP has to be a multiple of 16 on this arch (see the
     * note on initial_rsp above), and the allocator's alignment is not a
     * promise this code should be leaning on. */
    child->context.sp =
        align_down_u64((u64)(usize)child_stack + KERNEL_STACK_SIZE, 16);
    child->context.fp = 0;
    child->context.lr = (u64)(usize)arch_halt;
  }
#endif

  // 3. Clone address space with interrupts disabled
  /* CLONE_CHILD_SETTID: stage the child's id in the parent's copy of the word
   * so the clone carries it into the child, and restore the parent's value
   * once the pages are shared COW (see the comment on scheduler_fork_clone). */
  u32 ctid_saved = 0;
  int ctid_staged = 0;
  if (child_tid_addr) {
    u32 v = (u32)child->id;
    if (syscall_copyin(&ctid_saved, (const void *)(usize)child_tid_addr,
                       sizeof(ctid_saved)) == 0 &&
        syscall_copyout((void *)(usize)child_tid_addr, &v, sizeof(v)) == 0)
      ctid_staged = 1;
  }
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
  /* Every CPU, not only this one. A multithreaded parent has siblings running
   * the same address space on other CPUs, and they cache the same writable
   * translations that were just downgraded. A sibling then writes through its
   * stale entry into the now-shared frame, and when this thread later takes
   * the copy-on-write fault the page is copied away underneath it: two threads
   * of one process end up writing the same address in two different physical
   * pages. That is what corrupted the allocator's bookkeeping and killed sway
   * inside malloc. A local CR3 reload cannot fix what another CPU has cached.
   *
   * The shootdown waits for the other CPUs to acknowledge, so it needs
   * interrupts enabled; with them masked the local reload is all that is safe,
   * and a fork from such a context has no running siblings to worry about. */
  if (parent == current_task) {
    extern void paging_reload_cr3(void);
    extern void tlb_shootdown_all(void);
    if (interrupts_enabled())
      tlb_shootdown_all();
    else
      paging_reload_cr3();
  }

  /* Put the parent's own tid back. The page is read-only now, so this write
   * takes the COW fault and separates the two copies. */
  if (ctid_staged)
    syscall_copyout((void *)(usize)child_tid_addr, &ctid_saved,
                    sizeof(ctid_saved));

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
  /* M109: a fork inherits the parent's namespaces. */
  namespace_fork_inherit(parent->id, child->id);
  /* ...and its cgroup, which is where its resource limits come from. */
  cgroup_fork_inherit(parent->id, child->id);

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
  /* Alarms are the second set of deadlines in this kernel, and the one-shot
   * timer has to see both or it would sleep straight through an alarm(2). */
  sched_note_deadline(ticks);
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
static u64 g_task_entry_rip[TASK_SLOTS];
static u64 g_task_entry_rbp[TASK_SLOTS];

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
/* How far the dump will follow a VMA list before giving up on it.
 *
 * The dump reads live tasks with no lock — that is what makes it usable from a
 * watchdog — so a list it walks may be edited or freed underneath it, and a
 * `next` pointer into reused memory turns the walk into a loop that never
 * ends. One run was lost exactly that way: the console stopped mid-entry and
 * the machine spun at full load for eleven minutes with nothing to show for
 * it. A bound cannot make the walk correct, but it does make it finish. No
 * real process has thousands of mappings alive at once. */
#define SCHED_DUMP_VMA_MAX 4096u

static void sched_name_user_addr(struct task *t, u64 addr) {
  if (!t || !addr)
    return;
  unsigned guard = 0;
  for (struct vm_area *v = t->vma_list; v && guard < SCHED_DUMP_VMA_MAX;
       v = v->next, guard++) {
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
static u64 g_task_syscall[TASK_SLOTS];
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
/* The pidfs inode for a pid: the same number for every pidfd that names this
 * process, and never issued again once the process is gone. Zero when there is
 * no such task. */
u64 scheduler_task_pidfs_ino(usize pid) {
  struct task *t = scheduler_task_by_pid(pid);

  return t ? g_task_pidfs_ino[task_index(t)] : 0;
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
      resident += paging_user_resident(t->pml4_phys, v->start, v->end);
  }
  if (resident > g_task_maxrss_pages[idx])
    g_task_maxrss_pages[idx] = resident;
  return g_task_maxrss_pages[idx];
}

/* The resident set as it is NOW, rather than the peak.
 *
 * getrusage wants the high-water mark, so that is what task_rss_sample folds
 * and returns; /proc/<pid>/status VmRSS wants the current figure, and reporting
 * the peak there would say a process still holds memory it has already given
 * back. Same page-table walk, without the max. */
u64 task_rss_current_pages(struct task *t) {
  if (!t)
    return 0;
  u64 resident = 0;
  if (t->pml4_phys) {
    for (struct vm_area *v = t->vma_list; v; v = v->next)
      resident += paging_user_resident(t->pml4_phys, v->start, v->end);
  }
  usize idx = task_index(t);
  if (resident > g_task_maxrss_pages[idx])
    g_task_maxrss_pages[idx] = resident;
  return resident;
}

/* Total address space the task has mapped, resident or not -- VmSize. */
u64 task_vsize_bytes(const struct task *t) {
  if (!t)
    return 0;
  u64 total = 0;
  for (struct vm_area *v = t->vma_list; v; v = v->next)
    total += v->end > v->start ? v->end - v->start : 0;
  return total;
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
sched_sigwait_notify(T(i), sig);
    /* A waiting task must run to notice it; a running one will see it on its
     * next return to userspace. A tracee already in a ptrace stop stays in it. */
    (void)sched_wake_for_signal_ex(T(i), sig, 0);
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
static u16 g_task_ioprio[TASK_SLOTS];

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
static struct vfs_node *g_task_root_node[TASK_SLOTS];
static char g_task_root_path[TASK_SLOTS][VFS_MAX_PATH];

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
  /* The caller's user registers, handed to the child unchanged apart from a
   * zero return value (see struct clone_user_regs). */
  struct clone_user_regs uregs;
  int have_uregs;
  u64 user_lr;     /* aarch64: caller's x30 (see scheduler_clone_thread) */
#if defined(__aarch64__)
  /* The parent's register frame at the clone(2) syscall. A clone child must
   * inherit the whole register file, not just SP/PC: musl's vfork returns
   * into a caller whose callee-saved registers are live. */
  struct interrupt_frame uframe;
  int have_uframe;
#endif
};

extern void x86_user_jump(usize entry, usize stack, usize argc, usize argv,
                          usize kstack_top);

static void clone_thread_kentry(void *arg) {
  struct clone_thread_args *cta = (struct clone_thread_args *)arg;
  u64 entry = cta->user_entry;
  u64 stack = cta->user_stack;
  u64 user_arg = cta->user_arg;
  u64 start_func = cta->start_func;
  struct clone_user_regs uregs = cta->uregs;
  int have_uregs = cta->have_uregs;
  u64 user_lr = cta->user_lr;
  (void)user_lr;
#if defined(__aarch64__)
  struct interrupt_frame uframe = cta->uframe;
  int have_uframe = cta->have_uframe;
#endif
  kfree(cta);
#ifndef __aarch64__
  if (have_uregs) {
    /* Linux hands the child the caller's register file. Anything else breaks
     * a libc that carries the thread entry point in a register across the
     * syscall, which both glibc's __clone3 (function in %r9, argument in %r8)
     * and its __clone (both on the new stack) do. */
    extern void x86_clone_thread_jump_regs(u64 entry, u64 stack,
                                           const struct clone_user_regs *r);
    sched_acct_leave_kernel();
    x86_clone_thread_jump_regs(entry, stack, &uregs);
  }
#else
  /* aarch64 carries the whole interrupt frame instead (have_uframe below),
   * which is strictly more than this path restores — scheduler_clone_thread
   * never sets have_uregs on this arch. */
  (void)have_uregs;
  (void)uregs;
#endif
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
   * struct that way (the M67 std::thread crash). x86_user_jump drops its 3rd positional
   * (user_arg) into %rdi = start_routine's void* argument. */
  u64 sp = ((u64)stack & ~0xFUL) - 8;
  *(volatile u64 *)(usize)sp = 0; /* return address: threads exit via SYS_EXIT_THREAD, never ret */
  sched_acct_leave_kernel(); /* M86: kernel-time interval ends at the ring-3 jump */
  /* The kernel stack top goes with the jump. This runs part-way down the
   * task's own C frames and erets from there, so SP_EL1 has to be put back
   * explicitly -- by the caller that knows whose task this is, rather than out
   * of a per-CPU slot that is only right until something publishes another. */
  x86_user_jump((usize)entry, (usize)sp, (usize)user_arg, 0,
                (usize)current_task->kernel_stack_ptr);
#elif defined(__aarch64__)
  /* AAPCS64: SP stays 16-byte aligned and there is no return address on the
   * stack (x30 holds it), so no dummy slot to build. x0 carries the argument;
   * for a musl clone that argument is 0, which is also the clone(2) return
   * value the child must see — musl's __clone branches on it and then picks
   * fn/arg back off the new stack itself. */
  {
    extern void aarch64_user_thread_jump(u64 entry, u64 sp, u64 arg, u64 lr,
                                         u64 kstack_top);
    extern void aarch64_eret_frame(struct interrupt_frame *f);
    u64 sp = stack & ~0xFULL;
    sched_acct_leave_kernel();
    if (have_uframe) {
      struct interrupt_frame f __attribute__((aligned(16))) = uframe;
      f.x0 = user_arg; /* clone(2) returns 0 in the child */
      f.sp_el0 = sp;
      f.elr = entry;
      /* CLONE_SETTLS: the thread pointer is a REGISTER on this arch, and only
       * the kernel can hand it to a brand-new thread — musl's __clone passes
       * it in the tls argument and the child then reads `self` straight out of
       * TPIDR_EL0. Without this the child ran on the parent's thread pointer,
       * so pthread_exit updated and woke the MAIN thread's detach_state while
       * the joiner waited on the child's — pthread_join hung forever.
       * (The initial thread sets TPIDR_EL0 itself, which is why the plain
       * exec path needs nothing here.) */
      if (task_tls_base(current_task))
        f.tpidr_el0 = task_tls_base(current_task);
      f.spsr &= 0xF0000000ULL; /* EL0t, IRQs unmasked, keep NZCV */
      /* Return through a frame placed AT the top of this task's kernel stack,
       * not through the local one.
       *
       * aarch64_eret_frame does `mov sp, x0` then RESTORE_REGS, so SP_EL1 ends
       * up sizeof(frame) above wherever the frame sat. From a local that is
       * the middle of a C frame, which is why this path used to be followed by
       * a global stack reset. Put the frame where SAVE_REGS would have put it
       * and the arithmetic lands on the stack top by itself -- the same thing
       * fork's child frame already does. Nothing above this point on the stack
       * is live: this path erets and never returns. */
      struct interrupt_frame *top =
          (struct interrupt_frame *)(usize)(current_task->kernel_stack_ptr -
                                            sizeof(struct interrupt_frame));
      *top = f;
      aarch64_eret_frame(top);
    }
    aarch64_user_thread_jump(entry, sp, user_arg, user_lr,
                             current_task->kernel_stack_ptr);
  }
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

#if defined(__aarch64__)
int scheduler_clone_thread(u64 flags, u64 entry, u64 user_stack, u64 arg,
                           u64 tls, u64 ctid,
                           u64 parent_tid_addr, u64 child_tid_addr,
                           u64 start_func, u64 user_lr,
                           const struct interrupt_frame *pframe) {
#else
int scheduler_clone_thread(u64 flags, u64 entry, u64 user_stack, u64 arg,
                           u64 tls, u64 ctid,
                           u64 parent_tid_addr, u64 child_tid_addr,
                           u64 start_func,
                           const struct clone_user_regs *uregs) {
#endif
  g_has_any_thread = 1;
  struct task *parent = current_task;
  if (!parent) return -EINVAL;
  /* Reject obviously non-canonical user addresses up front; the actual
   * user-mode access still goes through the page-fault path. */
  if (entry == 0 || user_stack == 0) return -EFAULT;
  if (entry >= USER_SPACE_LIMIT ||
      user_stack >= USER_SPACE_LIMIT)
    return -EFAULT;
  {
    int cg_err = cgroup_fork_allowed(parent->id);
    if (cg_err < 0)
      return cg_err;
  }

  struct clone_thread_args *cta = kzalloc(sizeof(*cta));
  if (!cta) return -ENOMEM;
  cta->user_entry = entry;
  cta->user_stack = user_stack;
  cta->user_arg = arg;
  cta->start_func = start_func;
#if defined(__aarch64__)
  cta->user_lr = user_lr;
  cta->have_uframe = pframe ? 1 : 0;
  if (pframe)
    cta->uframe = *pframe;
#else
  if (uregs) {
    cta->uregs = *uregs;
    cta->have_uregs = 1;
  }
#endif

  void *kstack = kmalloc(KERNEL_STACK_SIZE);
  if (!kstack) { kfree(cta); return -ENOMEM; }

  interrupts_disable();
  struct task *child = find_unused_task();
  interrupts_enable();
  if (!child) { kfree(kstack); kfree(cta); return -EAGAIN; }

  /* Bootstrap kernel context — same shape as kthread_create_impl, including
   * the headroom slot below the end of the allocation (see the note there:
   * putting the stack top exactly at the block end faults at exception entry
   * whenever that block happens to be the topmost one in the heap). */
  *(u64 *)kstack = KSTACK_CANARY;
  u64 stack_top =
      align_down_u64((u64)(usize)kstack + KERNEL_STACK_SIZE - 64, 16);
  child->kernel_stack_ptr = stack_top;
#ifdef __x86_64__
  u64 initial_rsp = stack_top - 16;
#elif defined(__aarch64__)
  /* No slot to reserve, and nothing to subtract.
   *
   * The `- 8` below is 32-bit x86's return-address slot. AAPCS64 keeps the
   * return address in x30 rather than on the stack, and requires SP to be
   * 16-byte aligned at every instruction that uses it — so subtracting eight
   * here left SP 8 mod 16 for the entire life of every kernel thread. That is
   * invisible under QEMU, whose SCTLR_EL1.SA comes up clear, and an immediate
   * SP-alignment fault (ESR EC 0x26) on a Cortex-A76, which has that check
   * enabled out of reset — which is how net_task died on the first real
   * arm64 hardware this kernel ran on. */
  u64 initial_rsp = stack_top;
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
#elif defined(__aarch64__)
  child->stack = kstack;
  child->entry = clone_thread_kentry;
  child->arg = cta;
  child->context.sp = initial_rsp;
  child->context.fp = 0;
  child->context.lr = (u64)(usize)kernel_thread_trampoline;
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
    extern void tlb_shootdown_all(void);
    child->pml4_phys = paging_clone_address_space(parent->pml4_phys);
    /* The clone flipped the parent's writable user pages to COW in place, and
     * every CPU that has one of those translations cached must be told —
     * not merely this one.
     *
     * The parent here is a whole process, threads included, and its siblings
     * run on other CPUs against the very same page tables. A sibling that
     * keeps its stale writable entry writes into the now-shared frame; when
     * the address is later copied for this address space, the copy is taken
     * from the frame the sibling is still writing, and from then on the two
     * threads have the same address backed by two different pages. Their
     * atomics no longer see each other, so the allocator's mutual exclusion
     * silently stops working and its bookkeeping is corrupted — a compositor
     * died calling a function pointer that had been overwritten with pixel
     * data from another thread's buffer. This is posix_spawn's path, so it
     * ran every time the compositor started a client.
     *
     * The shootdown waits for the other CPUs to answer and so needs
     * interrupts enabled; with them masked there are no siblings running to
     * worry about and the local reload is what is safe. */
    if (parent == current_task) {
      if (interrupts_enabled())
        tlb_shootdown_all();
      else
        paging_reload_cr3();
    }

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
  /* M109: the child starts in the parent's namespaces, threads included — a
   * thread of a process that unshared is in that process's namespaces. */
  namespace_fork_inherit(parent->id, child->id);
  cgroup_fork_inherit(parent->id, child->id);

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
  usize task_hwm = g_task_hwm;
  for (usize i = 0; i < task_hwm; i++) {
    struct task *t = T(i);
    if (!t || t == current_task) continue;
    if (t->state != TASK_DEAD) continue;
    if (!task_is_thread(t)) continue;
    /* stack_released only says that the task's saved SP no longer names the
     * outgoing CPU's stack. It does not say that another CPU has stopped
     * executing the task: a task can be marked DEAD by that CPU while its
     * exit-side scheduler_yield is still unwinding. Never clear the slot
     * until no CPU identifies it as current. */
    if (task_running_somewhere(t)) continue;
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
    if (t == current_task)
      panic("sched: reaping the task that is running");

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
  usize task_hwm = g_task_hwm;
  for (usize i = 0; i < task_hwm; i++) {
    struct task *t = T(i);
    if (t == current_task) continue;
    if (t->state != TASK_DEAD) continue;
    if (task_is_thread(t)) continue; /* threads: scheduler_reap_dead_threads */
    if (task_running_somewhere(t)) continue;
    still_have = 1;
    if (!__atomic_load_n(&t->stack_released, __ATOMIC_ACQUIRE)) continue;

    int has_live_parent = 0;
    for (usize p = 0; p < task_hwm; p++) {
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
    if (t == current_task)
      panic("sched: reaping the task that is running");

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
  /* An interrupt handler may yield and resume later on this same C frame.
   * IRQ entry is masked, and enabling interrupts before the handler's
   * RESTORE_REGS/eret lets the next timer interrupt nest on top of that live
   * frame. Preserve the caller's state across every return, including the
   * return after arch_context_switch resumes this task. */
  int restore_irqs = interrupts_enabled();
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

    if (restore_irqs)
      interrupts_enable();
    return 0; /* nothing runnable — caller's idle loop may drop the BKL */
  }

  if (old_task->state == TASK_RUNNING) {
    /* Stride Scheduler: increment pass of yielding task by its stride */
    usize old_idx = task_index(old_task);
    int stride = sched_stride_for_nice(g_task_nice[old_idx]);
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
#ifdef __aarch64__
    u64 crsp = new_task->context.sp;
#elif defined(__x86_64__)
    u64 crsp = new_task->context.rsp;
#endif
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
    if (*(u64 *)(usize)lo != KSTACK_CANARY) {
      console_write("sched: kernel stack overflow for pid ");
      console_write_dec((u64)new_task->id);
      console_write(" name=");
      console_write(new_task->name ? new_task->name : "(none)");
      console_write(" stack=0x");
      console_write_hex64(lo);
      console_write("\n");

      /* Name the neighbour, because "overflow" is usually a lie here.
       *
       * A stack grows DOWN toward this word, so its own overflow is one way to
       * clobber it -- but the threads this keeps firing on (m47-input-inject,
       * lkpi-events) have bodies that are a sleep loop and a work queue, and
       * cannot have spent 128 KiB. The other way in is a write running FORWARD
       * off the end of whatever block sits immediately below, and the heap
       * knows what that is. Print the value that replaced the canary too: a
       * small integer, a pointer or ASCII each point somewhere different. */
      console_write("  was=0x");
      console_write_hex64(*(u64 *)(usize)lo);
      console_write("\n");
      kheap_describe(lo, "  this block:");
      kheap_describe(lo - 1, "  block below:");
      {
        extern void kheap_validate(const char *func);
        kheap_validate("kstack-canary");
      }
      panic("kernel stack overflow");
    }
  }

  /* M86: close the outgoing task's CPU-time interval before `current_task`
   * moves on. Reads old_task->state, so it must run before the state of the
   * outgoing task is touched any further. */
  sched_acct_on_switch(old_task);

  /* Nobody else may be running it — and if somebody is, do not switch.
   *
   * The switcher resumes a task by loading its saved RSP and returning through
   * the word it points at. That word is a return address only while the task
   * really is suspended: one still running on another CPU has a context.rsp
   * naming a slot that is part of a live frame, and the `ret` jumps to
   * whatever it holds — the rip that results is a small integer, a system-call
   * argument executed as code.
   *
   * The pickers gate on this too, but they read the per-CPU pointers before
   * the winning CPU has published its own claim, so a narrow window survives
   * them. This is the last point where the question can still be asked, and
   * the answer costs one pass over the online CPUs. Declining to switch is
   * always safe: the task goes back to READY and is picked again once its
   * current CPU really lets go.
   */
  {
    struct percpu *me = get_percpu();
    int claimed_elsewhere = 0;

    for (int c = 0; c < g_max_cpus; c++) {
      struct percpu *pc = get_percpu_n(c);

      if (!pc || pc == me || (struct task *)pc->cur_task != new_task)
        continue;
      claimed_elsewhere = 1;
      break;
    }
    if (claimed_elsewhere) {
      static unsigned reported;

      if (reported < 8) {
        reported++;
        console_write("sched: declined to switch to pid ");
        console_write_dec((u64)new_task->id);
        console_write(" (");
        console_write(new_task->name ? new_task->name : "(none)");
        console_write("): still the current task of another cpu");
        /* Which CPU, which task, and how this CPU could have chosen it.
         *
         * "another cpu" alone cannot distinguish the two ways this happens: a
         * pick that reached across (which would mean the per-CPU resolution or
         * the runqueue handed over a task it should not have), or this CPU's
         * OWN idle task being recorded as current somewhere else. Print every
         * CPU's cur_task and idle_task next to the culprit and the answer is
         * one line of log instead of a run. */
        console_write(" me=");
        console_write_dec(me ? (u64)me->cpu_id : 99);
        console_write(" new=0x");
        console_write_hex64((u64)(usize)new_task);
        console_write(" state=");
        console_write_dec((u64)__atomic_load_n(&new_task->state,
                                               __ATOMIC_ACQUIRE));
        for (int c2 = 0; c2 < g_max_cpus; c2++) {
          struct percpu *pc2 = get_percpu_n(c2);

          if (!pc2)
            continue;
          console_write(" [");
          console_write_dec((u64)c2);
          console_write(" cur=0x");
          console_write_hex64((u64)(usize)pc2->cur_task);
          console_write(" idle=0x");
          console_write_hex64((u64)(usize)pc2->idle_task);
          console_write("]");
        }
        console_write("\n");
      }
      /* Hand it back. This CPU owns the task -- pick_next_task CAS'd it
       * READY -> RUNNING before returning it -- so declining without releasing
       * strands it in RUNNING with no CPU running it, and nothing ever picks it
       * again.
       *
       * Removing this store was tried, on the reasoning that the branch had
       * just established the task is another CPU's current task, so RUNNING is
       * the truthful state and publishing READY over it invites a third
       * claimer. That reasoning is wrong for a simpler reason than the race:
       * whatever another CPU is doing, THIS CPU holds the task, and a holder
       * that declines must release. If the other CPU really is running it, it
       * publishes READY itself on the way out and this store is redundant; if
       * it is not, this store is the only thing that puts the task back in
       * circulation. Leaving it RUNNING lost it for good -- the flag-on suite
       * fell to 914/1/469.
       *
       * (The report below prints every CPU's cur_task, and in one run neither
       * held the declined task: `pid 1170 /opt/busybox/bin/busybox me=1
       * new=0x1000013070 state=1 [0 cur=0x1000000090] [1 cur=0x1000b32920]`.
       * That is suggestive of a racy scan but does not prove it -- the dump is
       * taken several console writes after the decision, so the owner may
       * simply have moved on. It is recorded, not concluded.) */
      __atomic_store_n(&new_task->state, TASK_READY, __ATOMIC_RELEASE);
      /* The outgoing task keeps running: this CPU simply did not switch. It
       * was marked READY a moment ago and may have been enqueued; reclaim it
       * the same way the nothing-runnable path above does, or a picker will
       * chase a runqueue entry for a task that never left this CPU. */
      if (old_task->state == TASK_READY) {
        old_task->state = TASK_RUNNING;
        sched_rq_remove_task(old_task);
      } else {
        old_task->state = TASK_RUNNING;
      }
      if (restore_irqs)
        interrupts_enable();
      return 0;
    }
  }
  /* The invariant this whole hand-off protocol exists to keep: nobody else is
   * running the task we are about to resume.
   *
   * Checked HERE, before this CPU's cur_task is reassigned, so
   * task_running_somewhere() still answers about the other CPUs only -- a task
   * re-picking itself is a legitimate no-op switch and is excluded explicitly.
   *
   * Without it, two CPUs loading one saved context is not a fault: both run on
   * the same kernel stack, overwrite each other's register spills, and the
   * machine dies later somewhere unrelated -- a data abort on a task pointer
   * that reads back as a small integer, in pick_next_task or on an IRQ return,
   * with nothing left to say where it came from. Name it at the switch. */
  if (new_task != old_task && task_running_somewhere(new_task)) {
    console_write("sched: about to resume a task another CPU is running: pid ");
    console_write_dec((u64)new_task->id);
    console_write(" name=");
    console_write(new_task->name ? new_task->name : "(none)");
    console_write(" stack_released=");
    console_write_dec((u64)__atomic_load_n(&new_task->stack_released,
                                           __ATOMIC_ACQUIRE));
    console_write("\n");
    panic("sched: two CPUs claimed one task");
  }


  /* The outgoing task enters its switch-out window HERE, before this CPU's
   * cur_task stops naming it.
   *
   * From this store until arch_context_switch publishes its stack lease, the
   * outgoing task is in the one state nothing else can recognise: it is no
   * longer cur_task on any CPU, its lease is not yet published, and its
   * ->context still holds the values from its PREVIOUS switch-out. A picker
   * that meets it sees exactly what a parked task looks like. That is what
   * sched_handoff_recover acted on -- and a run with b1nix.ap-userspace showed
   * it doing so by name, one second before the machine died:
   *
   *   sched: stale kernel-stack lease (scan) on pid 169 /bin/m14_smoke
   *   EL1-FOREIGN-STACK: pid 0 name=boot sp=0x1000e8ded0 ...
   *                      sp_owner=/bin/m14_smoke/169
   *
   * -- the boot CPU running on m14_smoke's kernel stack, which is the
   * corruption this port has been chasing.
   *
   * It has to be here and not in the `state == TASK_RUNNING` branch above,
   * which is where it was first put and why it did not help: a task that
   * BLOCKS takes neither that branch nor its lease-clear (the block paths
   * clear their own lease before yielding), so every blocking switch left the
   * window unmarked -- and a waker CASing that task BLOCKED->READY inside its
   * prepare/commit window is precisely the case the recovery exists for. Both
   * paths reach this line. */
  if (new_task != old_task) {
    /* THE invariant that makes the whole hand-off protocol work, checked
     * instead of assumed.
     *
     * arch_context_switch is about to WRITE into old_task->context. Another CPU
     * may claim old_task the moment it sees stack_released == 1, and it then
     * READS that same context. So the lease must already be down here -- if it
     * is not, two CPUs are in old_task->context at once, one saving and one
     * loading, and the loader gets a half-written register file. That is
     * exactly the corruption signature this port keeps meeting: a saved slot
     * with one 32-bit half updated and the other stale, which is how a kernel
     * address acquires bit 32.
     *
     * scheduler_yield clears the lease itself only on the RUNNING path; every
     * other state relies on "the block paths clear the lease before yielding",
     * which is an assumption spread across a dozen call sites rather than
     * something the switch enforces. Enforce it here, where it is one line, and
     * name the state and the last clearing site if it was ever violated. */
    if (__atomic_load_n(&old_task->stack_released, __ATOMIC_ACQUIRE)) {
      static volatile int reported;

      if (!__atomic_exchange_n(&reported, 1, __ATOMIC_ACQ_REL)) {
        console_write("LEASE-UP-AT-SWITCH: pid ");
        console_write_dec((u64)old_task->id);
        console_write(" name=");
        console_write(old_task->name ? old_task->name : "(none)");
        console_write(" state=");
        console_write_dec((u64)old_task->state);
        console_write(" last_clear=");
        console_write(g_task_lease_site[task_index(old_task)]
                          ? g_task_lease_site[task_index(old_task)]
                          : "(never)");
        console_write(" incoming=");
        console_write(new_task->name ? new_task->name : "(none)");
        console_write("\n");
      }
      /* Down it goes regardless: this CPU is about to write that context, so
       * the stack is emphatically not released. */
      task_lease_clear(old_task, "switch-enforced");
    }
    __atomic_store_n(&g_task_switching_out[task_index(old_task)], 1,
                     __ATOMIC_RELEASE);
  }

  /* Who this CPU was running immediately before the incoming task.
   *
   * cur_task is reassigned on the next line, but SP still belongs to old_task
   * until arch_context_switch loads the new one -- a real, deliberate window
   * (the publish happens first so no other CPU can claim the incoming task
   * while its context is being loaded). Anything checking "is SP the current
   * task's stack?" in that window sees a legitimate mismatch, which is why
   * EL1-FOREIGN-STACK had to be downgraded from a panic to a one-shot report.
   * Recording the outgoing task lets that check tell the window apart from a
   * genuine overlap, so it can be a panic again. */
  sched_set_prev_task(new_task == old_task ? 0 : old_task);

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

  /* The switch is about to save THIS CPU's stack pointer into old_task's
   * context. If we are not actually on old_task's stack, that write hands one
   * task another's stack for good: the next resume of old_task runs on it, and
   * the failure surfaces later as a wild jump with nothing left to say where
   * it came from. Name it here, where both identities are still known. */
  if (old_task->stack) {
    u64 sp_now;
#ifdef __aarch64__
    __asm__ volatile("mov %0, sp" : "=r"(sp_now));
#else
    __asm__ volatile("movq %%rsp, %0" : "=r"(sp_now));
#endif
    u64 slo = (u64)(usize)old_task->stack;
    u64 shi = slo + KERNEL_STACK_SIZE;
    if (sp_now < slo || sp_now > shi) {
      console_write("sched: switching out pid ");
      console_write_dec((u64)old_task->id);
      console_write(" name=");
      console_write(old_task->name ? old_task->name : "(none)");
      console_write(" while running on somebody else's stack: sp=0x");
      console_write_hex64(sp_now);
      console_write(" own stack=0x");
      console_write_hex64(slo);
      console_write("..0x");
      console_write_hex64(shi);
      console_write(" incoming pid ");
      console_write_dec((u64)new_task->id);
      console_write(" name=");
      console_write(new_task->name ? new_task->name : "(none)");
      console_write("\n");
      panic("sched: context switch on a foreign kernel stack");
    }
  }
#ifdef __aarch64__
  /* The incoming context's resume address, before it is loaded into x30 and
   * ret'd through.
   *
   * The corruption this branch keeps landing on ends as `ret` to a value that
   * was never a return address -- seen as a struct task * (KHEAP_START+0x90,
   * the boot task) and as &g_percpu[1], a per-CPU block pointer. By the time
   * the CPU faults, the frame that held it is gone and the dump names the
   * wrong function. Catch it here, where the task whose context it is and the
   * task handing over are both still known.
   *
   * A context that has never run is exempt: its lr is a trampoline set at
   * creation, which is in text, so nothing legitimate is excluded. */
  {
    extern char __kernel_text_start[], __kernel_text_end[];
    u64 tlo = (u64)(usize)__kernel_text_start;
    u64 thi = (u64)(usize)__kernel_text_end;
    u64 nlr = new_task->context.lr;

    if (nlr && (nlr < tlo || nlr >= thi)) {
      console_write("sched: incoming pid ");
      console_write_dec((u64)new_task->id);
      console_write(" name=");
      console_write(new_task->name ? new_task->name : "(none)");
      console_write(" has context.lr=0x");
      console_write_hex64(nlr);
      console_write(" outside kernel text 0x");
      console_write_hex64(tlo);
      console_write("..0x");
      console_write_hex64(thi);
      console_write(" ctx.sp=0x");
      console_write_hex64(new_task->context.sp);
      console_write(" ctx.fp=0x");
      console_write_hex64(new_task->context.fp);
      console_write(" stack=0x");
      console_write_hex64((u64)(usize)new_task->stack);
      console_write(" outgoing pid ");
      console_write_dec((u64)old_task->id);
      console_write(" name=");
      console_write(old_task->name ? old_task->name : "(none)");
      console_write(" cpu=");
      {
        struct percpu *pc = get_percpu();
        console_write_dec(pc ? (u64)pc->cpu_id : 99);
      }
      console_write("\n");
      panic("sched: incoming context.lr is not a return address");
    }

    /* And the stack it will resume on. A task must come back on its OWN
     * stack: the failure being hunted is the boot/idle task found running
     * pick_next_task on a userspace task's kernel stack, which is this field
     * having become somebody else's. Checked here rather than at the fault,
     * where the frame is already gone and the backtrace names the wrong
     * function. */
    u64 nsp = new_task->context.sp;

    if (new_task->stack && nsp) {
      u64 nlo = (u64)(usize)new_task->stack;
      u64 nhi = nlo + KERNEL_STACK_SIZE;

      if (nsp <= nlo || nsp > nhi) {
        struct task *ow = scheduler_task_owning_stack(nsp);

        console_write("sched: incoming pid ");
        console_write_dec((u64)new_task->id);
        console_write(" name=");
        console_write(new_task->name ? new_task->name : "(none)");
        console_write(" would resume on ctx.sp=0x");
        console_write_hex64(nsp);
        console_write(" outside its own stack 0x");
        console_write_hex64(nlo);
        console_write("..0x");
        console_write_hex64(nhi);
        console_write(" sp_owner=");
        console_write(ow ? (ow->name ? ow->name : "(none)") : "(unowned)");
        console_write("/");
        console_write_dec(ow ? (u64)ow->id : 0);
        console_write(" ctx.lr=0x");
        console_write_hex64(nlr);
        ksym_print(nlr);
        console_write(" outgoing pid ");
        console_write_dec((u64)old_task->id);
        console_write(" name=");
        console_write(old_task->name ? old_task->name : "(none)");
        console_write(" cpu=");
        {
          struct percpu *pc = get_percpu();
          console_write_dec(pc ? (u64)pc->cpu_id : 99);
        }
        console_write("\n");
        panic("sched: incoming context.sp is another task's stack");
      }
    }
  }
#endif

  g_last_switch_tick = scheduler_ticks;
  arch_context_switch(&old_task->context, &new_task->context,
                      &old_task->stack_released);

  /* Switched in: SP now belongs to whoever is running here, so the window in
   * which SP legitimately still names the OUTGOING task is over. */
  sched_set_prev_task(0);

  /* Resumed: this task is executing again, so it is no longer switching out.
   *
   * Clearing it HERE and not only where a picker claims the task is what makes
   * the marker safe to consult. The two claim sites are CAS paths, and not
   * every resume goes through one -- a stealable worker started from
   * aarch64_ap_main's phase-1 loop is switched to directly, and a task
   * re-picking itself never wins a CAS at all -- so tasks accumulated a marker
   * that was never taken down, and sched_handoff_recover then refused to help
   * any of them for the rest of the boot. Measured: with the marker set and
   * never cleared, the ap-userspace suite fell from 1112 passed to 584, with
   * 797 blocked. The window the marker describes ends when execution resumes,
   * and this is that point. */
  __atomic_store_n(&g_task_switching_out[task_index(old_task)], 0,
                   __ATOMIC_RELEASE);
#ifdef __aarch64__
  /* arch_context_switch returns only when this task is resumed. The outgoing
   * task was published before the switch so other CPUs cannot claim it while
   * its context is live; on resume, publish this CPU's task and its
   * per-task architectural state again before any C code observes it. Without
   * this, the resumed stack runs with current_task, SP_EL1's published top,
   * TTBR0 and TLS still naming the task we switched to. */
  current_task = old_task;
  arch_set_kernel_stack(old_task->kernel_stack_ptr);
  paging_switch_address_space(old_task->pml4_phys);
  {
    u64 fsbase = task_tls_base(old_task);
    extern void arch_set_fs_base(u64 base);
    arch_set_fs_base(fsbase);
  }
  task_fpu_restore(old_task);
#endif
  if (restore_irqs)
    interrupts_enable();
  return 1; /* we switched out and have since been resumed */
}

void scheduler_block_current(void) {
  /* Preserve the caller's interrupt state, the way scheduler_yield does.
   *
   * This used to unmask unconditionally on the way out, so a caller that
   * already had interrupts masked -- an IRQ handler, or any section that
   * disabled them for a reason -- got them back on behind its own back. The
   * next interrupt then nested on top of a live frame, which is exactly the
   * hazard the comment at the top of scheduler_yield describes: the handler's
   * RESTORE_REGS/eret is still pending on that stack, and a frame the compiler
   * believes it owns gets written through. It surfaced as a `ret` into a
   * garbage x30 -- a kernel .bss address, a heap address, a small integer --
   * with a backtrace that says only sync_el1, in whichever test happened to be
   * running. */
  int restore_irqs = interrupts_enabled();

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
  current_task->wait_chan = 0;
  task_lease_clear(current_task, __func__);
  current_task->state = TASK_BLOCKED;
  scheduler_yield();
  if (restore_irqs)
    interrupts_enable();
}

void scheduler_wake_task(usize task_id) {
  int woke = 0;
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
      sched_wake_enqueue(T(i));
      woke = 1;
      break;
    }

    expected = TASK_SLEEPING;
    if (__atomic_compare_exchange_n(&T(i)->state, &expected, TASK_READY,
                                    0, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
      sched_wake_enqueue(T(i));
      woke = 1;
      break;
    }
  }

  interrupts_restore(flags);
  /* Tell the other CPUs there is work rather than letting them find it on
   * their next tick. An ordinary userspace task is not stealable, so a wake
   * puts it on the global runqueue, where a CPU sitting in `sti; hlt` picks it
   * up only at its next 10 ms LAPIC tick. Every blocking wait — futex, pipes,
   * sockets — paid that. The other make-runnable paths already kick; this one,
   * the hottest, did not. */
  if (woke)
    ipi_reschedule_all();
}

void scheduler_block_on(void *chan) {
  /* Caller's interrupt state is preserved — see scheduler_block_current. */
  int restore_irqs = interrupts_enabled();

  interrupts_disable();

  if (current_task == 0) {
    panic("scheduler_block_on without running task");
  }

  if (current_task->state == TASK_READY) {
    current_task->state = TASK_RUNNING;
    current_task->wait_chan = 0;
    if (restore_irqs)
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
  if (restore_irqs)
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
  /* Caller's interrupt state is preserved — see scheduler_block_current. */
  int restore_irqs = interrupts_enabled();

  interrupts_disable();

  if (current_task == 0) {
    panic("scheduler_block_on_timeout without running task");
  }

  if (current_task->state == TASK_READY) {
    current_task->state = TASK_RUNNING;
    current_task->wait_chan = 0;
    current_task->wake_tick = 0;
    if (restore_irqs)
      interrupts_enable();
    return;
  }

  if (current_task->state == TASK_BLOCKED || current_task->state == TASK_SLEEPING) {
    current_task->state = TASK_RUNNING;
  } else if (current_task->state != TASK_RUNNING) {
    panic("scheduler_block_on_timeout without running task");
  }

  current_task->wait_chan = chan;
  if (timeout_ticks) {
    current_task->wake_tick = scheduler_ticks + timeout_ticks;
    sched_note_deadline(current_task->wake_tick);
  }
  /* M28 T4: claim the stack lease before publishing BLOCKED — see
   * scheduler_block_current for the full race. */
  task_lease_clear(current_task, __func__);
  current_task->state = TASK_BLOCKED;
  scheduler_yield();
  /* Drop any unfired deadline: an explicit wake may have resumed us early. */
  current_task->wake_tick = 0;
  if (restore_irqs)
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
  if (timeout_ticks) {
    current_task->wake_tick = scheduler_ticks + timeout_ticks;
    sched_note_deadline(current_task->wake_tick);
  }
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
  /* "May I give up the CPU and expect to get it back?" -- which is what every
   * caller uses this for, driver completion loops above all. A task that is not
   * RUNNING must answer no: scheduler_yield republishes READY only for a
   * RUNNING task, so yielding while BLOCKED switches the task out for good, and
   * a loop that was relying on being rescheduled to check its own timeout never
   * checks it again.
   *
   * Reachable from more places than it looks: scheduler_yield's prologue runs
   * the thread and zombie reapers, and those close file handles, which write
   * back through the filesystem to the block device -- all inheriting whatever
   * state the caller had, including the BLOCKED a wait_prepare just published.
   * Callers spin instead, bounded by their own timeouts, which is survivable
   * where being stranded is not. */
  return scheduler_started && current_task != 0 && interrupts_enabled() &&
         current_task->state == TASK_RUNNING;
}

/* The waker does not touch the woken task's wait_chan or wake_tick.
 *
 * Both belong to the task that is blocking, and it writes them on the way into
 * every wait. A waker that clears them after its CAS is racing the task it just
 * released: by the time the store lands, that task can already be running on
 * another CPU and back inside its wait loop with a fresh channel published —
 * and the stale zero then erases it. A task blocked on a channel nobody can
 * name is never woken again, which is how a compositor parked in epoll_wait
 * stopped answering its IPC socket while still, from outside, perfectly alive.
 * The same store applied to wake_tick strands a sleeper with no deadline. */
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
    struct task *t = T(i);
    if (!t || t->wait_chan != chan) continue;
    enum task_state expected = TASK_BLOCKED;
    if (__atomic_compare_exchange_n(&t->state, &expected, TASK_READY,
                                    0, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
      sched_wake_enqueue(t);
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
  (void)scheduler_sleep_ticks_state(ticks, 1);
}

/*
 * Sleep, and say what happened instead of insisting.
 *
 * A caller that sleeps in a loop -- a nanosleep finishing its deadline after an
 * early wake -- cannot decide beforehand that it is still allowed to sleep: it
 * reads its own state, and another CPU stops or kills it before the call lands.
 * Checking outside and calling a function that requires the answer is a race by
 * construction, so the decision belongs here, under the same interrupt
 * disable that acts on it.
 *
 * Returns SLEEP_OK when it slept, SLEEP_RETRY when the task must not sleep now
 * but still exists (stopped by a signal, or blocked on a channel -- the caller
 * yields and asks again, so a sleep survives SIGSTOP/SIGCONT rather than
 * returning short), and SLEEP_GONE when the task is being torn down.
 *
 * `strict` keeps the original contract for every other caller: a state that
 * cannot sleep is a bug there, and panics naming the state rather than leaving
 * one line in the log and a rebuild with printf to find it.
 */
int scheduler_sleep_ticks_state(u64 ticks, int strict) {
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
  if (current_task->state == TASK_SLEEPING || current_task->state == TASK_READY) {
    /* SLEEPING: a yield that found nothing else to run left our own state
     * behind (see below). READY: a waker on another CPU promoted us while we
     * were still executing here — it happens on any path that sleeps more than
     * once, which a sleep that keeps going until its deadline does. Both mean
     * the same thing at this point: the task is on this CPU and about to sleep
     * again, so put the state back and carry on. */
    current_task->state = TASK_RUNNING;
  } else if (current_task->state != TASK_RUNNING) {
    /* A different fault from "no task at all": a task that is on a wait
     * channel, stopped, or already dying and asked to sleep anyway. */
    int st = current_task->state;

    if (!strict) {
      interrupts_enable();
      return (st == TASK_DEAD || st == TASK_REAPING) ? SLEEP_GONE : SLEEP_RETRY;
    }
    static const char *const names[] = {"RUNNING", "READY",   "BLOCKED",
                                        "SLEEPING", "STOPPED", "DEAD"};
    klog_warn("scheduler_sleep_ticks: task is not running");
    klog_warn(st >= 0 && st < (int)(sizeof(names) / sizeof(names[0]))
                  ? names[st]
                  : "state out of range");
    panic("scheduler_sleep_ticks with a task that is not running");
  }

  current_task->wake_tick = scheduler_ticks + ticks;
  sched_note_deadline(current_task->wake_tick);
  /* A timed sleep waits on the clock, not on a channel. Leaving the previous
   * wait's channel in place invites a wake meant for that channel to cut this
   * sleep short, now that wakers no longer clear it. */
  current_task->wait_chan = 0;
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
      interrupts_enable_and_wait();
    }
  }
  current_task->state = TASK_RUNNING;
  return SLEEP_OK;
}

/* Silence watchdog (test mode only). A wedged instance prints nothing, so the
 * host harness can only report "stalled" and every later marker in that run
 * reads as missing. Dump the task table from inside the guest instead: state,
 * wait channel, fd-lock holder and lease site for every task is usually enough
 * to name the deadlock without a second, instrumented run. Dumps at most three
 * times IN A ROW so the host's own stall timer still gets to kill a hopeless
 * instance — the budget is per stall, not per boot. It used to be per boot, and
 * three early stalls that the instance recovered from spent it: the wedge that
 * actually ended the run (m32_smoke's TLS section, which costs every check
 * after it in the lane) then produced no dump at all and had to be chased
 * blind. Output resuming means the instance moved on, so the budget resets. */
/* 60 seconds of silence, in ticks. NOT a constant: the scheduler tick has been
 * programmed at 1 kHz since the LAPIC timer took it over, and the 100 that used
 * to be written here made this a SIX second watchdog -- short enough that an
 * ordinary slow test (a bounded probe loop, a TLS handshake) read as a wedge and
 * took the rest of the lane down with it. sched_tick_hz() reports what the timer
 * was actually armed with. */
#define SILENCE_WATCHDOG_TICKS (60ull * sched_tick_hz())
#define SILENCE_WATCHDOG_MAX_DUMPS 2
extern volatile u64 g_console_write_seq;

/* Ticks the clock advanced without this vCPU running. See the timer tick. */
static u64 g_stolen_ticks;

u64 sched_stolen_ticks(void) { return __atomic_load_n(&g_stolen_ticks, __ATOMIC_RELAXED); }

static const char *task_state_name(enum task_state st) {
  switch (st) {
  case TASK_RUNNING:  return "RUNNING";
  case TASK_READY:    return "READY";
  case TASK_BLOCKED:  return "BLOCKED";
  case TASK_SLEEPING: return "SLEEPING";
  case TASK_STOPPED:  return "STOPPED";
  case TASK_DEAD:     return "DEAD";
  case TASK_REAPING:  return "REAPING";
  default:            return "UNKNOWN";
  }
}

/* Silence is measured in time this machine was actually given.
 *
 * It used to be measured in wall time, and that made the watchdog fire on a
 * healthy guest whenever the HOST was busy: running the suite four lanes at a
 * time instead of three put 90 checks into BLOCKED and panicked two lanes with
 * "deadlock or hang detected", when nothing had deadlocked at all -- each vCPU
 * was simply getting a quarter of a core, so sixty seconds of wall clock held
 * fifteen seconds of guest.
 *
 * Rule 3 in docs/build-conventions.md: nothing here bounds a wait in wall time.
 * Subtracting the stolen ticks keeps the check honest in both directions. A
 * machine that is genuinely wedged still takes its timer interrupts, still
 * accrues no steal, and is still caught on the same budget. A machine that is
 * merely starved is no longer accused of a deadlock it does not have. */
static void serial_silence_watchdog(void) {
  static u64 last_seq;
  static u64 last_change_tick;
  static u64 last_change_steal;
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
    last_change_steal = g_stolen_ticks;
    dumps = 0;
    return;
  }
  {
    u64 elapsed = scheduler_ticks - last_change_tick;
    u64 stolen = g_stolen_ticks - last_change_steal;

    if (elapsed - (stolen < elapsed ? stolen : elapsed) <
        SILENCE_WATCHDOG_TICKS)
      return;
  }
  last_change_tick = scheduler_ticks;
  last_change_steal = g_stolen_ticks;
  dumps++;
  /* Before the dump: it lands in the same ring and would otherwise be all the
   * tail has left in it. */
  kmsg_capture_tail();

  console_write("\nSMOKE-GUEST-WATCHDOG: no console output for 60s — task dump:\n");
  scheduler_dump_tasks();

  /* The same answer in one line per task, printed after the dump rather than
   * before it.
   *
   * The dump is longer than a phone panel is tall, so on a board whose console
   * is that panel everything worth reading scrolls off the top the moment it
   * runs — including the marker naming the test that stalled. What lands last
   * is what survives, so the short form goes here: who is alive, what state
   * they are in, and what they are waiting on. */
  /* Replayed before the alive list, not after: both are short enough to share
   * the panel, and this one is the longer of the two. */
  kmsg_print_captured();

  /* How close the boot task came to the end of its stack. It is the one stack
   * with no canary on it -- the check below is guarded by task->stack, which is
   * 0 for a region that is not a heap block -- so this figure, read late, is
   * the only evidence that it did or did not overflow. */
  {
    extern u64 boot_stack_peak_bytes(void);
    extern u64 boot_stack_size_bytes(void);
    u64 total = boot_stack_size_bytes();
    u64 peak = boot_stack_peak_bytes();

    console_write("SMOKE-GUEST-WATCHDOG: boot stack peak=");
    console_write_dec(peak);
    console_write(" of ");
    console_write_dec(total);
    if (total)
      {
        console_write(" (");
        console_write_dec(peak * 100 / total);
        console_write("%)");
      }
    console_write("\n");
  }

  /* Is the machine's clock still running? Everything above sleeps on
   * scheduler_ticks, which only the boot CPU advances; if it has stopped, every
   * sleep(2) hangs for ever, wake_sleepers never runs, and the system looks
   * alive (interrupts, context switches, console) while making no progress --
   * exactly the shape of this stall. The hardware counter in the line prefix is
   * independent of it, so the two together say which of the machine and the
   * tick stopped. */
  {
    console_write("SMOKE-GUEST-WATCHDOG: scheduler_ticks=");
    console_write_dec(scheduler_ticks);
    console_write(" (");
    console_write_dec(scheduler_ticks / sched_tick_hz());
    console_write("s of ticks) last_switch_tick=");
    console_write_dec(g_last_switch_tick);
    console_write("\n");
  }
  /* For every READY task, the reason the picker will not take it. A stall where
   * runnable tasks are never scheduled and the idle task runs for ever is
   * always one of these gates saying no; without them printed, "3 tasks READY,
   * boot RUNNING, clock healthy" is a dead end. */
  {
    {
    console_write("SMOKE-GUEST-WATCHDOG: picks calls=");
    console_write_dec(g_pick_calls);
    console_write(" rq=");
    console_write_dec(g_pick_rq);
    console_write(" scan=");
    console_write_dec(g_pick_scan);
    console_write(" idle=");
    console_write_dec(g_pick_idle);
    console_write(" none=");
    console_write_dec(g_pick_null);
    console_write(" last=");
    console_write_dec((u64)g_last_pick_id);
    console_write("\n  scan seen=");
    console_write_dec(g_scan_seen);
    console_write(" best_id=");
    console_write_dec((u64)g_scan_best_id);
    console_write(" rej_lease=");
    console_write_dec(g_scan_rej_lease);
    console_write(" rej_running=");
    console_write_dec(g_scan_rej_running);
    console_write(" rej_cas=");
    console_write_dec(g_scan_rej_cas);
    console_write(" boot_pass=");
    console_write_dec(g_task_pass[0]);
    console_write("\n  current=");
    console_write_dec(current_task ? (u64)current_task->id : 9999);
    console_write(" state=");
    console_write_dec(current_task ? (u64)current_task->state : 99);
    console_write("\n");
  }
  console_write("SMOKE-GUEST-WATCHDOG: why READY tasks are not picked:\n");
    for (usize i = 0; i < g_task_hwm; i++) {
      struct task *t = T(i);

      if (!t || t->state != TASK_READY)
        continue;
      console_write("  pid ");
      console_write_dec((u64)t->id);
      console_write(" '");
      console_write(t->name ? t->name : "(none)");
      console_write("' lease=");
      console_write_dec((u64)__atomic_load_n(&t->stack_released,
                                             __ATOMIC_ACQUIRE));
      console_write(" stealable=");
      console_write_dec((u64)t->stealable);
      console_write(" ap_runnable=");
      console_write_dec((u64)t->ap_runnable);
      console_write(" affinity=0x");
      console_write_hex64(g_task_affinity[task_index(t)]);
      console_write(" running_somewhere=");
      console_write_dec((u64)task_running_somewhere(t));
      console_write(" allowed_on_cpu0=");
      console_write_dec((u64)sched_task_allowed_on_cpu(t, 0));
      console_write(" prio=");
      console_write_dec((u64)t->priority);
      console_write(" pass=");
      console_write_dec(g_task_pass[task_index(t)]);
      console_write("\n");
    }
  }
  {
    extern u64 g_nvme_io_submits, g_nvme_io_completions;
    console_write("SMOKE-GUEST-WATCHDOG: nvme submits=");
    console_write_dec(g_nvme_io_submits);
    console_write(" completions=");
    console_write_dec(g_nvme_io_completions);
    console_write("\n");
    if (g_nvme_io_submits != g_nvme_io_completions) {
      extern void nvme_debug_dump(void);
      nvme_debug_dump();
    }
  }
  console_write("SMOKE-GUEST-WATCHDOG: alive tasks (pid state chan comm):\n");
  for (usize i = 0; i < g_task_hwm; i++) {
    struct task *t = T(i);

    if (t->state == TASK_UNUSED || t->state == TASK_DEAD ||
        t->state == TASK_REAPING)
      continue;
    console_write("  ");
    console_write_dec(t->id);
    console_write(" ");
    console_write(task_state_name(t->state));
    console_write(" 0x");
    console_write_hex64((u64)(usize)t->wait_chan);
    console_write(" ");
    console_write(t->name);
    /* WHERE it is parked, which is the one thing every dump so far has left
     * out. A task that is not on a CPU has its resume point on top of the
     * saved context.rsp and its frame chain in context.rbp, so a few frames of that chain say which
     * kernel call each task stopped inside -- the difference between "blocked
     * in poll" and "blocked in the socket receive path" is the whole
     * investigation. Every dereference is bounds-checked against the task's own
     * stack, so a corrupt frame pointer prints nothing instead of faulting the
     * dump. */
    if (t != current_task && t->stack) {
      u64 lo = (u64)(usize)t->stack;
      u64 hi = lo + KERNEL_STACK_SIZE;
#if defined(__aarch64__)
      u64 fp = t->context.fp;

      console_write("\n      at 0x");
      console_write_hex64(t->context.lr);
      ksym_print(t->context.lr);
#else
      /* x86_64 has no link register, but the resume address is not lost: it is
       * the return address arch_context_switch will pop, i.e. the word at the
       * saved RSP. Bounds-checked against this task's own stack, so a corrupt
       * context prints nothing instead of faulting the dump. */
      u64 fp = t->context.rbp;
      u64 sp = TASK_CTX_SP(t);
      u64 pc = 0;

      if (sp >= lo && sp + 8 <= hi && !(sp & 7))
        pc = *(u64 *)(usize)sp;

      console_write("\n      at 0x");
      console_write_hex64(pc);
      ksym_print(pc);
#endif
      for (int d = 0; d < 14; d++) {
        u64 *frame;

        if (fp < lo || fp + 16 > hi || (fp & 7))
          break;
        frame = (u64 *)(usize)fp;
        console_write("\n       <- 0x");
        console_write_hex64(frame[1]);
        ksym_print(frame[1]);
        fp = frame[0];
      }
    }
    console_write("\n");
  }

  /* Where PID 1 went.
   *
   * The alive list above skips UNUSED/DEAD/REAPING, so an init that is missing
   * from it says only "not running" — and "never scheduled", "exited" and
   * "already reaped" need different fixes. This looks the slot up by id and
   * reports whatever state it is in, including the states the list hides, or
   * says plainly that no slot carries that id any more. */
  {
    usize init_pid = scheduler_get_init_pid();
    struct task *found = 0;

    for (usize i = 0; i < g_task_hwm && !found; i++)
      if (T(i)->id == init_pid && T(i)->state != TASK_UNUSED)
        found = T(i);

    console_write("SMOKE-GUEST-WATCHDOG: init pid=");
    console_write_dec((u64)init_pid);
    if (found) {
      console_write(" state=");
      console_write(task_state_name(found->state));
      console_write(" comm=");
      console_write(found->name);
    } else {
      console_write(" has no live task slot (hwm=");
      console_write_dec((u64)g_task_hwm);
      console_write(")");
    }
    console_write("\n");
  }

  /* What the boot settled on, reprinted where it is still readable.
   *
   * Both facts are announced when they happen, near the top of the boot -- and
   * on a board whose only console is a scrolling panel that is exactly where
   * nobody can see them by the time something goes wrong. "root=initramfs" is
   * the entire diagnosis for a machine that came up with none of its userspace:
   * the real root never mounted, so PID 1 came out of the embedded file set and
   * every service that lives in the image was never there to start. */
  {
    extern const char *boot_summary_root(void);
    extern const char *boot_summary_init(void);

    /* How much of that silence was this machine not being given the CPU. A
     * large number here means the host was oversubscribed and the dump below
     * is unlikely to show a real wedge. */
    console_write("SMOKE-GUEST-WATCHDOG: stolen-ticks ");
    console_write_dec(g_stolen_ticks);
    console_write(" of ");
    console_write_dec(scheduler_ticks);
    console_write("\n");

    console_write("SMOKE-GUEST-WATCHDOG: root=");
    console_write(boot_summary_root());
    console_write(" init=");
    console_write(boot_summary_init());
    console_write("\n");
  }

  /* Sentinel. Everything above can be pushed off the top of a phone panel, and
   * "the new dump did not print" and "the phone is running the old build" look
   * identical from the outside. This line is the last thing the watchdog says,
   * so its absence is an answer rather than a guess. */
  console_write("SMOKE-GUEST-WATCHDOG: end (dump ");
  console_write_dec((u64)dumps);
  console_write(" of ");
  console_write_dec((u64)SILENCE_WATCHDOG_MAX_DUMPS);
  console_write(")\n");

  last_seq = g_console_write_seq;

  if (dumps >= SILENCE_WATCHDOG_MAX_DUMPS) {
    panic("watchdog: deadlock or hang detected (silence for >120s)");
  }
}


void scheduler_on_timer_tick(void) {
  /* Cheapest possible net over the whole kernel: if this CPU's cur_task ever
   * stops being a task, every `current_task->field = ...` in the tree is
   * writing into whatever it points at instead. Checked once per tick per CPU,
   * which bounds the damage to one tick rather than to whenever the wreckage
   * finally faults somewhere unrelated. */
  if (!scheduler_started || current_task == 0) {
    return;
  }

  task_chunks_verify("timer-tick");

  /* The tick counter follows the CLOCK, not the number of timer interrupts
   * that were delivered.
   *
   * A periodic LAPIC timer latches one pending interrupt, no more: every
   * window this kernel spends with interrupts masked, and every window KVM
   * spends not running the vCPU, costs whole ticks. Measured under KVM at
   * 1 kHz, about a third of them never arrived -- so every sleep, timeout and
   * alarm built on this counter ran about half as long again as it asked for,
   * while the log timestamps (which read the TSC) disagreed with it by the
   * same factor.
   *
   * So: advance to where the monotonic clock says we are, and fall back to a
   * plain increment when it cannot be ahead -- which is exactly the case
   * before the TSC handover, when that clock is itself derived from this
   * counter. */
  {
    u64 ns_per_tick = 1000000000ull / sched_tick_hz();
    u64 want = ns_per_tick ? ktime_monotonic_ns() / ns_per_tick : 0;

    if (want > scheduler_ticks) {
      /* The size of that jump is time this vCPU did not get to run.
       *
       * The clock moved and we did not: either the host was running somebody
       * else, or this kernel sat with interrupts masked. Counting it is what
       * lets the hang watchdog tell "wedged" from "starved" -- see
       * serial_silence_watchdog. Gaps of one tick are ordinary rounding and
       * are not counted; real starvation arrives in tens. */
      u64 jump = want - scheduler_ticks;

      if (jump > 2)
        g_stolen_ticks += jump - 1;
      scheduler_ticks = want;
    } else {
      scheduler_ticks++;
    }
  }

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

  /* Per-CPU block integrity. Every writer of cur_task is probed and none of
   * them ever publishes another CPU's idle task, yet a dump caught g_percpu[0]
   * holding g_ap_idle_tasks[1] -- with -smp 2 there is exactly one such task,
   * so both CPUs were pointing at one struct and one kernel stack. Nobody wrote
   * it there, which makes it corruption of the per-CPU array itself. Catch it
   * on the tick, where "when" and "what it became" are still answerable. */
  {
    static volatile int pcpu_reported;

    for (int c = 0; c < g_max_cpus; c++) {
      struct percpu *pc = get_percpu_n(c);
      struct task *ct = pc ? (struct task *)pc->cur_task : 0;
      int owner;

      if (!pc || !pc->cpu_online || !ct)
        continue;
      owner = sched_ap_idle_cpu_of(ct);
      if (owner == c || (owner < 0 && sched_task_ptr_valid(ct)))
        continue; /* its own idle task, or a real table slot */
      if (__atomic_exchange_n(&pcpu_reported, 1, __ATOMIC_ACQ_REL))
        break;
      console_write("sched: percpu[");
      console_write_dec((u64)c);
      console_write("].cur_task is 0x");
      console_write_hex64((u64)(usize)ct);
      console_write(" at tick ");
      console_write_dec(scheduler_ticks);
      console_write(" — ");
      if (owner >= 0) {
        console_write("that is cpu ");
        console_write_dec((u64)owner);
        console_write("'s idle task");
      } else {
        console_write("not a task slot at all");
      }
      console_write(", name='");
      console_write(ct->name ? ct->name : "(none)");
      console_write("' stack=0x");
      console_write_hex64((u64)(usize)ct->stack);
      console_write("\n");
      scheduler_dump_tasks();
      break;
    }
  }

  /* Stall detector. This kernel had none, and the cost of that is a lane that
   * goes silent for the harness's whole 180 s window and says nothing at all
   * about why -- the difference between "the timer died" and "everything is
   * blocked on a lost wake-up" is invisible from outside. A healthy system
   * switches tasks many times a second, so this stays quiet; when it does fire
   * the machine is already wedged, so dumping from the tick costs nothing.
   * Reported at most once a minute so a wedge cannot bury its own evidence. */
  {
    static u64 last_report_tick;
    u64 quiet = scheduler_ticks - g_last_switch_tick;

    if (quiet > 30ull * sched_tick_hz() &&
        (last_report_tick == 0 ||
         scheduler_ticks - last_report_tick > 60ull * sched_tick_hz())) {
      last_report_tick = scheduler_ticks;
      console_write("sched: STALL — no context switch for ");
      console_write_dec(quiet / sched_tick_hz());
      console_write("s (tick ");
      console_write_dec(scheduler_ticks);
      console_write(", current '");
      console_write(current_task->name ? current_task->name : "(none)");
      console_write("'). The timer is alive; every task below is where it "
                    "stopped.\n");
      scheduler_dump_tasks();
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

  /* Diagnostic poke, off unless asked for.
   *
   * A stall that ends the moment anything unrelated happens — a signal, a task
   * dump — is a lost wake-up, and the question is which side lost it. Waking
   * every poller ten times a second answers it: if the run then proceeds, the
   * wake that went missing was one that should have reached a poll/epoll
   * sleeper, and the hunt belongs in the code that fails to announce readiness
   * rather than in the futex path. `b1nix.poll-poke`. */
  {
    static int poke = -1;

    if (poke < 0)
      poke = bootinfo_has_flag("b1nix.poll-poke") ? 1 : 0;
    if (poke && (scheduler_ticks % 10) == 0) {
      extern void *vfs_poll_chan;

      scheduler_wake_all(vfs_poll_chan);
    }
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
#if defined(__x86_64__)
  u32 lo, hi;
  __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));
  return ((u64)hi << 32) | lo;
#else
  /* AArch64's equivalent free-running counter, same contract: a monotonic
   * cycle-ish stamp whose rate arch_cpu_khz() reports. */
  u64 cnt;
  __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(cnt));
  return cnt;
#endif
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

/* PR_SET_NAME: a task's comm, when it has chosen one.
 *
 * comm starts as the basename of what was executed, but a process may rename
 * itself and readers believe the new name — systemd renames PID 1 to "systemd"
 * (it is exec'd as /sbin/init), and anything looking at /proc/1/comm to decide
 * which init is running saw "init". Kept in a side table because struct task
 * must not grow. */
#define TASK_COMM_MAX 64
static struct {
  usize pid;
  char name[16];
} g_comm[TASK_COMM_MAX];

int scheduler_set_comm(usize pid, const char *name) {
  if (!name)
    return -EINVAL;
  int free_slot = -1;
  for (int i = 0; i < TASK_COMM_MAX; i++) {
    if (g_comm[i].pid == pid) {
      free_slot = i;
      break;
    }
    if (free_slot < 0 && g_comm[i].pid == 0)
      free_slot = i;
  }
  if (free_slot < 0)
    free_slot = (int)(pid % TASK_COMM_MAX); /* reuse rather than refuse */
  usize j = 0;
  for (; name[j] && j < sizeof(g_comm[0].name) - 1; j++)
    g_comm[free_slot].name[j] = name[j];
  g_comm[free_slot].name[j] = '\0';
  g_comm[free_slot].pid = pid;
  return 0;
}

const char *scheduler_comm_override(usize pid) {
  for (int i = 0; i < TASK_COMM_MAX; i++)
    if (g_comm[i].pid == pid)
      return g_comm[i].name;
  return 0;
}

void scheduler_clear_comm_internal(usize pid) {
  for (int i = 0; i < TASK_COMM_MAX; i++)
    if (g_comm[i].pid == pid) {
      g_comm[i].pid = 0;
      g_comm[i].name[0] = '\0';
      return;
    }
}

int scheduler_get_pdeathsig(usize pid) {
  for (int i = 0; i < PDEATHSIG_MAX; i++)
    if (g_pdeathsig[i].pid == pid)
      return g_pdeathsig[i].signo;
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

/* Release what a dying task registered, on the paths that do not run
 * scheduler_exit_current.
 *
 * A task killed by SIGKILL -- which is what exit_group posts to every sibling
 * -- or by a fatal default action is marked DEAD from inside the scheduler,
 * and every resource it holds is released by hand right there. Two were
 * missing from that list, and both are the same shape: state kept in a table
 * keyed by the task, which a recycled task slot then inherits.
 *
 * The address-space mutator lock: a thread killed inside mmap/munmap left the
 * slot set for ever. It is shared by hash across address spaces, so the
 * casualty was not only that process -- every other one hashing the same way
 * spun in vma_mutator_lock until the machine went quiet, with no panic and
 * nothing in the log but one warning naming an owner whose task slot had since
 * been freed and reused.
 *
 * The rseq(2) area: glibc registers one for every thread it creates and treats
 * a refusal as fatal ("Fatal glibc error: rseq registration failed"). A leaked
 * entry both consumes a table slot for ever and, once the task slot is reused,
 * makes the new thread's first registration look like a conflicting
 * re-registration of a different area -- so the thread dies at start-up.
 */
static void release_task_registrations(struct task *t) {
  extern void syscall_release_vma_locks(struct task *t);

  syscall_release_vma_locks(t);
  rseq_task_cleanup(t);
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
sched_sigwait_notify(sibling, SIGKILL);
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
        sched_wake_enqueue(sibling);
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
  EXIT_STAGE(10);
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
  EXIT_STAGE(1);
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
        {
          static unsigned reported;

          if (reported < 8) {
            reported++;
            console_write("exit: parked leader pid ");
            console_write_dec((u64)me);
            console_write(" (");
            console_write(current_task->name ? current_task->name : "?");
            console_write(") reports SIGKILL instead of its own exit code ");
            console_write_dec((u64)(unsigned)exit_code);
            console_write("; override=");
            console_write_dec((u64)g_task_parked_override[idx]);
            console_write("\n");
          }
        }
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
      EXIT_STAGE(2);
      scheduler_block_on_timeout(&g_task_parked_leader[idx], 20);
    }
    g_task_parked_leader[idx] = 0;
  }
  EXIT_STAGE(3);
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
      /* Hand the time over, do not copy it.
       *
       * task_group_cputime_ns walks every slot that is not TASK_UNUSED, and a
       * thread that has exited sits at TASK_DEAD until something reaps it --
       * so between this fold and that reap its time was counted twice, once
       * from its own row and once from the leader's `gone` row, and then the
       * total DROPPED by that amount when the slot was finally freed. A
       * process's CPU time going backwards is wrong on its own; what it looked
       * like from userspace was M86's rusage-self-group failing intermittently
       * with the group's user time (146 ms) short of the sum of its two
       * threads, and its SYSTEM time (2 ms) below the calling thread's own
       * (3 ms) -- a total that cannot include the thread reading it. Whether
       * the reap landed between two getrusage calls is pure timing, which is
       * why it came and went with host load. */
      g_task_utime_ns[idx] = 0;
      g_task_stime_ns[idx] = 0;
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
  /* Hand back the address-space mutex if this task still holds it.
   *
   * It is a yielding lock with no owner recorded until now and no release
   * except by the caller that took it, and a task can die inside a mapping
   * call — exit_group SIGKILLs its siblings wherever they are. The slot is
   * shared by hash across address spaces, so a leaked one takes unrelated
   * processes down with it: they spin in vma_mutator_lock forever and the
   * guest goes quiet with no panic to explain it. */
  {
    extern void syscall_release_vma_locks(struct task *t);

    syscall_release_vma_locks(current_task);
  }
  /* M109: drop this task's namespace row. The namespaces themselves are
   * reclaimed later, from unshare/setns — releasing a mount namespace means
   * dropping VFS references, which has no business running here. */
  namespace_task_exit(current_task->id);
  cgroup_task_exit(current_task->id);
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

  /* Who left, when, and with what.
   *
   * A browser that stalls because a process it depends on quietly went away
   * gives no sign of it: the survivors simply wait. One line per exiting
   * thread is a handful of lines a run and says immediately whether a
   * disappearance preceded the stall. b1nix.trace-exit turns it on. */
  if (bootinfo_has_flag("b1nix.trace-exit") && current_task->pml4_phys) {
    console_write("EXIT: pid=");
    console_write_dec(current_task->id);
    console_write(" tgid=");
    console_write_dec(g_task_tgid[task_index(current_task)]);
    console_write(" ppid=");
    console_write_dec(current_task->parent_id);
    console_write(" code=0x");
    console_write_hex64((u64)(unsigned)exit_code);
    console_write(" name=");
    console_write(current_task->name ? current_task->name : "?");
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
    /* A dying task must not leave an inode write-locked: every waiter
     * behind it blocks for ever, and above them a whole process chain.
     * See vfs_release_inode_locks_of. */
    vfs_release_inode_locks_of((u64)current_task->id);
    /* Same class of leak, same place to catch it: a task that dies holding the
     * NVMe I/O mutex wedges every later I/O to that device. */
    {
      extern void nvme_release_io_lock_of(u64 pid);
      nvme_release_io_lock_of((u64)current_task->id);
    }
    current_task->state = TASK_DEAD;
    scheduler_yield();
    panic("dead thread resumed");
  }

  EXIT_STAGE(20);
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

  EXIT_STAGE(30);
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
  /* A dying task must not leave an inode write-locked: every waiter
   * behind it blocks for ever, and above them a whole process chain.
   * See vfs_release_inode_locks_of. */
  vfs_release_inode_locks_of((u64)current_task->id);
    /* Same class of leak, same place to catch it: a task that dies holding the
     * NVMe I/O mutex wedges every later I/O to that device. */
    {
      extern void nvme_release_io_lock_of(u64 pid);
      nvme_release_io_lock_of((u64)current_task->id);
    }
  current_task->state = TASK_DEAD;
  g_have_proc_zombies = 1; /* arm the orphan sweep in scheduler_yield */

  EXIT_STAGE(40);
  post_sigchld_to_parent(current_task->parent_id, 0);

  /* F6 (M28 #7): kick the BSP (or whichever CPU runs the parent kthread)
   * out of sti;hlt so it picks the parent immediately instead of waiting
   * for the next 10 ms LAPIC tick. Particularly important for the
   * test-driver init kthread on the BSP, which sits in sti;hlt during
   * userspace test runs. */
  scheduler_notify_wait_event(current_task->parent_id);

  EXIT_STAGE(50);
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
      /* This wait has no channel — it ends when a child changes state, and
       * the wakers reach it by id. Clear whatever channel the previous wait
       * left behind so a wake meant for that channel cannot land here. */
      current_task->wait_chan = 0;
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
              cpu_relax();
              tlb_shootdown_poll();
            }

            /* Reclaim RUNNING here, BEFORE any of the teardown below, not
             * after it.
             *
             * This waitpid published TASK_BLOCKED to wait for a child. Having
             * found one, it goes on to tear the child down -- and that teardown
             * sleeps: vfs_close_handle flushes the page cache, which writes
             * through ext4 to the block device, which waits for the request to
             * complete. A yield anywhere in there switches this task out while
             * it is still BLOCKED, and scheduler_yield only republishes READY
             * for a RUNNING task, so it leaves with wait_chan == 0 and nothing
             * in the kernel can ever wake it again.
             *
             * That is not hypothetical: it is what wedged the sys lane. init
             * sat BLOCKED for ever with its resume point inside
             * do_vblk_req's wait loop, the ext4 writeback of a reaped child's
             * fd unfinished, and the whole machine idle behind it -- openrc,
             * dropbear and the SSH test all waiting on an I/O that had no
             * runnable task left to collect it. The call was already here, ten
             * lines further down, on the other side of the sleeping part. */
            if (may_block)
              scheduler_waitpid_fast_return();

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
            if (T(i) == current_task)
              panic("sched: freeing the kernel stack we are running on");
            kfree(T(i)->stack);
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
      /* This wait has no channel — it ends when a child changes state, and
       * the wakers reach it by id. Clear whatever channel the previous wait
       * left behind so a wake meant for that channel cannot land here. */
      current_task->wait_chan = 0;
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
                  cpu_relax();
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
                /* RUNNING before the sleeping part, not after it -- same
                 * reason as the other reap site above: closing the child's
                 * handles writes back through the filesystem, and a yield
                 * taken while this task is still BLOCKED strands it for good. */
                if (may_block)
                  scheduler_waitpid_fast_return();
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
                if (child == current_task)
                  panic("sched: freeing the kernel stack we are running on");
                kfree(child->stack);
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

/* Which slot of the task table the running task occupies.
 *
 * For per-task side tables kept outside struct task: the imported-driver shim
 * needs somewhere stable to hang its idea of `current`, and a per-CPU slot is
 * not that — any other task running on the same CPU overwrites it, and a waiter
 * that saved the pointer then gets somebody else woken in its place. The index
 * is what the scheduler already uses internally; MAX_TASKS bounds it. */
/* Wake a task without touching a runqueue.
 *
 * scheduler_wake_task enqueues, and enqueueing takes the runqueue lock. From an
 * interrupt handler that is a deadlock waiting to happen: the code the
 * interrupt suspended may hold that very lock on this CPU, and the next timer
 * tick then spins on it forever — which is exactly how the i915 fence callback,
 * once it started actually waking anything, wedged the boot.
 *
 * The enqueue is an optimisation. pick_next_task also scans the task table for
 * READY tasks, so a task promoted here is found on the next pick; it costs a
 * scan, not a wakeup. Safe to call with interrupts off and locks held. */
void scheduler_wake_task_norq(usize task_id) {
  u64 flags = interrupts_save();

  for (usize i = 0; i < g_task_hwm; i++) {
    if (T(i)->id != task_id)
      continue;
    enum task_state expected = TASK_BLOCKED;
    if (!__atomic_compare_exchange_n(&T(i)->state, &expected, TASK_READY, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      expected = TASK_SLEEPING;
      __atomic_compare_exchange_n(&T(i)->state, &expected, TASK_READY, 0,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
    }
    break;
  }
  interrupts_restore(flags);
}

usize scheduler_current_slot(void) {
  return current_task ? task_index(current_task) : 0;
}

usize scheduler_max_task_slots(void) { return MAX_TASKS; }

struct task *scheduler_task_slot(usize index) {
  if (index >= g_task_hwm)
    return 0;
  struct task *t = T(index);
  if (t->state == TASK_UNUSED || t->state == TASK_DEAD ||
      t->state == TASK_REAPING)
    return 0;
  return t;
}

/* Which task owns the kernel stack `sp` lies in, or 0. The diagnostics that
 * catch an exception taken on a foreign stack can say WHICH stack it is only by
 * asking the table -- and "whose stack was the CPU actually on" is the fact that
 * separates a stale `cur_task` from a genuinely corrupted SP. */
struct task *scheduler_task_owning_stack(u64 sp) {
  for (usize i = 0; i < g_task_hwm; i++) {
    struct task *t = scheduler_task_slot(i);
    if (!t || !t->stack)
      continue;
    u64 lo = (u64)(usize)t->stack;
    if (sp >= lo && sp <= lo + KERNEL_STACK_SIZE)
      return t;
  }
  return 0;
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

void scheduler_note_syscall(u32 nr) {
  struct task *t = current_task;

  if (!t || !t->pml4_phys)
    return; /* kernel threads make no interesting sequence */
  usize idx = task_index(t);

  if (idx >= MAX_TASKS)
    return;
  u8 pos = g_sysring_pos[idx];

  g_sysring[idx][pos] = (u16)nr;
  g_sysring_pos[idx] = (u8)((pos + 1) % SYSRING_DEPTH);
}

/* Oldest first, so the line reads in the order the calls happened. */
static void scheduler_dump_sysring(usize idx) {
  if (idx >= MAX_TASKS)
    return;
  int any = 0;

  for (int k = 0; k < SYSRING_DEPTH; k++) {
    u8 pos = (u8)((g_sysring_pos[idx] + k) % SYSRING_DEPTH);

    if (!g_sysring[idx][pos] && !any)
      continue; /* ring not yet full — skip the empty prefix */
    if (!any) {
      console_write(" calls=");
      any = 1;
    }
    console_write_dec(g_sysring[idx][pos]);
    console_write(",");
  }
  if (any)
    console_write("\n");
}

static int g_dump_dead;

void scheduler_dump_tasks(void) {
  extern void futex_dump_waiters(void);
  {
    /* Where the run spends itself, printed first because it is the line most
     * often wanted and the dump below is long. */
    extern void syscall_prof_dump(void);
    extern void pf_prof_dump(void);

    syscall_prof_dump();
    pf_prof_dump();
  }
  futex_dump_waiters();
  {
    /* Which fcntl a spinning thread repeats. See vfs_fcntl_dump_counts. */
    extern void vfs_fcntl_dump_counts(void);
    vfs_fcntl_dump_counts();
  }
  {
    /* Who holds the page-cache lock. Every wedge this session ended with the
     * CPUs spinning on it, and nothing in the dump said whose it was. */
    extern void page_cache_dump_lock(void);
    page_cache_dump_lock();
  }
  scheduler_dump_thread_exits();
  console_write("tick=");
  console_write_dec(scheduler_ticks);

  /* Corpses are counted, not listed.
   *
   * A stalled run has one interesting task in it — the one that is not
   * finishing — and dozens of DEAD/REAPING slots that finished perfectly well.
   * Listed in full they are most of the dump, which on a host serial log is
   * merely noisy but on a board whose console is a 45-column panel pushes the
   * live tasks, and the marker that says which test stalled, off the top of the
   * screen before anyone can read them. A run chasing a reaping bug wants them
   * back: b1nix.dump-dead. */
  {
    static int dump_dead = -1;
    usize corpses = 0;

    if (dump_dead < 0)
      dump_dead = bootinfo_has_flag("b1nix.dump-dead") ? 1 : 0;
    if (!dump_dead) {
      for (usize i = 0; i < g_task_hwm; i++)
        if (T(i)->state == TASK_DEAD || T(i)->state == TASK_REAPING)
          corpses++;
      console_write(" dead/reaping=");
      console_write_dec(corpses);
      console_write(" (listed with b1nix.dump-dead)");
    }
    g_dump_dead = dump_dead;
  }

  console_write("\nID\tSTATE\tNAME\n");
  for (usize i = 0; i < g_task_hwm; i++) {
    if (!g_dump_dead &&
        (T(i)->state == TASK_DEAD || T(i)->state == TASK_REAPING))
      continue;
    if (T(i)->state != TASK_UNUSED) {
      console_write_hex64(T(i)->id);
      console_write("\t");

      const char *state_str = task_state_name(T(i)->state);

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
      /* Queued anywhere? Note that `queued=NO` is ordinary -- a yielding task
       * is READY without being enqueued and the picker scans the table. See
       * sched_rq_contains_task before drawing a conclusion from this. */
      if (T(i)->state == TASK_READY) {
        console_write(sched_rq_contains_task(T(i)) ? " queued=yes" : " queued=NO");
      }
      console_write(" rel=");
      console_write_dec(T(i)->stack_released);
      console_write(" ap=");
      console_write_dec(T(i)->ap_runnable);
      console_write(" steal=");
      console_write_dec(T(i)->stealable);
      /* Saved kernel RSP + fd_lock state: a task stuck holding its fd_lock
       * (the lock every other CPU then spins on) is only identifiable here. */
      console_write(" rsp=0x");
#ifdef __aarch64__
      console_write_hex64(T(i)->context.sp);
#elif defined(__x86_64__)
      console_write_hex64(T(i)->context.rsp);
#endif
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
      if (g_task_exiting[i] && g_task_exit_stage[i]) {
        console_write(" xstage=");
        console_write_dec(g_task_exit_stage[i]);
      }
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
        unsigned guard = 0;
        for (struct vm_area *v = T(i)->vma_list; v && guard < SCHED_DUMP_VMA_MAX;
             v = v->next, guard++) {
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
        /* Deeper for the process under investigation.
         *
         * Twenty-four words is enough to see that a thread is in a library and
         * not enough to say which call chain put it there. With the debug
         * symbols for the same binary on the host, a few hundred words resolve
         * into a readable trace — the walk cannot tell a return address from a
         * pointer that happens to look like one, but a symbol table can. Only
         * for the group leader, and only when asked, because it is a hundred
         * lines a thread. */
        unsigned deep_words = 24, deep_scan = 1024;

        if (bootinfo_has_flag("b1nix.deep-stack") &&
            g_task_tgid[i] == T(i)->id) {
          deep_words = 220;
          deep_scan = 8192;
        }
        for (unsigned k = 0; k < deep_scan && shown < deep_words; k++) {
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
          unsigned vguard = 0;
          for (struct vm_area *v = T(i)->vma_list;
               v && vguard < SCHED_DUMP_VMA_MAX; v = v->next, vguard++) {
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
            /* The main executable has no file behind its mappings — the ELF
             * loader builds them itself — so requiring a name threw away every
             * return address into the program under investigation and left only
             * the libraries. A deep walk therefore accepts any present mapping
             * and reports the name when there is one; telling code from data is
             * the symbol table's job, on the host, where there is one to
             * consult. */
            int named = v->node && v->node->name[0];

            if (value >= v->start && value < v->end &&
                (named || deep_words > 24) &&
                ({ u64 _pte = paging_user_pte(T(i)->pml4_phys,
                                              value & ~(u64)(PAGE_SIZE - 1));
                   (_pte & VMM_PRESENT) != 0; })) {
              console_write("\n      0x");
              console_write_hex64(value);
              console_write(" ");
              console_write(named ? v->node->name : "exe");
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
      if (T(i) != current_task && TASK_CTX_SP(T(i))) {
        extern char __kernel_text_start[], __kernel_text_end[];
        u64 lo = (u64)(usize)__kernel_text_start;
        u64 hi = (u64)(usize)__kernel_text_end;
        const u64 *stack = (const u64 *)(usize)TASK_CTX_SP(T(i));
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
      scheduler_dump_sysring(i);
    }
  }
  /* A dump that stops half way looks exactly like a dump that had nothing more
   * to say, and one run was read as "the machine went quiet" when in fact the
   * dump itself had wedged. This line is the proof it finished. */
  console_write("TASK-DUMP: end\n");
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

/* The fd-table ceiling in force on this machine. Computed once on first use —
 * PMM is up long before any fd is opened — and cached. Two CPUs racing here
 * compute the same value from the same inputs, so the race is benign. */
usize sched_fd_limit(void) {
  static usize cached;

  if (cached)
    return cached;

  u64 ram_mb = pmm_total_usable_memory() / (1024ULL * 1024ULL);
  /* Four descriptors per MiB of RAM: 1024 at 256 MiB (the old fixed value, so
   * nothing changes on a small guest) and 32768 on an 8 GiB machine. */
  u32 want = bootinfo_get_u32(
      "b1nix.max-fds",
      (u32)(ram_mb > 0x100000ULL ? 0x100000ULL : ram_mb) * 4u);

  if (want < SCHED_MIN_FD_LIMIT)
    want = SCHED_MIN_FD_LIMIT;
  if (want > SCHED_MAX_FD_LIMIT)
    want = SCHED_MAX_FD_LIMIT;
  cached = want;
  return cached;
}

/* `b1nix.trace-fd=<n>`: report every task that installs or removes descriptor
 * <n>, with the pid.
 *
 * "A descriptor the caller believes it holds is not in its table" is a whole
 * class of bug — systemd aborts on it (`fclose_nointr(f) != -EBADF`) and the
 * failing call names the syscall, not the moment the descriptor went away. The
 * moment is what is missing, so this reports it: the trace shows who took the
 * slot and who gave it back, in order. Off unless the flag is given. */
static int fd_trace_target(void) {
  static int target = -2; /* -2 = not read yet, -1 = off */

  if (target == -2) {
    u32 v = bootinfo_get_u32("b1nix.trace-fd", (u32)-1);
    target = (v == (u32)-1) ? -1 : (int)v;
  }
  return target;
}

static void fd_trace(const char *what, int fd, const void *handle) {
  if (fd != fd_trace_target())
    return;

  extern u64 syscall_current_number(void);
  /* What the descriptor refers to, not just that it moved. "descriptor 0 was
   * closed" and "the machine's /dev/null was closed" are different findings,
   * and only the second one says whether a kernel replaced a descriptor behind
   * the caller's back. */
  const struct vfs_handle *h = handle;
  const char *what_it_is = "?";

  if (h) {
    if (h->node && h->node->name[0])
      what_it_is = h->node->name;
    else
      switch (h->kind) {
      case VFS_HANDLE_PIPE_READ:  what_it_is = "pipe:r"; break;
      case VFS_HANDLE_PIPE_WRITE: what_it_is = "pipe:w"; break;
      case VFS_HANDLE_SOCKET:     what_it_is = "socket"; break;
      case VFS_HANDLE_EPOLL:      what_it_is = "epoll"; break;
      case VFS_HANDLE_SIGNALFD:   what_it_is = "signalfd"; break;
      case VFS_HANDLE_TIMERFD:    what_it_is = "timerfd"; break;
      case VFS_HANDLE_EVENTFD:    what_it_is = "eventfd"; break;
      case VFS_HANDLE_INOTIFY:    what_it_is = "inotify"; break;
      case VFS_HANDLE_PIDFD:      what_it_is = "pidfd"; break;
      default:                    what_it_is = "anon"; break;
      }
  }

  char line[200];
  snprintf(line, sizeof(line),
           "fd-trace: %s fd=%d pid=%u syscall=%llu handle=%p (%s)", what, fd,
           (unsigned)(current_task ? current_task->id : 0),
           (unsigned long long)syscall_current_number(), handle, what_it_is);
  klog_info(line);
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
      fd_trace("alloc", (int)i, handle);
      return (int)i;
    }
  }

  if (current_task->fd_capacity >= sched_fd_limit() || current_task->fd_capacity >= nofile_limit) {
    fd_lock_release();
    return -EMFILE;
  }

  usize new_capacity = current_task->fd_capacity * 2;
  if (new_capacity > sched_fd_limit())
    new_capacity = sched_fd_limit();
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
    if ((usize)fd >= sched_fd_limit()) {
      fd_lock_release();
      return -1;
    }
    usize new_capacity = current_task->fd_capacity;
    while (new_capacity <= (usize)fd) {
      new_capacity *= 2;
    }
    if (new_capacity > sched_fd_limit())
      new_capacity = sched_fd_limit();

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
  fd_trace("set", fd, handle);
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
  fd_trace("close", fd, 0);
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
  if (h)
    fd_trace("take", fd, h);
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
  /* A child changing state is also a readiness change on every pidfd that
   * names it, and a poller has no other way to hear about it.
   *
   * A pidfd is readable exactly when the process it holds has exited, and
   * b1nix's poll implementation re-examines its descriptors only when
   * something wakes vfs_poll_chan. Nothing did on this path, so an epoll_wait
   * on a pidfd slept through the death it was waiting for -- which is how
   * systemd waits for the child it runs its generators in, and it sat in that
   * epoll_wait for the rest of the boot.
   *
   * Announced before the SIGCHLD, and unconditionally: a pidfd holder need not
   * be the parent, and there is no cheap way to ask which of them there are.
   * Deaths are rare next to the wake this costs. */
  {
    extern void *vfs_poll_chan;
    scheduler_wake_all(vfs_poll_chan);
  }
  if (parent_id == 0)
    return;
  for (usize p = 0; p < g_task_hwm; p++) {
    if (T(p)->id == parent_id && T(p)->state != TASK_UNUSED) {
      if (job_control_event &&
          (T(p)->sigactions[SIGCHLD - 1].sa_flags & SA_NOCLDSTOP))
        return;
      __atomic_fetch_or(&T(p)->pending_signals, (1ULL << (SIGCHLD - 1)),
                        __ATOMIC_RELEASE);
sched_sigwait_notify(T(p), SIGCHLD);
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
sched_sigwait_notify(T(i), sig);

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
      /* Out of any interruptible wait — blocked, asleep or stopped — so the
       * target runs and acts on what was just posted to it. */
      (void)sched_wake_for_signal(T(i), sig);
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
      sched_wake_enqueue(t);
    return;
  }
  /* Post the stop and let the sibling park itself on its next return to ring 3.
   * Writing TASK_STOPPED into a task running on another CPU parks it before its
   * context has been saved, and the next picker then resumes it from a
   * half-written frame (the M40 lesson). */
  g_task_stop_quiet[task_index(t)] = 1;
  __atomic_fetch_or(&t->pending_signals, (1ULL << (sig - 1)), __ATOMIC_RELEASE);
sched_sigwait_notify(t, sig);
  /* A sibling asleep in an interruptible wait would otherwise ignore the group
   * stop until its syscall happened to finish — a thread blocked on a read that
   * never completes would keep the job "running" forever. Wake it so it takes
   * the signal now; the interrupted syscall restarts (SA_RESTART) or returns
   * EINTR through the usual path. Same per-state CAS scheduler_kill uses, so a
   * task that is concurrently dying on another CPU cannot be resurrected. */
  (void)sched_wake_for_signal_ex(t, sig, 0);
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
sched_sigwait_notify(T(i), sig);
      spin_unlock(&g_rt_lock);
      /* Wake a blocked or sleeping target so it re-checks pending signals.
       * Mirrors scheduler_kill's CAS so a kill racing the target's exit cannot
       * resurrect a DEAD/REAPING task. Done outside g_rt_lock to avoid nesting
       * it under the runqueue lock. A stopped target stays stopped, as it did
       * before this had a shared helper. */
      (void)sched_wake_for_signal_ex(T(i), sig, 0);
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
sched_sigwait_notify(T(i), sig);

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
      /* Out of any interruptible wait — blocked, asleep or stopped — so the
       * target runs and acts on what was just posted to it. */
      (void)sched_wake_for_signal(T(i), sig);
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
  if (allowed < 0) {
    /* A zombie is still a process. POSIX (and Linux) let kill(2) name a child
     * that has exited but not been reaped: the signal has nowhere to go and is
     * discarded, but the call SUCCEEDS -- the pid is valid until it is waited
     * for, which is the whole point of leaving the entry behind.
     *
     * find_live_task above refuses DEAD and REAPING, so every such kill came
     * back ESRCH. What that looked like from userspace was a parent that had
     * signalled a child a moment too late being told the child had never
     * existed, instead of being told nothing at all: M86's compute-loop check
     * reported a failed kill (errno 3) rather than the child's real exit
     * status, which is the only thing that could have explained the run.
     *
     * Reaping is what ends the pid, so REAPING counts as gone. */
    u64 zflags = interrupts_save();
    int zombie = 0;
    for (usize i = 0; i < g_task_hwm; i++) {
      struct task *z = T(i);

      if (z->id == pid && z->state == TASK_DEAD && !task_is_thread(z)) {
        zombie = 1;
        break;
      }
    }
    interrupts_restore(zflags);
    return zombie ? 0 : -ESRCH;
  }
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

/* The CALLER's own id, one per thread — not the thread-group id
 * scheduler_get_pid() answers with. Anything that keeps per-task state keyed by
 * a number needs this one: keyed by the group id, every thread of a process
 * shares the entry, which is the sharing such a table exists to remove. */
usize scheduler_current_task_id(void) {
  return current_task ? current_task->id : 0;
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
static struct vm_area *g_vma_cache[TASK_SLOTS];

/* The cache is stamped, not merely cleared.
 *
 * Clearing it named one task — the one doing the unmapping — while the list it
 * caches is shared by every CLONE_VM thread of the process. So a sibling kept
 * a pointer to a struct that had already been kfree'd, and went on serving
 * faults out of it: a mapping that no longer existed answered for an address
 * that by then belonged to somebody else's fresh mmap, and the page installed
 * there was written into a range its owner believed untouched. That is the
 * foreign data found in a freshly mapped destination, and it is why the
 * failure needed several cores and several threads to show itself.
 *
 * A counter bumped on every unlink invalidates every task's cache at once, at
 * the cost of one atomic on a path that already frees memory and shoots down
 * TLBs. Per-address-space stamping would keep more of the cache alive, but
 * unlinking is rare and correctness here is worth more than the hit rate. */
static u64 g_vma_cache_gen[TASK_SLOTS];
static u64 g_vma_gen;

void vma_cache_invalidate_all(void) {
  __atomic_add_fetch(&g_vma_gen, 1, __ATOMIC_SEQ_CST);
}

/* Freeing a mapping while a fault is walking the list.
 *
 * vma_lookup deliberately walks without the list lock — it runs in the page
 * fault path, the list is long in a browser, and holding a lock with
 * interrupts off across that walk wedged the machine. The exposure it leaves
 * is that the node under the walker can be kfree'd: the handler then reads
 * prot, node and offset out of freed memory and acts on them, which is a write
 * anywhere. Six threads doing nothing but mmap/fill/munmap on six CPUs kill
 * the guest in five seconds, and that is the shape of it.
 *
 * A walker counter closes it without taking a lock in the fault path: retiring
 * a mapping puts it on a list instead of freeing it, and the list is drained
 * only when no CPU is inside a walk. A walker that starts after the unlink is
 * published cannot reach a retired node, because it begins from a head that no
 * longer leads to one — so "no walkers right now" is enough to free.
 */
static volatile u32 g_vma_walkers;
static struct vm_area *g_vma_retired;

/* Drain what is safe to drain. Called with the list lock held. */
static void vma_retire_drain_locked(void) {
  if (!g_vma_retired)
    return;
  if (__atomic_load_n(&g_vma_walkers, __ATOMIC_SEQ_CST) != 0)
    return;

  struct vm_area *v = g_vma_retired;

  g_vma_retired = 0;
  while (v) {
    struct vm_area *next = v->retired_next;

    kfree(v);
    v = next;
  }
}

/* Hand a mapping over to be freed once no walker can be holding it. */
void vma_retire(struct vm_area *vma) {
  u64 flags;

  if (!vma)
    return;
  vma_list_lock(&flags);
  vma->retired_next = g_vma_retired;
  g_vma_retired = vma;
  vma_retire_drain_locked();
  vma_list_unlock(flags);
}

/* A chance to reclaim, taken from a path that is not a fault. */
void vma_retire_poll(void) {
  u64 flags;

  if (!__atomic_load_n((struct vm_area *volatile *)&g_vma_retired,
                       __ATOMIC_RELAXED))
    return;
  vma_list_lock(&flags);
  vma_retire_drain_locked();
  vma_list_unlock(flags);
}

struct vm_area *vma_lookup(struct task *t, u64 addr) {
  if (!t)
    return 0;

  usize slot = task_index(t);
  u64 gen = __atomic_load_n(&g_vma_gen, __ATOMIC_ACQUIRE);
  struct vm_area *hit = g_vma_cache[slot];

  /* The cached mapping is only trusted while nothing has been unlinked from
   * any list since it was cached; a freed one would be a use-after-free. */
  if (hit && g_vma_cache_gen[slot] == gen && addr >= hit->start && addr < hit->end)
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
  __atomic_add_fetch(&g_vma_walkers, 1, __ATOMIC_SEQ_CST);
  for (struct vm_area *v = t->vma_list; v && v->start <= addr; v = v->next) {
    if (addr < v->end) {
      /* Stamp first, publish second: a walker that reads the pointer must
       * never find it paired with a generation newer than the one it was
       * cached under. */
      g_vma_cache_gen[slot] = gen;
      __atomic_store_n(&g_vma_cache[slot], v, __ATOMIC_RELEASE);
      __atomic_sub_fetch(&g_vma_walkers, 1, __ATOMIC_SEQ_CST);
      return v;
    }
  }
  __atomic_sub_fetch(&g_vma_walkers, 1, __ATOMIC_SEQ_CST);
  return 0;
}

void vma_cache_forget(struct task *t) {
  if (t)
    g_vma_cache[task_index(t)] = 0;
}

/* What every thread of this address space believes the mapping list to be.
 *
 * Each task carries its own copy of the list head, kept in step by copying it
 * around on change. If one copy is stale, that thread walks a different list
 * from its siblings — and the free-area search that runs on it can hand out an
 * address another thread is already using. This prints the heads side by side,
 * and says which threads' lists cover a given range, which is the difference
 * between "the allocator is wrong" and "the allocator was told the wrong
 * thing". Diagnostic only; called when a placement has already gone wrong.
 */
void vma_report_space(u64 start, u64 end) {
  struct task *me = current_task;

  if (!me)
    return;
  console_write("  address-space view, threads of pml4 0x");
  console_write_hex64(me->pml4_phys);
  console_write(":\n");
  for (usize i = 0; i < g_task_hwm; i++) {
    struct task *t = T(i);

    if (t->state == TASK_UNUSED || t->pml4_phys != me->pml4_phys)
      continue;
    console_write("    tid ");
    console_write_dec(t->id);
    console_write(" head 0x");
    console_write_hex64((u64)(usize)t->vma_list);
    /* Does this thread's list cover the range in question? */
    {
      struct vm_area *cover = 0;
      unsigned steps = 0;

      for (struct vm_area *v = t->vma_list; v && ++steps < 100000u; v = v->next) {
        if (v->start < end && v->end > start) {
          cover = v;
          break;
        }
      }
      if (cover) {
        console_write(" COVERS 0x");
        console_write_hex64(cover->start);
        console_write("-0x");
        console_write_hex64(cover->end);
      } else {
        console_write(" does not cover it");
      }
    }
    console_write("\n");
  }
}

u64 vm_find_free_area(struct task *t, usize length) {
#if defined(__aarch64__)
  /* AArch64 keeps the whole kernel half — RAM identity map and device windows
   * — in L0[0], and that top-level entry is SHARED by pointer with every
   * process (see paging_clone_address_space, which only copies L0[1..511]).
   * So an mmap region anywhere below 512 GiB would land in page tables every
   * process shares: two programs' heaps aliased the same pages and musl's
   * mallocng metadata was silently overwritten. Start in the per-process half
   * instead (1 TiB, L0 index 2). */
  u64 start = 0x10000000000ULL;
  u64 end = 0x7FFFFFFFFFFFULL;
#elif defined(__x86_64__)
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

  /* The break is an obstacle too, and the VMA walk above cannot see it.
   *
   * A heap that has not grown yet is recorded as an empty VMA (start == end),
   * so it blocks nothing and a mapping is free to land exactly where the
   * break is about to grow. brk then maps its pages straight over the other
   * mapping's live data — a compositor's pixel buffer ended up on top of the
   * server's own structures that way, which reads as heap corruption at a
   * random later moment rather than as the placement error it is. Reserve the
   * range the break occupies, plus room to grow, and search again above it. */
  if (t->heap_start && candidate + length > t->heap_start) {
    u64 brk_end = t->user_brk > t->heap_start ? t->user_brk : t->heap_start;
    /* Headroom scaled to the heap itself rather than a fixed number: a
     * process that has already grown its break will grow it again, and one
     * that has not costs a page of address space. Address space is the cheap
     * resource here — running out of it is not what breaks, colliding is. */
    u64 heap_size = brk_end - t->heap_start;
    u64 headroom = heap_size < PAGE_SIZE ? PAGE_SIZE : heap_size;
    u64 guard = brk_end + headroom;

    if (guard < brk_end)
      return (u64)-1;
    guard = (guard + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (candidate < guard) {
      candidate = guard;
      /* One more pass: the region above the break may itself be taken. */
      for (struct vm_area *vma = t->vma_list; vma; vma = vma->next) {
        if (vma->end <= candidate)
          continue;
        if (vma->start >= candidate + length)
          break;
        candidate = vma->end;
        if (candidate + length > end || candidate + length < candidate)
          return (u64)-1;
      }
    }
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
    /* Every task's lookup cache may be pointing at what is about to be freed,
     * not just this one's: the list belongs to the address space, and its
     * threads each cache out of it. */
    vma_cache_forget(task);
    vma_cache_invalidate_all();
    if (vma->node) {
      if (vma->node->inode && vma->node->inode->mmap_close_cb)
        vma->node->inode->mmap_close_cb(vma->node);
      if (vma->node->inode && vma->node->inode->mmap_range_close_cb)
        vma->node->inode->mmap_range_close_cb(
            vma->node, (u64)vma->offset, (usize)(vma->end - vma->start));
      vfs_node_put(vma->node);
    }
    vma_retire(vma);
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
      release_task_registrations(current_task);
  scheduler_timer_cleanup_task(current_task->id); /* M74: free POSIX timers */
      ptrace_task_cleanup(current_task); /* M80: never leave a stale tracee link */
      task_lease_clear(current_task, __func__);
      /* A dying task must not leave an inode write-locked: every waiter
       * behind it blocks for ever, and above them a whole process chain.
       * See vfs_release_inode_locks_of. */
      vfs_release_inode_locks_of((u64)current_task->id);
    /* Same class of leak, same place to catch it: a task that dies holding the
     * NVMe I/O mutex wedges every later I/O to that device. */
    {
      extern void nvme_release_io_lock_of(u64 pid);
      nvme_release_io_lock_of((u64)current_task->id);
    }
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
        release_task_registrations(current_task);
  scheduler_timer_cleanup_task(current_task->id); /* M74: free POSIX timers */
        ptrace_task_cleanup(current_task); /* M80: never leave a stale tracee link */
        task_lease_clear(current_task, __func__);
        /* A dying task must not leave an inode write-locked: every waiter
         * behind it blocks for ever, and above them a whole process chain.
         * See vfs_release_inode_locks_of. */
        vfs_release_inode_locks_of((u64)current_task->id);
    /* Same class of leak, same place to catch it: a task that dies holding the
     * NVMe I/O mutex wedges every later I/O to that device. */
    {
      extern void nvme_release_io_lock_of(u64 pid);
      nvme_release_io_lock_of((u64)current_task->id);
    }
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

/* Is this signal's default action to do nothing?
 *
 * Only four are ignored by default. Everything else either terminates the
 * process or stops it, and both of those have to end a wait. */
static int sig_default_is_ignore(int sig) {
  return sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGCONT;
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
    /* SIGKILL cannot be caught, blocked or ignored, and a task that is about
     * to die must not be left waiting for a message that will never come. */
    if (i == SIGKILL || i == SIGSTOP)
      return 1;

    sighandler_t h = current_task->sigactions[i - 1].sa_handler;

    if (h == SIG_IGN)
      continue;
    /* A handler obviously interrupts the wait — the handler has to run. But so
     * does a signal left at its default disposition when that default is to
     * terminate the process: this returned 0 for those, so a process blocked
     * in recvmsg, read or accept could not be killed at all. What that looked
     * like was a Wayland client that survived every kill, a shell stuck in
     * wait4 for it forever, and a session that could not be torn down —
     * reported as "the compositor hangs the machine". */
    if (h == SIG_DFL && sig_default_is_ignore(i))
      continue;
    return 1;
  }
  return 0;
}

/* Like scheduler_signal_pending, but also true for a pending STOP signal.
 *
 * scheduler_signal_pending only reports signals with a user handler, because a
 * handler is what a blocking call has to return to userspace to run. A handler
 * cannot exist for SIGSTOP at all, so a task parked in poll() was invisible to
 * it and slept through its own stop. PTRACE_ATTACH depends on exactly this: it
 * leaves SIGSTOP pending and expects the tracee to stop at its next return to
 * userspace, which a task blocked in a syscall reaches only if the syscall
 * gives up first. Attaching to a process sitting in pause() — which is ppoll()
 * here, this arch having no SYS_pause — therefore never produced a stop.
 *
 * Only the stop group is added. A fatal default action deliberately is NOT:
 * the scheduler kills such a task itself at the next context switch
 * (scheduler_deliver_pending_signals), no return to userspace required, so
 * reporting those here only makes blocking calls bail out for a task that is
 * about to die anyway. Widening this to every fatal default was tried and
 * wedged the posix lane — the syscall returned -ERESTARTSYS, was restarted,
 * saw the same pending signal and span. Default-ignore signals (SIGCHLD,
 * SIGURG, SIGWINCH) must not interrupt anything either; waking a blocking call
 * on SIGCHLD is what used to break dropbear's session loop. */
int scheduler_signal_pending_or_stops(void) {
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
    if (h == SIG_IGN)
      continue;
    if (h != SIG_DFL)
      return 1; /* a handler to run */
    if (i == SIGSTOP || i == SIGTSTP || i == SIGTTIN || i == SIGTTOU)
      return 1; /* stopping needs a return to userspace to happen */
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

/* ── Nice / stride contract, checked in test mode ────────────────────────────
 *
 * The userspace half of this (M46's nice-biasing test) forks spinning workers
 * at nice -20 and 19 and compares how many iterations each completed. That is
 * the right test for "does nice change behaviour", and the wrong instrument for
 * "does nice produce the weighting it promises": it measures a loaded machine
 * over a 250 ms window, so it can tell a bias from no bias and little else.
 * Where it wanted more, it read /proc/b1nix-tasks and dropped the bytes on the
 * floor — the file prints the scheduler's dump to the console, so the values
 * went to the log and no assertion was ever made about them.
 *
 * These checks are made where the numbers live. Nothing here touches the live
 * runqueue: the selection rule is replayed against the same stride function the
 * scheduler uses, which is what makes the expected counts exact rather than
 * statistical. */
void sched_nice_selftest(void) {
  /* 1. The stride each nice value buys, at both ends and the default. */
  {
    int s_min = sched_stride_for_nice(-20); /* 40 tickets */
    int s_def = sched_stride_for_nice(0);   /* 20 tickets */
    int s_max = sched_stride_for_nice(19);  /* 1 ticket   */

    if (s_min == 25 && s_def == 50 && s_max == 1000) {
      console_write("M46-SCHED: ok stride-values\n");
    } else {
      console_write("M46-SCHED: FAIL stride-values -20=");
      console_write_dec((u64)s_min);
      console_write(" 0=");
      console_write_dec((u64)s_def);
      console_write(" 19=");
      console_write_dec((u64)s_max);
      console_write("\n");
    }
  }

  /* 2. Out-of-range nice clamps instead of dividing by zero or going negative.
   * setpriority(2) clamps at the syscall, so these values reach the scheduler
   * only through a kernel-internal caller — which is exactly the caller no
   * userspace test can stand in for. */
  {
    int lo = sched_stride_for_nice(-1000);
    int hi = sched_stride_for_nice(1000);

    if (lo == sched_stride_for_nice(-20) && hi == sched_stride_for_nice(19))
      console_write("M46-SCHED: ok stride-clamped\n");
    else
      console_write("M46-SCHED: FAIL stride-clamped\n");
  }

  /* 3. Monotonicity: a higher nice is never scheduled more often. Every
   * adjacent pair, so a sign error anywhere in the range is caught rather than
   * just at the ends. */
  {
    int ok = 1;

    for (int n = -20; n < 19; n++) {
      if (sched_stride_for_nice(n) > sched_stride_for_nice(n + 1)) {
        ok = 0;
        break;
      }
    }
    if (ok)
      console_write("M46-SCHED: ok stride-monotonic\n");
    else
      console_write("M46-SCHED: FAIL stride-monotonic\n");
  }

  /* 4. The rule the strides feed: always run the smallest pass, then advance it
   * by that task's stride. Replayed here for two tasks at nice -20 and nice 0,
   * whose strides are 25 and 50 — so over any run the nicer-to-others task must
   * be picked exactly twice as often. The scheduler's own pick adds priority
   * ordering and runnability on top; the weighting between equals is this. */
  {
    u64 pass[2] = {0, 0};
    int stride[2] = {sched_stride_for_nice(-20), sched_stride_for_nice(0)};
    unsigned picks[2] = {0, 0};

    for (unsigned i = 0; i < 300; i++) {
      int win = (pass[1] < pass[0]) ? 1 : 0; /* ties go to the first, as the
                                              * scheduler's strict < does */
      picks[win]++;
      pass[win] += (u64)stride[win];
    }
    if (picks[0] == 2 * picks[1]) {
      console_write("M46-SCHED: ok stride-selection\n");
    } else {
      console_write("M46-SCHED: FAIL stride-selection high=");
      console_write_dec((u64)picks[0]);
      console_write(" low=");
      console_write_dec((u64)picks[1]);
      console_write("\n");
    }
  }
}
