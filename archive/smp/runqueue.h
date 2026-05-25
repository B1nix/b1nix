#ifndef B1NIX_RUNQUEUE_H
#define B1NIX_RUNQUEUE_H

#include <b1nix/types.h>

/* Forward declaration (defined in sched.h) */
struct task;

/* Per-CPU runqueue — linked list of READY tasks. */
struct runqueue {
    struct task *head;
    struct task *tail;
};

/* Enqueue / Dequeue */
void rq_enqueue(struct runqueue *rq, struct task *t);
struct task *rq_dequeue(struct runqueue *rq);

/* Per-CPU enqueue + work stealing (implemented in runqueue.c) */
void sched_rq_enqueue_current(struct task *t);
struct task *sched_steal_task(void);

#endif
