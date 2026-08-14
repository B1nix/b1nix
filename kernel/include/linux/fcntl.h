/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_FCNTL_H
#define LKPI_LINUX_FCNTL_H
/* Open flags. ABI values, reproduced rather than chosen: userspace passes them
 * in and reads them back. */
#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT    0100
#define O_EXCL     0200
#define O_NONBLOCK 04000
#define O_CLOEXEC  02000000
#endif
