#ifndef B1NIX_RSEQ_H
#define B1NIX_RSEQ_H

#include <b1nix/types.h>

struct task;
struct interrupt_frame;

/* rseq(2). `unregister` selects RSEQ_FLAG_UNREGISTER. Returns 0 or -errno. */
int rseq_register(struct task *t, u64 uptr, u32 len, u32 sig, int unregister);

/* Refresh the registered area's cpu ids and, when `frame` describes where
 * userspace is about to resume, restart an interrupted critical section by
 * redirecting to its abort handler. Called on every return to ring 3 that can
 * follow a preemption. Passing a NULL frame updates the cpu ids only. */
void rseq_on_return_to_user(struct interrupt_frame *frame);

/* Drop a task's registration (exit) / a fresh child's inherited one (fork). */
void rseq_task_cleanup(struct task *t);
void rseq_fork_clear(struct task *child);

int rseq_is_registered(struct task *t);

#endif
