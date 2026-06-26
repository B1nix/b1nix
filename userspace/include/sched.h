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
/* Chromium port: b1nix scheduling is nice-based only (no real policies).
 * getscheduler always reports SCHED_OTHER; setscheduler accepts SCHED_OTHER and
 * rejects others (EINVAL); getparam reports priority 0. Honest, not faked. */
int sched_setscheduler(int pid, int policy, const struct sched_param *param);
int sched_getscheduler(int pid);
int sched_getparam(int pid, struct sched_param *param);
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

/* Dynamically-sized CPU-set API (glibc CPU_*_S + CPU_ALLOC family).
 * Added for the Chromium port (M60-62): base/system reads CPU affinity via the
 * dynamic interface. The size-parameterized macros operate on a flat array of
 * unsigned long words covering `setsize` bytes. */
#ifndef inhibit_libc
#include <stdlib.h>

#define CPU_ALLOC_SIZE(count) \
  ((size_t)((((count) + __NCPUBITS - 1) / __NCPUBITS) * sizeof(unsigned long)))
#define CPU_ALLOC(count)      ((cpu_set_t *)calloc(1, CPU_ALLOC_SIZE(count)))
#define CPU_FREE(set)         free(set)
#endif

#define CPU_ZERO_S(setsize, set) \
  do { \
    unsigned char *__b = (unsigned char *)(set); \
    for (size_t __i = 0; __i < (setsize); __i++) __b[__i] = 0; \
  } while (0)
#define CPU_SET_S(cpu, setsize, set) \
  ((void)(setsize), \
   (void)(((unsigned long *)(set))[(cpu) / __NCPUBITS] |= (1UL << ((cpu) % __NCPUBITS))))
#define CPU_CLR_S(cpu, setsize, set) \
  ((void)(setsize), \
   (void)(((unsigned long *)(set))[(cpu) / __NCPUBITS] &= ~(1UL << ((cpu) % __NCPUBITS))))
#define CPU_ISSET_S(cpu, setsize, set) \
  ((void)(setsize), \
   ((((unsigned long *)(set))[(cpu) / __NCPUBITS] & \
     (1UL << ((cpu) % __NCPUBITS))) != 0))

static inline int CPU_COUNT_S(size_t __setsize, const cpu_set_t *__set) {
  int __n = 0;
  const unsigned long *__w = (const unsigned long *)__set;
  size_t __words = __setsize / sizeof(unsigned long);
  for (size_t __i = 0; __i < __words; __i++)
    for (unsigned __b = 0; __b < __NCPUBITS; __b++)
      if (__w[__i] & (1UL << __b)) __n++;
  return __n;
}

int sched_getaffinity(int pid, size_t cpusetsize, cpu_set_t *mask);

/* CLONE_* flags. glibc exposes these via <sched.h>; b1nix's canonical copies live
 * in <syscall.h>, so guard each so including both headers never redefines. Only
 * CLONE_VFORK was missing from the syscall.h set (needed by the sandbox namespace
 * code, dead under --no-sandbox). Linux-canonical values. */
#ifndef CLONE_VM
#define CLONE_VM        0x00000100
#endif
#ifndef CLONE_FS
#define CLONE_FS        0x00000200
#endif
#ifndef CLONE_FILES
#define CLONE_FILES     0x00000400
#endif
#ifndef CLONE_SIGHAND
#define CLONE_SIGHAND   0x00000800
#endif
#ifndef CLONE_VFORK
#define CLONE_VFORK     0x00004000
#endif
#ifndef CLONE_THREAD
#define CLONE_THREAD    0x00010000
#endif

/* glibc-style clone(): run fn(arg) on a new stack (highest address), returning
 * the child TID to the parent. Variadic tail = pid_t *ptid, void *tls,
 * pid_t *ctid (used per the CLONE_* flags). When fn returns the child exits with
 * fn's return value. b1nix SYS_CLONE has no parent-tid slot, so ptid is emulated
 * (written from the returned tid) when CLONE_PARENT_SETTID is set. */
int clone(int (*fn)(void *), void *stack, int flags, void *arg, ...);

#ifdef __cplusplus
}
#endif

#endif /* _SCHED_H */
