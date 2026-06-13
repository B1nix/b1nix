#ifndef _SCHED_H
#define _SCHED_H

/* Minimal <sched.h> for b1nix userspace. Only what ported software actually
 * uses so far (sched_yield). Grow on demand. */
int sched_yield(void);

#endif /* _SCHED_H */
