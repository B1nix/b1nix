/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_IOPOLL_H
#define LKPI_LINUX_IOPOLL_H

#include <linux/delay.h>
#include <linux/ktime.h>
/*
 * Poll a register until a condition holds or a timeout expires.
 *
 * Upstream has a sleeping and a busy-waiting flavour; b1nix's udelay is a busy
 * wait either way (there is no sub-tick sleep), so both spell the same loop.
 * The timeout is honoured exactly: it is counted in the delay steps actually
 * taken, not read off a clock that only advances every 10 ms.
 */
#define read_poll_timeout(op, val, cond, sleep_us, timeout_us, sleep_before, ...) \
({                                                                          \
	u64 __left = (timeout_us);                                              \
	u64 __step = (sleep_us) ? (sleep_us) : 1;                               \
	int __err = 0;                                                          \
	if (sleep_before)                                                       \
		udelay(__step);                                                     \
	for (;;) {                                                              \
		(val) = op(__VA_ARGS__);                                            \
		if (cond)                                                           \
			break;                                                          \
		if ((timeout_us) && __left <= __step) { __err = -ETIMEDOUT;          \
			(val) = op(__VA_ARGS__); if (cond) __err = 0; break; }           \
		__left -= __step;                                                   \
		udelay(__step);                                                     \
	}                                                                       \
	__err;                                                                  \
})
#define read_poll_timeout_atomic(op, val, cond, delay_us, timeout_us, delay_before, ...) \
	read_poll_timeout(op, val, cond, delay_us, timeout_us, delay_before, __VA_ARGS__)
#define readx_poll_timeout(op, addr, val, cond, sleep_us, timeout_us) \
	read_poll_timeout(op, val, cond, sleep_us, timeout_us, false, addr)

/*
 * Perform `op` until `cond` holds or `timeout_us` passes (6.17). Timed against
 * the monotonic clock, as upstream's is: `op` may itself take time, so counting
 * sleep steps would stretch the timeout. 0 on success, -ETIMEDOUT otherwise;
 * `op` and `cond` are evaluated once more after the deadline.
 */
#define poll_timeout_us(op, cond, sleep_us, timeout_us, sleep_before_op)     \
({                                                                           \
	u64 __timeout_us = (timeout_us);                                     \
	unsigned long __sleep_us = (sleep_us);                               \
	ktime_t __timeout = ktime_add_us(ktime_get(), __timeout_us);         \
	int ___ret;                                                          \
	if ((sleep_before_op) && __sleep_us)                                 \
		usleep_range((__sleep_us >> 2) + 1, __sleep_us);             \
	for (;;) {                                                           \
		bool __expired = __timeout_us &&                             \
			ktime_compare(ktime_get(), __timeout) > 0;           \
		op;                                                          \
		if (cond) {                                                  \
			___ret = 0;                                          \
			break;                                               \
		}                                                            \
		if (__expired) {                                             \
			___ret = -ETIMEDOUT;                                 \
			break;                                               \
		}                                                            \
		if (__sleep_us)                                              \
			usleep_range((__sleep_us >> 2) + 1, __sleep_us);     \
		cpu_relax();                                                 \
	}                                                                    \
	___ret;                                                              \
})

/* The busy-waiting form, safe with interrupts off. */
#define poll_timeout_us_atomic(op, cond, delay_us, timeout_us, delay_before_op) \
({                                                                           \
	u64 __timeout_us = (timeout_us);                                     \
	s64 __left_ns = __timeout_us * 1000;                                 \
	unsigned long __delay_us = (delay_us);                               \
	u64 __delay_ns = __delay_us * 1000;                                  \
	int ___ret;                                                          \
	if ((delay_before_op) && __delay_us) {                               \
		udelay(__delay_us);                                          \
		__left_ns -= __delay_ns;                                     \
	}                                                                    \
	for (;;) {                                                           \
		bool __expired = __timeout_us && __left_ns < 0;              \
		op;                                                          \
		if (cond) {                                                  \
			___ret = 0;                                          \
			break;                                               \
		}                                                            \
		if (__expired) {                                             \
			___ret = -ETIMEDOUT;                                 \
			break;                                               \
		}                                                            \
		if (__delay_us) {                                            \
			udelay(__delay_us);                                  \
			__left_ns -= __delay_ns;                             \
		}                                                            \
		cpu_relax();                                                 \
		if (__timeout_us)                                            \
			__left_ns--;                                         \
	}                                                                    \
	___ret;                                                              \
})

#endif
