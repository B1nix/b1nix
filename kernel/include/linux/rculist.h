/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_RCULIST_H
#define LKPI_LINUX_RCULIST_H
#include <linux/list.h>
#include <lkpi/rcu.h>
/* List walks under an RCU read section. The publish/subscribe barriers are what
 * make a reader that sees a new node also see its contents. */
#define list_add_rcu(n, h)      list_add(n, h)
#define list_add_tail_rcu(n, h) list_add_tail(n, h)
#define list_del_rcu(n)         list_del(n)
#define list_for_each_entry_rcu(pos, head, member, cond) \
	list_for_each_entry(pos, head, member)
#endif
