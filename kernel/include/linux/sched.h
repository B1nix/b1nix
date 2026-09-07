/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SCHED_H
#define LKPI_LINUX_SCHED_H
#include <lkpi/lock.h>
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
/* Arm the park, the way Linux's two-statement sleep needs it.
 *
 * set_current_state(TASK_INTERRUPTIBLE) followed by schedule() is a contract: a
 * wake between the two cancels the sleep. A no-op here broke it, and the loss
 * was invisible except as a wait that always ran to its full timeout. */
static inline void set_current_state(int state)
{
	if (state != TASK_RUNNING)
		lkpi_prepare_to_sleep();
}
static inline void __set_current_state(int state) { set_current_state(state); }
/* Sleep, and say how much of the sleep was left.
 *
 * The return value is the contract: callers read zero as "the timeout expired"
 * and anything positive as "something woke me". Answering zero always made
 * every completed wait look like a timed-out one. */
static inline long schedule_timeout(long timeout)
{
	if (timeout <= 0)
		return 0;
	return (long)lkpi_sleep_jiffies((u64)timeout);
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
/* Wake a task that parked with schedule_timeout().
 *
 * A stub returning 0 made every wait in imported code run to its full timeout:
 * the wakeup that should have ended it early went nowhere. i915_request_wait is
 * built on exactly this — park, and let the fence callback wake you — so a
 * request the hardware finished in microseconds took the full two seconds and
 * then reported a timeout. */
static inline int wake_up_process(struct lkpi_task *task)
{ return lkpi_wake_task(task); }

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
#undef might_sleep /* defined by another shim header too; this copy is the one that took effect */
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

/* The allocation-scope helpers. Upstream keeps them in <linux/sched/mm.h> and
 * so do we, but imported code reaches them through this header — it includes
 * <linux/sched.h> and calls memalloc_nofs_save() from the same file. */
/*
 * Per-task flags.
 *
 * Only the allocation scopes are here, because they are the only ones a
 * filesystem reads: they say that reclaim from this task must not re-enter a
 * filesystem (NOFS) or issue I/O (NOIO), which is how a transaction protects
 * itself from being re-entered through the allocator. The values are
 * upstream's.
 */
#define PF_MEMALLOC        0x00000800
#define PF_KSWAPD          0x00020000
#define PF_MEMALLOC_NOFS   0x00040000
#define PF_MEMALLOC_NOIO   0x00080000
#define PF_LOCAL_THROTTLE  0x00100000
#define PF_KTHREAD         0x00200000
#define PF_MEMALLOC_NORECLAIM 0x00800000

#include <linux/sched/mm.h>

/*
 * Give the CPU up if something is waiting for the lock we hold.
 *
 * The point is a long loop that holds a lock: it checks, drops, yields and
 * retakes rather than starving every other waiter for the length of the walk.
 * `spin_needbreak` is the "is anyone waiting" half — answering false always is
 * safe (nobody yields early) and answering true always is a live-lock, so the
 * safe direction is the one taken until b1nix's spinlocks can report waiters.
 */
int cond_resched_rwlock_write(void *lock);
int cond_resched_rwlock_read(void *lock);
/* Takes the lkpi type directly rather than `spinlock_t`: this header is on the
 * <linux/types.h> chain that <linux/spinlock.h> starts, so the typedef has not
 * happened yet here. Same type either way. */
static inline int spin_needbreak(struct lkpi_spinlock *lock)
{ (void)lock; return 0; }

/* I/O priority of a task, which btrfs sets on its worker threads so that
 * background work does not outrank the writes it exists to serve. */
#define IOPRIO_CLASS_NONE 0
#define IOPRIO_CLASS_RT   1
#define IOPRIO_CLASS_BE   2
#define IOPRIO_CLASS_IDLE 3
#define IOPRIO_PRIO_VALUE(class, level) ((((class) & 0x7) << 13) | ((level) & 0x1fff))
int set_task_ioprio(struct lkpi_task *task, int ioprio);

/* The jiffies comparisons live in <linux/jiffies.h>, which already defines
 * them; a second set of the same macros is a redefinition rather than a
 * parallel spelling. */

#endif
