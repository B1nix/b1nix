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

#endif
