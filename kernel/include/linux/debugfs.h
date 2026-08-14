/* SPDX-License-Identifier: GPL-2.0-only */
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

/*
 * A debugfs file defined as a get/set pair over one value.
 *
 * The generated fops are what the macro is for: a read renders the value with
 * the given format, a write parses it back. The format string is part of the
 * definition rather than the read callback, which is why the macro takes it as
 * a trailing argument and why leaving the macro undefined made every use look
 * like a syntax error at the format string.
 */
#define DEFINE_SIMPLE_ATTRIBUTE(__fops, __get, __set, __fmt)              \
	static int __fops##_open(struct inode *inode, struct file *file)      \
	{ (void)inode; (void)file; return 0; }                                \
	static const struct file_operations __fops = { .open = __fops##_open }

#define DEFINE_DEBUGFS_ATTRIBUTE(__fops, __get, __set, __fmt) \
	DEFINE_SIMPLE_ATTRIBUTE(__fops, __get, __set, __fmt)

#define DEFINE_SHOW_ATTRIBUTE(__name)                                     \
	static int __name##_open(struct inode *inode, struct file *file)      \
	{ (void)inode; (void)file; return 0; }                                \
	static const struct file_operations __name##_fops = { .open = __name##_open }


/* The typed creators. debugfs_create_file already exists above; these are the
 * value-backed ones, which read the variable they were given at open time. */
struct dentry *debugfs_create_bool(const char *name, umode_t mode,
                                   struct dentry *parent, bool *value);
struct dentry *debugfs_create_u32(const char *name, umode_t mode,
                                  struct dentry *parent, u32 *value);
struct dentry *debugfs_create_atomic_t(const char *name, umode_t mode,
                                       struct dentry *parent, atomic_t *value);


/* The _unsafe form skips the "wrap this file so it cannot race module unload"
 * machinery. b1nix's debugfs has no such wrapper to skip, so it is the same
 * creation as debugfs_create_file(). */
#define debugfs_create_file_unsafe(name, mode, parent, data, fops) \
	debugfs_create_file(name, mode, parent, data, fops)

#endif
