#ifndef _SCHED_H
#define _SCHED_H

#ifdef __cplusplus
extern "C" {
#endif

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

/* CPU affinity. b1nix does not pin userspace tasks, so the affinity set is just
 * the set of online CPUs; sched_getaffinity reports it (real syscall). */
#include <stddef.h>

#define CPU_SETSIZE 1024
#define __NCPUBITS (8 * sizeof(unsigned long))
typedef struct {
  unsigned long __bits[CPU_SETSIZE / __NCPUBITS];
} cpu_set_t;

#define CPU_ZERO(set) \
  do { \
    unsigned long *__b = (set)->__bits; \
    for (unsigned __i = 0; __i < sizeof((set)->__bits) / sizeof(unsigned long); __i++) \
      __b[__i] = 0; \
  } while (0)
#define CPU_SET(cpu, set)   ((set)->__bits[(cpu) / __NCPUBITS] |= (1UL << ((cpu) % __NCPUBITS)))
#define CPU_CLR(cpu, set)   ((set)->__bits[(cpu) / __NCPUBITS] &= ~(1UL << ((cpu) % __NCPUBITS)))
#define CPU_ISSET(cpu, set) (((set)->__bits[(cpu) / __NCPUBITS] & (1UL << ((cpu) % __NCPUBITS))) != 0)

static inline int CPU_COUNT(const cpu_set_t *set) {
  int __n = 0;
  for (unsigned __i = 0; __i < sizeof(set->__bits) / sizeof(unsigned long); __i++)
    for (unsigned __b = 0; __b < __NCPUBITS; __b++)
      if (set->__bits[__i] & (1UL << __b)) __n++;
  return __n;
}

int sched_getaffinity(int pid, size_t cpusetsize, cpu_set_t *mask);

#ifdef __cplusplus
}
#endif

#endif /* _SCHED_H */
