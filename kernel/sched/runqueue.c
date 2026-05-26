#include <b1nix/lapic.h>
#include <b1nix/runqueue.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>

/* ── Locked enqueue / dequeue ──
 *
 * Use plain spin_lock (not irqsave) here: all callers of rq_enqueue /
 * rq_dequeue already hold interrupts disabled via scheduler_yield /
 * interrupts_disable().  Using irqsave would restore RFLAGS (enabling IRQs)
 * in the middle of the scheduler's critical section, causing re-entrancy.
 */

void rq_enqueue(struct runqueue *rq, struct task *t) {
    spin_lock(&rq->lock);
    t->next_run = NULL;
    if (rq->tail) { rq->tail->next_run = t; rq->tail = t; }
    else          { rq->head = t; rq->tail = t; }
    spin_unlock(&rq->lock);
}

struct task *rq_dequeue(struct runqueue *rq) {
    spin_lock(&rq->lock);
    struct task *t = rq->head;
    if (t) {
        rq->head = t->next_run;
        t->next_run = NULL;
        if (!rq->head) rq->tail = NULL;
    }
    spin_unlock(&rq->lock);
    return t;
}

/* Enqueue onto current CPU's runqueue */
void sched_rq_enqueue_current(struct task *t) {
    struct percpu *pcpu = get_percpu();
    if (pcpu) rq_enqueue(&pcpu->runqueue, t);
}

/* ── Work stealing ──
 *
 * Called by an idle CPU to migrate a READY task from a busier CPU.
 * Algorithm: round-robin scan of all online CPUs, skip self,
 * try to dequeue the head task.  If it is READY, keep it (stolen).
 * Otherwise push it back and move on.
 *
 * The per-runqueue spinlock ensures we never corrupt another CPU's queue.
 */
struct task *sched_steal_task(void) {
    struct percpu *self = get_percpu();

    for (int i = 0; i < MAX_CPUS; i++) {
        struct percpu *victim = get_percpu_n(i);

        /* Skip: CPU not online, is ourselves, or has empty queue */
        if (!victim)                    continue;
        if (victim == self)             continue;
        if (!victim->runqueue.head)     continue;  /* quick non-locked peek */

        struct task *t = rq_dequeue(&victim->runqueue);
        if (!t)
            continue;

        if (t->state == TASK_READY) {
            /* Successfully stolen — caller will run it on this CPU */
            return t;
        }

        /* Not runnable — put back */
        rq_enqueue(&victim->runqueue, t);
    }

    return NULL;
}
