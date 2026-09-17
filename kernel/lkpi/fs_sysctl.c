/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sysctl tables the imported code registers at boot, served under /proc/sys.
 *
 * A table is a list of named values and a handler per value; the handler
 * formats the value (or parses a new one). Publishing one means creating a
 * /proc/sys file per entry whose read runs that entry's own handler, which is
 * what Linux's proc_sys does. The file itself belongs to b1nix's procfs, which
 * this side reaches through procfs_sysctl_register (plain C types only).
 *
 * Read-only: nothing registered this way is writable yet, and an entry whose
 * mode asks for writes is published without its write bits rather than
 * pretending to accept a value it would drop.
 */
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/sysctl.h>
#include <linux/string.h>

void procfs_sysctl_register(const char *path, const char *name,
                            unsigned int mode,
                            long (*read)(void *entry, char *buf, unsigned long cap),
                            void *entry);

static long sysctl_entry_read(void *entry, char *buf, unsigned long cap)
{
	struct ctl_table *table = entry;
	size_t len = cap;
	loff_t pos = 0;
	int rc;

	if (!table->proc_handler)
		return -EIO;
	rc = table->proc_handler(table, 0, buf, &len, &pos);
	return rc ? rc : (long)len;
}

void lkpi_register_sysctl_init(const char *path, const struct ctl_table *table,
                               usize n)
{
	for (usize i = 0; i < n; i++) {
		if (!table[i].procname)
			continue;
		procfs_sysctl_register(path, table[i].procname,
		                       table[i].mode & 0444, sysctl_entry_read,
		                       (void *)&table[i]);
	}
}

/*
 * The read half of Linux's proc_doulongvec_minmax: each unsigned long of the
 * entry, tab-separated, then a newline. A read that does not start at the
 * beginning gets nothing, since the whole value is always returned at once.
 */
int proc_doulongvec_minmax(struct ctl_table *table, int write, void *buffer,
                           size_t *lenp, loff_t *ppos)
{
	unsigned long *v = table->data;
	int count = table->maxlen / (int)sizeof(unsigned long);
	size_t used = 0;

	if (write)
		return -EPERM;
	if (!v || *ppos) {
		*lenp = 0;
		return 0;
	}
	for (int i = 0; i < count; i++) {
		int n = snprintf((char *)buffer + used, *lenp - used, "%s%lu",
		                 i ? "\t" : "", v[i]);
		if (n < 0 || (size_t)n >= *lenp - used)
			return -ENOMEM;
		used += (size_t)n;
	}
	if (used + 1 >= *lenp)
		return -ENOMEM;
	((char *)buffer)[used++] = '\n';
	*lenp = used;
	*ppos += (loff_t)used;
	return 0;
}
