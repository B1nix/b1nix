/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_KTIME_H
#define LKPI_LINUX_KTIME_H
#include <lkpi/env.h>
#include <b1nix/types.h>
/* Monotonic time in nanoseconds, from the TSC — genuine nanosecond resolution.
 * It was the scheduler tick, 10 ms per step, which is fine for a timestamp and
 * wrong for a timeout: drivers poll hardware with microsecond deadlines on this
 * clock. See lkpi_monotonic_ns() in kernel/lkpi/env.c. */
typedef i64 ktime_t; /* i64, not s64: this header is reached before
                      * <linux/types.h> defines the Linux spellings. */
#define NSEC_PER_USEC 1000LL
#define NSEC_PER_MSEC 1000000LL
#define NSEC_PER_SEC  1000000000LL
#define USEC_PER_SEC  1000000LL
#define MSEC_PER_SEC  1000LL
static inline ktime_t ktime_get(void) { return (ktime_t)lkpi_monotonic_ns(); }
static inline ktime_t ktime_get_raw(void) { return ktime_get(); }
static inline i64 ktime_to_ns(ktime_t k) { return k; }
static inline i64 ktime_to_us(ktime_t k) { return k / NSEC_PER_USEC; }
static inline i64 ktime_to_ms(ktime_t k) { return k / NSEC_PER_MSEC; }
static inline ktime_t ns_to_ktime(i64 ns) { return ns; }
static inline ktime_t ms_to_ktime(i64 ms) { return ms * NSEC_PER_MSEC; }
static inline ktime_t ktime_sub(ktime_t a, ktime_t b) { return a - b; }
static inline ktime_t ktime_sub_ns(ktime_t a, u64 ns) { return a - (i64)ns; }
static inline ktime_t ktime_add_us(ktime_t a, u64 us)
{ return a + (i64)us * NSEC_PER_USEC; }
static inline ktime_t ktime_add(ktime_t a, ktime_t b) { return a + b; }
static inline ktime_t ktime_add_ns(ktime_t a, i64 ns) { return a + ns; }
static inline int ktime_after(ktime_t a, ktime_t b) { return a > b; }
static inline int ktime_before(ktime_t a, ktime_t b) { return a < b; }
static inline i64 ktime_ms_delta(ktime_t later, ktime_t earlier)
{ return (later - earlier) / NSEC_PER_MSEC; }
static inline i64 ktime_us_delta(ktime_t later, ktime_t earlier)
{ return (later - earlier) / NSEC_PER_USEC; }

static inline i64 ktime_compare(ktime_t a, ktime_t b)
{ return a < b ? -1 : (a > b ? 1 : 0); }

/*
 * The NMI-safe reader, defined here rather than by including
 * <linux/timekeeping.h>: that header sits above this one in the include order,
 * and pulling it in from here drags b1nix's own spinlock declarations into
 * translation units that already have the shim's — the collision <lkpi/env.h>
 * warns about, which surfaced as `spinlock_t` being redefined as `volatile int`
 * three files away.
 *
 * Same clock as everything else here. A TSC read plus arithmetic on locals,
 * taking no lock, so the NMI-safety upstream's name promises is real rather
 * than assumed.
 */
u64 lkpi_ticks(void);
static inline u64 ktime_get_raw_fast_ns(void) { return lkpi_monotonic_ns(); }


/* Time since boot, including any time the machine was suspended. b1nix never
 * suspends, so this is the monotonic clock — the same value, not an
 * approximation of a different one. */
static inline ktime_t ktime_get_boottime(void) { return ktime_get(); }
static inline u64 ktime_get_boottime_ns(void) { return (u64)ktime_to_ns(ktime_get()); }


/* The largest representable time. Used as "never" for a deadline. */
#define KTIME_MAX ((s64)~((u64)1 << 63))
#define KTIME_SEC_MAX (KTIME_MAX / 1000000000L)


/* Clock ids. Only the monotonic one is meaningful here: b1nix's kernel clock
 * does not step, so realtime and monotonic are the same series. */
#define CLOCK_MONOTONIC 1
#define CLOCK_REALTIME  0


/* Build a ktime from seconds and nanoseconds. */
static inline ktime_t ktime_set(long long secs, unsigned long nsecs)
{ return (ktime_t)(secs * 1000000000LL + (long long)nsecs); }


static inline ktime_t ktime_add_ms(ktime_t kt, u64 msec)
{ return kt + (ktime_t)(msec * 1000000ull); }
/* ktime_add_us already exists above. */
static inline ktime_t ktime_sub_ms(ktime_t kt, u64 msec)
{ return kt - (ktime_t)(msec * 1000000ull); }


/* The same tick-derived clock as ktime_get_raw(), in nanoseconds. Declared here
 * as well as in <linux/timekeeping.h> because callers reach one or the other. */
static inline u64 ktime_get_raw_ns(void) { return lkpi_ticks() * 10000000ull; }


#ifndef USEC_PER_MSEC
#define USEC_PER_MSEC 1000LL
#define NSEC_PER_MSEC 1000000LL
#endif

#endif
