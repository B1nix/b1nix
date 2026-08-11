/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_OOM_H
#define LKPI_LINUX_OOM_H
#include <linux/notifier.h>
/* The out-of-memory notifier chain. b1nix's allocator panics rather than
 * reclaiming (see kmalloc's contract), so no chain is ever walked and a
 * registration is recorded and never called. */
static inline int register_oom_notifier(struct notifier_block *nb) { (void)nb; return 0; }
static inline int unregister_oom_notifier(struct notifier_block *nb) { (void)nb; return 0; }
#endif
