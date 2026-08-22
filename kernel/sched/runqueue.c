#include <b1nix/lapic.h>
#include <b1nix/lockdep.h>
#include <b1nix/runqueue.h>
#include <b1nix/sched.h>
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

void rq_enqueue(struct runqueue *rq, struct task *t) {
    u64 flags;
    spin_lock_irqsave(&rq->lock, &flags);
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
    for (struct task *c = rq->head; c; c = c->next_run) {
        if (c == t) {
            LOCKDEP_RELEASE(LOCKDEP_LVL_RUNQUEUE);
            spin_unlock_irqrestore(&rq->lock, flags);
            return;
        }
    }
    t->next_run = NULL;
    if (rq->tail) { rq->tail->next_run = t; rq->tail = t; }
    else          { rq->head = t; rq->tail = t; }
    LOCKDEP_RELEASE(LOCKDEP_LVL_RUNQUEUE);
    spin_unlock_irqrestore(&rq->lock, flags);
}

struct task *rq_dequeue(struct runqueue *rq) {
    u64 flags;
    spin_lock_irqsave(&rq->lock, &flags);
    LOCKDEP_ACQUIRE(LOCKDEP_LVL_RUNQUEUE);
    struct task *t = rq->head;
    if (t) {
        rq->head = t->next_run;
        t->next_run = NULL;
        if (!rq->head) rq->tail = NULL;
    }
    LOCKDEP_RELEASE(LOCKDEP_LVL_RUNQUEUE);
    spin_unlock_irqrestore(&rq->lock, flags);
    return t;
}

int rq_remove(struct runqueue *rq, struct task *t) {
    int removed = 0;
    u64 flags;
    spin_lock_irqsave(&rq->lock, &flags);
    LOCKDEP_ACQUIRE(LOCKDEP_LVL_RUNQUEUE);
    struct task *prev = 0;
    struct task *cur = rq->head;
    while (cur) {
        struct task *next = cur->next_run;
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
