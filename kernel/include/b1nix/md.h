/*
 * Software RAID.
 *
 * An array is an ordinary block device: it registers with the block layer like
 * any disk, so partitions, mkfs, mount and the page cache reach it through the
 * same path they reach a SATA disk through. RAID 0 (striping) and RAID 1
 * (mirroring) are implemented.
 *
 * ON-DISK FORMAT — read this before assuming compatibility.
 *
 * The superblock below is b1nix's own, not Linux's md 1.x. Nothing in this
 * system can write a Linux superblock (there is no mdadm here) and nothing can
 * verify one, so claiming that format would be a promise no test could keep.
 * The layout is documented here and written by /bin/mdcreate; an array made on
 * Linux will not be recognised, and that is stated rather than discovered.
 */
#ifndef B1NIX_MD_H
#define B1NIX_MD_H

#include <b1nix/types.h>

#define MD_MAGIC        0x4231494e444d3031ULL /* "B1INDM01" little-endian */
#define MD_SB_LBA       0    /* the superblock lives in the member's first block */
#define MD_MAX_MEMBERS  8
#define MD_MAX_ARRAYS   4

#define MD_LEVEL_STRIPE 0
#define MD_LEVEL_MIRROR 1

/* One block, so a member's superblock costs exactly one sector-sized read. */
struct md_superblock {
	u64 magic;
	u32 level;         /* MD_LEVEL_* */
	u32 members;       /* how many members the array has in total */
	u32 member_index;  /* which one this disk is, 0-based */
	u32 chunk_blocks;  /* striping granularity, in blocks; ignored by a mirror */
	u64 data_blocks;   /* usable blocks on THIS member, after the header */
	u64 data_offset;   /* first usable block on this member */
	u64 array_uuid;    /* members of one array share this */
	u32 sb_csum;       /* sum of the fields above, so a stale block is not read as an array */
	u32 pad;
};

/* Scan every registered block device for superblocks and bring up whatever
 * complete (or, for a mirror, usable) arrays they describe. This is what the
 * RAID_AUTORUN ioctl calls. Returns the number of arrays started. */
void md_init(void);
int md_autorun(void);

/* Release every array. Members are left untouched. */
void md_stop_all(void);

#endif /* B1NIX_MD_H */
