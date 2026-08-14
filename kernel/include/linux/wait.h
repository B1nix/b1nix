/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_WAIT_H
#define LKPI_LINUX_WAIT_H
#include <lkpi/wait.h>
#include <linux/kernel.h>
/* Onto lkpi's wait queues: the predicate is re-tested after every wake, and the
 * two-phase park closes the lost-wakeup race. */
#define wake_up_interruptible(wq) wake_up(wq)
#define wake_up_all_interruptible(wq) wake_up_all(wq)
/*
 * A statement-expression, because wait_event is a statement and imported code
 * assigns the result. Always 0: b1nix kernel threads are not signal targets, so
 * the wait cannot be interrupted and a caller checking for -ERESTARTSYS never
 * sees it.
 */
#define wait_event_interruptible(wq, cond)  \
	({                                       \
		wait_event(wq, cond);                \
		0;                                   \
	})

/* Waking only the waiters interested in particular poll events. lkpi's queues
 * do not carry per-waiter event masks, so every waiter is woken and re-tests
 * its own predicate — more wakeups, never fewer. */
#define wake_up_interruptible_poll(wq, events) do { (void)(events); wake_up(wq); } while (0)
#define wake_up_poll(wq, events)              wake_up_interruptible_poll(wq, events)
#define wake_up_interruptible_all(wq)         wake_up_all(wq)

/*
 * Linux's form is an expression yielding the ticks remaining, so it is written
 * as a statement-expression here rather than through an out-parameter. The
 * out-parameter version lives in <lkpi/wait.h> as wait_event_timeout_r, which
 * this wraps.
 */
#define wait_event_timeout(wq, condition, timeout_ticks)     \
	({                                                       \
		u64 __ret = 0;                                       \
		wait_event_timeout_r(wq, condition, timeout_ticks, __ret); \
		__ret;                                               \
	})

#define wait_event_interruptible_timeout(wq, cond, t) \
	wait_event_timeout(wq, cond, t)

/*
 * Wait with a lock held: drop it around the sleep and retake it before
 * re-testing. Dropping is not optional — the condition is changed by someone
 * who needs that same lock, so holding it across the sleep is a deadlock.
 */
#define wait_event_lock_irq(wq, condition, lock)     \
	do {                                             \
		while (!(condition)) {                       \
			spin_unlock(&(lock));                    \
			wait_event(wq, condition);               \
			spin_lock(&(lock));                      \
		}                                            \
	} while (0)

/*
 * Waking a bit-waiter, without including <linux/wait_bit.h>.
 *
 * That header needs <linux/bitops.h>, and bitops is reached from here through
 * a chain that comes back to this file — so including it makes clear_bit
 * implicitly declared inside wait_bit.h and then redeclared static by bitops.
 * The wake side needs no bit operations at all: the word is the channel, so it
 * can live here and the waiting side stays where it needs its includes.
 */
static inline void wake_up_bit(void *word, int bit)
{ (void)bit; lkpi_wake_all(word); }


/*
 * A waiter declared on the stack, parked and unparked explicitly.
 *
 * This is the open-coded form of wait_event: declare an entry, add it to the
 * queue, test the condition, sleep, remove it. Drivers use it when the
 * condition needs work between the test and the sleep — i915's GMBUS does,
 * because it has to arm the hardware's interrupt in that window.
 *
 * b1nix's park is two-phase for the same reason the macro form is: publishing
 * on the channel before the final test is what stops a wake landing in the gap
 * from being lost.
 */
/*
 * The wake callback an entry carries.
 *
 * b1nix wakes by channel — the queue itself is the channel — so there is
 * nothing per-entry to do beyond what the waker already did. These exist
 * because imported code calls entry->func() unconditionally while walking a
 * queue: i915's sw-fence walks its wait list and calls every entry's func, so
 * an entry with a null func is a jump to zero, and one whose list linkage was
 * never initialised is a jump to whatever the stack held.
 */
int default_wake_function(struct wait_queue_entry *entry, unsigned mode,
                          int flags, void *key);
int autoremove_wake_function(struct wait_queue_entry *entry, unsigned mode,
                             int flags, void *key);

/*
 * The linkage is initialised here, not left to prepare_to_wait().
 *
 * prepare_to_wait() decides whether the entry is already queued by looking at
 * it, and an uninitialised list head on the stack makes that a coin toss — the
 * entry either goes on a queue twice or, worse, is left linked into a queue
 * that outlives the frame it lived in. That is how a wait entry from the boot
 * task's stack ended up being called by a workqueue thread long after the
 * frame was gone.
 */
#define DEFINE_WAIT(name)                                              \
	struct wait_queue_entry name = { .flags = 0, .private = 0,         \
	                                 .func = autoremove_wake_function, \
	                                 .entry = LIST_HEAD_INIT((name).entry) }
#define DEFINE_WAIT_FUNC(name, fn)                                     \
	struct wait_queue_entry name = { .flags = 0, .private = 0, .func = (fn), \
	                                 .entry = LIST_HEAD_INIT((name).entry) }

/* Add the entry if it is not already queued, then publish on the channel so a
 * wake between here and the caller's park is not lost. */
static inline void prepare_to_wait(struct wait_queue_head *wq,
                                   struct wait_queue_entry *entry, int state)
{
	(void)state;
	if (!entry->entry.next || entry->entry.next == &entry->entry)
		add_wait_queue(wq, entry);
	/*
	 * Armed with a deadline, always.
	 *
	 * Upstream lets a task sit on several queues at once and wake from any of
	 * them; b1nix parks on exactly one channel, so a second prepare_to_wait()
	 * replaces the first — and a queue that hashes to no channel at all, which
	 * is what bit_waitqueue() hands back here, replaces it with nothing. Either
	 * way the wake the caller is waiting for can arrive somewhere this task is
	 * no longer listening. i915 does exactly that while waiting for a commit's
	 * fence: two prepare_to_wait() calls, the second on a bit queue.
	 *
	 * The deadline is what makes that safe rather than fatal. Every caller of
	 * prepare_to_wait() re-tests its condition after schedule() returns — the
	 * pattern permits spurious wakeups — so waking each tick costs a re-test
	 * and turns an unreachable wake into at most 10 ms of latency.
	 */
	lkpi_wait_prepare_timeout(wq, 1);
}

static inline void finish_wait(struct wait_queue_head *wq,
                               struct wait_queue_entry *entry)
{
	lkpi_wait_cancel();
	remove_wait_queue(wq, entry);
}

static inline void init_wait_entry(struct wait_queue_entry *entry, int flags)
{
	entry->flags = (unsigned int)flags;
	entry->private = 0;
	entry->func = autoremove_wake_function;
	INIT_LIST_HEAD(&entry->entry);
}

/* The states a parked task can be in upstream. b1nix kernel threads are never
 * signal targets, so the interruptible ones behave as the uninterruptible one
 * — stated here rather than left to be discovered by a caller expecting
 * -ERESTARTSYS. */
#define TASK_RUNNING           0
#define TASK_INTERRUPTIBLE     1
#define TASK_UNINTERRUPTIBLE   2
#define TASK_KILLABLE          (TASK_UNINTERRUPTIBLE)


/* Bit waits travel with the wait interface upstream, and drivers call
 * wake_up_var()/clear_and_wake_up_bit() without including <linux/wait_bit.h>
 * themselves. Included last: <linux/wait_bit.h> builds on the queues above. */
#include <linux/wait_bit.h>

#endif
