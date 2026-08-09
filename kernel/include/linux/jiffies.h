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
#endif
