/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PART_STAT_H
#define LKPI_LINUX_PART_STAT_H

#include <linux/blkdev.h>

/*
 * Per-partition I/O statistics — the numbers behind /proc/diskstats.
 *
 * b1nix's block layer keeps its own counters and these are not wired to them:
 * a filesystem incrementing a read count is reporting the same I/O the block
 * layer below it is already counting, so wiring both would double every number.
 * `part_stat_read` returns zero, which reads as "no I/O yet" rather than as a
 * wrong figure.
 */

#define part_stat_inc(part, field)          do { (void)(part); } while (0)
#define part_stat_add(part, field, addnd)   do { (void)(part); (void)(addnd); } while (0)
#define part_stat_read(part, field)         ((unsigned long)0)
#define part_stat_read_accum(part, field)   ((unsigned long)0)
#define part_stat_local_inc(part, field)    do { (void)(part); } while (0)
#define part_stat_local_dec(part, field)    do { (void)(part); } while (0)
#define part_stat_lock()                    do { } while (0)
#define part_stat_unlock()                  do { } while (0)

#endif
