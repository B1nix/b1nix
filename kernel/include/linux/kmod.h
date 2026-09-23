/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_KMOD_H
#define LKPI_LINUX_KMOD_H

/*
 * On-demand module loading. request_module() itself lives in <linux/module.h>
 * here; the quota code asks for this header by name, and every format it could
 * ask to load is built in and registers itself at init, so the request fails
 * and the caller reports an unknown format rather than hanging.
 */
#include <linux/module.h>

/* Evaluate `x`; if it is not there, ask for the module and evaluate it again.
 * Every NLS table and filesystem here is built in, so the second look finds
 * what the first did. */
#define try_then_request_module(x, mod...) \
	((x) ?: (request_module(mod), (x)))

#endif /* LKPI_LINUX_KMOD_H */
