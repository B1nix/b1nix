#ifndef _SCHED_H
#define _SCHED_H

/* Minimal <sched.h> for b1nix userspace. Only what ported software actually
 * uses so far (sched_yield). Grow on demand. */
int sched_yield(void);
int sched_getcpu(void);


/* Scheduling parameters + policies. b1nix has a single scheduling class, so the
 * priority range is degenerate (min == max == 0) and policies are accepted but
 * not differentiated — enough for gthr-posix / libstdc++ threading. */
#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2
struct sched_param {
  int sched_priority;
};
int sched_get_priority_max(int policy);
int sched_get_priority_min(int policy);

#endif /* _SCHED_H */
