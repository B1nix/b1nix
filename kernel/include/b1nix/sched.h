#ifndef B1NIX_SCHED_H
#define B1NIX_SCHED_H

#include <b1nix/types.h>

struct task;
struct cred;

enum task_state {
  TASK_UNUSED = 0,
  TASK_RUNNING,
  TASK_READY,
  TASK_BLOCKED,
  TASK_SLEEPING,
  TASK_DEAD,
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

/* Signal actions */
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int)) - 1)

/* Signal flags */
#define SA_NOCLDSTOP 1
#define SA_NOCLDWAIT 2
#define SA_SIGINFO 4
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
  int fd_table[SCHED_MAX_FDS];
  int fd_flags[SCHED_MAX_FDS];
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
  u64 mmap_bump;

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
};

extern struct task *current_task;

/* ── Scheduler ── */

void scheduler_init(void);
int kthread_create(const char *name, kernel_thread_entry entry, void *arg);
int scheduler_fork_current(void);
void scheduler_yield(void);
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
int scheduler_fd_alloc(int handle);
int scheduler_fd_get(int fd);
int scheduler_fd_set(int fd, int handle);
int scheduler_fd_close(int fd);
int scheduler_fd_flags_get(int fd);
int scheduler_fd_flags_set(int fd, int flags);
void scheduler_fd_close_on_exec(void);

/* ── Signal API ── */
int scheduler_kill(usize task_id, int sig);
int scheduler_sigaction(int sig, const struct sigaction *act,
                        struct sigaction *old);
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
u64 vm_find_free_area(struct task *t, usize length);
struct vm_area *vma_split(struct task *t, struct vm_area *vma, u64 addr);
void vma_delete_range(struct task *t, u64 start, u64 end);

#endif
