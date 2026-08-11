/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_JIFFIES_H
#define LKPI_LINUX_JIFFIES_H
#include <lkpi/env.h>
#include <b1nix/types.h>
/* A jiffy here is a b1nix scheduler tick: 10 ms, so HZ is 100. Imported code
 * that converts with msecs_to_jiffies gets the right answer; code that assumes
 * a particular HZ would be wrong on Linux too. */
#define HZ 100
#define jiffies (lkpi_ticks())
static inline unsigned long msecs_to_jiffies(unsigned int m)
{ return (m + 9) / 10; }
static inline unsigned long usecs_to_jiffies(unsigned int u)
{ return (u + 9999) / 10000; }
static inline unsigned int jiffies_to_msecs(unsigned long j)
{ return (unsigned int)(j * 10); }
static inline unsigned int jiffies_to_usecs(unsigned long j)
{ return (unsigned int)(j * 10000); }
static inline u64 nsecs_to_jiffies64(u64 ns)
{ return ns / (10ull * 1000000ull); }
static inline unsigned long nsecs_to_jiffies(u64 ns)
{ return (unsigned long)nsecs_to_jiffies64(ns); }

#define time_after(a, b)  ((long)((b) - (a)) < 0)
#define time_before(a, b) time_after(b, a)
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)

/* The largest delay a timer may be armed for. Half the signed range, the way
 * Linux picks it, so that jiffies + MAX_JIFFY_OFFSET cannot wrap past the
 * comparison macros. */
#define MAX_JIFFY_OFFSET ((long)(~0UL >> 2))


/* Upstream's time comparison macros go through typecheck(), so the header that
 * defines them has to be reachable from here. */
#include <linux/typecheck.h>


/* Round a timeout up to the next second boundary so unrelated timers coalesce
 * and the CPU wakes once instead of twice. b1nix's tick is 10 ms, so the
 * rounding is to the next multiple of 100 ticks. */
static inline unsigned long round_jiffies_up_relative(unsigned long j)
{ return ((j + 99) / 100) * 100; }
static inline unsigned long round_jiffies_up(unsigned long j)
{ return round_jiffies_up_relative(j); }


/* Whether `t` lies within [a, b], on a clock that wraps. The subtraction is
 * what makes it wrap-safe: comparing the raw values would fail exactly once
 * every time the counter rolls over, which is the bug this exists to avoid. */
#define time_in_range(t, a, b) \
	(((long)((t) - (a)) >= 0) && ((long)((b) - (t)) >= 0))
#define time_after(a, b)  ((long)((b) - (a)) < 0)
#define time_before(a, b) time_after(b, a)
#define time_after_eq(a, b)  ((long)((a) - (b)) >= 0)
#define time_before_eq(a, b) time_after_eq(b, a)


/* One jiffy is 10 ms here — see the tick rate in <lkpi/env.h>. */
static inline u64 jiffies_to_nsecs(unsigned long j) { return (u64)j * 10000000ull; }

/* The 32-bit wrap-safe comparison, for counters that are sampled as u32. Same
 * signed-difference test as time_after(), narrowed. */
#define time_after32(a, b)   ((s32)((u32)(b) - (u32)(a)) < 0)
#define time_before32(a, b)  time_after32(b, a)


/* The full-width tick counter. b1nix's is 64-bit already, so this is the same
 * counter jiffies reads — there is no 32-bit wrap to work around. */
static inline u64 get_jiffies_64(void) { return (u64)jiffies; }

#endif
