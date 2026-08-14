/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_SCHED_H
#define LKPI_LINUX_SCHED_H
#include <linux/errno.h>
#include <lkpi/env.h>
#include <linux/types.h>
/* Scheduling, onto b1nix's. Ticks here are b1nix ticks (10 ms); imported code
 * that reasons in jiffies gets the same unit, which is what <linux/jiffies.h>
 * documents. */
#define TASK_RUNNING         0
#define TASK_INTERRUPTIBLE   1
#define TASK_UNINTERRUPTIBLE 2
#define MAX_SCHEDULE_TIMEOUT ((long)(~0UL >> 1))
static inline void cond_resched(void) { lkpi_yield(); }
static inline void set_current_state(int state) { (void)state; }
static inline void __set_current_state(int state) { (void)state; }
static inline long schedule_timeout(long timeout)
{
	if (timeout > 0)
		lkpi_sleep_ticks((u64)timeout);
	return 0;
}
/*
 * schedule(), as the caller of prepare_to_wait() means it.
 *
 * Upstream splits a park into two statements: prepare_to_wait() publishes the
 * waiter and marks the task not-runnable, and schedule() is what actually gives
 * up the CPU. b1nix's park is the same two phases, so schedule() must be the
 * second one — not a yield. A yield here left the task marked blocked and
 * simply switched away from it: it was never on a run queue again and never on
 * any channel a wake could reach, which is how a compositor's modeset commit
 * came to sit inside intel_atomic_commit_tail() forever.
 *
 * Still a yield when nothing armed a wait, because that is what a plain
 * schedule() means in imported code that only wants to be preempted.
 */
static inline void schedule(void) { lkpi_schedule(); }
static inline int signal_pending(void *t) { (void)t; return 0; }

/* `current` and struct task_struct come from <linux/types.h>, which every
 * imported file reaches; see there. */

/* Raise the submission thread to a real-time policy. b1nix has priorities but
 * not this interface; the request is declined and the thread runs at its
 * normal priority, which costs latency and not correctness. */
/* Wake a task that parked itself. lkpi's wait queues wake by channel, so this
 * is only reached for a task holding a pointer rather than a channel — nothing
 * in the core does, and the day one does it needs a real per-task wake. */
static inline int wake_up_process(void *task) { (void)task; return 0; }

static inline int sched_set_fifo(void *task) { (void)task; return -EINVAL; }
static inline int sched_set_fifo_low(void *task) { (void)task; return -EINVAL; }

/* Sleeping for a fixed time with no wakeup to wait for. Uninterruptible here is
 * not a weaker promise than upstream's: b1nix kernel threads are not signal
 * targets, so no sleep in this file can be cut short. */
static inline long schedule_timeout_uninterruptible(long timeout)
{ return schedule_timeout(timeout); }
static inline long schedule_timeout_interruptible(long timeout)
{ return schedule_timeout(timeout); }
static inline long schedule_timeout_killable(long timeout)
{ return schedule_timeout(timeout); }


/*
 * An assertion that the caller is somewhere it may sleep.
 *
 * Upstream's checks the current context and complains loudly when a sleeping
 * function is reached from an atomic one — the class of bug that otherwise
 * shows up as a deadlock under load. b1nix knows the same fact
 * (lkpi_can_block), so this is a real check rather than a no-op: it reports
 * through the same log a caller would look at.
 */
void lkpi_might_sleep(const char *where);
#define might_sleep() lkpi_might_sleep(__func__)
#define might_sleep_if(cond) do { if (cond) might_sleep(); } while (0)


/* Whether a sleep should be cut short by a pending signal. b1nix kernel threads
 * are not signal targets, so nothing interrupts one and the answer is always
 * no — which is why every wait_event_interruptible here returns 0. */
static inline int signal_pending_state(unsigned int state, void *task)
{ (void)state; (void)task; return 0; }
/* signal_pending is already defined above. */


/* An assertion that this context may allocate. GFP_ATOMIC never sleeps, so it
 * is always allowed; anything else may, and is checked the same way a sleeping
 * call is. */
#define might_alloc(gfp) do { if (!((gfp) & __GFP_ATOMIC)) might_sleep(); } while (0)


/* Yield if something else is waiting. b1nix's scheduler is preemptive, so a
 * long loop is not starving anyone — but the yield is real rather than empty,
 * because these sit in loops that walk thousands of pages and letting a higher
 * priority task in is the whole point. */
/* cond_resched is defined above. This is the form that drops a lock across the
 * yield — the lock is what makes it necessary, since yielding while holding one
 * is how a long walk blocks everything else on the same object. */
#define cond_resched_lock(lock) \
	({ spin_unlock(lock); cond_resched(); spin_lock(lock); 0; })


/* Is a reschedule pending on this CPU? b1nix sets the flag from the timer tick;
 * this reads it. */
bool need_resched(void);

#define TASK_NORMAL (TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)

/* Sleep with the "this task is waiting on I/O" accounting. b1nix's scheduler
 * keeps no iowait accounting, so this is the plain timed sleep — the wait is
 * the same length, it is simply not attributed. */
long io_schedule_timeout(long timeout);

/*
 * A reference to a task's pid object.
 *
 * Declared and deliberately not defined. b1nix identifies tasks by their struct
 * task pointer and pid number; there is no separate refcounted pid object that
 * outlives the task, which is the whole point of upstream's — it lets a driver
 * hold a task identity safely after the task exits. Returning the raw pid would
 * hand back something that can be recycled under the holder.
 */
enum pid_type { PIDTYPE_PID, PIDTYPE_TGID, PIDTYPE_PGID, PIDTYPE_SID, PIDTYPE_MAX };
struct pid;
struct pid *get_task_pid(struct lkpi_task *task, enum pid_type type);

#endif
