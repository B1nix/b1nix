/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_KTIME_H
#define LKPI_LINUX_KTIME_H
#include <lkpi/env.h>
#include <b1nix/types.h>
/* Monotonic time in nanoseconds, derived from the scheduler's tick. The
 * resolution is therefore 10 ms, not a nanosecond — imported code that
 * timestamps a vblank gets tick granularity and the header says so rather than
 * letting the units imply a precision that is not there. */
typedef i64 ktime_t; /* i64, not s64: this header is reached before
                      * <linux/types.h> defines the Linux spellings. */
#define NSEC_PER_USEC 1000LL
#define NSEC_PER_MSEC 1000000LL
#define NSEC_PER_SEC  1000000000LL
#define USEC_PER_SEC  1000000LL
#define MSEC_PER_SEC  1000LL
static inline ktime_t ktime_get(void)
{ return (ktime_t)lkpi_ticks() * 10 * NSEC_PER_MSEC; }
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
#endif
