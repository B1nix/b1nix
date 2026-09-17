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
#define O_NOCTTY   0400
#define O_TRUNC    01000
#define O_APPEND   02000
#define O_DSYNC    010000
#define O_DIRECT   040000
#define O_LARGEFILE 0100000
#define O_DIRECTORY 0200000
#define O_NOFOLLOW 0400000
#define O_NOATIME  01000000
#define O_CLOEXEC  02000000
#define __O_SYNC   04000000
#define O_SYNC     (__O_SYNC | O_DSYNC)
#define O_PATH     010000000

/* 64-bit only: every open is a large-file open. */
#ifndef force_o_largefile
#define force_o_largefile() (true)
#endif

/* The directory a relative path is resolved against when no descriptor is
 * named. ABI value. */
#define AT_FDCWD -100
#endif
