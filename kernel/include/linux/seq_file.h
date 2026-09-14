/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SEQ_FILE_H
#define LKPI_LINUX_SEQ_FILE_H

#include <linux/printk.h>
#include <linux/types.h>

/* Declared rather than included: <linux/fs.h> pulls the device model in behind
 * it, and a header this low in the include graph must not decide that order for
 * every translation unit that prints one line. */
struct file;
struct inode;
struct file_operations;

/*
 * seq_file — the buffered iterator behind Linux's /proc and debugfs text files.
 *
 * The shape a driver writes against is: its open() calls single_open() with a
 * show() function, and its fops read through seq_read(). Everything the show()
 * prints goes into a buffer, and reads are served out of that buffer at the
 * offset the reader is at. The buffer is what makes a dump longer than one read
 * possible at all: the show() runs once, to completion, and later reads
 * continue through what it produced rather than re-running it and seeing a
 * different answer halfway.
 *
 * Growth is the part that is easy to get wrong. A show() that does not fit is
 * not truncated: the buffer doubles and the show() runs again from scratch,
 * which is why `overflow` exists and why a show() must be free of side effects.
 * Linux does the same thing for the same reason.
 *
 * The iterator form (seq_open with start/next/stop/show) is here because the
 * DRM core's own debugfs uses it; it renders every element into the same
 * buffer, so it is bounded by memory rather than by a page.
 */

struct seq_file;

struct seq_operations {
	void *(*start)(struct seq_file *m, loff_t *pos);
	void (*stop)(struct seq_file *m, void *v);
	void *(*next)(struct seq_file *m, void *v, loff_t *pos);
	int (*show)(struct seq_file *m, void *v);
};

struct seq_file {
	char *buf;
	usize size;
	usize count;
	/* Set when a print did not fit. The reader grows the buffer and renders
	 * again rather than serving a value with its tail missing. */
	int overflow;
	/* What the driver attached: single_open's data, or its iterator's. */
	void *private;
	const struct seq_operations *op;
	/* Owned by this seq_file when it allocated the buffer itself, which is
	 * the case for everything that goes through seq_read. */
	int owns_buf;
	int rendered;
	int (*single_show)(struct seq_file *m, void *v);
	/* The open file this sequence is being written to. Both filesystems reach
	 * it to find the superblock behind a /sys or /proc read. */
	struct file *file;
};

int seq_printf(struct seq_file *m, const char *fmt, ...) __printf(2, 3);
int seq_puts(struct seq_file *m, const char *s);
/* A mount option whose value may contain characters that would end the field:
 * upstream escapes them, and so should this once anything parses the output
 * back. Nothing does yet, so the value is printed as it is. */
int seq_show_option(struct seq_file *m, const char *name, const char *value);
int seq_putc(struct seq_file *m, char c);
/* Append raw bytes, for a binary blob such as an EDID. */
int seq_write(struct seq_file *m, const void *data, usize len);

/*
 * The open/read/release triple a driver's file_operations point at.
 *
 * single_open is for a file that is one rendering of one thing; seq_open is for
 * one that walks a sequence. Both attach the seq_file to file->private_data,
 * which is where seq_read finds it.
 */
int single_open(struct file *file, int (*show)(struct seq_file *, void *),
                void *data);
int single_release(struct inode *inode, struct file *file);
int seq_open(struct file *file, const struct seq_operations *op);
int seq_release(struct inode *inode, struct file *file);
ssize_t seq_read(struct file *file, char __user *buf, size_t size, loff_t *ppos);
loff_t seq_lseek(struct file *file, loff_t offset, int whence);


/* Did the last write run out of room in the seq buffer? b1nix's seq_file grows
 * its buffer rather than truncating, so a write never overflows and this is
 * always false — callers use it to decide to retry with a bigger buffer, which
 * is a retry that is not needed here. */
static inline bool seq_has_overflowed(struct seq_file *m) { (void)m; return false; }

/* single_open() with a caller-chosen initial buffer size. b1nix's seq_file
 * grows on demand, so the size is a hint that changes nothing but the number of
 * growth steps. */
int single_open_size(struct file *file, int (*show)(struct seq_file *, void *),
                     void *data, usize size);

/*
 * The sentinel a seq_operations `start` returns for the header line.
 *
 * It is a pointer value that is deliberately not a valid object: `show` is
 * called with it once, prints the header, and the iteration proper begins at
 * the next `next`. Returning NULL instead would end the sequence before it
 * started.
 */
#define SEQ_START_TOKEN ((void *)1)

/*
 * Print a string with the given characters escaped.
 *
 * Mount options go through it: a device name containing a space or a comma
 * would otherwise produce a /proc/mounts line that cannot be parsed back.
 */
void seq_escape(struct seq_file *m, const char *s, const char *esc);
void seq_escape_str(struct seq_file *m, const char *src, unsigned int flags,
                    const char *esc);

#endif
