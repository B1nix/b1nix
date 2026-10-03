/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SIGNAL_H
#define LKPI_LINUX_SIGNAL_H
#include <linux/types.h>
#include <linux/sched/signal.h>

/* The kernel's signal set: 64 signals, one word, the x86_64 layout. */
#ifndef _NSIG
#define _NSIG       64
#define _NSIG_BPW   64
#define _NSIG_WORDS 1
#endif
typedef lkpi_sigset_t sigset_t;

#define SIGHUP   1
#define SIGINT   2
#define SIGKILL  9
#define SIGSEGV  11
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define sigmask(sig) (1UL << ((sig) - 1))

static inline void sigemptyset(sigset_t *set) { set->sig[0] = 0; }
static inline void sigfillset(sigset_t *set) { set->sig[0] = ~0UL; }
static inline void sigaddsetmask(sigset_t *set, unsigned long mask) { set->sig[0] |= mask; }
static inline void sigdelsetmask(sigset_t *set, unsigned long mask) { set->sig[0] &= ~mask; }
static inline int sigismember(sigset_t *set, int sig) { return (set->sig[0] >> (sig - 1)) & 1; }

/* The calling task's blocked mask, as sigprocmask(2) changes it: lkpi hands
 * it to the scheduler, which owns the real mask. */
int sigprocmask(int how, sigset_t *set, sigset_t *oldset);
#endif
