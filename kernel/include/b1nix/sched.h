#ifndef B1NIX_SCHED_H
#define B1NIX_SCHED_H
#define B1NIX_WNOHANG 1
#define B1NIX_WUNTRACED 2
#define B1NIX_WCONTINUED 8

#include <b1nix/types.h>
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
#else
  u64 rsp;
  u64 rbp;
  u64 rbx;
  u64 r12;
  u64 r13;
  u64 r14;
  u64 r15;
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
#define SCHED_MAX_FDS 64
#define SCHED_MAX_FD_LIMIT 1024

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

  /* Signal handling */
  u64 pending_signals;             /* bitmask of pending signals */
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
   * shape #1/#2 from docs/m28-t4-blocker.md.
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
int scheduler_clone_thread(u64 flags, u64 entry, u64 user_stack, u64 arg,
                           u64 tls, u64 ctid);

/* M29: futex. op is B1NIX_FUTEX_WAIT or B1NIX_FUTEX_WAKE. Returns 0 on success,
 * -errno otherwise. WAIT blocks if *uaddr == val; WAKE wakes up to val
 * waiters on uaddr. */
int scheduler_futex(u64 uaddr, int op, int val);
void scheduler_futex_wake_addr(u64 uaddr, int val);

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
void scheduler_wake_task(usize task_id);
void scheduler_wake_all(void *chan);
void scheduler_sleep_ticks(u64 ticks);
void scheduler_on_timer_tick(void);
void scheduler_exit_current(int exit_code) __attribute__((noreturn));
int scheduler_wait(usize pid, int *status);
int scheduler_waitpid(usize pid, int *status, int options);
usize scheduler_task_count(void);
void scheduler_dump_tasks(void);
void scheduler_set_stdout(int fd);
int scheduler_get_stdout(void);
void scheduler_fd_table_init_current(void);
int scheduler_fd_alloc(struct vfs_handle *handle);
struct vfs_handle *scheduler_fd_get(int fd);
int scheduler_fd_set(int fd, struct vfs_handle *handle);
int scheduler_fd_close(int fd);
int scheduler_fd_flags_get(int fd);
int scheduler_fd_flags_set(int fd, int flags);
void scheduler_fd_close_on_exec(void);

/* ── Signal API ── */
int scheduler_kill(usize task_id, int sig);
int scheduler_kill_process_group(usize pgrp, int sig);
int scheduler_sigaction(int sig, const struct sigaction *act,
                        struct sigaction *old);
int scheduler_sigprocmask(int how, const u64 *set, u64 *oldset);
void scheduler_deliver_pending_signals(void);
sighandler_t scheduler_get_sighandler(int sig);
usize scheduler_get_pid(void);
void scheduler_set_user_image(void *image);
struct cred *scheduler_get_current_cred(void);
const char *scheduler_get_cwd(void);
int scheduler_set_cwd(const char *path);
u64 scheduler_get_uptime_ticks(void);
u64 scheduler_brk_get(void);
u64 scheduler_mmap_bump_alloc(usize length);
int scheduler_set_priority(usize pid, int priority);
int scheduler_get_priority(usize pid);
usize scheduler_setsid(void);
usize scheduler_getpgrp(void);
int scheduler_setpgrp(usize pid, usize pgrp);
int scheduler_is_pgrp_in_session(usize pgrp, usize session_id);
u64 vm_find_free_area(struct task *t, usize length);
struct vm_area *vma_split(struct task *t, struct vm_area *vma, u64 addr);
void vma_delete_range(struct task *t, u64 start, u64 end);

#endif
