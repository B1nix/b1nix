/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_FILE_H
#define LKPI_LINUX_FILE_H
#include <linux/types.h>
/* The seq_file interface the debugfs attribute macros around file.h use. */
#include <linux/seq_file.h>
/* Descriptor handling. b1nix's VFS owns the real table; these are the names the
 * core uses when it hands a buffer or a fence to userspace, wired up with the
 * first ioctl that does so. */
struct file;
struct file_operations;
struct file *fget(unsigned int fd);
void fput(struct file *f);
int get_unused_fd_flags(unsigned int flags);
void fd_install(unsigned int fd, struct file *f);
void put_unused_fd(unsigned int fd);

/*
 * The lightweight descriptor lookup: `fd_file` is the file, and `flags` records
 * whether fdput must drop a reference. Wired up with the VFS bridge.
 */
struct fd { struct file *file; unsigned int flags; };
struct fd fdget(unsigned int fd);
void fdput(struct fd f);

/* The 6.12+ accessors, and the scope-bound form `CLASS(fd, f)(fd)`, which
 * drops the reference when `f` goes out of scope. */
#include <linux/cleanup.h>
#define fd_file(f)  ((f).file)
#define fd_empty(f) (unlikely(!(f).file))
struct fd fdget_raw(unsigned int fd);
DEFINE_CLASS(fd, struct fd, fdput(_T), fdget(fd), int fd)
DEFINE_CLASS(fd_raw, struct fd, fdput(_T), fdget_raw(fd), int fd)

/* Another reference on a file the caller already holds one on. */
struct file *get_file(struct file *f);
/* A reference on the file *f points at, if it is still live; NULL once its
 * last reference has gone, even though *f may not have been cleared yet. */
struct file *get_file_active(struct file **f);

#endif
