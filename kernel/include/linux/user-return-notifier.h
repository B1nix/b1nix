/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_USER_RETURN_NOTIFIER_H
#define LKPI_LINUX_USER_RETURN_NOTIFIER_H
#include <linux/list.h>
/* Called once, on this CPU, the next time it returns to user mode: KVM puts
 * the host's values back in the MSRs a guest was allowed to change. */
struct user_return_notifier {
	void (*on_user_return)(struct user_return_notifier *urn);
	struct hlist_node link;
};
void user_return_notifier_register(struct user_return_notifier *urn);
void user_return_notifier_unregister(struct user_return_notifier *urn);
#endif
