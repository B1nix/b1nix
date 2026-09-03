#ifndef B1NIX_RUNQUEUE_H
#define B1NIX_RUNQUEUE_H

#include <b1nix/types.h>

/* Forward declaration (defined in sched.h) */
struct task;

/* struct runqueue is defined in lapic.h (percpu struct), used here by reference */

/* Enqueue / Dequeue */
void rq_enqueue(struct runqueue *rq, struct task *t);
struct task *rq_dequeue(struct runqueue *rq);
int rq_remove(struct runqueue *rq, struct task *t);

/* Per-CPU enqueue + work stealing */
void sched_rq_enqueue_current(struct task *t);
void sched_rq_remove_task(struct task *t);
/* Unlink from every runqueue (used when a task slot is recycled). */
void sched_rq_remove_task_all(struct task *t);
struct task *sched_steal_task(void);
/* 1 if `t` is present in any runqueue right now. Diagnostic; bounded walk. */
int sched_rq_contains_task(struct task *t);

#endif
