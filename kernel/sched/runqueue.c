#include <b1nix/lapic.h>
#include <b1nix/lockdep.h>
#include <b1nix/runqueue.h>
#include <b1nix/sched.h>
#include <b1nix/console.h>
#include <b1nix/klog.h>
#include <b1nix/spinlock.h>

/* ── Locked enqueue / dequeue ──
 *
 * MUST use spin_lock_irqsave: the runqueue lock is taken from BOTH thread
 * context (scheduler_yield, fork, waitpid's sched_rq_remove_task, ...) AND
 * interrupt context (the LAPIC timer tick runs scheduler_on_timer_tick ->
 * scheduler_yield -> wake_sleepers/pick_next_task -> rq_dequeue). A thread-side
 * acquirer that runs with interrupts enabled — e.g. scheduler_waitpid_fast_return
 * — would otherwise be interrupted mid-critical-section by the timer, whose ISR
 * re-enters rq_dequeue on the SAME CPU and spins on the lock its own interrupted
 * frame still holds: an unrecoverable self-deadlock (caught as the residual
 * -smp 4 silent hang). irqsave/irqrestore makes every acquirer interrupt-safe
 * regardless of its caller's IRQ state. NOTE: this is NOT the same as a plain
 * irqsave "re-enabling IRQs mid-section" — save/restore captures and restores
 * the caller's flags, so an already-IRQs-off scheduler critical section stays
 * off across the whole pick/switch; only a careless IRQs-on caller is corrected.
 */

/*
 * Who holds a runqueue lock.
 *
 * A spinlock here is a bare int with nowhere to record an owner, so a lockup
 * report could name the lock and the CPUs waiting on it but never the CPU that
 * was holding it - which is the only thing that makes a deadlock actionable.
 * This side table carries that: every acquirer stamps its CPU and its own
 * caller, and the lockup path prints them.
 *
 * A missed clear costs a stale line in a panic report and nothing else, so
 * the table is deliberately lock-free.
 */
#define RQ_OWNER_SLOTS 16
static struct {
    const void *lock;
    u32 cpu;
    u64 caller;
} g_rq_owner[RQ_OWNER_SLOTS];

static void rq_owner_set(struct runqueue *rq, u64 caller) {
    struct percpu *pcpu = get_percpu();

    for (u32 i = 0; i < RQ_OWNER_SLOTS; i++) {
        const void *slot = g_rq_owner[i].lock;

        if (slot && slot != (const void *)&rq->lock)
            continue;
        g_rq_owner[i].lock = (const void *)&rq->lock;
        g_rq_owner[i].cpu = pcpu ? pcpu->cpu_id : 0xffffffffu;
        g_rq_owner[i].caller = caller;
        return;
    }
}

static void rq_owner_clear(struct runqueue *rq) {
    for (u32 i = 0; i < RQ_OWNER_SLOTS; i++) {
        if (g_rq_owner[i].lock == (const void *)&rq->lock) {
            g_rq_owner[i].lock = 0;
            return;
        }
    }
}

/* Called from the spinlock lockup path, which knows only the address. */
void rq_describe_lock(const void *lock) {
    extern void ksym_print(u64 addr);

    for (u32 i = 0; i < RQ_OWNER_SLOTS; i++) {
        if (g_rq_owner[i].lock != lock)
            continue;
        console_write("\n  runqueue lock last taken by cpu ");
        console_write_dec(g_rq_owner[i].cpu);
        console_write(" from 0x");
        console_write_hex64(g_rq_owner[i].caller);
        ksym_print(g_rq_owner[i].caller);
        return;
    }
}

/*
 * A runqueue list must be finite.
 *
 * Every walk in this file follows next_run to NULL, so a chain that loops back
 * on itself is not a corrupted list that recovers - it is a walk that never
 * ends, inside the lock, which is what wedged a Raspberry Pi: three CPUs
 * reported a lockup on the global runqueue's lock while the fourth was still
 * in this loop. Bound the walk by the size of the task table (a list can hold
 * at most every task once), and report what is in the ring instead of spinning
 * in it. MAX_TASKS is 4096; a real runqueue never comes close.
 */
#define RQ_WALK_LIMIT 4096

static void rq_report_bad(struct runqueue *rq, const char *what,
                          const struct task *bad) {
    console_write("\nRUNQUEUE CORRUPT: ");
    console_write(what);
    console_write(" ptr=0x");
    console_write_hex64((u64)(usize)bad);
    console_write(" lock=0x");
    console_write_hex64((u64)(usize)&rq->lock);
    console_write("\n");
}

static void rq_report_cycle(struct runqueue *rq) {
    struct task *c = rq->head;

    console_write("\nRUNQUEUE CYCLE: lock=0x");
    console_write_hex64((u64)(usize)&rq->lock);
    console_write(" head=0x");
    console_write_hex64((u64)(usize)rq->head);
    console_write(" tail=0x");
    console_write_hex64((u64)(usize)rq->tail);
    console_write("\n  chain:");
    for (u32 i = 0; i < 24 && c; i++, c = c->next_run) {
        console_write("\n    [");
        console_write_dec(i);
        console_write("] task=0x");
        console_write_hex64((u64)(usize)c);
        console_write(" id=");
        console_write_dec(c->id);
        console_write(" state=");
        console_write_dec((u64)c->state);
        console_write(" stealable=");
        console_write_dec((u64)c->stealable);
        console_write(" name=");
        console_write(c->name ? c->name : "?");
    }
    console_write("\n");
}

void rq_enqueue(struct runqueue *rq, struct task *t) {
    u64 flags;
    spin_lock_irqsave(&rq->lock, &flags);
    rq_owner_set(rq, (u64)(usize)__builtin_return_address(0));
    LOCKDEP_ACQUIRE(LOCKDEP_LVL_RUNQUEUE);
    /* Idempotent enqueue: a task must appear on a runqueue at most once.
     * Linking a task that is already queued corrupts the singly-linked
     * next_run chain — in particular enqueuing the current tail does
     * rq->tail->next_run = t with tail == t, i.e. t->next_run = t, a self
     * cycle that makes rq_remove / rq_dequeue traversal loop forever (a silent
     * -smp hang: caught with CPU0 spinning in rq_remove's list walk). This
     * happens when a task races two make-runnable events — e.g. a waitpid that
     * re-publishes BLOCKED and is woken again before its previous stale runqueue
     * entry was scrubbed. Skip the re-link; the task is already runnable. */
    u32 walked = 0;
    for (struct task *c = rq->head; c; c = c->next_run) {
        if (c == t) {
            LOCKDEP_RELEASE(LOCKDEP_LVL_RUNQUEUE);
            rq_owner_clear(rq);
            spin_unlock_irqrestore(&rq->lock, flags);
            return;
        }
        if (++walked > RQ_WALK_LIMIT) {
            rq_report_cycle(rq);
            panic("runqueue list is cyclic");
        }
        if (c->next_run && !sched_task_ptr_valid(c->next_run)) {
            rq_report_bad(rq, "next_run is not a task slot", c->next_run);
            rq_report_cycle(rq);
            panic("runqueue list is corrupt");
        }
    }
    t->next_run = NULL;
    if (rq->tail) { rq->tail->next_run = t; rq->tail = t; }
    else          { rq->head = t; rq->tail = t; }
    LOCKDEP_RELEASE(LOCKDEP_LVL_RUNQUEUE);
    rq_owner_clear(rq);
    spin_unlock_irqrestore(&rq->lock, flags);
}

struct task *rq_dequeue(struct runqueue *rq) {
    u64 flags;
    spin_lock_irqsave(&rq->lock, &flags);
    rq_owner_set(rq, (u64)(usize)__builtin_return_address(0));
    LOCKDEP_ACQUIRE(LOCKDEP_LVL_RUNQUEUE);
    struct task *t = rq->head;
    if (t && !sched_task_ptr_valid(t)) {
        rq_report_bad(rq, "head is not a task slot", t);
        panic("runqueue list is corrupt");
    }
    if (t) {
        if (t->next_run && !sched_task_ptr_valid(t->next_run)) {
            rq_report_bad(rq, "next_run is not a task slot", t->next_run);
            panic("runqueue list is corrupt");
        }
        rq->head = t->next_run;
        t->next_run = NULL;
        if (!rq->head) rq->tail = NULL;
    }
    LOCKDEP_RELEASE(LOCKDEP_LVL_RUNQUEUE);
    rq_owner_clear(rq);
    spin_unlock_irqrestore(&rq->lock, flags);
    return t;
}

int rq_remove(struct runqueue *rq, struct task *t) {
    int removed = 0;
    u64 flags;
    spin_lock_irqsave(&rq->lock, &flags);
    rq_owner_set(rq, (u64)(usize)__builtin_return_address(0));
    LOCKDEP_ACQUIRE(LOCKDEP_LVL_RUNQUEUE);
    struct task *prev = 0;
    struct task *cur = rq->head;
    u32 rwalked = 0;
    /* Bounded and validated, exactly as rq_enqueue and rq_dequeue already are.
     *
     * This walk had neither, and it is the one that runs with the lock HELD and
     * interrupts DISABLED for its whole length. A cyclic next_run chain -- a
     * real, observed condition on this arch, which is why the other two walks
     * check for it and why sched_rq_remove_task_all exists to prevent it -- made
     * this loop spin for ever holding the runqueue lock. Every other CPU then
     * piles up behind it, and what the machine prints is
     *
     *   SPINLOCK LOCKUP on cpu 1: lock=<g_global_rq> value=1
     *   holder: not recorded
     *
     * with no clue that a corrupt list is behind it: the holder cannot report
     * because it never leaves the loop, and the spinners have nothing to name.
     * The two sibling walks turn the same corruption into an immediate, named
     * panic. This one now does too. */
    while (cur) {
        struct task *next = cur->next_run;

        if (++rwalked > RQ_WALK_LIMIT) {
            rq_report_cycle(rq);
            panic("runqueue list is cyclic");
        }
        if (next && !sched_task_ptr_valid(next)) {
            rq_report_bad(rq, "next_run is not a task slot", next);
            rq_report_cycle(rq);
            panic("runqueue list is corrupt");
        }
        if (cur == t) {
            if (prev) prev->next_run = next;
            else rq->head = next;
            if (rq->tail == cur) rq->tail = prev;
            cur->next_run = 0;
            removed = 1;
            cur = next;
            continue;
        }
        prev = cur;
        cur = next;
    }
    LOCKDEP_RELEASE(LOCKDEP_LVL_RUNQUEUE);
    rq_owner_clear(rq);
    spin_unlock_irqrestore(&rq->lock, flags);
    return removed;
}

/* Make a READY task runnable.
 *
 * Stealable CPU-bound workers go on THIS CPU's per-CPU runqueue so an idle AP
 * can steal them via sched_steal_task (the M24b work-stealing path). Ordinary
 * tasks (userspace processes, kernel threads) go on the shared global runqueue
 * so any CPU can schedule them under the Big Kernel Lock — this is what lets
 * userspace run on Application Processors. */
extern struct runqueue *sched_global_rq(void);

void sched_rq_enqueue_current(struct task *t) {
    if (t->stealable) {
        struct percpu *pcpu = get_percpu();
        if (pcpu) rq_enqueue(&pcpu->runqueue, t);
    } else {
        rq_enqueue(sched_global_rq(), t);
    }
}

/* Scrub a task slot out of EVERY runqueue, whatever its stealable flag says.
 *
 * Called when a slot is released back to the task table. A slot that is still
 * linked in a runqueue when it is recycled is not a stale entry that gets
 * dropped later — the next occupant WRITES THROUGH the link: find_unused_task
 * memsets the slot (truncating the chain) and fork's
 * `memcpy(child, parent, sizeof *child)` then stores the *parent's* next_run
 * into a node that is still in the chain. With the parent one hop ahead of the
 * recycled slot that stores the slot's own address into its own next_run — a
 * self cycle, reached from the head, which is exactly the "runqueue list is
 * cyclic" panic seen on aarch64: a chain of identical /bin/sh entries, hit by
 * the very fork whose memcpy had just built it.
 *
 * sched_rq_remove_task cannot be used here: it picks the queue from
 * t->stealable, and a slot being torn down is precisely where that field is no
 * longer trustworthy. Sweep them all. */
/* Is this task queued anywhere right now?
 *
 * Read this with care, because the obvious reading is wrong: `queued=NO` is
 * the NORMAL state for a task that yielded. scheduler_yield publishes
 * state=READY without enqueueing, and pick_next_task scans the task table
 * rather than relying on the queues -- so a task no queue holds is still
 * perfectly findable. Measured: on a healthy boot, `boot`, `net_task` and both
 * lkpi workers all report queued=NO while running normally.
 *
 * What it is good for is the opposite direction. It was added to test the
 * theory that a task stuck READY for a minute had been lost by the runqueue,
 * and it disproved that theory in one run. Walks the global queue and every
 * per-CPU one, bounded exactly as the other walks in this file are. */
int sched_rq_contains_task(struct task *t)
{
	if (!t)
		return 0;
	for (int cpu = -1; cpu < (int)MAX_CPUS; cpu++) {
		struct runqueue *rq = 0;

		if (cpu < 0) {
			rq = sched_global_rq();
		} else {
			struct percpu *p = get_percpu_n((u32)cpu);

			rq = p ? &p->runqueue : 0;
		}
		if (!rq)
			continue;

		u32 walked = 0;

		for (struct task *c = rq->head; c; c = c->next_run) {
			if (c == t)
				return 1;
			if (++walked > RQ_WALK_LIMIT)
				break;
			if (c->next_run && !sched_task_ptr_valid(c->next_run))
				break;
		}
	}
	return 0;
}

void sched_rq_remove_task_all(struct task *t) {
    rq_remove(sched_global_rq(), t);
    for (int i = 0; i < g_max_cpus; i++) {
        struct percpu *pcpu = get_percpu_n(i);
        if (pcpu) rq_remove(&pcpu->runqueue, t);
    }
}

void sched_rq_remove_task(struct task *t) {
    if (t->stealable) {
        for (int i = 0; i < g_max_cpus; i++) {
            struct percpu *pcpu = get_percpu_n(i);
            if (pcpu) rq_remove(&pcpu->runqueue, t);
        }
    } else {
        rq_remove(sched_global_rq(), t);
    }
}

/* ── Work stealing ──
 *
 * Called by an idle CPU to migrate a READY, *stealable* task from a busier
 * CPU.  Algorithm: round-robin scan of all online CPUs, skip self, try to
 * dequeue the head task.  Only tasks explicitly marked stealable (self
 * contained CPU-bound kernel workers) are taken — ordinary userspace tasks
 * are put back untouched, because the kernel's syscall/VFS paths are not yet
 * SMP-safe for parallel kernel-mode execution.  A non-stealable or non-ready
 * head task is re-enqueued (to its tail) and we move on.
 *
 * The per-runqueue spinlock ensures we never corrupt another CPU's queue.
 */
struct task *sched_steal_task(void) {
    struct percpu *self = get_percpu();

    for (int i = 0; i < g_max_cpus; i++) {
        struct percpu *victim = get_percpu_n(i);

        /* Skip: CPU not online, is ourselves, or has empty queue */
        if (!victim)                    continue;
        if (victim == self)             continue;
        if (!victim->runqueue.head)     continue;  /* quick non-locked peek */

        struct task *t = rq_dequeue(&victim->runqueue);
        if (!t)
            continue;

        struct percpu *self_pcpu = get_percpu();
        if (self_pcpu && !sched_task_allowed_on_cpu(t, self_pcpu->cpu_id)) {
          /* Pinned away from this CPU by sched_setaffinity — put it back. */
          rq_enqueue(&victim->runqueue, t);
          continue;
        }
        /* Claim it the way every other picker does.
         *
         * This path used to take a task on a plain `state == READY` read, with
         * none of the protections the runqueue and scan paths grew: no wait
         * for the outgoing CPU to publish its stack hand-off, no check that
         * the task is not some CPU's current task, and no atomic claim. A task
         * that a waker had marked READY while it went on executing was
         * therefore stolen out from under the CPU running it, and two CPUs
         * ended up returning through one kernel stack — which surfaces as a
         * jump to a small integer with no backtrace.
         */
        if (t->state == TASK_READY && t->stealable &&
            __atomic_load_n(&t->stack_released, __ATOMIC_ACQUIRE) &&
            !task_running_somewhere(t)) {
            enum task_state expected = TASK_READY;

            if (__atomic_compare_exchange_n(&t->state, &expected, TASK_RUNNING,
                                            0, __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED)) {
                /* The stack belongs to this CPU from here on. */
                __atomic_store_n(&t->stack_released, 0, __ATOMIC_RELEASE);
                return t;
            }
        }

        /* Not stealable / not runnable — put back */
        rq_enqueue(&victim->runqueue, t);
    }

    return NULL;
}

/* Is there anything besides the caller waiting for this CPU?
 *
 * Asked by a task that is about to wait out a sub-tick sleep on the clock: if
 * nothing else wants the CPU, waiting precisely costs nothing, because the
 * alternative is an idle loop. If something does, precision is not worth
 * taking its turn away — a task spinning out a sleep is indistinguishable from
 * a busy one to the scheduler, and a cooperative-bias test watched its
 * favoured workers get four turns against seventy thousand while sleepers
 * yielded in front of them.
 *
 * A peek, not a count: the queues are read without their locks because the
 * answer is a hint and a wrong one costs only precision. */
int sched_other_work_pending(void) {
    struct runqueue *g = sched_global_rq();

    if (g && g->head)
        return 1;
    for (int i = 0; i < g_max_cpus; i++) {
        struct percpu *pc = get_percpu_n(i);

        if (pc && pc->runqueue.head)
            return 1;
    }
    return 0;
}
