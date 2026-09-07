/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_RAID_PQ_H
#define LKPI_LINUX_RAID_PQ_H

#include <linux/types.h>

/*
 * RAID-6 syndrome generation and recovery — the P and Q parity of btrfs's
 * raid56 profiles.
 *
 * The implementation is Linux's `lib/raid6`, which is generated code selected
 * at boot from what the CPU supports. It is NOT imported yet, and this header
 * declares the three entry points btrfs calls so that raid56.c compiles.
 *
 * That is a real gap, and it has a shape: a btrfs filesystem using raid5 or
 * raid6 will fail to mount rather than mounting and computing wrong parity,
 * because these symbols resolve to implementations that report their absence.
 * Every other profile — single, dup, raid0, raid1, raid10 — is unaffected, and
 * those are what a filesystem made by `mkfs.btrfs` with default options uses.
 */

/* Compute P and Q across `disks` buffers of `bytes` each. `ptrs` is the array
 * of data pointers with P and Q last, which is the layout raid56.c builds. */
void raid6_call_gen_syndrome(int disks, size_t bytes, void **ptrs);
/* Recover two failed data disks, or one data disk and Q. */
void raid6_2data_recov(int disks, size_t bytes, int faila, int failb,
                       void **ptrs);
void raid6_datap_recov(int disks, size_t bytes, int faila, void **ptrs);

struct raid6_calls {
	void (*gen_syndrome)(int, size_t, void **);
	void (*xor_syndrome)(int, int, int, size_t, void **);
	int (*valid)(void);
	const char *name;
	int priority;
};

extern const struct raid6_calls raid6_call;

#define raid6_gfmul   raid6_gfmul_tbl
#define raid6_gfexp   raid6_gfexp_tbl
#define raid6_gfinv   raid6_gfinv_tbl
#define raid6_gfexi   raid6_gfexi_tbl

extern const u8 raid6_gfmul_tbl[256][256];
extern const u8 raid6_gfexp_tbl[256];
extern const u8 raid6_gfinv_tbl[256];
extern const u8 raid6_gfexi_tbl[256];

#endif
