#ifndef B1NIX_SCHED_H
#define B1NIX_SCHED_H

#include <b1nix/types.h>
#include <b1nix/posix.h>
#include <b1nix/spinlock.h>
#include <b1nix/lapic.h>   /* struct percpu + get_percpu() for the current_task macro */

struct vfs_handle;
struct task;
struct cred;

enum task_state {
  TASK_UNUSED = 0,
  TASK_RUNNING,
  TASK_READY,
  TASK_BLOCKED,
  TASK_SLEEPING,
  TASK_STOPPED,
  TASK_DEAD,
  /* Intermediate state between TASK_DEAD and TASK_UNUSED — the parent (or any
   * waitpid caller in the flat process model) has won the atomic CAS from
   * DEAD->REAPING and is now freeing the task's resources (user_image,
   * page tables, name, stack). free_task_slot() flips it to UNUSED at the end.
   *
   * Without this distinct state, two CPUs racing waitpid against the same
   * dead child both observe state == DEAD and proceed to free its memory
   * twice — a kernel UAF. Under BKL this couldn't happen (only one CPU in
   * kernel code at a time); under fine-grained locking it can, which is
   * what M28 #7 is preparing for. Skipped by every walker that already
   * skips DEAD (scheduler_task_count, pick_next_task, scheduler_dump_tasks). */
  TASK_REAPING,
};

struct cpu_context {
#ifdef __aarch64__
  u64 x19;
  u64 x20;
  u64 x21;
  u64 x22;
  u64 x23;
  u64 x24;
  u64 x25;
  u64 x26;
  u64 x27;
  u64 x28;
  u64 fp;
  u64 lr;
  u64 sp;
#elif defined(__x86_64__)
  u64 rsp;
  u64 rbp;
  u64 rbx;
  u64 r12;
  u64 r13;
  u64 r14;
  u64 r15;
#else
  u32 esp;
  u32 ebp;
  u32 ebx;
  u32 esi;
  u32 edi;
#endif
};

/* ── Signals ── */
#define SIGABRT 1
#define SIGALRM 2
#define SIGBUS 3
#define SIGCHLD 4
#define SIGCONT 5
#define SIGFPE 6
#define SIGHUP 7
#define SIGILL 8
#define SIGINT 9
#define SIGKILL 10
#define SIGPIPE 11
#define SIGQUIT 12
#define SIGSEGV 13
#define SIGSTOP 14
#define SIGTERM 15
#define SIGTSTP 16
#define SIGTTIN 17
#define SIGTTOU 18
#define SIGUSR1 19
#define SIGUSR2 20
#define SIGSYS 21
#define SIGTRAP 22
#define SIGURG 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGIO 29
#define SIGPWR 30

#define NSIG 31

/* M74 POSIX real-time signals. The standard signals above occupy 1..30 (the
 * in-struct sigactions[31] array and pending/blocked bits 0..30). RT signals
 * SIGRTMIN..SIGRTMAX use the still-free upper bits 31..62 of the u64 masks; their
 * sigactions and a FIFO of queued (signo, sigval) payloads live in side-tables
 * (struct task cannot grow — the M29 LAPIC page-table invariant), so the 1..31
 * delivery path is left byte-identical and RT is handled additively. */
#define SIGRTMIN 32
#define SIGRTMAX 63
#define NSIG_MAX 63
#define SIG_IS_RT(s) ((s) >= SIGRTMIN && (s) <= SIGRTMAX)
#define SCHED_MAX_FDS 64
#define SCHED_MAX_FD_LIMIT 1024

/* task->exit_code encoding: low 8 bits carry the exit status or the signal
 * number; this bit marks death-by-signal. Numeric overloading (the old
 * "128+sig" range) misreported a legitimate exit(128..158) — e.g. the libc
 * assert()'s _exit(139) — as WIFSIGNALED to waitpid. */
#define TASK_EXIT_SIGNALED 0x10000

/* Signal actions */
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int)) - 1)

/* Signal flags */
#define SA_NOCLDSTOP 1
#define SA_NOCLDWAIT 2
#define SA_SIGINFO 4
#define SA_RESTORER 0x04000000
#define SA_ONSTACK 0x08000000
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000

/* sigaltstack ss_flags and minimum size (must match userspace signal.h). */
#define SS_ONSTACK 1
#define SS_DISABLE 2
#define MINSIGSTKSZ 2048

/* Alternate signal stack descriptor. Layout MUST match userspace stack_t
 * (signal.h): { void *ss_sp; int ss_flags; unsigned long ss_size; }. Use
 * pointer-width (usize) for ss_sp/ss_size so the byte layout is identical on
 * both the 64-bit and 32-bit ABIs — the syscall copies sizeof(kstack_t) bytes
 * straight from the user struct. */
typedef struct {
  usize ss_sp;   /* user pointer to the base of the alt stack */
  int ss_flags;  /* SS_DISABLE / SS_ONSTACK (query-only) */
  usize ss_size; /* size of the alt stack in bytes */
} kstack_t;

typedef void (*sighandler_t)(int);

struct vm_area {
  u64 start;
  u64 end;
  u32 prot;
  u32 flags;
  struct vfs_node *node;
  isize offset;
  struct vm_area *next;
};

typedef void (*kernel_thread_entry)(void *arg);

struct sigaction {
  sighandler_t sa_handler;
  u64 sa_flags;
  void (*sa_restorer)(void);
  u64 sa_mask; /* signals to block during handler */
};

/* M74: RT-signal payload (POSIX union sigval), carried by sigqueue and by a
 * SIGEV_SIGNAL timer's sigev_value and delivered to an SA_SIGINFO handler as
 * siginfo->si_value. */
union sigval {
  int sival_int;
  void *sival_ptr;
};

typedef unsigned long rlim_t;
struct rlimit {
  rlim_t rlim_cur;
  rlim_t rlim_max;
};

#define RLIMIT_CPU    0
#define RLIMIT_FSIZE  1
#define RLIMIT_DATA   2
#define RLIMIT_STACK  3
#define RLIMIT_CORE   4
#define RLIMIT_NOFILE 7
#define RLIMIT_AS     9
#define RLIM_INFINITY ((rlim_t)-1)

struct task {
  usize id;
  const char *name;
  enum task_state state;
  struct cpu_context context;
  kernel_thread_entry entry;
  void *arg;
  void *stack;
  u64 kernel_stack_ptr;
  u64 saved_user_rsp;
  u64 wake_tick;
  void *wait_chan;
  int stdout_fd;
  struct vfs_handle **fd_table;
  int *fd_flags;
  usize fd_capacity;
  spinlock_t fd_lock; /* protects fd_table/fd_flags/fd_capacity */
  int priority;
  int exit_code;
  usize parent_id;
  char cwd[64];
  u64 user_brk;
  u64 heap_start;
  u16 umask;
  usize process_group_id;
  usize session_id;
  char env[16][64]; // Replaced TASK_ENV_MAX/TASK_ENV_VALUE_MAX with literals
                    // for header simplicity

  /* Signal handling. pending_signals is updated with 8-byte __atomic ops from
   * ISR/signal/scheduler paths; force 8-byte alignment so those atomics are
   * naturally aligned on i686 (where a bare u64 field is only 4-byte aligned —
   * the lock-prefixed op stays correct but warns and pays a cache penalty).
   * Tasks are kmalloc'd in chunks, so the small sizeof bump is harmless. */
  __attribute__((aligned(8))) u64 pending_signals; /* bitmask of pending signals */
  u64 blocked_signals;             /* bitmask of blocked signals */
  struct sigaction sigactions[31]; /* NSIG = 31 */

  /* Credentials */
  struct cred *cred;
  /* Userspace Image */
  void *user_image;
  u64 pml4_phys;
  struct vm_area *vma_list;
  int in_kernel_syscall;
  int last_stop_signal;
  int stop_report_pending;
  int continued_report_pending;

  /* x86 FPU/SSE state. 512-byte FXSAVE area, 16-byte aligned for
   * fxsave/fxrstor. Saved/restored across context switches so userspace
   * XMM/MXCSR/x87 state survives scheduling (e.g. the native GCC's cc1).
   * fpu_initialized is 0 until the task has run once; the first switch-in
   * loads a clean FPU image instead of the uninitialized buffer. */
  __attribute__((aligned(16))) u8 fpu_state[512];
  int fpu_initialized;

  /* SMP work-stealing: when set, this task is a self-contained CPU-bound
   * kernel worker that an idle Application Processor may steal and run on its
   * own idle context (see ap_main / ap_worker_trampoline). Ordinary userspace
   * tasks leave this 0 so APs never migrate them — the kernel's syscall/VFS
   * paths are not yet SMP-safe for parallel kernel-mode execution. */
  int stealable;

  /* SMP: set when an Application Processor may run this task. Only real
   * userspace ELF processes qualify — they enter ring 3 and so release the Big
   * Kernel Lock, letting cores run in parallel. Kernel threads (daemons like
   * net_task, builtins, the boot/idle task) stay on the BSP: they execute
   * entirely in ring 0 and would hold the BKL across their cooperative yields,
   * monopolising it. Set at creation (kthread_create_user) and inherited by
   * fork. */
  int ap_runnable;

  /* Kernel-stack lease flag for cross-CPU reap safety (M28 T4 prerequisite).
   *
   * A dying task on CPU A executes `scheduler_exit_current` → publishes
   * `state = TASK_DEAD` → runs `scheduler_yield`'s body → calls
   * `arch_context_switch` — and is STILL using its own kernel_stack as RSP
   * the entire time, right up to the `mov 0(%rsi), %rsp` swap inside the
   * asm. Meanwhile, under T4 (no BKL), the parent's `scheduler_waitpid`
   * on CPU B can win the `DEAD → REAPING` CAS the instant DEAD is
   * published and `kfree` the stack out from under CPU A. The freed page
   * returns to kheap, gets handed to some other allocation, and CPU A's
   * subsequent stack pushes (or, much later, the iret frame on this
   * page after a context-switch-back) land on corrupted bytes —
   * shape #1/#2 of the SMP context-switch-back race.
   *
   * Protocol: the death-path clears `stack_released = 0` BEFORE writing
   * `state = TASK_DEAD` (x86 TSO orders the stores). `arch_context_switch`
   * sets `*released_publish = 1` AFTER the RSP swap. `scheduler_waitpid`
   * spins on `stack_released == 1` after winning the CAS, before kfree. */
  volatile int stack_released;

  /* SMP runqueue linkage (must be last field for ABI compat) */
  struct task *next_run;
};

/* M29: per-task thread metadata stored in parallel side-tables indexed by
 * the task's slot index in the chunked g_task_chunks[]. Kept out of struct
 * task itself because adding fields to that struct broke an unrelated
 * paging invariant on M14 (LAPIC PT became unreachable from user pml4
 * after kheap allocation order shifted; the root cause is a pre-existing
 * latent issue that surfaces only when struct task grows). Accessors live
 * in kernel/sched/scheduler.c. */
int  task_is_thread(const struct task *t);
void task_set_is_thread(struct task *t, int v);
u64  task_tls_base(const struct task *t);
void task_set_tls_base(struct task *t, u64 base);
u64  task_child_tid_clear(const struct task *t);
void task_set_child_tid_clear(struct task *t, u64 addr);
u64  task_saved_sigmask(const struct task *t);
int  task_has_saved_sigmask(const struct task *t);
void task_set_saved_sigmask(struct task *t, u64 mask, int has_saved);
void task_clear_saved_sigmask(struct task *t);
/* M63: seccomp filter chain + no_new_privs (side-table backed, see seccomp.c). */
void *task_seccomp_filter(const struct task *t);
void  task_set_seccomp_filter(struct task *t, void *f);
int   task_no_new_privs(const struct task *t);
void  task_set_no_new_privs(struct task *t, int v);
/* sigaltstack side-table (per-task, NOT a struct task field). */
void task_get_altstack(const struct task *t, kstack_t *out);
int  task_set_altstack(struct task *t, const kstack_t *ss);
/* Returns the top (highest address) of the registered alt stack, or 0 if no
 * usable alt stack is set. Signal delivery uses this to honor SA_ONSTACK. */
u64  task_altstack_top(const struct task *t);
/* Non-zero if `sp` lies within the registered alt stack range. */
int  task_on_altstack(const struct task *t, u64 sp);
u64  task_alarm_ticks(const struct task *t);
void task_set_alarm_ticks(struct task *t, u64 ticks);
u64  task_alarm_interval_ticks(const struct task *t);
void task_set_alarm_interval_ticks(struct task *t, u64 ticks);
usize task_tgid(const struct task *t);
u64  task_utime(const struct task *t);
u64  task_stime(const struct task *t);
u64  task_cutime(const struct task *t);
u64  task_cstime(const struct task *t);
/* Last userspace RIP at the moment the LAPIC timer tick preempted this task
 * (0 for kernel tasks / never-preempted). Watchdog diagnostic: names the user
 * function a wedged thread group is spinning in. */
u64  task_user_rip(const struct task *t);
void task_set_user_rip(struct task *t, u64 rip);
int  scheduler_getrlimit(int resource, struct rlimit *rlim);
int  scheduler_getrlimit_task(const struct task *t, int resource,
                              struct rlimit *rlim);
/* Upper bound on concurrent tasks (the pid ceiling reported by
 * /proc/sys/kernel/pid_max). */
usize scheduler_max_tasks(void);
/* Post a pending signal without scheduler_kill's stop/continue side effects. */
int scheduler_post_signal(usize pid, int sig);
/* ioprio(2): per-task I/O class+level, consumed by the block layer's admission
 * gate (kernel/dev/blk.c). */
int scheduler_set_ioprio(usize pid, int ioprio);
int scheduler_get_ioprio(usize pid);
/* chroot(2): the calling task's filesystem root (NULL = the real root) and the
 * absolute path it was set from. scheduler_set_root takes ownership of one
 * reference on `node`. */
struct vfs_node *scheduler_get_root_node(void);
const char *scheduler_get_root_path(void);
int scheduler_set_root(struct vfs_node *node, const char *path);
/* sched_setaffinity(2): per-task CPU mask (0 = any online CPU). */
int sched_task_allowed_on_cpu(const struct task *t, int cpu);
int scheduler_set_affinity(usize pid, u64 mask);
u64 scheduler_get_affinity(usize pid);
/* swapoff(2): page every address space's swapped pages back in. */
usize scheduler_swapin_all_tasks(void);
int  scheduler_setrlimit(int resource, const struct rlimit *rlim);

/* Per-CPU current task. `current_task` is the task running on THIS CPU; each
 * core has its own slot in struct percpu (cur_task), so APs and the BSP never
 * share one "current". Expands to an lvalue, so existing reads/writes
 * (current_task = t, current_task->field, current_task == 0) all work
 * unchanged. Requires GS to be initialized (percpu_init) before first use. */
#define current_task (get_percpu()->cur_task)

/* ── Scheduler ── */

void scheduler_init(void);
int kthread_create(const char *name, kernel_thread_entry entry, void *arg);
/* Like kthread_create, but marks the task ap_runnable so Application Processors
 * may run it (used for userspace processes, which enter ring 3 and release the
 * BKL). See struct task::ap_runnable. */
int kthread_create_user(const char *name, kernel_thread_entry entry, void *arg,
                        int ap_runnable);
/* Create a stealable CPU-bound kernel worker (see struct task::stealable).
 * Returns the task id, or -1 on failure. The worker is enqueued READY on the
 * creating CPU's runqueue; an idle AP may steal and run it. */
int sched_create_stealable_worker(const char *name, kernel_thread_entry entry,
                                  void *arg);
/* Reap a finished stealable worker (called by an AP after the worker parks). */
void sched_ap_reap_worker(struct task *t);
/* SMP work-stealing self-test (M24b). No-op unless >1 CPU is online and
 * test mode is active. */
void smp_selftest_run(void);

/* M28 #9 — ctx-switch + light-syscall rdtsc benchmark. Single-CPU only;
 * test mode only. See kernel/sched/m28_ctxbench.c for what's measured. */
void m28_ctxbench_run(void);

/* M28 #4 — SMP heap_lock contention benchmark (1 vs N cores hammering
 * kmalloc/kfree). >1 CPU + test mode only. See kernel/sched/m28_heapbench.c. */
void m28_heapbench_run(void);
int scheduler_fork_current(void);

/* M29: SYS_CLONE entry. Creates a new task that runs `entry(arg)` in ring 3
 * on the provided user stack. `flags` is a B1NIX_CLONE_* bitmask. Returns
 * the new task id (TID) on success, or -errno on failure.
 *
 * Supported flags:
 *   CLONE_VM       — share parent's pml4 + vma_list + user_image (no copy).
 *   CLONE_FS       — share cwd / umask / env (no copy).
 *   CLONE_FILES    — share fd_table / fd_flags / fd_capacity (no copy).
 *   CLONE_SIGHAND  — copy parent's signal action table (b1nix has no per-
 *                    process signal sharing yet; thread signals are best-
 *                    effort).
 *   CLONE_THREAD   — make parent_id = current's parent_id so waitpid skips
 *                    this task (it is joined via futex on child_tid).
 *   CLONE_SETTLS   — initialize tls_base from `tls`.
 *   CLONE_CHILD_CLEARTID — store `ctid` and write 0 + futex_wake on exit.
 */
/* CLONE_VFORK handshake: the child calls this once it has execve()d (or on
 * exit) to let a vfork parent continue. */
void scheduler_vfork_release(void);

int scheduler_clone_thread(u64 flags, u64 entry, u64 user_stack, u64 arg,
                           u64 tls, u64 ctid,
                           u64 parent_tid_addr, u64 child_tid_addr,
                           u64 start_func);

/* M29: futex. op is B1NIX_FUTEX_WAIT or B1NIX_FUTEX_WAKE. Returns 0 on success,
 * -errno otherwise. WAIT blocks if *uaddr == val; WAKE wakes up to val
 * waiters on uaddr. For WAIT, timeout_ms > 0 arms a relative timer deadline
 * (10 ms granularity); the call returns -ETIMEDOUT if it elapses before a wake.
 * timeout_ms == 0 means block indefinitely. */
int scheduler_futex(u64 uaddr, int op, int val, u64 timeout_ms);
void scheduler_futex_wake_addr(u64 uaddr, int val);
void scheduler_futex_cleanup_task(usize task_id);

/* M29: reap any DEAD thread tasks (created via SYS_CLONE with is_thread=1)
 * whose kernel stack is no longer in use. Called from scheduler_yield. */
void scheduler_reap_dead_threads(void);
/* Cooperatively switch to another runnable task. Returns 1 if it context
 * switched (and has since been resumed), 0 if nothing was runnable. An idle
 * loop uses the 0 return to drop the Big Kernel Lock before parking. */
int scheduler_yield(void);
/* The shared runqueue of READY non-stealable tasks (scheduler.c). */
struct runqueue *sched_global_rq(void);
/* Initialise (and return) the dedicated idle task for AP `cpu` running on the
 * given kernel stack top. The AP runs it as current_task and the cooperative
 * scheduler parks back to it when no other task is runnable. */
struct task *scheduler_setup_ap_idle(int cpu, u64 kstack_top);
/* Bitmask of CPU ids that have run a syscall from an ELF userspace task — the
 * M24b proof that userspace executes on APs (defined in syscall.c). */
u32 sched_user_cpu_mask(void);
void scheduler_block_current(void);
void scheduler_block_on(void *chan);
void scheduler_block_on_timeout(void *chan, u64 timeout_ticks);

/* Monotonic scheduler tick counter (10 ms cadence). */
u64 scheduler_get_ticks(void);
void scheduler_wait_prepare(void *chan);
void scheduler_wait_prepare_timeout(void *chan, u64 timeout_ticks);
void scheduler_wait_commit(void);
void scheduler_wait_cancel(void);
/* True when the current context may park on a wait channel (scheduler live, real
 * task context, interrupts enabled). Drivers fall back to polling when false. */
int scheduler_can_block(void);
/* Kernel CSPRNG-ish entropy source (rdrand, xorshift64* fallback). Shared by
 * SYS_GETRANDOM and the M71 ASLR load-base randomizer. */
u64 kernel_random_u64(void);
void scheduler_wake_task(usize task_id);
void scheduler_wake_all(void *chan);
void scheduler_notify_wait_event(usize parent_id);
void scheduler_sleep_ticks(u64 ticks);
void scheduler_on_timer_tick(void);
void scheduler_charge_tick(int is_user);
void scheduler_exit_current(int exit_code) __attribute__((noreturn));
/* Tell the scheduler which pid is /bin/init, so its death is reported loudly
 * instead of ending the run in silence (see scheduler_exit_current). */
void scheduler_set_init_pid(usize pid);
/* Non-zero once /bin/init has been spawned; 0 before that. */
usize scheduler_get_init_pid(void);
void scheduler_exit_group(int exit_code) __attribute__((noreturn));
int scheduler_wait(usize pid, int *status);
int scheduler_waitpid(usize pid, int *status, int options);
int scheduler_waitid(idtype_t idtype, usize id, siginfo_t *infop, int options);
usize scheduler_task_count(void);
void scheduler_dump_tasks(void);
void scheduler_lease_clear_here(const char *site);
/* M34: read-only task-table introspection for procfs / ps / top. */
usize scheduler_task_slots(void);
struct task *scheduler_task_slot(usize index);
struct task *scheduler_task_by_pid(usize pid);
const char *scheduler_state_name(int state);
void scheduler_get_ctty(int *type, int *index);
void scheduler_set_ctty(struct task *t, int type, int index);
void scheduler_set_stdout(int fd);
int scheduler_get_stdout(void);
void scheduler_fd_table_init_current(void);
int scheduler_fd_alloc(struct vfs_handle *handle);
struct vfs_handle *scheduler_fd_get(int fd);
struct vfs_handle *scheduler_fd_get_retain(int fd);
int scheduler_fd_set(int fd, struct vfs_handle *handle);
int scheduler_fd_close(int fd);
struct vfs_handle *scheduler_fd_take(int fd);
int scheduler_fd_flags_get(int fd);
int scheduler_fd_flags_set(int fd, int flags);
void scheduler_fd_close_on_exec(void);

/* ── Signal API ── */
int scheduler_kill(usize task_id, int sig);
/* OOM reclaim: SIGKILL the current userspace task (the memory demander). Returns
 * 1 if a victim was signalled (skips kernel threads and init). */
int scheduler_oom_kill_current(void);
int scheduler_kill_process_group(usize pgrp, int sig);
int scheduler_kill_all(int sig);
int scheduler_sigaction(int sig, const struct sigaction *act,
                        struct sigaction *old);
int scheduler_sigprocmask(int how, const u64 *set, u64 *oldset);
/* M74: queue an RT signal (SIGRTMIN..SIGRTMAX) with a payload to a task. Unlike
 * scheduler_kill, repeated calls QUEUE (do not coalesce): each enqueues a
 * (signo, value) entry delivered FIFO, lowest signo first. si_code is SI_QUEUE
 * for sigqueue(3) or SI_TIMER for a POSIX timer. Returns 0, -EAGAIN if the
 * per-task RT queue is full, -EINVAL/-ESRCH otherwise. */
int scheduler_sigqueue(usize task_id, int sig, union sigval value, int si_code);
/* RT-signal delivery helpers, called from the arch signal path (interrupts off):
 * dequeue the oldest queued instance of `sig` (FIFO) and look up its handler. */
int scheduler_rt_dequeue_current(int sig, int *si_code, union sigval *value,
                                 int *more);
struct sigaction *scheduler_rt_action_current(int sig);
/* siginfo si_code origins (POSIX). */
#define B1NIX_SI_USER 0
#define B1NIX_SI_QUEUE (-1)
#define B1NIX_SI_TIMER (-2)

/* M74 POSIX per-process timers (timer_create/settime/gettime/delete). Times are
 * in scheduler ticks (100 Hz). On expiry the timer queues `signo` (an RT signal
 * carries `value` as si_value). */
int scheduler_timer_create(int signo, union sigval value);
int scheduler_timer_settime(int id, u64 first_ticks, u64 interval_ticks,
                            u64 *old_remaining, u64 *old_interval);
int scheduler_timer_gettime(int id, u64 *remaining, u64 *interval);
int scheduler_timer_delete(int id);
void scheduler_timer_cleanup_task(usize task_id);
void scheduler_deliver_pending_signals(void);
int  scheduler_signal_pending(void);
/* M56 signalfd helpers. */
u64  scheduler_peek_pending_signals(u64 mask);
int  scheduler_consume_pending_signal(int sig);
sighandler_t scheduler_get_sighandler(int sig);
usize scheduler_get_pid(void);

/* Reserve PID 1 for the next task created (the userspace init process). The
 * boot/idle task is PID 0, so 1 is otherwise never handed out. */
void scheduler_reserve_init_pid(void);
void scheduler_set_user_image(void *image);
struct cred *scheduler_get_current_cred(void);
const char *scheduler_get_cwd(void);
int scheduler_set_cwd(const char *path);
u64 scheduler_get_uptime_ticks(void);
u64 scheduler_brk_get(void);
u64 scheduler_mmap_bump_alloc(usize length);
int scheduler_set_priority(usize pid, int priority);
int scheduler_get_priority(usize pid);
isize scheduler_setsid(void);
isize scheduler_getsid(usize pid);
usize scheduler_getpgrp(void);
int scheduler_setpgrp(usize pid, usize pgrp);
isize scheduler_getpgid(usize pid);
void scheduler_mark_execed_current(void);
int scheduler_is_pgrp_in_session(usize pgrp, usize session_id);
u64 vm_find_free_area(struct task *t, usize length);
struct vm_area *vma_split(struct task *t, struct vm_area *vma, u64 addr);
void vma_delete_range(struct task *t, u64 start, u64 end);

#endif
