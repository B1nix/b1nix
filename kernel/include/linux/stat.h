/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_STAT_H
#define LKPI_LINUX_STAT_H
/* The mode bits, for the attribute files a driver registers. Octal, as they are
 * everywhere else they appear. */
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IRUGO (S_IRUSR | S_IRGRP | S_IROTH)
#define S_IWUSR_IRUGO (S_IWUSR | S_IRUGO)
#define S_IRWXU 0700
#endif
