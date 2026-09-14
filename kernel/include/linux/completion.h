/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_COMPLETION_H
#define LKPI_LINUX_COMPLETION_H
#include <lkpi/completion.h>
#include <linux/types.h>
/* Onto lkpi's completions (M99): counting, so a complete() before anyone waits
 * is consumed by the next wait rather than lost. */
#define DECLARE_COMPLETION_ONSTACK(name) struct completion name = { 0, 0 }
#define wait_for_completion_interruptible(c) (wait_for_completion(c), 0)
#define wait_for_completion_interruptible_timeout(c, t) \
	wait_for_completion_timeout(c, t)
/* Wait for the completion, marked as an I/O wait. Same wait; the distinction is
 * accounting b1nix does not keep. */
#define wait_for_completion_io(x) wait_for_completion(x)
#define wait_for_completion_io_timeout(x, t) wait_for_completion_timeout(x, t)

#endif
