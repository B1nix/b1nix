/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_WRITEBACK_H
#define LKPI_LINUX_WRITEBACK_H

/*
 * Writeback control: how hard a caller wants dirty pages flushed. b1nix's GEM
 * pages have no backing store to write to — see <linux/shmem_fs.h> — so a
 * driver that asks for writeback gets a structure nothing reads. The modes are
 * defined because callers fill the structure in before a call that discards it.
 */
enum writeback_sync_modes { WB_SYNC_NONE = 0, WB_SYNC_ALL = 1 };
struct writeback_control {
	long nr_to_write;
	enum writeback_sync_modes sync_mode;
	unsigned range_cyclic : 1;
	unsigned for_reclaim : 1;
	loff_t range_start;
	loff_t range_end;
};

#endif
