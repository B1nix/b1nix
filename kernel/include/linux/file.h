/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_FILE_H
#define LKPI_LINUX_FILE_H
#include <linux/types.h>
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
#endif
