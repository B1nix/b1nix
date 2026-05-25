#include <b1nix/lapic.h>
#include <b1nix/runqueue.h>
#include <b1nix/sched.h>

void rq_enqueue(struct runqueue *rq, struct task *t) {
    t->next_run = NULL;
    if (rq->tail) { rq->tail->next_run = t; rq->tail = t; }
    else { rq->head = t; rq->tail = t; }
}

struct task *rq_dequeue(struct runqueue *rq) {
    struct task *t = rq->head;
    if (t) { rq->head = t->next_run; t->next_run = NULL; if (!rq->head) rq->tail = NULL; }
    return t;
}

void sched_rq_enqueue_current(struct task *t) {
    struct percpu *pcpu = get_percpu();
    if (pcpu) rq_enqueue(&pcpu->runqueue, t);
}

struct task *sched_steal_task(void) { (void)get_percpu(); return NULL; }
