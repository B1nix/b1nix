/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * linuxkpi: the inode.
 *
 * Allocation, the hash that makes a lookup by inode number O(1), the reference
 * and link counts, the timestamps, and the permission checks. A filesystem
 * supplies `alloc_inode` and `evict_inode`; everything between those two is
 * here.
 *
 * The lifecycle is the part worth stating, because every bug in it is a
 * use-after-free or a leak:
 *
 *   new_inode / iget_locked      allocate, with i_count = 1
 *   unlock_new_inode             publish: I_NEW clears and waiters wake
 *   igrab / ihold                take another reference
 *   iput                         drop one; the last drop evicts
 *
 * `I_NEW` is what makes the second half of iget_locked safe: a second lookup
 * for the same inode finds it in the hash, sees I_NEW, and waits rather than
 * using an inode that has not been read off the disk yet.
 */

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/writeback.h>
#include <linux/security.h>
#include <linux/capability.h>
#include <linux/hash.h>
#include <linux/cache.h>
/* inode_maybe_inc_iversion: the i_version counter lives with the inode, and
 * only this header knows the queried-bit encoding. */
#include <linux/iversion.h>
#include <lkpi/page.h>

/* ── the hash ───────────────────────────────────────────────────── */

/*
 * One table for every superblock, keyed by (sb, ino).
 *
 * A per-superblock table would be tidier and is what upstream effectively has
 * through its own hashing; one shared table costs a comparison per lookup and
 * saves an allocation per mount. The superblock is part of the key rather than
 * of the bucket, so two filesystems with the same inode numbers do not collide.
 */
#define LKPI_INODE_HASH_BITS 12
#define LKPI_INODE_HASH_SIZE (1u << LKPI_INODE_HASH_BITS)

static struct hlist_head inode_hashtable[LKPI_INODE_HASH_SIZE];
static spinlock_t inode_hash_lock;
static int inode_hash_ready;

static void inode_hash_init(void)
{
	unsigned int i;

	if (inode_hash_ready)
		return;
	for (i = 0; i < LKPI_INODE_HASH_SIZE; i++)
		INIT_HLIST_HEAD(&inode_hashtable[i]);
	spin_lock_init(&inode_hash_lock);
	inode_hash_ready = 1;
}

static unsigned long inode_hash(struct super_block *sb, unsigned long hashval)
{
	unsigned long tmp;

	/*
	 * The superblock pointer is mixed in, not just appended: two filesystems
	 * mounted at once have inode numbers from the same small range, and a
	 * hash that ignored the superblock would put every root inode in one
	 * bucket.
	 */
	tmp = (hashval * (unsigned long)GOLDEN_RATIO_32) ^
	      ((unsigned long)sb / L1_CACHE_BYTES);
	return tmp & (LKPI_INODE_HASH_SIZE - 1);
}

void __insert_inode_hash(struct inode *inode, unsigned long hashval)
{
	unsigned long flags;

	inode_hash_init();
	spin_lock_irqsave(&inode_hash_lock, flags);
	hlist_add_head(&inode->i_hash, &inode_hashtable[inode_hash(inode->i_sb,
	                                                           hashval)]);
	spin_unlock_irqrestore(&inode_hash_lock, flags);
}

void remove_inode_hash(struct inode *inode)
{
	unsigned long flags;

	inode_hash_init();
	spin_lock_irqsave(&inode_hash_lock, flags);
	if (!hlist_unhashed(&inode->i_hash))
		hlist_del_init(&inode->i_hash);
	spin_unlock_irqrestore(&inode_hash_lock, flags);
}

/* ── allocation ─────────────────────────────────────────────────── */

static void inode_init_always(struct super_block *sb, struct inode *inode)
{
	inode->i_sb = sb;
	inode->i_flags = 0;
	inode->i_state = 0;
	inode->i_blkbits = sb ? sb->s_blocksize_bits : 12;
	inode->i_opflags = 0;
	inode->i_size = 0;
	inode->i_blocks = 0;
	inode->i_bytes = 0;
	inode->__i_nlink = 1;
	atomic_set(&inode->i_count, 1);
	atomic_set(&inode->i_dio_count, 0);
	atomic_set(&inode->i_writecount, 0);
	atomic64_set(&inode->i_version, 0);
	spin_lock_init(&inode->i_lock);
	init_rwsem(&inode->i_rwsem);
	INIT_HLIST_NODE(&inode->i_hash);
	INIT_LIST_HEAD(&inode->i_io_list);
	INIT_LIST_HEAD(&inode->i_lru);
	INIT_LIST_HEAD(&inode->i_sb_list);
	INIT_LIST_HEAD(&inode->i_wb_list);
	INIT_LIST_HEAD(&inode->i_devices);
	inode->i_acl = ACL_NOT_CACHED;
	inode->i_default_acl = ACL_NOT_CACHED;
	inode->i_private = NULL;
	inode->i_link = NULL;
	inode->i_generation = 0;
	inode->i_rdev = 0;

	/*
	 * The inode's own mapping. It is embedded rather than allocated because
	 * every inode has exactly one, and `i_mapping` points at it — a
	 * filesystem may repoint it (a block device inode shares the device's),
	 * which is why the pointer exists at all.
	 */
	memset(&inode->i_data, 0, sizeof(inode->i_data));
	inode->i_data.host = inode;
	inode->i_data.gfp_mask = GFP_KERNEL;
	xa_init(&inode->i_data.i_pages);
	init_rwsem(&inode->i_data.invalidate_lock);
	spin_lock_init(&inode->i_data.private_lock);
	INIT_LIST_HEAD(&inode->i_data.private_list);
	inode->i_mapping = &inode->i_data;
}

void inode_init_once(struct inode *inode)
{
	memset(inode, 0, sizeof(*inode));
	INIT_HLIST_NODE(&inode->i_hash);
	INIT_LIST_HEAD(&inode->i_io_list);
	INIT_LIST_HEAD(&inode->i_lru);
	INIT_LIST_HEAD(&inode->i_sb_list);
	INIT_LIST_HEAD(&inode->i_wb_list);
	INIT_LIST_HEAD(&inode->i_devices);
	spin_lock_init(&inode->i_lock);
	init_rwsem(&inode->i_rwsem);
}

/*
 * Allocate through the filesystem's own allocator when it has one.
 *
 * It has to be the filesystem's: btrfs's inode is a `btrfs_inode` with the VFS
 * inode embedded in the middle of it, and allocating a bare `struct inode`
 * would leave every field around it uninitialised — which the first
 * BTRFS_I(inode) dereference then reads.
 */
static struct inode *alloc_inode(struct super_block *sb)
{
	struct inode *inode;

	if (sb && sb->s_op && sb->s_op->alloc_inode)
		inode = sb->s_op->alloc_inode(sb);
	else
		inode = kzalloc(sizeof(*inode), GFP_KERNEL);
	if (!inode)
		return NULL;
	inode_init_always(sb, inode);
	return inode;
}

struct inode *new_inode(struct super_block *sb)
{
	struct inode *inode = alloc_inode(sb);

	if (inode && sb) {
		unsigned long flags;

		spin_lock_irqsave(&sb->s_inode_list_lock, flags);
		list_add(&inode->i_sb_list, &sb->s_inodes);
		spin_unlock_irqrestore(&sb->s_inode_list_lock, flags);
	}
	return inode;
}

struct inode *alloc_inode_sb(struct super_block *sb, struct kmem_cache *cache,
                             gfp_t gfp)
{
	/* The cache is the filesystem's own; going through it rather than the
	 * general heap is what keeps its inodes together and what its constructor
	 * expects. */
	(void)gfp;
	return kmem_cache_alloc(cache, GFP_KERNEL);
	(void)sb;
}

/* ── lookup ─────────────────────────────────────────────────────── */

static struct inode *find_inode_locked(struct super_block *sb,
                                       unsigned long hashval,
                                       int (*test)(struct inode *, void *),
                                       void *data, unsigned long ino)
{
	struct hlist_head *head = &inode_hashtable[inode_hash(sb, hashval)];
	struct inode *inode;

	hlist_for_each_entry(inode, head, i_hash) {
		if (inode->i_sb != sb)
			continue;
		if (test) {
			if (!test(inode, data))
				continue;
		} else if (inode->i_ino != ino) {
			continue;
		}
		return inode;
	}
	return NULL;
}

/*
 * Wait for an inode somebody else is still reading off the disk.
 *
 * The waiter parks on the inode's own address, which is the channel
 * unlock_new_inode wakes. Re-testing after the wake rather than assuming is
 * what makes a shared wakeup harmless.
 */
static void wait_on_inode_new(struct inode *inode)
{
	while (inode->i_state & I_NEW) {
		lkpi_wait_prepare(inode);
		if (inode->i_state & I_NEW)
			lkpi_wait_commit();
		else
			lkpi_wait_cancel();
	}
}

struct inode *iget_locked(struct super_block *sb, unsigned long ino)
{
	struct inode *inode;
	unsigned long flags;

	inode_hash_init();
again:
	spin_lock_irqsave(&inode_hash_lock, flags);
	inode = find_inode_locked(sb, ino, NULL, NULL, ino);
	if (inode) {
		atomic_inc(&inode->i_count);
		spin_unlock_irqrestore(&inode_hash_lock, flags);
		wait_on_inode_new(inode);
		return inode;
	}
	spin_unlock_irqrestore(&inode_hash_lock, flags);

	inode = alloc_inode(sb);
	if (!inode)
		return NULL;
	inode->i_ino = ino;
	/*
	 * I_NEW is set before the inode is published, and cleared by
	 * unlock_new_inode once the caller has read it off the disk. Publishing
	 * without it would let a second lookup use an empty inode.
	 */
	inode->i_state = I_NEW;

	spin_lock_irqsave(&inode_hash_lock, flags);
	if (find_inode_locked(sb, ino, NULL, NULL, ino)) {
		/* Somebody inserted the same inode while we allocated. Theirs wins —
		 * it may already be locked by its creator. */
		spin_unlock_irqrestore(&inode_hash_lock, flags);
		if (sb && sb->s_op && sb->s_op->destroy_inode)
			sb->s_op->destroy_inode(inode);
		else
			kfree(inode);
		goto again;
	}
	hlist_add_head(&inode->i_hash, &inode_hashtable[inode_hash(sb, ino)]);
	if (sb) {
		list_add(&inode->i_sb_list, &sb->s_inodes);
	}
	spin_unlock_irqrestore(&inode_hash_lock, flags);
	return inode;
}

struct inode *iget5_locked(struct super_block *sb, unsigned long hashval,
                           int (*test)(struct inode *, void *),
                           int (*set)(struct inode *, void *), void *data)
{
	struct inode *inode;
	unsigned long flags;

	inode_hash_init();
again:
	spin_lock_irqsave(&inode_hash_lock, flags);
	inode = find_inode_locked(sb, hashval, test, data, 0);
	if (inode) {
		atomic_inc(&inode->i_count);
		spin_unlock_irqrestore(&inode_hash_lock, flags);
		wait_on_inode_new(inode);
		return inode;
	}
	spin_unlock_irqrestore(&inode_hash_lock, flags);

	inode = alloc_inode(sb);
	if (!inode)
		return NULL;
	inode->i_state = I_NEW;
	/* `set` is what writes the filesystem's own key into the new inode —
	 * btrfs's location, for instance — and must run before it is published,
	 * or `test` will not recognise it. */
	if (set && set(inode, data)) {
		if (sb && sb->s_op && sb->s_op->destroy_inode)
			sb->s_op->destroy_inode(inode);
		else
			kfree(inode);
		return NULL;
	}

	spin_lock_irqsave(&inode_hash_lock, flags);
	if (find_inode_locked(sb, hashval, test, data, 0)) {
		spin_unlock_irqrestore(&inode_hash_lock, flags);
		if (sb && sb->s_op && sb->s_op->destroy_inode)
			sb->s_op->destroy_inode(inode);
		else
			kfree(inode);
		goto again;
	}
	hlist_add_head(&inode->i_hash, &inode_hashtable[inode_hash(sb, hashval)]);
	if (sb)
		list_add(&inode->i_sb_list, &sb->s_inodes);
	spin_unlock_irqrestore(&inode_hash_lock, flags);
	return inode;
}

struct inode *ilookup(struct super_block *sb, unsigned long ino)
{
	struct inode *inode;
	unsigned long flags;

	inode_hash_init();
	spin_lock_irqsave(&inode_hash_lock, flags);
	inode = find_inode_locked(sb, ino, NULL, NULL, ino);
	if (inode)
		atomic_inc(&inode->i_count);
	spin_unlock_irqrestore(&inode_hash_lock, flags);
	if (inode)
		wait_on_inode_new(inode);
	return inode;
}

struct inode *ilookup5(struct super_block *sb, unsigned long hashval,
                       int (*test)(struct inode *, void *), void *data)
{
	struct inode *inode;
	unsigned long flags;

	inode_hash_init();
	spin_lock_irqsave(&inode_hash_lock, flags);
	inode = find_inode_locked(sb, hashval, test, data, 0);
	if (inode)
		atomic_inc(&inode->i_count);
	spin_unlock_irqrestore(&inode_hash_lock, flags);
	if (inode)
		wait_on_inode_new(inode);
	return inode;
}

struct inode *find_inode_by_ino_rcu(struct super_block *sb, unsigned long ino)
{
	/* The RCU form: a lookup with no reference taken, for a caller that only
	 * wants to know whether the inode is cached. It still takes the lock here
	 * — b1nix's hash is not RCU-protected — which makes it stricter than
	 * upstream's and never wrong. */
	struct inode *inode;
	unsigned long flags;

	inode_hash_init();
	spin_lock_irqsave(&inode_hash_lock, flags);
	inode = find_inode_locked(sb, ino, NULL, NULL, ino);
	spin_unlock_irqrestore(&inode_hash_lock, flags);
	return inode;
}

int insert_inode_locked4(struct inode *inode, unsigned long hashval,
                         int (*test)(struct inode *, void *), void *data)
{
	struct super_block *sb = inode->i_sb;
	unsigned long flags;

	inode_hash_init();
	spin_lock_irqsave(&inode_hash_lock, flags);
	if (find_inode_locked(sb, hashval, test, data, 0)) {
		spin_unlock_irqrestore(&inode_hash_lock, flags);
		return -EBUSY;
	}
	inode->i_state |= I_NEW;
	hlist_add_head(&inode->i_hash, &inode_hashtable[inode_hash(sb, hashval)]);
	spin_unlock_irqrestore(&inode_hash_lock, flags);
	return 0;
}

int insert_inode_locked(struct inode *inode)
{
	return insert_inode_locked4(inode, inode->i_ino, NULL, NULL);
}

/* ── publishing and disposal ────────────────────────────────────── */

void unlock_new_inode(struct inode *inode)
{
	unsigned long flags;

	spin_lock_irqsave(&inode->i_lock, flags);
	inode->i_state &= ~I_NEW;
	spin_unlock_irqrestore(&inode->i_lock, flags);
	/* Everything parked in wait_on_inode_new is woken; each re-tests. */
	lkpi_wake_all(inode);
}

void iget_failed(struct inode *inode)
{
	/*
	 * The read off the disk failed. The inode must leave the hash before it
	 * is freed — a second lookup finding it would use freed memory — and it
	 * is marked bad first so that anything already waiting on I_NEW gets an
	 * inode that fails every operation rather than one that looks empty.
	 */
	make_bad_inode(inode);
	remove_inode_hash(inode);
	unlock_new_inode(inode);
	iput(inode);
}

void discard_new_inode(struct inode *inode)
{
	remove_inode_hash(inode);
	unlock_new_inode(inode);
	iput(inode);
}

void ihold(struct inode *inode)
{
	atomic_inc(&inode->i_count);
}

struct inode *igrab(struct inode *inode)
{
	/*
	 * Take a reference unless the inode is on its way out. Resurrecting one
	 * that is already being evicted is a use-after-free wearing the name of
	 * the thing that prevents one.
	 */
	unsigned long flags;
	struct inode *ret = NULL;

	spin_lock_irqsave(&inode->i_lock, flags);
	if (!(inode->i_state & (I_FREEING | I_WILL_FREE))) {
		atomic_inc(&inode->i_count);
		ret = inode;
	}
	spin_unlock_irqrestore(&inode->i_lock, flags);
	return ret;
}

void clear_inode(struct inode *inode)
{
	inode->i_state = I_FREEING | I_CLEAR;
}

/* One more reference on an inode the caller already knows is alive. Not iget:
 * it must not bring back an inode that is on its way out, and the caller's own
 * lock is what guarantees it is not. */
void __iget(struct inode *inode)
{
	if (inode)
		atomic_inc(&inode->i_count);
}

static void evict(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	unsigned long flags;

	spin_lock_irqsave(&inode->i_lock, flags);
	inode->i_state |= I_FREEING;
	spin_unlock_irqrestore(&inode->i_lock, flags);

	/*
	 * Everything cached for this inode goes first, then the filesystem's own
	 * teardown, then the memory. The order is not adjustable: evict_inode
	 * expects the page cache already gone (btrfs asserts it), and freeing the
	 * inode before evict_inode would pull the ground from under it.
	 */
	truncate_inode_pages_final(&inode->i_data);
	remove_inode_hash(inode);

	spin_lock_irqsave(&inode_hash_lock, flags);
	if (!list_empty(&inode->i_sb_list))
		list_del_init(&inode->i_sb_list);
	spin_unlock_irqrestore(&inode_hash_lock, flags);

	if (sb && sb->s_op && sb->s_op->evict_inode)
		sb->s_op->evict_inode(inode);
	else
		clear_inode(inode);

	if (sb && sb->s_op && sb->s_op->destroy_inode)
		sb->s_op->destroy_inode(inode);
	else if (sb && sb->s_op && sb->s_op->free_inode)
		sb->s_op->free_inode(inode);
	else
		kfree(inode);
}

void iput(struct inode *inode)
{
	struct super_block *sb;
	int drop;

	if (!inode)
		return;
	if (!atomic_dec_and_test(&inode->i_count))
		return;

	sb = inode->i_sb;
	drop = (sb && sb->s_op && sb->s_op->drop_inode) ?
	       sb->s_op->drop_inode(inode) : generic_drop_inode(inode);
	/*
	 * A file that still has a name stays cached with no references, as
	 * upstream's iput_final keeps it. Evicting it on the last put threw away
	 * its page cache -- including data written and not yet flushed, which
	 * btrfs holds as delalloc -- so `cp a b; mv b c` lost the file's contents
	 * when the rename dropped b's dentry. The superblock evicts what is left
	 * at unmount (evict_inodes).
	 */
	if (!drop && sb && (sb->s_flags & SB_ACTIVE))
		return;
	if (!drop)
		filemap_write_and_wait(inode->i_mapping);
	evict(inode);
}

/* Evict every cached inode nothing references any more. Called at unmount,
 * after the dentries are gone and before the filesystem's put_super. */
void evict_inodes(struct super_block *sb)
{
	struct inode *inode, *next;

	list_for_each_entry_safe(inode, next, &sb->s_inodes, i_sb_list) {
		if (atomic_read(&inode->i_count) != 0)
			continue;
		if (inode->i_state & (I_NEW | I_FREEING))
			continue;
		evict(inode);
	}
}

int generic_drop_inode(struct inode *inode)
{
	/* Drop it when nothing links to it any more, or when it is already off
	 * the hash. An inode with links stays cached. */
	return !inode->i_nlink || inode_unhashed(inode);
}

int generic_delete_inode(struct inode *inode)
{
	(void)inode;
	return 1;
}

/* ── bad inodes ─────────────────────────────────────────────────── */

/*
 * An inode whose read failed.
 *
 * It is marked rather than discarded because callers already hold it: a lookup
 * that hit a corrupt inode must return something that fails every subsequent
 * operation, not a NULL that reads as "no such file".
 */
static const struct inode_operations bad_inode_ops;

void make_bad_inode(struct inode *inode)
{
	inode->i_mode = S_IFREG;
	inode->i_op = &bad_inode_ops;
	inode->i_fop = NULL;
	inode->i_opflags = 0;
}

bool is_bad_inode(struct inode *inode)
{
	return inode && inode->i_op == &bad_inode_ops;
}

/* ── counts and sizes ───────────────────────────────────────────── */

void set_nlink(struct inode *inode, unsigned int nlink)
{
	inode->__i_nlink = nlink;
}

void inc_nlink(struct inode *inode)
{
	inode->__i_nlink++;
}

void drop_nlink(struct inode *inode)
{
	/* Never below zero: an unbalanced drop would wrap the count and make a
	 * deleted inode look like one with four billion links. */
	if (inode->__i_nlink)
		inode->__i_nlink--;
}

void clear_nlink(struct inode *inode)
{
	inode->__i_nlink = 0;
}

void inode_inc_link_count(struct inode *inode)
{
	inc_nlink(inode);
	mark_inode_dirty(inode);
}

void inode_dec_link_count(struct inode *inode)
{
	drop_nlink(inode);
	mark_inode_dirty(inode);
}

/*
 * i_blocks and i_bytes together are the allocated size: whole 512-byte units in
 * the first, the remainder in the second. Adding therefore has to carry — a
 * version that only touched i_bytes loses 512 bytes of accounting every time it
 * wraps, and `du` reports it.
 */
void __inode_add_bytes(struct inode *inode, loff_t bytes)
{
	inode->i_blocks += bytes >> 9;
	inode->i_bytes += (unsigned short)(bytes & 511);
	if (inode->i_bytes >= 512) {
		inode->i_blocks++;
		inode->i_bytes -= 512;
	}
}

void inode_add_bytes(struct inode *inode, loff_t bytes)
{
	unsigned long flags;

	spin_lock_irqsave(&inode->i_lock, flags);
	__inode_add_bytes(inode, bytes);
	spin_unlock_irqrestore(&inode->i_lock, flags);
}

void __inode_sub_bytes(struct inode *inode, loff_t bytes)
{
	inode->i_blocks -= bytes >> 9;
	if (inode->i_bytes < (bytes & 511)) {
		inode->i_blocks--;
		inode->i_bytes += 512;
	}
	inode->i_bytes -= (unsigned short)(bytes & 511);
}

void inode_sub_bytes(struct inode *inode, loff_t bytes)
{
	unsigned long flags;

	spin_lock_irqsave(&inode->i_lock, flags);
	__inode_sub_bytes(inode, bytes);
	spin_unlock_irqrestore(&inode->i_lock, flags);
}

/* With i_lock already held by the caller. inode_get_bytes below is the same
 * read; it does not take the lock either, because the two fields are read in
 * one go and a torn read of them is not a thing this architecture does — but
 * the two names exist upstream and callers pick between them deliberately. */
loff_t __inode_get_bytes(struct inode *inode)
{
	return (((loff_t)inode->i_blocks) << 9) + inode->i_bytes;
}

loff_t inode_get_bytes(struct inode *inode)
{
	return __inode_get_bytes(inode);
}

void inode_set_bytes(struct inode *inode, loff_t bytes)
{
	inode->i_blocks = bytes >> 9;
	inode->i_bytes = (unsigned short)(bytes & 511);
}

/* ── time ───────────────────────────────────────────────────────── */

/*
 * The current time, truncated to what the filesystem can store.
 *
 * The truncation is not cosmetic: an ext4 inode with 128-byte inodes holds
 * seconds only, and a timestamp with nanoseconds written into it and read back
 * differs from what was set — which a filesystem check reports.
 */
struct timespec64 current_time(struct inode *inode)
{
	struct timespec64 now;
	u32 gran = inode && inode->i_sb ? inode->i_sb->s_time_gran : 1;

	ktime_get_real_ts64(&now);
	if (gran > 1) {
		if (gran == 1000000000u)
			now.tv_nsec = 0;
		else
			now.tv_nsec -= now.tv_nsec % (long)gran;
	}
	return now;
}

struct timespec64 inode_set_ctime_current(struct inode *inode)
{
	struct timespec64 now = current_time(inode);

	inode_set_ctime_to_ts(inode, now);
	return now;
}

struct timespec64 timestamp_truncate(struct timespec64 t, struct inode *inode)
{
	u32 gran = inode && inode->i_sb ? inode->i_sb->s_time_gran : 1;

	if (gran > 1 && gran != 1000000000u)
		t.tv_nsec -= t.tv_nsec % (long)gran;
	else if (gran == 1000000000u)
		t.tv_nsec = 0;
	return t;
}

/*
 * Refresh the timestamps a write or a read requires, reporting whether
 * anything changed.
 *
 * The return value is what tells the caller to mark the inode dirty; a void
 * version would dirty the inode on every access, including the ones that wrote
 * the same value back.
 */
bool inode_update_timestamps(struct inode *inode, int flags)
{
	struct timespec64 now = current_time(inode);
	bool updated = false;

	if (flags & S_ATIME) {
		if (inode->i_atime.tv_sec != now.tv_sec ||
		    inode->i_atime.tv_nsec != now.tv_nsec) {
			inode->i_atime = now;
			updated = true;
		}
	}
	if (flags & S_MTIME) {
		if (inode->i_mtime.tv_sec != now.tv_sec ||
		    inode->i_mtime.tv_nsec != now.tv_nsec) {
			inode->i_mtime = now;
			updated = true;
		}
	}
	if (flags & S_CTIME) {
		struct timespec64 ctime = inode_get_ctime(inode);

		if (ctime.tv_sec != now.tv_sec || ctime.tv_nsec != now.tv_nsec) {
			inode_set_ctime_to_ts(inode, now);
			updated = true;
		}
	}
	if (flags & S_VERSION)
		updated |= inode_maybe_inc_iversion(inode, false);
	return updated;
}

int inode_update_time(struct inode *inode, int flags)
{
	if (inode->i_op && inode->i_op->update_time)
		return inode->i_op->update_time(inode, flags);
	return generic_update_time(inode, flags);
}

int generic_update_time(struct inode *inode, int flags)
{
	if (inode_update_timestamps(inode, flags))
		__mark_inode_dirty(inode, I_DIRTY_SYNC);
	return 0;
}

void touch_atime(const struct path *path)
{
	struct inode *inode;

	if (!path || !path->dentry)
		return;
	inode = d_inode(path->dentry);
	if (!inode || IS_NOATIME(inode) || IS_RDONLY(inode))
		return;
	inode_update_time(inode, S_ATIME);
}

/* ── dirtying ───────────────────────────────────────────────────── */

/*
 * Mark an inode dirty.
 *
 * The filesystem's `dirty_inode` is what records it — btrfs joins the running
 * transaction there — and the state bits are what a later writeback pass reads.
 * A version that only set the bits would leave the filesystem unaware that
 * anything changed, and the inode would never be written.
 */
void __mark_inode_dirty(struct inode *inode, int flags)
{
	struct super_block *sb = inode->i_sb;
	unsigned long lflags;
	int was;

	spin_lock_irqsave(&inode->i_lock, lflags);
	was = inode->i_state & I_DIRTY_ALL;
	inode->i_state |= flags;
	spin_unlock_irqrestore(&inode->i_lock, lflags);

	if ((flags & I_DIRTY_INODE) && sb && sb->s_op && sb->s_op->dirty_inode)
		sb->s_op->dirty_inode(inode, flags);
	(void)was;
}

void inode_io_list_del(struct inode *inode)
{
	if (!list_empty(&inode->i_io_list))
		list_del_init(&inode->i_io_list);
}

int inode_needs_sync(struct inode *inode)
{
	return IS_SYNC(inode) || (S_ISDIR(inode->i_mode) && IS_DIRSYNC(inode));
}

void inode_set_flags(struct inode *inode, unsigned int flags, unsigned int mask)
{
	inode->i_flags = (inode->i_flags & ~mask) | (flags & mask);
}

void inode_nohighmem(struct inode *inode)
{
	/* "Do not allocate this inode's pages from high memory." There is no high
	 * memory here — the whole of RAM is in the direct map — so the mask is
	 * already right. */
	mapping_set_gfp_mask(inode->i_mapping, GFP_KERNEL);
}

void cache_no_acl(struct inode *inode)
{
	/* NULL, not ACL_NOT_CACHED: this says "this inode has no ACL", which is
	 * an answer, where the sentinel means "nobody has looked yet". */
	inode->i_acl = NULL;
	inode->i_default_acl = NULL;
}

void invalidate_inode_buffers(struct inode *inode)
{
	(void)inode;
	/* The buffer list hangs off the mapping's private_list, which only the
	 * buffer-head layer populates; btrfs does not use it. */
}

/* ── direct I/O accounting ──────────────────────────────────────── */

void inode_dio_end(struct inode *inode)
{
	if (atomic_dec_and_test(&inode->i_dio_count))
		lkpi_wake_all(&inode->i_dio_count);
}

/*
 * Wait for direct I/O to drain.
 *
 * Taken before a truncate: direct I/O bypasses the page cache, so nothing else
 * would stop a read landing in blocks the truncate has already freed.
 */
void inode_dio_wait(struct inode *inode)
{
	while (atomic_read(&inode->i_dio_count)) {
		lkpi_wait_prepare(&inode->i_dio_count);
		if (atomic_read(&inode->i_dio_count))
			lkpi_wait_commit();
		else
			lkpi_wait_cancel();
	}
}

/* ── ownership and permission ───────────────────────────────────── */

void inode_init_owner(struct mnt_idmap *idmap, struct inode *inode,
                      const struct inode *dir, umode_t mode)
{
	inode_fsuid_set(inode, idmap);
	if (dir && (dir->i_mode & S_ISGID)) {
		/*
		 * A setgid directory passes its group to what is created in it, and
		 * passes the setgid bit itself to subdirectories. That is what makes
		 * a shared project directory work, and dropping it silently changes
		 * who can read new files.
		 */
		inode->i_gid = dir->i_gid;
		if (S_ISDIR(mode))
			mode |= S_ISGID;
	} else {
		inode_fsgid_set(inode, idmap);
	}
	inode->i_mode = mode;
}

bool inode_owner_or_capable(struct mnt_idmap *idmap, const struct inode *inode)
{
	kuid_t uid = i_uid_into_vfsuid(idmap, inode);

	if (uid_eq(current_fsuid(), uid))
		return true;
	/* Root, or a process holding CAP_FOWNER. Comparing uids alone would
	 * refuse root; the capability alone would let anyone through. */
	return capable(CAP_FOWNER);
}

int generic_permission(struct mnt_idmap *idmap, struct inode *inode, int mask)
{
	umode_t mode = inode->i_mode;
	int granted;

	mask &= MAY_READ | MAY_WRITE | MAY_EXEC | MAY_APPEND;
	if (!mask)
		return 0;

	if (uid_eq(current_fsuid(), i_uid_into_vfsuid(idmap, inode)))
		granted = (mode >> 6) & 7;
	else if (in_group_p(i_gid_into_vfsgid(idmap, inode)))
		granted = (mode >> 3) & 7;
	else
		granted = mode & 7;

	/*
	 * Clear from the request every permission the mode grants; what is left
	 * is what the caller does not have. Written as three tests rather than
	 * one expression because the two bit layouts (rwx in the mode, MAY_* in
	 * the mask) are only coincidentally in the same order, and a clever
	 * conversion between them is exactly where an off-by-one permission bug
	 * would live.
	 */
	if ((granted & 4) && (mask & MAY_READ))
		mask &= ~MAY_READ;
	if ((granted & 2) && (mask & (MAY_WRITE | MAY_APPEND)))
		mask &= ~(MAY_WRITE | MAY_APPEND);
	if ((granted & 1) && (mask & MAY_EXEC))
		mask &= ~MAY_EXEC;
	if (!mask)
		return 0;

	/* CAP_DAC_OVERRIDE is the last word, and deliberately not the first: a
	 * capable process still gets the ordinary answer when the ordinary answer
	 * is yes, which keeps the common path cheap. */
	if (capable(CAP_DAC_OVERRIDE))
		return 0;
	return -EACCES;
}

int inode_permission(struct mnt_idmap *idmap, struct inode *inode, int mask)
{
	if ((mask & MAY_WRITE) && IS_RDONLY(inode) &&
	    (S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
	     S_ISLNK(inode->i_mode)))
		return -EROFS;
	if ((mask & MAY_WRITE) && IS_IMMUTABLE(inode))
		return -EPERM;
	if (inode->i_op && inode->i_op->permission)
		return inode->i_op->permission(idmap, inode, mask);
	return generic_permission(idmap, inode, mask);
}

int check_sticky(struct mnt_idmap *idmap, struct inode *dir,
                 struct inode *inode)
{
	/*
	 * The sticky bit on a directory: only the owner of a file (or of the
	 * directory, or root) may remove it. /tmp is the reason it exists.
	 */
	if (!(dir->i_mode & S_ISVTX))
		return 0;
	if (uid_eq(i_uid_into_vfsuid(idmap, inode), current_fsuid()))
		return 0;
	if (uid_eq(i_uid_into_vfsuid(idmap, dir), current_fsuid()))
		return 0;
	return capable(CAP_FOWNER) ? 0 : -EPERM;
}

int setattr_prepare(struct mnt_idmap *idmap, struct dentry *dentry,
                    struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	unsigned int ia_valid = attr->ia_valid;

	if (ia_valid & ATTR_FORCE)
		return 0;
	if ((ia_valid & (ATTR_UID | ATTR_GID | ATTR_MODE)) &&
	    !inode_owner_or_capable(idmap, inode))
		return -EPERM;
	if (ia_valid & ATTR_SIZE) {
		int err = inode_newsize_ok(inode, attr->ia_size);

		if (err)
			return err;
	}
	return 0;
}

void setattr_copy(struct mnt_idmap *idmap, struct inode *inode,
                  const struct iattr *attr)
{
	unsigned int ia_valid = attr->ia_valid;

	i_uid_update(idmap, attr, inode);
	i_gid_update(idmap, attr, inode);
	if (ia_valid & ATTR_ATIME)
		inode->i_atime = attr->ia_atime;
	if (ia_valid & ATTR_MTIME)
		inode->i_mtime = attr->ia_mtime;
	if (ia_valid & ATTR_CTIME)
		inode_set_ctime_to_ts(inode, attr->ia_ctime);
	if (ia_valid & ATTR_MODE) {
		umode_t mode = attr->ia_mode;

		/*
		 * Setgid is dropped when the caller is not in the file's group.
		 * Keeping it would let a user create a file that runs as a group
		 * they are not in — which is the whole of the setgid escalation.
		 */
		if (!in_group_p(i_gid_into_vfsgid(idmap, inode)) &&
		    !capable(CAP_FSETID))
			mode &= ~S_ISGID;
		inode->i_mode = mode;
	}
}

int inode_newsize_ok(const struct inode *inode, loff_t offset)
{
	if (offset < 0)
		return -EINVAL;
	if (inode->i_sb && offset > inode->i_sb->s_maxbytes)
		return -EFBIG;
	return 0;
}

void generic_fillattr(struct mnt_idmap *idmap, u32 request_mask,
                      struct inode *inode, struct kstat *stat)
{
	(void)request_mask;
	stat->dev = inode->i_sb ? inode->i_sb->s_dev : 0;
	stat->ino = inode->i_ino;
	stat->mode = inode->i_mode;
	stat->nlink = inode->i_nlink;
	stat->uid = i_uid_into_vfsuid(idmap, inode);
	stat->gid = i_gid_into_vfsgid(idmap, inode);
	stat->rdev = inode->i_rdev;
	stat->size = i_size_read(inode);
	stat->atime = inode->i_atime;
	stat->mtime = inode->i_mtime;
	stat->ctime = inode_get_ctime(inode);
	stat->blksize = i_blocksize(inode);
	stat->blocks = inode->i_blocks;
	stat->result_mask = STATX_BASIC_STATS;
}

void init_special_inode(struct inode *inode, umode_t mode, dev_t rdev)
{
	inode->i_mode = mode;
	/*
	 * A device inode's operations come from the device layer, not the
	 * filesystem: the filesystem stores the number and nothing else. Wiring
	 * them is the bridge's job, and until then such an inode is created
	 * correctly on disk and cannot be opened.
	 */
	if (S_ISCHR(mode) || S_ISBLK(mode))
		inode->i_rdev = rdev;
	else if (S_ISFIFO(mode) || S_ISSOCK(mode))
		inode->i_rdev = 0;
}

int file_remove_privs(struct file *file)
{
	struct inode *inode = file_inode(file);

	/*
	 * A write by anyone other than the owner clears setuid and setgid. It is
	 * the counterpart of the check in setattr_copy: without it, writing to a
	 * setuid binary leaves it setuid with new contents.
	 */
	if (!inode || IS_NOSEC(inode))
		return 0;
	if (!(inode->i_mode & (S_ISUID | S_ISGID)))
		return 0;
	if (inode_owner_or_capable(&nop_mnt_idmap, inode))
		return 0;
	inode->i_mode &= ~(S_ISUID | S_ISGID);
	mark_inode_dirty(inode);
	return 0;
}

int file_update_time(struct file *file)
{
	struct inode *inode = file_inode(file);

	if (!inode)
		return 0;
	return inode_update_time(inode, S_MTIME | S_CTIME | S_VERSION);
}

int file_modified(struct file *file)
{
	int ret = file_remove_privs(file);

	if (ret)
		return ret;
	return file_update_time(file);
}

bool is_quota_modification(struct mnt_idmap *idmap, struct inode *inode,
                           struct iattr *ia)
{
	(void)idmap;
	(void)inode;
	/* No quotas are accounted, so no attribute change moves space between
	 * them — and a filesystem that believed otherwise would open a larger
	 * transaction than it needs. */
	(void)ia;
	return false;
}

bool fsuidgid_has_mapping(struct super_block *sb, struct mnt_idmap *idmap)
{
	(void)sb;
	(void)idmap;
	/* One user namespace, so every id maps. */
	return true;
}

int sb_is_blkdev_sb(struct super_block *sb)
{
	/* True only for the block device's own pseudo-filesystem, which b1nix
	 * does not have: every superblock here belongs to a real filesystem. */
	(void)sb;
	return 0;
}

struct backing_dev_info *inode_to_bdi(struct inode *inode)
{
	return inode && inode->i_sb ? inode->i_sb->s_bdi : NULL;
}
