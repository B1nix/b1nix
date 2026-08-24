/*
 * Software RAID: striping and mirroring, as a real block device.
 *
 * The array registers with the block layer, so everything above it — the block
 * cache, partition scanning, mkfs, mount — works through the ordinary path and
 * knows nothing about RAID. Members are addressed through their own
 * read_blocks/write_blocks rather than through the cache: caching the same
 * block twice, once as the array's and once as the member's, wastes memory and
 * gives two copies that can disagree.
 *
 * What a mirror promises, and what it does NOT:
 *
 *   - A read is served by the first member that answers. A member that fails a
 *     read is marked failed and not asked again, so one bad disk costs one
 *     failed read rather than every read.
 *   - A write goes to every live member and succeeds if at least one does. A
 *     member that fails a write is marked failed.
 *   - There is no resynchronisation. A member that comes back after failing is
 *     stale, and this code will not silently start reading from it: failed
 *     members stay failed until the array is stopped and assembled again.
 *     Rebuilding is a feature, and pretending to have it would be worse than
 *     not having it.
 */

#include <b1nix/blk.h>
#include <b1nix/klog.h>
#include <b1nix/mm.h>
#include <b1nix/md.h>
#include <b1nix/spinlock.h>
#include <string.h>

struct md_array {
	struct block_device dev;
	int level;
	u32 members;
	u32 chunk_blocks;
	u64 array_uuid;
	struct block_device *member[MD_MAX_MEMBERS];
	u64 data_offset[MD_MAX_MEMBERS];
	u8 failed[MD_MAX_MEMBERS];
	int used;
	/* Assembled and serving. Not "has a size" and not "has a name": the node
	 * is registered from boot and a candidate gains its size during the scan,
	 * so either of those would let a second autorun re-start a running array
	 * -- which for a stripe would multiply its size by the member count all
	 * over again. */
	int started;
};

static struct md_array g_md[MD_MAX_ARRAYS];
static spinlock_t md_lock;

static u32 md_csum(const struct md_superblock *sb)
{
	/* Every field that describes the array, so a block that merely happens to
	 * start with the magic is rejected rather than trusted. */
	u64 acc = sb->magic + sb->level + sb->members + sb->member_index +
		  sb->chunk_blocks + sb->data_blocks + sb->data_offset +
		  sb->array_uuid;
	return (u32)(acc ^ (acc >> 32));
}

static int md_read_sb(struct block_device *dev, struct md_superblock *out)
{
	if (!dev || !dev->read_blocks || dev->block_size < sizeof(*out))
		return -1;

	char *buf = kmalloc(dev->block_size);
	if (!buf)
		return -1;
	int rc = dev->read_blocks(dev, MD_SB_LBA, 1, buf);
	if (rc < 0) {
		kfree(buf);
		return -1;
	}
	struct md_superblock sb;
	memcpy(&sb, buf, sizeof(sb));
	kfree(buf);

	if (sb.magic != MD_MAGIC)
		return -1;
	if (sb.sb_csum != md_csum(&sb))
		return -1;
	if (sb.level != MD_LEVEL_STRIPE && sb.level != MD_LEVEL_MIRROR)
		return -1;
	if (sb.members == 0 || sb.members > MD_MAX_MEMBERS)
		return -1;
	if (sb.member_index >= sb.members)
		return -1;
	if (sb.level == MD_LEVEL_STRIPE && sb.chunk_blocks == 0)
		return -1;
	*out = sb;
	return 0;
}

/* Which member holds array block `lba`, and where on it, for a stripe. */
static void md_stripe_map(struct md_array *a, u64 lba, u32 *idx, u64 *dev_lba)
{
	u64 chunk = lba / a->chunk_blocks;
	u64 off = lba % a->chunk_blocks;

	*idx = (u32)(chunk % a->members);
	*dev_lba = (chunk / a->members) * a->chunk_blocks + off +
		   a->data_offset[*idx];
}

static void md_fail(struct md_array *a, u32 idx, const char *what)
{
	if (a->failed[idx])
		return;
	a->failed[idx] = 1;
	klog_warn("md: member failed");
	klog_warn(what);
}

static int md_live_members(struct md_array *a)
{
	int n = 0;

	for (u32 i = 0; i < a->members; i++)
		if (a->member[i] && !a->failed[i])
			n++;
	return n;
}

static int md_read(struct block_device *dev, u64 lba, u32 count, void *buffer)
{
	struct md_array *a = (struct md_array *)dev;
	char *out = buffer;

	for (u32 b = 0; b < count; b++) {
		u64 cur = lba + b;
		char *dst = out + (usize)b * dev->block_size;

		if (a->level == MD_LEVEL_STRIPE) {
			u32 idx;
			u64 dev_lba;

			md_stripe_map(a, cur, &idx, &dev_lba);
			if (!a->member[idx] || a->failed[idx])
				return -1; /* a stripe cannot survive a lost member */
			if (a->member[idx]->read_blocks(a->member[idx], dev_lba, 1,
							dst) < 0) {
				md_fail(a, idx, "stripe read");
				return -1;
			}
			continue;
		}

		/* Mirror: the first member that answers wins. */
		int served = 0;
		for (u32 i = 0; i < a->members && !served; i++) {
			if (!a->member[i] || a->failed[i])
				continue;
			if (a->member[i]->read_blocks(a->member[i],
						      cur + a->data_offset[i], 1,
						      dst) < 0) {
				md_fail(a, i, "mirror read");
				continue;
			}
			served = 1;
		}
		if (!served)
			return -1;
	}
	return 0;
}

static int md_write(struct block_device *dev, u64 lba, u32 count,
		    const void *buffer)
{
	struct md_array *a = (struct md_array *)dev;
	const char *in = buffer;

	for (u32 b = 0; b < count; b++) {
		u64 cur = lba + b;
		const char *src = in + (usize)b * dev->block_size;

		if (a->level == MD_LEVEL_STRIPE) {
			u32 idx;
			u64 dev_lba;

			md_stripe_map(a, cur, &idx, &dev_lba);
			if (!a->member[idx] || a->failed[idx])
				return -1;
			if (a->member[idx]->write_blocks(a->member[idx], dev_lba,
							 1, src) < 0) {
				md_fail(a, idx, "stripe write");
				return -1;
			}
			continue;
		}

		/* Mirror: write everywhere, and succeed if anywhere took it. */
		int ok = 0;
		for (u32 i = 0; i < a->members; i++) {
			if (!a->member[i] || a->failed[i])
				continue;
			if (a->member[i]->write_blocks(a->member[i],
						       cur + a->data_offset[i], 1,
						       src) < 0) {
				md_fail(a, i, "mirror write");
				continue;
			}
			ok = 1;
		}
		if (!ok)
			return -1;
	}
	return 0;
}

static struct md_array *md_find_or_create(u64 uuid, const struct md_superblock *sb)
{
	for (int i = 0; i < MD_MAX_ARRAYS; i++)
		if (g_md[i].used && g_md[i].array_uuid == uuid)
			return &g_md[i];

	for (int i = 0; i < MD_MAX_ARRAYS; i++) {
		if (g_md[i].used)
			continue;
		/* Keep the registered block_device: only the array description is
		 * being filled in. Zeroing the slot here would unpublish the node
		 * the caller is holding open. */
		g_md[i].level = 0;
		g_md[i].members = 0;
		g_md[i].chunk_blocks = 0;
		memset(g_md[i].member, 0, sizeof(g_md[i].member));
		memset(g_md[i].data_offset, 0, sizeof(g_md[i].data_offset));
		memset(g_md[i].failed, 0, sizeof(g_md[i].failed));
		g_md[i].dev.block_count = 0;
		g_md[i].started = 0;
		g_md[i].used = 1;
		g_md[i].array_uuid = uuid;
		g_md[i].level = (int)sb->level;
		g_md[i].members = sb->members;
		g_md[i].chunk_blocks = sb->chunk_blocks;
		return &g_md[i];
	}
	return 0;
}

int md_autorun(void)
{
	int started = 0;
	u64 flags;

	spin_lock_irqsave(&md_lock, &flags);

	/* Pass one: collect members. */
	usize n = blk_count();
	for (usize i = 0; i < n; i++) {
		struct block_device *dev = blk_at(i);
		struct md_superblock sb;

		if (!dev || !dev->name)
			continue;
		/* An array is not its own member: skip anything already assembled. */
		if (dev->name[0] == 'm' && dev->name[1] == 'd')
			continue;
		if (md_read_sb(dev, &sb) != 0)
			continue;

		struct md_array *a = md_find_or_create(sb.array_uuid, &sb);
		if (!a || a->started)
			continue;
		if (a->level != (int)sb.level || a->members != sb.members) {
			klog_warn("md: member disagrees about the array it belongs to");
			continue;
		}
		if (a->member[sb.member_index])
			continue; /* two disks claiming one slot: keep the first */
		a->member[sb.member_index] = dev;
		a->data_offset[sb.member_index] = sb.data_offset;
		a->dev.block_size = dev->block_size;
		/* Capacity: a stripe is the sum of its members, a mirror is one
		 * member. Both are computed from the smallest member's usable
		 * size, so an array of unequal disks is honest about what it can
		 * hold. */
		if (a->dev.block_count == 0 || sb.data_blocks < a->dev.block_count)
			a->dev.block_count = sb.data_blocks;
	}

	/* Pass two: start whatever can run. */
	for (int i = 0; i < MD_MAX_ARRAYS; i++) {
		struct md_array *a = &g_md[i];

		if (!a->used || a->started)
			continue;

		int have = md_live_members(a);
		if (have == 0) {
			a->used = 0;
			continue;
		}
		if (a->level == MD_LEVEL_STRIPE && have != (int)a->members) {
			/* Striping has no redundancy: a missing member means
			 * missing data, and starting the array would hand out
			 * whatever the gaps happen to contain. */
			klog_warn("md: incomplete stripe not started");
			a->used = 0;
			continue;
		}

		if (a->level == MD_LEVEL_STRIPE)
			a->dev.block_count *= a->members;

		/* The node already exists (see md_init): raidautorun has to OPEN
		 * /dev/md0 before it can ask for assembly, so an array that only
		 * appears once assembled could never be assembled at all. What
		 * happens here is that the empty device gains its geometry. */
		a->dev.read_blocks = md_read;
		a->dev.write_blocks = md_write;
		a->started = 1;
		started++;
		if (a->level == MD_LEVEL_MIRROR && have < (int)a->members)
			klog_warn("md: mirror started degraded");
	}

	spin_unlock_irqrestore(&md_lock, flags);
	return started;
}

void md_stop_all(void)
{
	u64 flags;

	spin_lock_irqsave(&md_lock, &flags);
	for (int i = 0; i < MD_MAX_ARRAYS; i++) {
		if (!g_md[i].used)
			continue;
		if (g_md[i].dev.name)
			blk_unregister(&g_md[i].dev);
		memset(&g_md[i], 0, sizeof(g_md[i]));
	}
	spin_unlock_irqrestore(&md_lock, flags);
}

/*
 * Publish empty arrays at boot.
 *
 * md0..md3 exist from the start with no members and no size, exactly as
 * /dev/md0 does on a Linux system before anything is assembled. Their block
 * count is zero until md_autorun() finds members for them, so a read of an
 * unassembled array fails rather than returning whatever memory it points at.
 */
void md_init(void)
{
	for (int i = 0; i < MD_MAX_ARRAYS; i++) {
		struct md_array *a = &g_md[i];
		char *nm = kmalloc(8);

		memset(a, 0, sizeof(*a));
		if (!nm)
			return;
		nm[0] = 'm'; nm[1] = 'd';
		nm[2] = (char)('0' + i);
		nm[3] = '\0';
		a->dev.name = nm;
		a->dev.bus = BLK_BUS_MD;
		a->dev.block_size = 512;
		a->dev.block_count = 0;
		blk_register(&a->dev);
	}
}
