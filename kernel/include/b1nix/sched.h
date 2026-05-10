#ifndef B1NIX_SCHED_H
#define B1NIX_SCHED_H

#include <b1nix/types.h>

/* ── Signals ── */
#define SIGABRT     1
#define SIGALRM     2
#define SIGBUS      3
#define SIGCHLD     4
#define SIGCONT     5
#define SIGFPE      6
#define SIGHUP      7
#define SIGILL      8
#define SIGINT      9
#define SIGKILL     10
#define SIGPIPE     11
#define SIGQUIT     12
#define SIGSEGV     13
#define SIGSTOP     14
#define SIGTERM     15
#define SIGTSTP     16
#define SIGTTIN     17
#define SIGTTOU     18
#define SIGUSR1     19
#define SIGUSR2     20
#define SIGSYS      21
#define SIGTRAP     22
#define SIGURG      23
#define SIGXCPU     24
#define SIGXFSZ     25
#define SIGVTALRM   26
#define SIGPROF     27
#define SIGWINCH    28
#define SIGIO       29
#define SIGPWR      30

#define NSIG        31
#define SCHED_MAX_FDS 64

/* Signal actions */
#define SIG_DFL     ((void (*)(int))0)
#define SIG_IGN     ((void (*)(int))1)
#define SIG_ERR     ((void (*)(int))-1)

/* Signal flags */
#define SA_NOCLDSTOP   1
#define SA_NOCLDWAIT   2
#define SA_SIGINFO     4
#define SA_ONSTACK     0x08000000
#define SA_RESTART     0x10000000
#define SA_NODEFER     0x40000000
#define SA_RESETHAND   0x80000000

typedef void (*sighandler_t)(int);

struct sigaction {
    sighandler_t sa_handler;
    u64          sa_flags;
    void         (*sa_restorer)(void);
    u64          sa_mask;         /* signals to block during handler */
};

/* ── Scheduler ── */

typedef void (*kernel_thread_entry)(void *arg);

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
int  scheduler_kill(usize task_id, int sig);
int  scheduler_sigaction(int sig, const struct sigaction *act, struct sigaction *old);
void scheduler_deliver_pending_signals(void);
sighandler_t scheduler_get_sighandler(int sig);
usize scheduler_get_pid(void);
void scheduler_set_user_image(void *image);
struct cred *scheduler_get_current_cred(void);
const char *scheduler_get_cwd(void);
int scheduler_set_cwd(const char *path);
u64 scheduler_get_uptime_ticks(void);
u64 scheduler_brk_get(void);
usize scheduler_brk_set(u64 new_brk);
int scheduler_set_priority(usize pid, int priority);
int scheduler_get_priority(usize pid);
usize scheduler_setsid(void);
usize scheduler_getpgrp(void);
int scheduler_setpgrp(usize pid, usize pgrp);

#endif
