/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_DEBUGFS_H
#define LKPI_LINUX_DEBUGFS_H
#include <linux/types.h>
/*
 * debugfs, over the same registry that backs /sys attributes, rooted at
 * /sys/kernel/debug. b1nix has no separate debugfs mount and does not need one:
 * a driver cannot tell the difference between its files appearing there and
 * appearing under a mount of their own, and a whole filesystem for a handful of
 * diagnostic dumps would be code with no behaviour behind it.
 *
 * A file is declared with a file_operations rather than a show(). Its open() is
 * performed on first read — b1nix's /sys has no open path of its own to hang it
 * on — which is what lets the usual shape work: an open() that calls
 * single_open(), and fops.read = seq_read. The read is given the reader's
 * offset, so a dump longer than one read continues rather than restarting.
 */
struct dentry;

/* A table of registers a driver dumps into debugfs. */
struct debugfs_reg32 { const char *name; unsigned long offset; };
struct debugfs_regset32 {
	const struct debugfs_reg32 *regs;
	int nregs;
	void __iomem *base;
	struct device *dev;
};
struct file_operations;

/* A NULL parent means the debugfs root. */
struct dentry *debugfs_create_dir(const char *name, struct dentry *parent);
struct dentry *debugfs_create_file(const char *name, umode_t mode,
                                   struct dentry *parent, void *data,
                                   const struct file_operations *fops);
void debugfs_remove(struct dentry *d);
void debugfs_remove_recursive(struct dentry *d);
#endif
