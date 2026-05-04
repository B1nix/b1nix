#ifndef B1NIX_SCHED_H
#define B1NIX_SCHED_H

#include <b1nix/types.h>

typedef void (*kernel_thread_entry)(void *arg);

void scheduler_init(void);
int kthread_create(const char *name, kernel_thread_entry entry, void *arg);
void scheduler_yield(void);
void scheduler_block_current(void);
void scheduler_wake_task(usize task_id);
void scheduler_sleep_ticks(u64 ticks);
void scheduler_on_timer_tick(void);
void scheduler_exit_current(void) __attribute__((noreturn));
usize scheduler_task_count(void);
void scheduler_dump_tasks(void);
void scheduler_set_stdout(int fd);
int scheduler_get_stdout(void);

#endif
