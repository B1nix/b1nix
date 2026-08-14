/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SYSCTL_H
#define LKPI_LINUX_SYSCTL_H
#include <linux/types.h>
/* sysctl tables. b1nix exposes its tunables through /proc/sys with its own
 * plumbing and nothing imported registers a table here — these names exist
 * because a table names its handler even when the table is never registered,
 * and an unregistered table is inert rather than wrong. */
struct ctl_table;
int proc_dointvec(struct ctl_table *, int, void *, size_t *, loff_t *);
int proc_dointvec_minmax(struct ctl_table *, int, void *, size_t *, loff_t *);

/* The /proc/sys entries a driver registers for its tunables. b1nix has no
 * sysctl tree, so registration reports success and creates nothing: the
 * tunables keep their compiled-in defaults and are simply not writable from
 * userspace. */
struct ctl_table_header;
/* One tunable. Nothing here publishes them — see below — but a driver defines
 * the table statically, so the shape has to be complete. */
struct ctl_table {
	const char *procname;
	void *data;
	int maxlen;
	umode_t mode;
	int (*proc_handler)(struct ctl_table *, int, void *, usize *, loff_t *);
	void *extra1;
	void *extra2;
};
static inline struct ctl_table_header *register_sysctl(const char *path,
                                                       struct ctl_table *table)
{ (void)path; (void)table; return NULL; }
static inline void unregister_sysctl_table(struct ctl_table_header *h)
{ (void)h; }

extern int sysctl_zero_value;
extern int sysctl_one_value;
#define SYSCTL_ZERO (&sysctl_zero_value)
#define SYSCTL_ONE  (&sysctl_one_value)

#endif
