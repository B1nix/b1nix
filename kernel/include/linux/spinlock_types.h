/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SPINLOCK_TYPES_H
#define LKPI_LINUX_SPINLOCK_TYPES_H
/* Upstream splits the types out so a header can embed a lock without pulling in
 * the operations. b1nix declares both together, so this is the split-out name. */
#include <linux/spinlock.h>
#endif
