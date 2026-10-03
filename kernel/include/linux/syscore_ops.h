/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SYSCORE_OPS_H
#define LKPI_LINUX_SYSCORE_OPS_H
#include <linux/list.h>
/* Called on one CPU with interrupts off around a system sleep and at
 * shutdown: KVM switches virtualization off and back on through these.
 * lkpi registers them with the kernel's own sleep path. */
struct syscore_ops {
	struct list_head node;
	int (*suspend)(void);
	void (*resume)(void);
	void (*shutdown)(void);
};
void register_syscore_ops(struct syscore_ops *ops);
void unregister_syscore_ops(struct syscore_ops *ops);
#endif
