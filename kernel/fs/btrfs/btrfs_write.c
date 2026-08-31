/* btrfs, write side.
 *
 * btrfs never overwrites live metadata: a change copies every block on the
 * path from the leaf up to the root into freshly allocated space, and the
 * transaction becomes real only when the super block is rewritten to name the
 * new roots. That property is what makes a writable btrfs driver approachable
 * without a journal — and it is also why the write path needs an allocator,
 * an extent tree and a checksum tree before it can store a single byte.
 *
 * What is implemented here: allocation from the block groups the filesystem
 * already has, copy-on-write insertion/update/removal in any tree (including
 * leaf and node splits), data extents with their extent-tree references and
 * their crc32c checksums, inode size and time updates, and a commit that
 * rewrites the super block after the data is on the medium.
 *
 * What is deliberately NOT implemented, and refused at mount rather than
 * guessed at: compression, multiple devices, RAID profiles, snapshots and
 * anything sharing extents between trees, the free-space tree (a writable
 * mount would have to keep it in step), the block-group tree, quota groups,
 * and growing the filesystem by creating new chunks. A write that would need
 * any of those fails with an errno instead of producing a filesystem that
 * only this driver can read.
 */

#include <b1nix/blk.h>
#include <b1nix/bootinfo.h>
#include <b1nix/btrfs.h>
#include <b1nix/errno.h>
#include <b1nix/klog.h>
#include <b1nix/mm.h>
#include <b1nix/rtc.h>
#include <b1nix/sched.h>
#include <string.h>
#include <stdio.h>

typedef int (*btrfs_leaf_fn)(struct btrfs_fs_info *fs, const u8 *leaf,
                             void *arg);
static int walk_tree(struct btrfs_fs_info *fs, u64 bytenr, btrfs_leaf_fn fn,
                     void *arg, int depth);

/* ── The allocation map ────────────────────────────────────────────────────*/

/* Every allocated range, sorted. Built once at mount from the extent tree and
 * maintained in memory afterwards, because the alternative — the free-space
 * tree — is a second on-disk structure this driver would have to keep
 * correct, and getting it subtly wrong is indistinguishable from working
 * until another implementation reads the disk. */

static int allocs_reserve(struct btrfs_fs_info *fs, u32 want) {
    if (want <= fs->allocs_cap)
        return 0;

    u32 cap = fs->allocs_cap ? fs->allocs_cap * 2 : 256;

    while (cap < want)
        cap *= 2;
    if (cap > BTRFS_MAX_ALLOCS)
        cap = BTRFS_MAX_ALLOCS;
    if (want > cap)
        return -ENOSPC;

    struct btrfs_alloc_range *n =
        kmalloc((usize)cap * sizeof(struct btrfs_alloc_range));

    if (!n)
        return -ENOMEM;
    if (fs->allocs) {
        memcpy(n, fs->allocs,
               (usize)fs->nallocs * sizeof(struct btrfs_alloc_range));
        kfree(fs->allocs);
    }
    fs->allocs = n;
    fs->allocs_cap = cap;
    return 0;
}

/* Insert keeping the array sorted by start. */
static int allocs_add(struct btrfs_fs_info *fs, u64 start, u64 len) {
    int rc = allocs_reserve(fs, fs->nallocs + 1);

    if (rc < 0)
        return rc;

    u32 i = 0;

    while (i < fs->nallocs && fs->allocs[i].start < start)
        i++;
    memmove(&fs->allocs[i + 1], &fs->allocs[i],
            (usize)(fs->nallocs - i) * sizeof(struct btrfs_alloc_range));
    fs->allocs[i].start = start;
    fs->allocs[i].len = len;
    fs->nallocs++;
    return 0;
}

static void allocs_remove(struct btrfs_fs_info *fs, u64 start) {
    for (u32 i = 0; i < fs->nallocs; i++) {
        if (fs->allocs[i].start != start)
            continue;
        memmove(&fs->allocs[i], &fs->allocs[i + 1],
                (usize)(fs->nallocs - i - 1) * sizeof(struct btrfs_alloc_range));
        fs->nallocs--;
        return;
    }
}

static struct btrfs_block_group *bg_of(struct btrfs_fs_info *fs, u64 bytenr) {
    for (u32 i = 0; i < fs->nbgs; i++) {
        if (bytenr >= fs->bgs[i].start &&
            bytenr < fs->bgs[i].start + fs->bgs[i].len)
            return &fs->bgs[i];
    }
    return 0;
}

/* First free run of `len` bytes in a block group of the wanted type.
 *
 * A first-fit walk over the sorted allocation list. btrfs's own allocator is
 * far cleverer about locality; this one only has to be CORRECT, since every
 * range it hands out is one the extent tree says nothing owns. */
static int pinned_overlaps(struct btrfs_fs_info *fs, u64 start, u64 len) {
    for (u32 i = 0; i < fs->npinned; i++) {
        if (start < fs->pinned[i].start + fs->pinned[i].len &&
            fs->pinned[i].start < start + len)
            return 1;
    }
    return 0;
}

static int btrfs_alloc_chunk(struct btrfs_fs_info *fs, u64 bg_type,
                             u64 min_bytes);

static u64 btrfs_alloc_bytes(struct btrfs_fs_info *fs, u64 len, u64 bg_type) {
    u64 align = bg_type == BTRFS_BLOCK_GROUP_METADATA ? fs->sb.nodesize
                                                      : fs->sb.sectorsize;

    if (!align)
        return 0;
    len = (len + align - 1) & ~(align - 1);

    for (u32 g = 0; g < fs->nbgs; g++) {
        struct btrfs_block_group *bg = &fs->bgs[g];

        if (!(bg->flags & bg_type))
            continue;

        u64 cur = (bg->start + align - 1) & ~(align - 1);
        u64 end = bg->start + bg->len;

        for (u32 i = 0; i < fs->nallocs && cur + len <= end; i++) {
            u64 a_start = fs->allocs[i].start;
            u64 a_end = a_start + fs->allocs[i].len;

            if (a_end <= cur)
                continue;
            if (a_start >= cur + len)
                break; /* the gap at cur is wide enough */
            cur = (a_end + align - 1) & ~(align - 1);
        }
        while (cur + len <= end && pinned_overlaps(fs, cur, len)) {
            /* Step past the pinned range rather than giving up on the group:
             * a transaction that freed a lot leaves many small holes it may
             * not use, and the space after them is perfectly good. */
            u64 next = cur + align;

            for (u32 i = 0; i < fs->npinned; i++) {
                u64 p_end = fs->pinned[i].start + fs->pinned[i].len;

                if (cur < p_end && fs->pinned[i].start < cur + len &&
                    p_end > next)
                    next = (p_end + align - 1) & ~(align - 1);
            }
            cur = next;
            for (u32 i = 0; i < fs->nallocs && cur + len <= end; i++) {
                u64 a_start = fs->allocs[i].start;
                u64 a_end = a_start + fs->allocs[i].len;

                if (a_end <= cur)
                    continue;
                if (a_start >= cur + len)
                    break;
                cur = (a_end + align - 1) & ~(align - 1);
            }
        }
        if (cur + len > end)
            continue;
        if (allocs_add(fs, cur, len) < 0)
            return 0;
        bg->used += len;
        bg->dirty = 1;
        return cur;
    }

    return 0;
}

/* How much of `bg_type` is free across the groups that exist. */
static u64 btrfs_free_space_in(struct btrfs_fs_info *fs, u64 bg_type) {
    u64 total = 0;

    for (u32 g = 0; g < fs->nbgs; g++) {
        struct btrfs_block_group *bg = &fs->bgs[g];

        if (!(bg->flags & bg_type))
            continue;
        total += bg->len > bg->used ? bg->len - bg->used : 0;
    }
    return total;
}

/* Freed, but not yet reusable.
 *
 * The range leaves the allocation map, but stays PINNED until the transaction
 * commits: the tree on disk still points at it until the super block is
 * rewritten. Handing it out again inside the same transaction is not subtle —
 * copy-on-write allocates a new block, records it, then drops the old one,
 * and an allocator that answers the next request with the address it just
 * freed writes the new root over a block the tree is still using. */
static int pinned_add(struct btrfs_fs_info *fs, u64 start, u64 len) {
    if (fs->npinned == fs->pinned_cap) {
        u32 cap = fs->pinned_cap ? fs->pinned_cap * 2 : 64;
        struct btrfs_alloc_range *n =
            kmalloc((usize)cap * sizeof(struct btrfs_alloc_range));

        if (!n)
            return -ENOMEM;
        if (fs->pinned) {
            memcpy(n, fs->pinned,
                   (usize)fs->npinned * sizeof(struct btrfs_alloc_range));
            kfree(fs->pinned);
        }
        fs->pinned = n;
        fs->pinned_cap = cap;
    }
    fs->pinned[fs->npinned].start = start;
    fs->pinned[fs->npinned].len = len;
    fs->npinned++;
    return 0;
}

static void btrfs_free_bytes(struct btrfs_fs_info *fs, u64 start, u64 len) {
    struct btrfs_block_group *bg = bg_of(fs, start);

    allocs_remove(fs, start);
    pinned_add(fs, start, len);
    if (bg) {
        bg->used = bg->used > len ? bg->used - len : 0;
        bg->dirty = 1;
    }
}

static int extent_insert_tree_block_now(struct btrfs_fs_info *fs, u64 bytenr,
                                        u64 owner, u8 level);
static int extent_drop_ref_now(struct btrfs_fs_info *fs, u64 bytenr, u64 len);
static int extent_insert_data_now(struct btrfs_fs_info *fs, u64 bytenr,
                                  u64 len, u64 root, u64 objectid,
                                  u64 file_offset);

/* ── Delayed references ────────────────────────────────────────────────────
 *
 * Recording an allocation in the extent tree is itself an allocation: the
 * insert copies the extent tree's own path into new blocks, and each of those
 * blocks needs its own extent record. Doing that inline recurses, and it
 * recursed straight off the kernel stack.
 *
 * btrfs's answer is delayed refs, and this is the same idea in miniature:
 * every extent-tree update is queued, and the queue is drained in a LOOP by
 * whoever is at the top of the operation. Draining an entry may queue more —
 * that is expected, and the loop absorbs it at constant stack depth. */

static int pending_push(struct btrfs_fs_info *fs,
                        const struct btrfs_delayed_ref *ref) {
    if (fs->npending == fs->pending_cap) {
        u32 cap = fs->pending_cap ? fs->pending_cap * 2 : 64;
        struct btrfs_delayed_ref *n =
            kmalloc((usize)cap * sizeof(struct btrfs_delayed_ref));

        if (!n)
            return -ENOMEM;
        if (fs->pending) {
            memcpy(n, fs->pending,
                   (usize)fs->npending * sizeof(struct btrfs_delayed_ref));
            kfree(fs->pending);
        }
        fs->pending = n;
        fs->pending_cap = cap;
    }
    fs->pending[fs->npending++] = *ref;
    return 0;
}

static int extent_insert_tree_block(struct btrfs_fs_info *fs, u64 bytenr,
                                    u64 owner, u8 level) {
    struct btrfs_delayed_ref ref = {bytenr, fs->sb.nodesize, owner, 0, 0,
                                    level, 0, 1};

    return pending_push(fs, &ref);
}

static int extent_insert_data(struct btrfs_fs_info *fs, u64 bytenr, u64 len,
                              u64 root, u64 objectid, u64 file_offset) {
    struct btrfs_delayed_ref ref = {bytenr, len, root, objectid, file_offset,
                                    0, 1, 1};

    return pending_push(fs, &ref);
}

static int extent_drop_ref(struct btrfs_fs_info *fs, u64 bytenr, u64 len) {
    struct btrfs_delayed_ref ref = {bytenr, len, 0, 0, 0, 0, 0, 0};

    return pending_push(fs, &ref);
}

/* Apply everything queued, including whatever applying it queues in turn.
 *
 * Entries are applied in order and never cancelled against one another: with
 * freed space pinned for the rest of the transaction, an add and a drop can
 * no longer name the same block, and a "cancellation" would have been the
 * allocator reusing an address the tree still points at. */
static int btrfs_run_delayed_refs(struct btrfs_fs_info *fs) {
    if (fs->running_refs)
        return 0; /* already draining, further entries land in the queue */
    fs->running_refs = 1;

    int rc = 0;

    for (u32 guard = 0; fs->npending && guard < 100000u; guard++) {
        struct btrfs_delayed_ref ref = fs->pending[0];

        memmove(&fs->pending[0], &fs->pending[1],
                (usize)(fs->npending - 1) * sizeof(struct btrfs_delayed_ref));
        fs->npending--;

        if (!ref.add)
            rc = extent_drop_ref_now(fs, ref.bytenr, ref.len);
        else if (ref.is_data)
            rc = extent_insert_data_now(fs, ref.bytenr, ref.len, ref.owner,
                                        ref.objectid, ref.file_off);
        else
            rc = extent_insert_tree_block_now(fs, ref.bytenr, ref.owner,
                                              ref.level);
        if (rc < 0)
            break;
    }
    if (rc == 0 && fs->npending) {
        klog_info("btrfs: delayed references did not settle");
        rc = -EIO;
    }
    fs->running_refs = 0;
    return rc;
}

/* ── Writing blocks ────────────────────────────────────────────────────────*/

/* A write must land in a block group meant for what is being written. A tree
 * block in a data group, or file data in a metadata group, corrupts whatever
 * shares the range — and shows up only as a checksum failure much later, on a
 * block nobody was writing at the time. Checked rather than assumed. */
static int range_kind_ok(struct btrfs_fs_info *fs, u64 logical, u64 len,
                         int metadata) {
    struct btrfs_block_group *bg = bg_of(fs, logical);

    if (!bg)
        return 0;
    if (logical + len > bg->start + bg->len)
        return 0;
    if (metadata)
        return (bg->flags & (BTRFS_BLOCK_GROUP_METADATA |
                             BTRFS_BLOCK_GROUP_SYSTEM)) != 0;
    return (bg->flags & BTRFS_BLOCK_GROUP_DATA) != 0;
}

/* Put bytes on the medium, by logical address. */
static int btrfs_write_raw(struct btrfs_fs_info *fs, u64 logical,
                           const void *buf, u32 len) {
    u64 physical = btrfs_map(fs, logical);

    if (!physical || (len % 512))
        return -EIO;
    if (blk_write_cached(fs->bdev, physical / 512, len / 512, buf) < 0)
        return -EIO;
    return 0;
}

/* Stamp a tree block's header and checksum, then put it on the disk. */
static int btrfs_write_block(struct btrfs_fs_info *fs, u64 logical, u8 *buf) {
    if (fs->nbgs && !range_kind_ok(fs, logical, fs->sb.nodesize, 1)) {
        char line[128];

        snprintf(line, sizeof(line),
                 "btrfs: tree block at %llu is not in a metadata group",
                 (unsigned long long)logical);
        klog_info(line);
        return -EIO;
    }
    struct btrfs_header *h = (struct btrfs_header *)buf;
    u32 nodesize = fs->sb.nodesize;

    h->bytenr = logical;
    h->generation = fs->trans_gen;
    memcpy(h->fsid, fs->sb.fsid, sizeof(h->fsid));

    u32 csum = btrfs_crc32c(buf + sizeof(h->csum), nodesize - sizeof(h->csum));

    memset(h->csum, 0, sizeof(h->csum));
    memcpy(h->csum, &csum, sizeof(csum));

    return btrfs_write_raw(fs, logical, buf, nodesize);
}

/* ── Paths ─────────────────────────────────────────────────────────────────*/

#define BTRFS_MAX_LEVEL 8

/* The blocks from the root down to one leaf, and the slot taken in each. A
 * write copies this whole chain, which is why it has to be remembered rather
 * than rediscovered: the parent is the only place a new child address can be
 * recorded. */
struct btrfs_path {
    u8 *nodes[BTRFS_MAX_LEVEL];
    u64 bytenr[BTRFS_MAX_LEVEL];
    u32 slots[BTRFS_MAX_LEVEL];
    int levels; /* nodes[0] is the leaf */
};

static void path_release(struct btrfs_path *p) {
    for (int i = 0; i < BTRFS_MAX_LEVEL; i++) {
        if (p->nodes[i]) {
            kfree(p->nodes[i]);
            p->nodes[i] = 0;
        }
    }
    p->levels = 0;
}

static struct btrfs_item *leaf_item_mut(u8 *leaf, u32 i) {
    return (struct btrfs_item *)(leaf + sizeof(struct btrfs_header) +
                                 (usize)i * sizeof(struct btrfs_item));
}

static u8 *item_data_mut(u8 *leaf, struct btrfs_item *it) {
    return leaf + sizeof(struct btrfs_header) + it->offset;
}

static struct btrfs_key_ptr *node_ptrs(u8 *node) {
    return (struct btrfs_key_ptr *)(node + sizeof(struct btrfs_header));
}

/* Bytes not yet used by items in a leaf: the gap between the item array and
 * the data that grows down from the end of the block. */
static u32 leaf_free_space(struct btrfs_fs_info *fs, const u8 *leaf) {
    const struct btrfs_header *h = (const struct btrfs_header *)leaf;
    u32 data_end = fs->sb.nodesize - sizeof(struct btrfs_header);
    u32 items = h->nritems * sizeof(struct btrfs_item);

    if (h->nritems)
        data_end = btrfs_leaf_item(leaf, h->nritems - 1)->offset;
    return data_end > items ? data_end - items : 0;
}

static u32 node_free_slots(struct btrfs_fs_info *fs, const u8 *node) {
    const struct btrfs_header *h = (const struct btrfs_header *)node;
    u32 cap = (fs->sb.nodesize - sizeof(struct btrfs_header)) /
              sizeof(struct btrfs_key_ptr);

    return cap > h->nritems ? cap - h->nritems : 0;
}

/* Descend to the leaf that would hold the key, remembering the way down. */
static int btrfs_search_path(struct btrfs_fs_info *fs, u64 root_bytenr,
                             u64 objectid, u8 type, u64 offset,
                             struct btrfs_path *path) {
    u64 block = root_bytenr;
    u8 *stack[BTRFS_MAX_LEVEL];
    u64 stack_bytenr[BTRFS_MAX_LEVEL];
    u32 stack_slot[BTRFS_MAX_LEVEL];
    int depth = 0;

    memset(path, 0, sizeof(*path));
    while (1) {
        if (depth >= BTRFS_MAX_LEVEL) {
            for (int i = 0; i < depth; i++)
                kfree(stack[i]);
            return -EIO;
        }

        u8 *buf = btrfs_read_block(fs, block);

        if (!buf) {
            for (int i = 0; i < depth; i++)
                kfree(stack[i]);
            return -EIO;
        }

        const struct btrfs_header *h = (const struct btrfs_header *)buf;
        u32 n = h->nritems;

        stack[depth] = buf;
        stack_bytenr[depth] = block;

        if (h->level == 0) {
            u32 i = 0;

            while (i < n && btrfs_key_cmp(&btrfs_leaf_item(buf, i)->key,
                                          objectid, type, offset) < 0)
                i++;
            stack_slot[depth] = i;
            depth++;
            break;
        }
        if (n == 0) {
            for (int i = 0; i <= depth; i++)
                kfree(stack[i]);
            return -EIO;
        }

        const struct btrfs_key_ptr *ptrs =
            (const struct btrfs_key_ptr *)(buf + sizeof(struct btrfs_header));
        u32 pick = 0;

        for (u32 i = 0; i < n; i++) {
            if (btrfs_key_cmp(&ptrs[i].key, objectid, type, offset) <= 0)
                pick = i;
            else
                break;
        }
        stack_slot[depth] = pick;
        block = ptrs[pick].blockptr;
        depth++;
    }

    /* Reverse: the leaf becomes level 0 of the path. */
    path->levels = depth;
    for (int i = 0; i < depth; i++) {
        int lvl = depth - 1 - i;

        path->nodes[lvl] = stack[i];
        path->bytenr[lvl] = stack_bytenr[i];
        path->slots[lvl] = stack_slot[i];
    }
    return 0;
}

/* ── Copy-on-write ─────────────────────────────────────────────────────────*/

/* Which root a tree block belongs to, so its extent-tree backref names the
 * right owner. */
struct btrfs_root_ref {
    u64 objectid;   /* BTRFS_FS_TREE_OBJECTID and friends */
    u64 *bytenr;    /* where this mount records the root's address */
    u8 *level;
};


/* Which block group a tree's own blocks must come from.
 *
 * The chunk tree is special and has to be: it is the tree that says where
 * logical addresses live, so it cannot be found through itself. The super
 * block carries a bootstrap map — sys_chunk_array — covering the SYSTEM
 * groups, and that is the only mapping available before the chunk tree is
 * read. A chunk-tree block written into a metadata group is at an address
 * nothing can resolve yet: btrfs-progs says "cannot read chunk root" and the
 * filesystem will not open at all. */
static u64 tree_block_group(u64 root_objectid) {
    return root_objectid == BTRFS_CHUNK_TREE_OBJECTID
               ? BTRFS_BLOCK_GROUP_SYSTEM
               : BTRFS_BLOCK_GROUP_METADATA;
}

/* Write every block of a path into new space, bottom up, and hand the new
 * root address back through `root`.
 *
 * The old blocks are released here. That is safe within one transaction
 * because nothing else refers to them any more: this driver commits a single
 * transaction per mount session and keeps no snapshots, so a block that has
 * left the tree has no other owner. */
static int cow_path(struct btrfs_fs_info *fs, struct btrfs_path *path,
                    struct btrfs_root_ref *root) {
    u32 nodesize = fs->sb.nodesize;

    for (int lvl = 0; lvl < path->levels; lvl++) {
        u64 old = path->bytenr[lvl];
        struct btrfs_header *h = (struct btrfs_header *)path->nodes[lvl];
        /* A block this transaction already copied is ours to overwrite.
         *
         * Without this the write path does not terminate: recording a new
         * tree block in the extent tree copies the extent tree's own path,
         * which produces more new blocks, which need recording, and so on.
         * btrfs makes the same distinction — copy-on-write applies to blocks
         * from an EARLIER transaction, because those are the ones another
         * reader may still be looking at. A block allocated and written in
         * this transaction has never been visible to anyone. */
        int already_ours = old && h->generation == fs->trans_gen;
        u64 nb = old;

        if (!already_ours) {
            nb = btrfs_alloc_bytes(fs, nodesize,
                                   tree_block_group(root->objectid));
            if (!nb)
                return -ENOSPC;
        }

        h->owner = root->objectid;

        int rc = btrfs_write_block(fs, nb, path->nodes[lvl]);

        if (rc < 0)
            return rc;
        if (!already_ours) {
            rc = extent_insert_tree_block(fs, nb, root->objectid, h->level);
            if (rc < 0)
                return rc;
            if (old) {
                rc = extent_drop_ref(fs, old, nodesize);
                if (rc < 0)
                    return rc;
            }
        }
        path->bytenr[lvl] = nb;

        if (lvl + 1 < path->levels) {
            struct btrfs_key_ptr *ptrs = node_ptrs(path->nodes[lvl + 1]);

            ptrs[path->slots[lvl + 1]].blockptr = nb;
            ptrs[path->slots[lvl + 1]].generation = fs->trans_gen;
        } else {
            *root->bytenr = nb;
            *root->level = h->level;
        }
        if (root->objectid == BTRFS_CHUNK_TREE_OBJECTID)
            fs->chunk_dirty = 1;
    }
    fs->dirty = 1;
    return 0;
}

/* ── Item insertion and removal ────────────────────────────────────────────*/

/* Make room for one item at `slot` in a leaf and copy `data` into it.
 * The caller has already checked that leaf_free_space() allows it. */
static void leaf_insert_at(struct btrfs_fs_info *fs, u8 *leaf, u32 slot,
                           const struct btrfs_disk_key *key, const void *data,
                           u32 size) {
    struct btrfs_header *h = (struct btrfs_header *)leaf;
    u32 n = h->nritems;
    u8 *ldata = leaf + sizeof(struct btrfs_header);
    u32 data_end = fs->sb.nodesize - sizeof(struct btrfs_header);

    if (n)
        data_end = btrfs_leaf_item(leaf, n - 1)->offset;

    /* Shift the data of every item at or after the slot down by `size`, so
     * the new item's bytes land immediately above them. */
    if (slot < n) {
        u32 first_off = btrfs_leaf_item(leaf, slot)->offset;
        u32 moved = first_off + leaf_item_mut(leaf, slot)->size - data_end;

        memmove(ldata + data_end - size, ldata + data_end, moved);
        for (u32 i = slot; i < n; i++)
            leaf_item_mut(leaf, i)->offset -= size;
        memmove(leaf_item_mut(leaf, slot + 1), leaf_item_mut(leaf, slot),
                (usize)(n - slot) * sizeof(struct btrfs_item));
    }

    /* Item data is packed downward from the end of the block and is dense:
     * item[i-1] starts exactly where item[i]'s data ends. The new item
     * therefore begins where the data of the item now at slot+1 ends — that
     * item is the one that used to be at `slot`, and its offset has already
     * been moved down by `size`. Subtracting `size` a second time here is
     * what made two items' data overlap, and the damage only showed up in
     * the middle of a leaf: an append lands in the other branch, which was
     * right, so btrfs check stayed happy until the first insert with a
     * neighbour above it. */
    u32 new_off = slot < n ? btrfs_leaf_item(leaf, slot + 1)->offset +
                                 btrfs_leaf_item(leaf, slot + 1)->size
                           : data_end - size;
    struct btrfs_item *it = leaf_item_mut(leaf, slot);

    it->key = *key;
    it->offset = new_off;
    it->size = size;
    if (data && size)
        memcpy(ldata + new_off, data, size);
    else if (size)
        memset(ldata + new_off, 0, size);
    h->nritems = n + 1;
}

static void leaf_delete_at(struct btrfs_fs_info *fs, u8 *leaf, u32 slot) {
    struct btrfs_header *h = (struct btrfs_header *)leaf;
    u32 n = h->nritems;

    if (slot >= n)
        return;

    u8 *ldata = leaf + sizeof(struct btrfs_header);
    u32 size = btrfs_leaf_item(leaf, slot)->size;
    u32 off = btrfs_leaf_item(leaf, slot)->offset;
    u32 data_end = btrfs_leaf_item(leaf, n - 1)->offset;

    if (off > data_end)
        memmove(ldata + data_end + size, ldata + data_end, off - data_end);
    for (u32 i = slot + 1; i < n; i++)
        leaf_item_mut(leaf, i)->offset += size;
    memmove(leaf_item_mut(leaf, slot), leaf_item_mut(leaf, slot + 1),
            (usize)(n - slot - 1) * sizeof(struct btrfs_item));
    h->nritems = n - 1;
    (void)fs;
}

static int split_leaf(struct btrfs_fs_info *fs, struct btrfs_path *path,
                      struct btrfs_root_ref *root);
static int node_insert_child(struct btrfs_fs_info *fs, struct btrfs_path *path,
                             struct btrfs_root_ref *root, int lvl,
                             const struct btrfs_disk_key *key, u64 child);

/* Insert one item into a tree, copy-on-write. */
static int btrfs_insert_item(struct btrfs_fs_info *fs,
                             struct btrfs_root_ref *root, u64 objectid,
                             u8 type, u64 key_offset, const void *data,
                             u32 size) {
    u32 need = size + sizeof(struct btrfs_item);

    for (int attempt = 0; attempt < 2; attempt++) {
        struct btrfs_path path;
        int rc = btrfs_search_path(fs, *root->bytenr, objectid, type,
                                   key_offset, &path);

        if (rc < 0)
            return rc;
        if (leaf_free_space(fs, path.nodes[0]) < need) {
            rc = split_leaf(fs, &path, root);
            path_release(&path);
            if (rc < 0)
                return rc;
            continue; /* descend again; the halves both have room */
        }

        struct btrfs_disk_key key = {objectid, type, key_offset};

        leaf_insert_at(fs, path.nodes[0], path.slots[0], &key, data, size);

        /* A new first item changes the key the parent advertises. */
        if (path.slots[0] == 0) {
            for (int lvl = 1; lvl < path.levels; lvl++) {
                node_ptrs(path.nodes[lvl])[path.slots[lvl]].key = key;
                if (path.slots[lvl] != 0)
                    break;
            }
        }
        rc = cow_path(fs, &path, root);
        path_release(&path);
        return rc;
    }
    return -ENOSPC;
}

/* Replace the data of an existing item of the same size. */
static int btrfs_update_item(struct btrfs_fs_info *fs,
                             struct btrfs_root_ref *root, u64 objectid,
                             u8 type, u64 key_offset, const void *data,
                             u32 size) {
    struct btrfs_path path;
    int rc = btrfs_search_path(fs, *root->bytenr, objectid, type, key_offset,
                               &path);

    if (rc < 0)
        return rc;

    const struct btrfs_header *h = (const struct btrfs_header *)path.nodes[0];

    if (path.slots[0] >= h->nritems ||
        btrfs_key_cmp(&btrfs_leaf_item(path.nodes[0], path.slots[0])->key,
                      objectid, type, key_offset) != 0) {
        path_release(&path);
        return -ENOENT;
    }

    struct btrfs_item *it = leaf_item_mut(path.nodes[0], path.slots[0]);

    if (it->size != size) {
        path_release(&path);
        return -EINVAL;
    }
    memcpy(item_data_mut(path.nodes[0], it), data, size);
    rc = cow_path(fs, &path, root);
    path_release(&path);
    return rc;
}

static int btrfs_delete_item(struct btrfs_fs_info *fs,
                             struct btrfs_root_ref *root, u64 objectid,
                             u8 type, u64 key_offset) {
    struct btrfs_path path;
    int rc = btrfs_search_path(fs, *root->bytenr, objectid, type, key_offset,
                               &path);

    if (rc < 0)
        return rc;

    const struct btrfs_header *h = (const struct btrfs_header *)path.nodes[0];

    if (path.slots[0] >= h->nritems ||
        btrfs_key_cmp(&btrfs_leaf_item(path.nodes[0], path.slots[0])->key,
                      objectid, type, key_offset) != 0) {
        path_release(&path);
        return -ENOENT;
    }
    leaf_delete_at(fs, path.nodes[0], path.slots[0]);

    /* An emptied leaf is left in place rather than merged away: it is a legal
     * (if wasteful) tree, and merging is where a B-tree implementation earns
     * its subtle bugs. The leaf is reused as soon as anything lands in its
     * key range. */
    if (((struct btrfs_header *)path.nodes[0])->nritems && path.slots[0] == 0) {
        struct btrfs_disk_key k = btrfs_leaf_item(path.nodes[0], 0)->key;

        for (int lvl = 1; lvl < path.levels; lvl++) {
            node_ptrs(path.nodes[lvl])[path.slots[lvl]].key = k;
            if (path.slots[lvl] != 0)
                break;
        }
    }
    rc = cow_path(fs, &path, root);
    path_release(&path);
    return rc;
}

/* Split the internal node at `lvl` in two and KEEP THE PATH POINTING AT THE
 * HALF THAT OWNS IT.
 *
 * This is the part that is easy to get subtly wrong. After the upper half of a
 * node moves into a new block, a path whose slot was in that upper half still
 * refers to the old block at an index that no longer exists there. Everything
 * downstream then edits the wrong node — and the damage is invisible from
 * inside: the driver reads its own writes back happily, while btrfs check
 * reports back-references that lead nowhere. So the path is moved across with
 * the keys. */
static int split_node(struct btrfs_fs_info *fs, struct btrfs_path *path,
                      struct btrfs_root_ref *root, int lvl) {
    u32 nodesize = fs->sb.nodesize;
    u8 *node = path->nodes[lvl];
    struct btrfs_header *h = (struct btrfs_header *)node;

    if (h->nritems < 2)
        return -ENOSPC;

    u8 *right = kzalloc(nodesize);

    if (!right)
        return -ENOMEM;

    struct btrfs_header *rh = (struct btrfs_header *)right;
    u32 mid = h->nritems / 2;

    rh->level = h->level;
    rh->owner = h->owner;
    memcpy(rh->chunk_tree_uuid, h->chunk_tree_uuid, sizeof(rh->chunk_tree_uuid));
    rh->nritems = h->nritems - mid;
    memcpy(node_ptrs(right), node_ptrs(node) + mid,
           (usize)rh->nritems * sizeof(struct btrfs_key_ptr));
    h->nritems = mid;

    u64 right_bytenr = btrfs_alloc_bytes(fs, nodesize,
                                         tree_block_group(root->objectid));

    if (!right_bytenr) {
        kfree(right);
        return -ENOSPC;
    }

    struct btrfs_disk_key right_key = node_ptrs(right)[0].key;
    int rc = btrfs_write_block(fs, right_bytenr, right);

    if (rc == 0)
        rc = extent_insert_tree_block(fs, right_bytenr, root->objectid,
                                      rh->level);
    if (rc < 0) {
        kfree(right);
        return rc;
    }

    /* Publish the new half one level up, which may split that level too. */
    rc = node_insert_child(fs, path, root, lvl + 1, &right_key, right_bytenr);
    if (rc < 0) {
        kfree(right);
        return rc;
    }

    if (path->slots[lvl] >= mid) {
        /* The path belongs to the upper half. Take the new block into the
         * path, and leave the old one written where it stands: it is still a
         * live node, just a shorter one, and cow_path no longer walks it. */
        rc = btrfs_write_block(fs, path->bytenr[lvl], node);
        if (rc < 0) {
            kfree(right);
            return rc;
        }
        kfree(path->nodes[lvl]);
        path->nodes[lvl] = right;
        path->bytenr[lvl] = right_bytenr;
        path->slots[lvl] -= mid;
        /* The parent slot the path uses must be the one naming the right
         * half, which node_insert_child put immediately after the left. */
        path->slots[lvl + 1]++;
    } else {
        kfree(right);
    }
    return 0;
}

/* Add one child to an internal node, splitting upward if it is full and
 * growing a new root when the split reaches the top. `path` is updated so it
 * still describes the way down to the leaf the caller is working on. */
static int node_insert_child(struct btrfs_fs_info *fs, struct btrfs_path *path,
                             struct btrfs_root_ref *root, int lvl,
                             const struct btrfs_disk_key *key, u64 child) {
    u32 nodesize = fs->sb.nodesize;

    if (lvl >= path->levels) {
        /* Above the old root: a new root with two children. */
        u8 *newroot = kzalloc(nodesize);

        if (!newroot)
            return -ENOMEM;

        struct btrfs_header *rh = (struct btrfs_header *)newroot;
        struct btrfs_header *oh =
            (struct btrfs_header *)path->nodes[path->levels - 1];

        rh->level = oh->level + 1;
        rh->nritems = 2;
        rh->owner = root->objectid;
        memcpy(rh->chunk_tree_uuid, oh->chunk_tree_uuid,
               sizeof(rh->chunk_tree_uuid));

        struct btrfs_key_ptr *p = node_ptrs(newroot);

        p[0].key = oh->level == 0
                       ? btrfs_leaf_item(path->nodes[path->levels - 1], 0)->key
                       : node_ptrs(path->nodes[path->levels - 1])[0].key;
        p[0].blockptr = path->bytenr[path->levels - 1];
        p[0].generation = fs->trans_gen;
        p[1].key = *key;
        p[1].blockptr = child;
        p[1].generation = fs->trans_gen;

        path->nodes[path->levels] = newroot;
        path->bytenr[path->levels] = 0; /* allocated by cow_path */
        path->slots[path->levels] = 0;  /* the path runs through the old root */
        path->levels++;
        return 0;
    }

    if (node_free_slots(fs, path->nodes[lvl]) == 0) {
        int rc = split_node(fs, path, root, lvl);

        if (rc < 0)
            return rc;
        /* After the split the path may have moved to the other half, and the
         * slot to insert after is the path's own slot there. */
    }

    u8 *node = path->nodes[lvl];
    struct btrfs_header *h = (struct btrfs_header *)node;
    u32 slot = path->slots[lvl] + 1;
    struct btrfs_key_ptr *p = node_ptrs(node);

    memmove(&p[slot + 1], &p[slot],
            (usize)(h->nritems - slot) * sizeof(struct btrfs_key_ptr));
    p[slot].key = *key;
    p[slot].blockptr = child;
    p[slot].generation = fs->trans_gen;
    h->nritems++;
    return 0;
}

/* Split the leaf the path ends on, publish both halves, and commit the path.
 *
 * The caller retries its insert from the root afterwards rather than being
 * handed a half-updated path: after the split the tree on disk is consistent,
 * the second descent lands in whichever half owns the key, and there is no
 * case where a freshly created root has to be taught about a leaf the path
 * does not run through. One retry always suffices — a leaf that has just been
 * halved has room. */
static int split_leaf(struct btrfs_fs_info *fs, struct btrfs_path *path,
                      struct btrfs_root_ref *root) {
    u32 nodesize = fs->sb.nodesize;
    u8 *left = path->nodes[0];
    struct btrfs_header *lh = (struct btrfs_header *)left;

    if (lh->nritems < 2)
        return -ENOSPC; /* a single item already fills the block */

    u8 *right = kzalloc(nodesize);

    if (!right)
        return -ENOMEM;

    struct btrfs_header *rh = (struct btrfs_header *)right;

    rh->level = 0;
    rh->owner = lh->owner;
    memcpy(rh->chunk_tree_uuid, lh->chunk_tree_uuid,
           sizeof(rh->chunk_tree_uuid));

    u32 mid = lh->nritems / 2;

    for (u32 i = mid; i < lh->nritems; i++) {
        const struct btrfs_item *it = btrfs_leaf_item(left, i);

        leaf_insert_at(fs, right, i - mid, &it->key, btrfs_item_data(left, it),
                       it->size);
    }
    for (u32 i = lh->nritems; i > mid; i--)
        leaf_delete_at(fs, left, i - 1);

    u64 right_bytenr =
        btrfs_alloc_bytes(fs, nodesize, tree_block_group(root->objectid));

    if (!right_bytenr) {
        kfree(right);
        return -ENOSPC;
    }

    struct btrfs_disk_key right_key = btrfs_leaf_item(right, 0)->key;
    int rc = btrfs_write_block(fs, right_bytenr, right);

    if (rc == 0)
        rc = extent_insert_tree_block(fs, right_bytenr, root->objectid, 0);
    kfree(right);
    if (rc < 0)
        return rc;

    rc = node_insert_child(fs, path, root, 1, &right_key, right_bytenr);
    if (rc < 0)
        return rc;
    return cow_path(fs, path, root);
}



/* ── The extent tree ───────────────────────────────────────────────────────*/

/* Every allocation btrfs makes is recorded here, with a back-reference saying
 * who owns it. btrfs check reads this tree and the trees that point into it
 * and insists the two agree, so a driver that allocates without recording —
 * or records without a backref — produces a filesystem that mounts and then
 * fails its first check. */

static struct btrfs_root_ref extent_root_ref(struct btrfs_fs_info *fs) {
    struct btrfs_root_ref r = {BTRFS_EXTENT_TREE_OBJECTID,
                               &fs->extent_root_bytenr, &fs->extent_root_level};

    return r;
}

static struct btrfs_root_ref csum_root_ref(struct btrfs_fs_info *fs) {
    struct btrfs_root_ref r = {BTRFS_CSUM_TREE_OBJECTID, &fs->csum_root_bytenr,
                               &fs->csum_root_level};

    return r;
}

static int fs_skinny(struct btrfs_fs_info *fs) {
    return (fs->sb.incompat_flags &
            BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA) != 0;
}

/* A tree block's own extent record: refs=1, owned by one tree. */
static int extent_insert_tree_block_now(struct btrfs_fs_info *fs, u64 bytenr,
                                        u64 owner, u8 level) {
    struct btrfs_root_ref er = extent_root_ref(fs);
    u8 buf[sizeof(struct btrfs_extent_item) + 33 + 9];
    struct btrfs_extent_item *ei = (struct btrfs_extent_item *)buf;
    u32 size = sizeof(*ei);

    memset(buf, 0, sizeof(buf));
    ei->refs = 1;
    ei->generation = fs->trans_gen;
    ei->flags = BTRFS_EXTENT_FLAG_TREE_BLOCK;

    if (!fs_skinny(fs)) {
        /* Older layout: the block's first key and its level ride along in a
         * btrfs_tree_block_info before the backrefs. */
        memset(buf + size, 0, 33);
        buf[size + 32] = level;
        size += 33;
    }
    buf[size] = BTRFS_TREE_BLOCK_REF_KEY;
    memcpy(buf + size + 1, &owner, sizeof(owner));
    size += 9;

    if (fs_skinny(fs))
        return btrfs_insert_item(fs, &er, bytenr, BTRFS_METADATA_ITEM_KEY,
                                 level, buf, size);
    return btrfs_insert_item(fs, &er, bytenr, BTRFS_EXTENT_ITEM_KEY,
                             fs->sb.nodesize, buf, size);
}

/* A data extent, referenced by one file range in one tree. */
static int extent_insert_data_now(struct btrfs_fs_info *fs, u64 bytenr,
                                  u64 len, u64 root, u64 objectid,
                                  u64 file_offset) {
    struct btrfs_root_ref er = extent_root_ref(fs);
    u8 buf[sizeof(struct btrfs_extent_item) + 1 +
           sizeof(struct btrfs_extent_data_ref)];
    struct btrfs_extent_item *ei = (struct btrfs_extent_item *)buf;
    u32 size = sizeof(*ei);

    memset(buf, 0, sizeof(buf));
    ei->refs = 1;
    ei->generation = fs->trans_gen;
    ei->flags = BTRFS_EXTENT_FLAG_DATA;

    buf[size++] = BTRFS_EXTENT_DATA_REF_KEY;

    struct btrfs_extent_data_ref dref;

    memset(&dref, 0, sizeof(dref));
    dref.root = root;
    dref.objectid = objectid;
    dref.offset = file_offset;
    dref.count = 1;
    memcpy(buf + size, &dref, sizeof(dref));
    size += sizeof(dref);

    return btrfs_insert_item(fs, &er, bytenr, BTRFS_EXTENT_ITEM_KEY, len, buf,
                             size);
}

/* Find the extent item covering `bytenr`, whatever its layout. */
static int extent_find(struct btrfs_fs_info *fs, u64 bytenr, u8 *type_out,
                       u64 *keyoff_out, u64 *refs_out) {
    struct btrfs_path path;
    int rc = btrfs_search_path(fs, fs->extent_root_bytenr, bytenr, 0, 0,
                               &path);

    if (rc < 0)
        return rc;

    const struct btrfs_header *h = (const struct btrfs_header *)path.nodes[0];
    int found = -ENOENT;

    for (u32 i = path.slots[0]; i < h->nritems; i++) {
        const struct btrfs_item *it = btrfs_leaf_item(path.nodes[0], i);

        if (it->key.objectid != bytenr)
            break;
        if (it->key.type != BTRFS_EXTENT_ITEM_KEY &&
            it->key.type != BTRFS_METADATA_ITEM_KEY)
            continue;

        const struct btrfs_extent_item *ei =
            (const struct btrfs_extent_item *)btrfs_item_data(path.nodes[0],
                                                              it);

        *type_out = it->key.type;
        *keyoff_out = it->key.offset;
        *refs_out = ei->refs;
        found = 0;
        break;
    }
    path_release(&path);
    return found;
}

/* Drop one reference. The last one takes the extent record with it and
 * returns the space to the allocator. */
static int extent_drop_ref_now(struct btrfs_fs_info *fs, u64 bytenr, u64 len) {
    struct btrfs_root_ref er = extent_root_ref(fs);
    u8 type;
    u64 keyoff, refs;
    int rc = extent_find(fs, bytenr, &type, &keyoff, &refs);

    if (rc < 0)
        return rc == -ENOENT ? 0 : rc;
    if (refs > 1) {
        /* Shared with something this driver did not create — a snapshot or a
         * reflink. It has no way to maintain the other side's accounting, so
         * it refuses rather than corrupting it. */
        return -EOPNOTSUPP;
    }
    rc = btrfs_delete_item(fs, &er, bytenr, type, keyoff);
    if (rc < 0)
        return rc;
    btrfs_free_bytes(fs, bytenr, len);
    return 0;
}

/* ── Checksums ─────────────────────────────────────────────────────────────*/

/* One crc32c per sector, stored in the csum tree keyed by disk address. A
 * data extent without them reads back as corrupt on Linux, so they are part
 * of writing data, not an optional extra. */
static int csum_insert_range(struct btrfs_fs_info *fs, u64 bytenr,
                             const u8 *data, u64 len) {
    struct btrfs_root_ref cr = csum_root_ref(fs);
    u32 sector = fs->sb.sectorsize;
    u32 nsectors = (u32)(len / sector);
    u32 *sums = kmalloc((usize)nsectors * sizeof(u32));

    if (!sums)
        return -ENOMEM;
    for (u32 i = 0; i < nsectors; i++)
        sums[i] = btrfs_crc32c(data + (usize)i * sector, sector);

    int rc = btrfs_insert_item(fs, &cr, BTRFS_EXTENT_CSUM_OBJECTID,
                               BTRFS_EXTENT_CSUM_KEY, bytenr, sums,
                               nsectors * sizeof(u32));

    kfree(sums);
    return rc;
}

/* Remove the checksums covering [bytenr, bytenr+len).
 *
 * The item that holds them may cover much more than this range — mkfs writes
 * one item per contiguous run of data — so the surviving head and tail are
 * re-inserted as items of their own. Anything else would either leave
 * checksums for freed space (which btrfs check reports) or drop checksums
 * that still belong to live data. */
static int csum_remove_range(struct btrfs_fs_info *fs, u64 bytenr, u64 len) {
    struct btrfs_root_ref cr = csum_root_ref(fs);
    u32 sector = fs->sb.sectorsize;
    u64 end = bytenr + len;

    while (1) {
        struct btrfs_path path;
        int rc = btrfs_search_path(fs, fs->csum_root_bytenr,
                                   BTRFS_EXTENT_CSUM_OBJECTID,
                                   BTRFS_EXTENT_CSUM_KEY, bytenr, &path);

        if (rc < 0)
            return rc;

        const struct btrfs_header *h =
            (const struct btrfs_header *)path.nodes[0];
        u32 slot = path.slots[0];

        /* The item before the search result may still reach into the range. */
        if (slot > 0) {
            const struct btrfs_item *prev =
                btrfs_leaf_item(path.nodes[0], slot - 1);

            if (prev->key.objectid == BTRFS_EXTENT_CSUM_OBJECTID &&
                prev->key.type == BTRFS_EXTENT_CSUM_KEY &&
                prev->key.offset +
                        (u64)(prev->size / sizeof(u32)) * sector > bytenr)
                slot--;
        }
        if (slot >= h->nritems) {
            path_release(&path);
            return 0;
        }

        const struct btrfs_item *it = btrfs_leaf_item(path.nodes[0], slot);

        if (it->key.objectid != BTRFS_EXTENT_CSUM_OBJECTID ||
            it->key.type != BTRFS_EXTENT_CSUM_KEY || it->key.offset >= end) {
            path_release(&path);
            return 0;
        }

        u64 item_start = it->key.offset;
        u32 item_sectors = it->size / sizeof(u32);
        u64 item_end = item_start + (u64)item_sectors * sector;

        if (item_end <= bytenr) {
            path_release(&path);
            return 0;
        }

        /* Copy the parts that survive before the item goes away. */
        u32 head = item_start < bytenr
                       ? (u32)((bytenr - item_start) / sector)
                       : 0;
        u32 tail = item_end > end ? (u32)((item_end - end) / sector) : 0;
        u32 *headbuf = 0, *tailbuf = 0;
        const u32 *src =
            (const u32 *)btrfs_item_data(path.nodes[0], it);

        if (head) {
            headbuf = kmalloc((usize)head * sizeof(u32));
            if (!headbuf) {
                path_release(&path);
                return -ENOMEM;
            }
            memcpy(headbuf, src, (usize)head * sizeof(u32));
        }
        if (tail) {
            tailbuf = kmalloc((usize)tail * sizeof(u32));
            if (!tailbuf) {
                if (headbuf)
                    kfree(headbuf);
                path_release(&path);
                return -ENOMEM;
            }
            memcpy(tailbuf, src + (item_sectors - tail),
                   (usize)tail * sizeof(u32));
        }
        path_release(&path);

        rc = btrfs_delete_item(fs, &cr, BTRFS_EXTENT_CSUM_OBJECTID,
                               BTRFS_EXTENT_CSUM_KEY, item_start);
        if (rc == 0 && head)
            rc = btrfs_insert_item(fs, &cr, BTRFS_EXTENT_CSUM_OBJECTID,
                                   BTRFS_EXTENT_CSUM_KEY, item_start, headbuf,
                                   head * sizeof(u32));
        if (rc == 0 && tail)
            rc = btrfs_insert_item(fs, &cr, BTRFS_EXTENT_CSUM_OBJECTID,
                                   BTRFS_EXTENT_CSUM_KEY, end, tailbuf,
                                   tail * sizeof(u32));
        if (headbuf)
            kfree(headbuf);
        if (tailbuf)
            kfree(tailbuf);
        if (rc < 0)
            return rc;

        if (item_end >= end)
            return 0;
        bytenr = item_end; /* the range spans more than one item */
    }
}

/* ── Files ─────────────────────────────────────────────────────────────────*/

static struct btrfs_root_ref fs_root_ref(struct btrfs_fs_info *fs) {
    struct btrfs_root_ref r = {BTRFS_FS_TREE_OBJECTID, &fs->fs_root_bytenr,
                               &fs->fs_root_level};

    return r;
}

static int inode_load(struct btrfs_fs_info *fs, u64 objectid,
                      struct btrfs_inode_item *out) {
    struct btrfs_path path;
    int rc = btrfs_search_path(fs, fs->fs_root_bytenr, objectid,
                               BTRFS_INODE_ITEM_KEY, 0, &path);

    if (rc < 0)
        return rc;

    const struct btrfs_header *h = (const struct btrfs_header *)path.nodes[0];

    if (path.slots[0] >= h->nritems) {
        path_release(&path);
        return -ENOENT;
    }

    const struct btrfs_item *it = btrfs_leaf_item(path.nodes[0], path.slots[0]);

    if (btrfs_key_cmp(&it->key, objectid, BTRFS_INODE_ITEM_KEY, 0) != 0 ||
        it->size < sizeof(*out)) {
        path_release(&path);
        return -ENOENT;
    }
    memcpy(out, btrfs_item_data(path.nodes[0], it), sizeof(*out));
    path_release(&path);
    return 0;
}

/* Read data straight off the medium, by logical address. Used when a piece of
 * an extent survives an overwrite and has to be carried into a new one. */
static int data_read_raw(struct btrfs_fs_info *fs, u64 bytenr, void *buf,
                         u64 len) {
    u64 physical = btrfs_map(fs, bytenr);

    if (!physical || (len % 512))
        return -EIO;
    if (blk_read_cached(fs->bdev, physical / 512, (u32)(len / 512), buf) < 0)
        return -EIO;
    return 0;
}

/* Copy the part of an extent that an overwrite does not touch into an extent
 * of its own.
 *
 * btrfs would instead leave both halves pointing into the SAME allocation and
 * raise its reference count — cheaper, and the reason its extents carry an
 * offset at all. This driver copies, because sharing an allocation between
 * two file ranges means maintaining shared back-references, and a wrong
 * refcount is the kind of damage that only shows up much later, on somebody
 * else's kernel. The cost is one read and one write of the surviving bytes;
 * the gain is that every extent this driver writes has exactly one owner. */
static int extent_copy_piece(struct btrfs_fs_info *fs, u64 objectid,
                             u64 src_bytenr, u64 file_off, u64 len) {
    struct btrfs_root_ref fr = fs_root_ref(fs);
    u8 *buf = kmalloc(len);

    if (!buf)
        return -ENOMEM;

    int rc = data_read_raw(fs, src_bytenr, buf, len);

    if (rc < 0) {
        kfree(buf);
        return rc;
    }

    u64 nb = btrfs_alloc_bytes(fs, len, BTRFS_BLOCK_GROUP_DATA);

    if (!nb) {
        kfree(buf);
        return -ENOSPC;
    }
    if (fs->nbgs && !range_kind_ok(fs, nb, len, 0)) {
        klog_info("btrfs: copied extent piece is not in a data group");
        kfree(buf);
        btrfs_free_bytes(fs, nb, len);
        return -EIO;
    }
    rc = btrfs_write_raw(fs, nb, buf, (u32)len);
    if (rc == 0)
        rc = csum_insert_range(fs, nb, buf, len);
    kfree(buf);
    if (rc == 0)
        rc = extent_insert_data(fs, nb, len, BTRFS_FS_TREE_OBJECTID, objectid,
                                file_off);
    if (rc < 0) {
        btrfs_free_bytes(fs, nb, len);
        return rc;
    }

    struct btrfs_file_extent_item fe;

    memset(&fe, 0, sizeof(fe));
    fe.generation = fs->trans_gen;
    fe.ram_bytes = len;
    fe.type = BTRFS_FILE_EXTENT_REG;
    fe.disk_bytenr = nb;
    fe.disk_num_bytes = len;
    fe.offset = 0;
    fe.num_bytes = len;
    return btrfs_insert_item(fs, &fr, objectid, BTRFS_EXTENT_DATA_KEY,
                             file_off, &fe, sizeof(fe));
}

/* Every EXTENT_DATA item of one file that overlaps [start, end), removed with
 * the extents and checksums they own. Inline extents are dropped whole: an
 * inline extent covers the entire file, so a write anywhere in it replaces it
 * with a regular extent. */
static int file_drop_range(struct btrfs_fs_info *fs, u64 objectid, u64 start,
                           u64 end, u64 *freed_bytes) {
    struct btrfs_root_ref fr = fs_root_ref(fs);

    while (1) {
        struct btrfs_path path;
        int rc = btrfs_search_path(fs, fs->fs_root_bytenr, objectid,
                                   BTRFS_EXTENT_DATA_KEY, start, &path);

        if (rc < 0)
            return rc;

        const struct btrfs_header *h =
            (const struct btrfs_header *)path.nodes[0];
        u32 slot = path.slots[0];

        /* An extent starting before `start` can still reach into the range. */
        if (slot > 0) {
            const struct btrfs_item *prev =
                btrfs_leaf_item(path.nodes[0], slot - 1);

            if (prev->key.objectid == objectid &&
                prev->key.type == BTRFS_EXTENT_DATA_KEY)
                slot--;
        }

        u64 hit_off = 0, disk_bytenr = 0, disk_len = 0;
        u64 ram_bytes = 0;
        u64 head_len = 0, tail_len = 0;
        int hit = 0, inline_ext = 0;

        for (u32 i = slot; i < h->nritems; i++) {
            const struct btrfs_item *it = btrfs_leaf_item(path.nodes[0], i);

            if (it->key.objectid != objectid ||
                it->key.type != BTRFS_EXTENT_DATA_KEY)
                break;

            const struct btrfs_file_extent_item *fe =
                (const struct btrfs_file_extent_item *)btrfs_item_data(
                    path.nodes[0], it);
            u64 ext_start = it->key.offset;
            u64 ext_len = fe->type == BTRFS_FILE_EXTENT_INLINE
                              ? fe->ram_bytes
                              : fe->num_bytes;

            if (ext_start + ext_len <= start)
                continue;
            if (ext_start >= end)
                break;

            hit = 1;
            hit_off = ext_start;
            ram_bytes = ext_len;
            if (fe->type == BTRFS_FILE_EXTENT_INLINE) {
                inline_ext = 1;
            } else {
                if (fe->compression || fe->encryption) {
                    path_release(&path);
                    return -EOPNOTSUPP;
                }
                /* An extent that only partly overlaps the range keeps its
                 * untouched head and tail, carried into extents of their own
                 * by extent_copy_piece below. */
                if (fe->offset) {
                    /* A slice of a larger allocation: only something that
                     * shares extents makes those, and this driver does not. */
                    path_release(&path);
                    return -EOPNOTSUPP;
                }
                disk_bytenr = fe->disk_bytenr;
                disk_len = fe->disk_num_bytes;
                if (ext_start < start)
                    head_len = start - ext_start;
                if (ext_start + ext_len > end)
                    tail_len = ext_start + ext_len - end;
            }
            break;
        }
        path_release(&path);
        if (!hit)
            return 0;

        rc = btrfs_delete_item(fs, &fr, objectid, BTRFS_EXTENT_DATA_KEY,
                               hit_off);
        if (rc < 0)
            return rc;
        if (!inline_ext && disk_bytenr) {
            /* Save the surviving pieces before the allocation goes away: the
             * copies read from it, and extent_drop_ref hands it back to the
             * allocator, which may hand it straight out again. */
            if (head_len)
                rc = extent_copy_piece(fs, objectid, disk_bytenr, hit_off,
                                       head_len);
            if (rc == 0 && tail_len)
                rc = extent_copy_piece(fs, objectid,
                                       disk_bytenr + (disk_len - tail_len),
                                       hit_off + ram_bytes - tail_len,
                                       tail_len);
            if (rc < 0)
                return rc;
            rc = csum_remove_range(fs, disk_bytenr, disk_len);
            if (rc == 0)
                rc = extent_drop_ref(fs, disk_bytenr, disk_len);
            if (rc < 0)
                return rc;
            if (freed_bytes)
                *freed_bytes += disk_len - head_len - tail_len;
        }
        if (inline_ext)
            return 0; /* an inline extent is the whole file */
        start = hit_off + ram_bytes;
        if (start >= end)
            return 0;
    }
}

/* Make sure there is room BEFORE the operation starts.
 *
 * Growing the filesystem writes a chunk item, a device extent and a block
 * group item — three tree insertions. Doing that from inside the allocator,
 * which is called from the middle of a copy-on-write that is already editing
 * one of those trees, re-enters the tree with a live path over it and leaves
 * the result unreadable: btrfs check could not find a single block group. So
 * the check happens up front, where nothing is half-modified. The margin
 * covers the metadata one operation can need — a split at every level of
 * every tree it touches — rather than being an exact figure. */
static int btrfs_reserve(struct btrfs_fs_info *fs, u64 bg_type, u64 need) {
    u64 margin = bg_type == BTRFS_BLOCK_GROUP_METADATA
                     ? (u64)fs->sb.nodesize * 64
                     : 0;

    if (!fs->rw)
        return -EROFS;
    if (btrfs_free_space_in(fs, bg_type) >= need + margin)
        return 0;
    if (fs->growing)
        return -ENOSPC;

    fs->growing = 1;

    int rc = btrfs_alloc_chunk(fs, bg_type, need + margin);

    fs->growing = 0;
    if (rc < 0)
        return rc;
    /* Recording the new chunk allocated metadata of its own; make sure the
     * caller's own need still fits. */
    return btrfs_free_space_in(fs, bg_type) >= need ? 0 : -ENOSPC;
}

/* Both kinds, for an operation that writes data and the metadata to describe
 * it. */
static int btrfs_reserve_both(struct btrfs_fs_info *fs, u64 data_bytes) {
    int rc = btrfs_reserve(fs, BTRFS_BLOCK_GROUP_METADATA, 0);

    if (rc == 0 && data_bytes)
        rc = btrfs_reserve(fs, BTRFS_BLOCK_GROUP_DATA, data_bytes);
    return rc;
}

isize btrfs_file_write(struct btrfs_fs_info *fs, u64 objectid, u64 offset,
                       const void *buf, usize len, u64 *new_size_out) {
    if (!fs->rw)
        return -EROFS;

    u32 sector = fs->sb.sectorsize;

    if (!sector || (offset % sector))
        return -EINVAL;
    if (!len)
        return 0;

    struct btrfs_inode_item inode;
    int rc = inode_load(fs, objectid, &inode);

    if (rc < 0)
        return rc;

    u64 disk_len = ((u64)len + sector - 1) & ~((u64)sector - 1);

    rc = btrfs_reserve_both(fs, disk_len);
    if (rc < 0)
        return rc;
    u64 freed = 0;

    rc = file_drop_range(fs, objectid, offset, offset + disk_len, &freed);
    if (rc < 0)
        return rc;

    u64 bytenr = btrfs_alloc_bytes(fs, disk_len, BTRFS_BLOCK_GROUP_DATA);

    if (!bytenr)
        return -ENOSPC;

    /* The tail of the last sector is written as zeroes rather than left as
     * whatever the disk held: the checksum covers the whole sector, so its
     * contents have to be known. */
    u8 *page = kzalloc(disk_len);

    if (!page) {
        btrfs_free_bytes(fs, bytenr, disk_len);
        return -ENOMEM;
    }
    memcpy(page, buf, len);

    if (fs->nbgs && !range_kind_ok(fs, bytenr, disk_len, 0)) {
        char line[128];

        snprintf(line, sizeof(line),
                 "btrfs: file data at %llu is not in a data group",
                 (unsigned long long)bytenr);
        klog_info(line);
        kfree(page);
        btrfs_free_bytes(fs, bytenr, disk_len);
        return -EIO;
    }
    rc = btrfs_write_raw(fs, bytenr, page, (u32)disk_len);
    if (rc == 0)
        rc = csum_insert_range(fs, bytenr, page, disk_len);
    kfree(page);
    if (rc == 0)
        rc = extent_insert_data(fs, bytenr, disk_len, BTRFS_FS_TREE_OBJECTID,
                                objectid, offset);
    if (rc < 0) {
        btrfs_free_bytes(fs, bytenr, disk_len);
        return rc;
    }

    struct btrfs_file_extent_item fe;

    memset(&fe, 0, sizeof(fe));
    fe.generation = fs->trans_gen;
    fe.ram_bytes = disk_len;
    fe.type = BTRFS_FILE_EXTENT_REG;
    fe.disk_bytenr = bytenr;
    fe.disk_num_bytes = disk_len;
    fe.offset = 0;
    fe.num_bytes = disk_len;

    struct btrfs_root_ref fr = fs_root_ref(fs);

    rc = btrfs_insert_item(fs, &fr, objectid, BTRFS_EXTENT_DATA_KEY, offset,
                           &fe, sizeof(fe));
    if (rc < 0)
        return rc;

    u64 end = offset + len;

    if (end > inode.size)
        inode.size = end;
    inode.nbytes = inode.nbytes + disk_len >= freed
                       ? inode.nbytes + disk_len - freed
                       : 0;
    inode.generation = fs->trans_gen;
    inode.transid = fs->trans_gen;
    inode.sequence++;

    u64 now = rtc_now_unix_seconds();

    inode.mtime.sec = now;
    inode.mtime.nsec = 0;
    inode.ctime = inode.mtime;

    rc = btrfs_update_item(fs, &fr, objectid, BTRFS_INODE_ITEM_KEY, 0, &inode,
                           sizeof(inode));
    if (rc == 0)
        rc = btrfs_run_delayed_refs(fs);
    if (rc < 0)
        return rc;
    if (new_size_out)
        *new_size_out = inode.size;
    return (isize)len;
}

/* ── Bringing a writable mount up ──────────────────────────────────────────*/

/* Walk every leaf of a tree, in key order. The extent tree is read whole at
 * mount because the allocator needs to know what is taken, and this driver
 * refuses the free-space tree that would otherwise answer that question. */
static int walk_tree(struct btrfs_fs_info *fs, u64 bytenr, btrfs_leaf_fn fn,
                     void *arg, int depth) {
    if (depth > BTRFS_MAX_LEVEL)
        return -EIO;

    u8 *buf = btrfs_read_block(fs, bytenr);

    if (!buf)
        return -EIO;

    const struct btrfs_header *h = (const struct btrfs_header *)buf;
    int rc = 0;

    if (h->level == 0) {
        rc = fn ? fn(fs, buf, arg) : 0;
        kfree(buf);
        return rc;
    }

    u32 n = h->nritems;
    u64 *children = kmalloc((usize)n * sizeof(u64));

    if (!children) {
        kfree(buf);
        return -ENOMEM;
    }

    const struct btrfs_key_ptr *ptrs =
        (const struct btrfs_key_ptr *)(buf + sizeof(struct btrfs_header));

    for (u32 i = 0; i < n; i++)
        children[i] = ptrs[i].blockptr;
    kfree(buf);

    for (u32 i = 0; i < n && rc == 0; i++)
        rc = walk_tree(fs, children[i], fn, arg, depth + 1);
    kfree(children);
    return rc;
}

static int collect_extents(struct btrfs_fs_info *fs, const u8 *leaf,
                           void *arg) {
    const struct btrfs_header *h = (const struct btrfs_header *)leaf;

    (void)arg;
    for (u32 i = 0; i < h->nritems; i++) {
        const struct btrfs_item *it = btrfs_leaf_item(leaf, i);
        u64 len;

        switch (it->key.type) {
        case BTRFS_EXTENT_ITEM_KEY:
            len = it->key.offset;
            break;
        case BTRFS_METADATA_ITEM_KEY:
            len = fs->sb.nodesize;
            break;
        case BTRFS_BLOCK_GROUP_ITEM_KEY: {
            const struct btrfs_block_group_item *bgi =
                (const struct btrfs_block_group_item *)btrfs_item_data(leaf,
                                                                       it);

            if (fs->nbgs >= BTRFS_MAX_BLOCK_GROUPS)
                return -ENOSPC;
            fs->bgs[fs->nbgs].start = it->key.objectid;
            fs->bgs[fs->nbgs].len = it->key.offset;
            fs->bgs[fs->nbgs].flags = bgi->flags;
            fs->bgs[fs->nbgs].used = bgi->used;
            fs->nbgs++;
            continue;
        }
        default:
            continue;
        }
        if (allocs_add(fs, it->key.objectid, len) < 0)
            return -ENOSPC;
    }
    return 0;
}

/* The address of one tree, from the root tree. */
static int find_root(struct btrfs_fs_info *fs, u64 objectid, u64 *bytenr_out,
                     u8 *level_out, u64 *dirid_out) {
    struct btrfs_path path;
    int rc = btrfs_search_path(fs, fs->sb.root, objectid,
                               BTRFS_ROOT_ITEM_KEY, 0, &path);

    if (rc < 0)
        return rc;

    const struct btrfs_header *h = (const struct btrfs_header *)path.nodes[0];
    int found = -ENOENT;

    for (u32 i = path.slots[0]; i < h->nritems; i++) {
        const struct btrfs_item *it = btrfs_leaf_item(path.nodes[0], i);

        if (it->key.objectid != objectid)
            break;
        if (it->key.type != BTRFS_ROOT_ITEM_KEY)
            continue;

        const struct btrfs_root_item *ri =
            (const struct btrfs_root_item *)btrfs_item_data(path.nodes[0], it);

        *bytenr_out = ri->bytenr;
        *level_out = ri->level;
        if (dirid_out)
            *dirid_out = ri->root_dirid;
        found = 0;
        break;
    }
    path_release(&path);
    return found;
}

/* Refuse a writable mount whose on-disk features this driver cannot keep
 * correct. Each of these has consequences for what a write must update; a
 * mount that ignored one would produce a filesystem that passes its own
 * reads and fails btrfs check. */
static int btrfs_check_rw_features(struct btrfs_fs_info *fs) {
    u64 known_incompat =
        BTRFS_FEATURE_INCOMPAT_MIXED_BACKREF |
        BTRFS_FEATURE_INCOMPAT_DEFAULT_SUBVOL |
        BTRFS_FEATURE_INCOMPAT_BIG_METADATA |
        BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF |
        BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA |
        BTRFS_FEATURE_INCOMPAT_NO_HOLES;

    if (fs->sb.incompat_flags & ~known_incompat) {
        klog_info("btrfs: unsupported incompat features; mounting read-only");
        return -EOPNOTSUPP;
    }
    if (fs->sb.compat_ro_flags & BTRFS_FEATURE_COMPAT_RO_VERITY) {
        klog_info("btrfs: fs-verity present; mounting read-only");
        return -EOPNOTSUPP;
    }
    if (fs->sb.log_root) {
        klog_info("btrfs: unreplayed log tree; mounting read-only");
        return -EOPNOTSUPP;
    }
    if (fs->sb.sectorsize != 4096) {
        klog_info("btrfs: sector size is not 4096; mounting read-only");
        return -EOPNOTSUPP;
    }
    return 0;
}

int btrfs_rw_setup(struct btrfs_fs_info *fs) {
    int rc = btrfs_check_rw_features(fs);

    if (rc < 0)
        return rc;

    u64 dirid = 0;

    rc = find_root(fs, BTRFS_EXTENT_TREE_OBJECTID, &fs->extent_root_bytenr,
                   &fs->extent_root_level, 0);
    if (rc == 0)
        rc = find_root(fs, BTRFS_CSUM_TREE_OBJECTID, &fs->csum_root_bytenr,
                       &fs->csum_root_level, 0);
    if (rc == 0)
        rc = find_root(fs, BTRFS_FS_TREE_OBJECTID, &fs->fs_root_bytenr,
                       &fs->fs_root_level, &dirid);
    if (rc == 0 &&
        (fs->sb.compat_ro_flags & BTRFS_FEATURE_COMPAT_RO_BLOCK_GROUP_TREE)) {
        if (find_root(fs, BTRFS_BLOCK_GROUP_TREE_OBJECTID, &fs->bgt_root_bytenr,
                      &fs->bgt_root_level, 0) == 0) {
            fs->has_bgt = 1;
        } else {
            klog_info("btrfs: block-group tree root missing; mounting read-only");
            rc = -EOPNOTSUPP;
        }
    }
    if (rc == 0 &&
        (fs->sb.compat_ro_flags & BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE)) {
        /* The filesystem keeps a free-space tree, so this driver has to keep
         * it right. Without its root there is nothing to update and the
         * mount stays read-only rather than leaving the next kernel a map
         * that points at live data. */
        if (find_root(fs, BTRFS_FREE_SPACE_TREE_OBJECTID, &fs->fst_root_bytenr,
                      &fs->fst_root_level, 0) == 0) {
            fs->has_fst = 1;
        } else {
            klog_info("btrfs: free-space tree root missing; mounting read-only");
            rc = -EOPNOTSUPP;
        }
    }
    if (rc < 0) {
        klog_info("btrfs: a tree root is missing; mounting read-only");
        return rc;
    }
    fs->fs_root_dirid = dirid;
    /* The chunk tree's address comes from the super block, not the root tree —
     * it is the one tree that has to be found before any mapping exists. */
    fs->chunk_root_bytenr = fs->sb.chunk_root;
    fs->chunk_root_level = fs->sb.chunk_root_level;
    if (find_root(fs, BTRFS_DEV_TREE_OBJECTID, &fs->dev_root_bytenr,
                  &fs->dev_root_level, 0) < 0) {
        klog_info("btrfs: device tree missing; mounting read-only");
        return -EOPNOTSUPP;
    }

    fs->nbgs = 0;
    fs->nallocs = 0;
    rc = walk_tree(fs, fs->extent_root_bytenr, collect_extents, 0, 0);
    if (rc == 0 && fs->has_bgt)
        rc = walk_tree(fs, fs->bgt_root_bytenr, collect_extents, 0, 0);
    if (rc < 0) {
        klog_info("btrfs: extent tree too large to map; mounting read-only");
        kfree(fs->allocs);
        fs->allocs = 0;
        fs->allocs_cap = fs->nallocs = fs->nbgs = 0;
        return rc;
    }

    /* The blocks the trees themselves live in are recorded in the extent tree
     * like everything else, so the map is complete as it stands. The super
     * block's own 64 KiB is not part of any block group and never allocated
     * from, so it needs no entry. */
    fs->trans_gen = fs->sb.generation + 1;
    fs->rw = 1;
    fs->dirty = 0;
    return 0;
}

void btrfs_rw_teardown(struct btrfs_fs_info *fs) {
    if (fs->pinned)
        kfree(fs->pinned);
    fs->pinned = 0;
    fs->pinned_cap = fs->npinned = 0;
    if (fs->pending)
        kfree(fs->pending);
    fs->pending = 0;
    fs->pending_cap = fs->npending = 0;
    if (fs->allocs)
        kfree(fs->allocs);
    fs->allocs = 0;
    fs->allocs_cap = fs->nallocs = 0;
    fs->rw = 0;
}

static struct btrfs_root_ref bg_root_ref(struct btrfs_fs_info *fs) {
    if (fs->has_bgt) {
        struct btrfs_root_ref r = {BTRFS_BLOCK_GROUP_TREE_OBJECTID,
                                   &fs->bgt_root_bytenr, &fs->bgt_root_level};

        return r;
    }
    return extent_root_ref(fs);
}

/* ── Growing the filesystem ────────────────────────────────────────────────
 *
 * A block group is a fixed span of the logical address space backed by a
 * chunk, and when they are all full the filesystem does not run out of disk —
 * it runs out of MAPPED disk. Making a new chunk is four records that have to
 * agree: a CHUNK_ITEM saying which device range backs which logical range, a
 * DEV_EXTENT saying that device range is taken, a BLOCK_GROUP_ITEM saying what
 * may be stored there, and the device's own bytes_used.
 *
 * Single device, single stripe, no RAID: the profiles this driver refuses to
 * read it will certainly not create. */

static struct btrfs_root_ref chunk_root_ref(struct btrfs_fs_info *fs) {
    struct btrfs_root_ref r = {BTRFS_CHUNK_TREE_OBJECTID,
                               &fs->chunk_root_bytenr, &fs->chunk_root_level};

    return r;
}

static struct btrfs_root_ref dev_root_ref(struct btrfs_fs_info *fs) {
    struct btrfs_root_ref r = {BTRFS_DEV_TREE_OBJECTID, &fs->dev_root_bytenr,
                               &fs->dev_root_level};

    return r;
}

/* Where the next chunk can live on the device: the first physical gap wide
 * enough, past the first megabyte the format reserves. */
static int device_find_space(struct btrfs_fs_info *fs, u64 want, u64 *out) {
    const struct btrfs_dev_item *di =
        (const struct btrfs_dev_item *)fs->sb.dev_item;
    u64 cur = 1024 * 1024;
    u64 limit = di->total_bytes;
    u64 from = 0;

    if (fs->bdev->block_count &&
        limit > (u64)fs->bdev->block_count * 512)
        limit = (u64)fs->bdev->block_count * 512;

    /* Walk the dev tree in key order; its DEV_EXTENTs are the taken ranges. */
    while (1) {
        struct btrfs_path path;
        int rc = btrfs_search_path(fs, fs->dev_root_bytenr, di->devid,
                                   BTRFS_DEV_EXTENT_KEY, from, &path);

        if (rc < 0)
            return rc;

        const struct btrfs_header *h =
            (const struct btrfs_header *)path.nodes[0];
        int progressed = 0, done = 0;
        u64 last = from;

        for (u32 i = path.slots[0]; i < h->nritems; i++) {
            const struct btrfs_item *it = btrfs_leaf_item(path.nodes[0], i);

            if (it->key.objectid != di->devid ||
                it->key.type != BTRFS_DEV_EXTENT_KEY) {
                done = 1;
                break;
            }

            const struct btrfs_dev_extent *de =
                (const struct btrfs_dev_extent *)btrfs_item_data(path.nodes[0],
                                                                 it);
            u64 e_start = it->key.offset;
            u64 e_end = e_start + de->length;

            if (e_end <= cur)
                continue;
            if (e_start >= cur + want)
                break; /* the gap at cur is wide enough */
            cur = e_end;
            last = e_start;
            progressed = 1;
        }
        path_release(&path);
        if (done || !progressed)
            break;
        from = last + 1;
    }
    if (cur + want > limit)
        return -ENOSPC;
    *out = cur;
    return 0;
}

/* The logical address just past every chunk there is. New chunks go there,
 * which keeps the logical space contiguous and the map simple. */
static u64 chunk_next_logical(struct btrfs_fs_info *fs) {
    u64 next = 0;

    for (u32 i = 0; i < fs->nchunks; i++) {
        u64 end = fs->chunks[i].logical + fs->chunks[i].length;

        if (end > next)
            next = end;
    }
    return next;
}

/* Make one chunk of `bg_type`, and the block group that lives in it. */
static int btrfs_alloc_chunk(struct btrfs_fs_info *fs, u64 bg_type,
                             u64 min_bytes) {
    if (fs->nchunks >= BTRFS_MAX_CHUNKS || fs->nbgs >= BTRFS_MAX_BLOCK_GROUPS)
        return -ENOSPC;

    struct btrfs_dev_item *di = (struct btrfs_dev_item *)fs->sb.dev_item;
    /* Sizes in the spirit of btrfs's own: a data chunk is worth making large,
     * metadata grows in smaller steps. Both are clamped to what is actually
     * free on the device, and rounded to the stripe. */
    u64 want = bg_type == BTRFS_BLOCK_GROUP_DATA ? 128ull * 1024 * 1024
                                                 : 32ull * 1024 * 1024;

    if (want < min_bytes)
        want = (min_bytes + 1024 * 1024 - 1) & ~(1024ull * 1024 - 1);

    u64 physical = 0;
    int rc = device_find_space(fs, want, &physical);

    while (rc == -ENOSPC && want > min_bytes && want > 16ull * 1024 * 1024) {
        want /= 2;
        rc = device_find_space(fs, want, &physical);
    }
    if (rc < 0)
        return rc;

    u64 logical = chunk_next_logical(fs);

    /* The chunk item: one stripe, on this device, at `physical`. */
    u8 buf[sizeof(struct btrfs_chunk) + sizeof(struct btrfs_stripe)];
    struct btrfs_chunk *ck = (struct btrfs_chunk *)buf;

    memset(buf, 0, sizeof(buf));
    ck->length = want;
    ck->owner = BTRFS_EXTENT_TREE_OBJECTID;
    ck->stripe_len = 65536;
    ck->type = bg_type;
    ck->io_align = 65536;
    ck->io_width = 65536;
    ck->sector_size = fs->sb.sectorsize;
    ck->num_stripes = 1;
    ck->sub_stripes = 1;
    ck->stripes[0].devid = di->devid;
    ck->stripes[0].physical = physical;
    memcpy(ck->stripes[0].dev_uuid, di->uuid, sizeof(di->uuid));

    struct btrfs_root_ref cr = chunk_root_ref(fs);

    rc = btrfs_insert_item(fs, &cr, BTRFS_FIRST_CHUNK_TREE_OBJECTID,
                           BTRFS_CHUNK_ITEM_KEY, logical, buf, sizeof(buf));
    if (rc < 0)
        return rc;

    /* The device extent that says this physical range is spoken for. */
    struct btrfs_dev_extent de;
    struct btrfs_root_ref dr = dev_root_ref(fs);

    memset(&de, 0, sizeof(de));
    de.chunk_tree = BTRFS_CHUNK_TREE_OBJECTID;
    de.chunk_objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
    de.chunk_offset = logical;
    de.length = want;
    memcpy(de.chunk_tree_uuid, fs->sb.fsid, sizeof(de.chunk_tree_uuid));
    rc = btrfs_insert_item(fs, &dr, di->devid, BTRFS_DEV_EXTENT_KEY, physical,
                           &de, sizeof(de));
    if (rc < 0)
        return rc;

    /* The block group itself, empty. */
    struct btrfs_block_group_item bgi;
    struct btrfs_root_ref br = bg_root_ref(fs);

    memset(&bgi, 0, sizeof(bgi));
    bgi.used = 0;
    bgi.chunk_objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
    bgi.flags = bg_type;
    rc = btrfs_insert_item(fs, &br, logical, BTRFS_BLOCK_GROUP_ITEM_KEY, want,
                           &bgi, sizeof(bgi));
    if (rc < 0)
        return rc;

    /* Only now does the mapping exist for this driver, and only now may
     * anything be allocated from it: an allocation handed out before the
     * chunk item was written would have no logical-to-physical mapping and
     * every write to it would fail. */
    fs->chunks[fs->nchunks].logical = logical;
    fs->chunks[fs->nchunks].length = want;
    fs->chunks[fs->nchunks].physical = physical;
    fs->nchunks++;

    fs->bgs[fs->nbgs].start = logical;
    fs->bgs[fs->nbgs].len = want;
    fs->bgs[fs->nbgs].flags = bg_type;
    fs->bgs[fs->nbgs].used = 0;
    fs->bgs[fs->nbgs].dirty = 1;
    fs->nbgs++;

    di->bytes_used += want;
    fs->dirty = 1;
    return 0;
}

/* ── Commit ────────────────────────────────────────────────────────────────*/

/* ── The free-space tree ───────────────────────────────────────────────────
 *
 * A second on-disk record of what is free, which btrfs keeps so a mount does
 * not have to read the whole extent tree the way this driver does. Once the
 * feature is on, the tree is authoritative for anything that mounts the
 * filesystem afterwards: leaving it stale would hand the next kernel a map
 * that says free space is where live data is.
 *
 * It is REBUILT at commit rather than maintained as each allocation happens.
 * The allocation map in memory already knows the answer exactly, and a
 * rebuild cannot drift out of step with it the way a hundred incremental
 * updates can. Bitmaps are never written — extents are always legal, and the
 * choice between the two is per block group and free for an implementation to
 * make. */

static struct btrfs_root_ref fst_root_ref(struct btrfs_fs_info *fs) {
    struct btrfs_root_ref r = {BTRFS_FREE_SPACE_TREE_OBJECTID,
                               &fs->fst_root_bytenr, &fs->fst_root_level};

    return r;
}

/* Every key the free-space tree holds for one block group. Collected first
 * and deleted afterwards, because deleting while walking invalidates the
 * walk. */
static int fst_collect_keys(struct btrfs_fs_info *fs, u64 bg_start, u64 bg_len,
                            struct btrfs_disk_key **out, u32 *count) {
    u32 cap = 64, n = 0;
    struct btrfs_disk_key *keys =
        kmalloc((usize)cap * sizeof(struct btrfs_disk_key));
    u64 from = bg_start;

    if (!keys)
        return -ENOMEM;

    while (1) {
        struct btrfs_path path;
        int rc = btrfs_search_path(fs, fs->fst_root_bytenr, from, 0, 0, &path);

        if (rc < 0) {
            kfree(keys);
            return rc;
        }

        const struct btrfs_header *h =
            (const struct btrfs_header *)path.nodes[0];
        u64 last = from;
        int progressed = 0, done = 0;

        for (u32 i = path.slots[0]; i < h->nritems; i++) {
            const struct btrfs_item *it = btrfs_leaf_item(path.nodes[0], i);

            if (it->key.objectid >= bg_start + bg_len) {
                done = 1;
                break;
            }
            if (it->key.type != BTRFS_FREE_SPACE_INFO_KEY &&
                it->key.type != BTRFS_FREE_SPACE_EXTENT_KEY &&
                it->key.type != BTRFS_FREE_SPACE_BITMAP_KEY)
                continue;
            if (n == cap) {
                u32 ncap = cap * 2;
                struct btrfs_disk_key *nk =
                    kmalloc((usize)ncap * sizeof(struct btrfs_disk_key));

                if (!nk) {
                    kfree(keys);
                    path_release(&path);
                    return -ENOMEM;
                }
                memcpy(nk, keys, (usize)n * sizeof(struct btrfs_disk_key));
                kfree(keys);
                keys = nk;
                cap = ncap;
            }
            keys[n++] = it->key;
            last = it->key.objectid;
            progressed = 1;
        }
        path_release(&path);
        if (done || !progressed)
            break;
        from = last + 1;
    }
    *out = keys;
    *count = n;
    return 0;
}

/* Bring one block group's free-space records in line with the allocation map.
 *
 * The DIFFERENCE, not a rewrite. Deleting every record and writing it back
 * costs one tree operation per free range on every commit — and since each of
 * those operations allocates, the commit loop has to run again, and the whole
 * cost repeats. Applying only what actually changed converges in two or three
 * passes instead of a dozen, and turned a ten-second commit into a prompt
 * one. */
static int fst_rebuild_group(struct btrfs_fs_info *fs,
                             const struct btrfs_block_group *bg,
                             int *changed) {
    struct btrfs_root_ref fr = fst_root_ref(fs);
    struct btrfs_disk_key *old = 0;
    u32 nold = 0;
    int rc = fst_collect_keys(fs, bg->start, bg->len, &old, &nold);

    if (rc < 0)
        return rc;

    /* The free ranges, derived from the allocation map. */
    u64 end = bg->start + bg->len;
    u64 cur = bg->start;
    u32 nfree = 0;
    struct btrfs_alloc_range *want =
        kmalloc((usize)(fs->nallocs + 2) * sizeof(struct btrfs_alloc_range));

    if (!want) {
        kfree(old);
        return -ENOMEM;
    }
    for (u32 i = 0; i < fs->nallocs && cur < end; i++) {
        u64 a_start = fs->allocs[i].start;
        u64 a_end = a_start + fs->allocs[i].len;

        if (a_end <= cur)
            continue;
        if (a_start >= end)
            break;
        if (a_start > cur) {
            want[nfree].start = cur;
            want[nfree].len = a_start - cur;
            nfree++;
        }
        if (a_end > cur)
            cur = a_end;
    }
    if (cur < end) {
        want[nfree].start = cur;
        want[nfree].len = end - cur;
        nfree++;
    }

    /* Delete the records that are no longer free ranges. */
    u32 have = 0;

    for (u32 i = 0; i < nold && rc == 0; i++) {
        if (old[i].type != BTRFS_FREE_SPACE_EXTENT_KEY)
            continue;

        int keep = 0;

        for (u32 j = 0; j < nfree; j++) {
            if (old[i].objectid == want[j].start && old[i].offset == want[j].len) {
                keep = 1;
                break;
            }
        }
        if (keep) {
            have++;
            continue;
        }
        rc = btrfs_delete_item(fs, &fr, old[i].objectid, old[i].type,
                               old[i].offset);
        *changed = 1;
    }

    /* Insert the ones that are new. */
    for (u32 j = 0; j < nfree && rc == 0; j++) {
        int present = 0;

        for (u32 i = 0; i < nold; i++) {
            if (old[i].type == BTRFS_FREE_SPACE_EXTENT_KEY &&
                old[i].objectid == want[j].start &&
                old[i].offset == want[j].len) {
                present = 1;
                break;
            }
        }
        if (present)
            continue;
        rc = btrfs_insert_item(fs, &fr, want[j].start,
                               BTRFS_FREE_SPACE_EXTENT_KEY, want[j].len, 0, 0);
        *changed = 1;
    }

    /* And the count the group's own record advertises. */
    if (rc == 0 && (have != nfree || *changed)) {
        struct btrfs_free_space_info info;
        struct btrfs_path path;

        rc = btrfs_search_path(fs, fs->fst_root_bytenr, bg->start,
                               BTRFS_FREE_SPACE_INFO_KEY, bg->len, &path);
        if (rc == 0) {
            const struct btrfs_header *h =
                (const struct btrfs_header *)path.nodes[0];
            int found = path.slots[0] < h->nritems &&
                        btrfs_key_cmp(&btrfs_leaf_item(path.nodes[0],
                                                       path.slots[0])->key,
                                      bg->start, BTRFS_FREE_SPACE_INFO_KEY,
                                      bg->len) == 0;

            if (found)
                memcpy(&info,
                       btrfs_item_data(path.nodes[0],
                                       btrfs_leaf_item(path.nodes[0],
                                                       path.slots[0])),
                       sizeof(info));
            path_release(&path);
            if (found && info.extent_count != nfree) {
                info.extent_count = nfree;
                rc = btrfs_update_item(fs, &fr, bg->start,
                                       BTRFS_FREE_SPACE_INFO_KEY, bg->len,
                                       &info, sizeof(info));
                *changed = 1;
            } else if (!found) {
                info.extent_count = nfree;
                info.flags = 0;
                rc = btrfs_insert_item(fs, &fr, bg->start,
                                       BTRFS_FREE_SPACE_INFO_KEY, bg->len,
                                       &info, sizeof(info));
                *changed = 1;
            }
        }
    }
    kfree(old);
    kfree(want);
    return rc;
}

static int fst_rebuild(struct btrfs_fs_info *fs, int *changed) {
    *changed = 0;
    if (!fs->has_fst)
        return 0;
    for (u32 i = 0; i < fs->nbgs; i++) {
        if (!fs->bgs[i].dirty)
            continue;

        int group_changed = 0;
        int rc = fst_rebuild_group(fs, &fs->bgs[i], &group_changed);

        if (rc < 0)
            return rc;
        /* Cleared only after the records match what the map says. Rewriting
         * them allocates, which dirties the group again; the commit loop runs
         * until a pass changes nothing. */
        if (!group_changed)
            fs->bgs[i].dirty = 0;
        *changed |= group_changed;
    }
    return 0;
}

static int update_block_group_items(struct btrfs_fs_info *fs, int *changed) {
    struct btrfs_root_ref er = bg_root_ref(fs);
    u64 bg_tree = fs->has_bgt ? fs->bgt_root_bytenr : fs->extent_root_bytenr;

    *changed = 0;
    for (u32 i = 0; i < fs->nbgs; i++) {
        struct btrfs_path path;
        int rc = btrfs_search_path(fs, bg_tree, fs->bgs[i].start,
                                   BTRFS_BLOCK_GROUP_ITEM_KEY, fs->bgs[i].len,
                                   &path);

        if (rc < 0)
            return rc;

        const struct btrfs_header *h =
            (const struct btrfs_header *)path.nodes[0];

        if (path.slots[0] >= h->nritems) {
            path_release(&path);
            continue;
        }

        const struct btrfs_item *it =
            btrfs_leaf_item(path.nodes[0], path.slots[0]);

        if (btrfs_key_cmp(&it->key, fs->bgs[i].start,
                          BTRFS_BLOCK_GROUP_ITEM_KEY, fs->bgs[i].len) != 0) {
            path_release(&path);
            continue;
        }

        struct btrfs_block_group_item bgi;

        memcpy(&bgi, btrfs_item_data(path.nodes[0], it), sizeof(bgi));
        path_release(&path);
        if (bgi.used == fs->bgs[i].used)
            continue;
        bgi.used = fs->bgs[i].used;
        rc = btrfs_update_item(fs, &er, fs->bgs[i].start,
                               BTRFS_BLOCK_GROUP_ITEM_KEY, fs->bgs[i].len,
                               &bgi, sizeof(bgi));
        if (rc < 0)
            return rc;
        *changed = 1;
    }
    return 0;
}

static int update_root_item(struct btrfs_fs_info *fs, u64 objectid,
                            u64 bytenr, u8 level) {
    struct btrfs_root_ref rr = {BTRFS_ROOT_TREE_OBJECTID, &fs->sb.root,
                                &fs->sb.root_level};
    struct btrfs_path path;
    int rc = btrfs_search_path(fs, fs->sb.root, objectid, BTRFS_ROOT_ITEM_KEY,
                               0, &path);

    if (rc < 0)
        return rc;

    const struct btrfs_header *h = (const struct btrfs_header *)path.nodes[0];
    struct btrfs_root_item ri;
    u32 size = 0;
    int found = 0;

    for (u32 i = path.slots[0]; i < h->nritems; i++) {
        const struct btrfs_item *it = btrfs_leaf_item(path.nodes[0], i);

        if (it->key.objectid != objectid)
            break;
        if (it->key.type != BTRFS_ROOT_ITEM_KEY)
            continue;
        size = it->size;
        memcpy(&ri, btrfs_item_data(path.nodes[0], it),
               size < sizeof(ri) ? size : sizeof(ri));
        found = 1;
        break;
    }
    path_release(&path);
    if (!found)
        return -ENOENT;
    if (ri.bytenr == bytenr && ri.level == level)
        return 0;

    /* The item on disk may be longer than the struct this driver knows (btrfs
     * has extended it over time); only the head is rewritten, so a full-size
     * update needs the original bytes back. */
    u8 *buf = kmalloc(size);

    if (!buf)
        return -ENOMEM;

    rc = btrfs_search_path(fs, fs->sb.root, objectid, BTRFS_ROOT_ITEM_KEY, 0,
                           &path);
    if (rc < 0) {
        kfree(buf);
        return rc;
    }
    memcpy(buf, btrfs_item_data(path.nodes[0],
                                btrfs_leaf_item(path.nodes[0], path.slots[0])),
           size);
    path_release(&path);

    struct btrfs_root_item *out = (struct btrfs_root_item *)buf;

    out->bytenr = bytenr;
    out->level = level;
    out->generation = fs->trans_gen;
    /* The extended part of a root item repeats the generation, and btrfs
     * treats the two disagreeing as a reason to distrust the item: it then
     * expects the tree's blocks to carry SHARED back-references, the form
     * used for snapshotted trees, and reports every ordinary one as missing.
     * The offsets are counted rather than declared because this driver's
     * struct stops at the original fields — the on-disk item is longer, and
     * only the head of it is understood here. */
    {
        const u32 gen_v2_off = 239; /* end of the original root item */
        const u32 ctransid_off = gen_v2_off + 8 + 16 * 3;

        if (size >= gen_v2_off + 8)
            memcpy(buf + gen_v2_off, &fs->trans_gen, sizeof(fs->trans_gen));
        if (size >= ctransid_off + 8)
            memcpy(buf + ctransid_off, &fs->trans_gen, sizeof(fs->trans_gen));
    }
    rc = btrfs_update_item(fs, &rr, objectid, BTRFS_ROOT_ITEM_KEY, 0, buf,
                           size);
    kfree(buf);
    return rc;
}

/* Write the super block, and its mirrors where the device is long enough.
 *
 * The mirrors are at fixed physical offsets rather than logical ones — they
 * are how btrfs finds a filesystem before it can read a chunk tree — so they
 * are written directly. */
static int write_super(struct btrfs_fs_info *fs) {
    static const u64 offsets[] = {65536ULL, 67108864ULL, 274877906944ULL};
    u8 *buf = kzalloc(4096);

    if (!buf)
        return -ENOMEM;

    u64 total = 0;

    for (u32 i = 0; i < fs->nbgs; i++)
        total += fs->bgs[i].used;
    fs->sb.bytes_used = total;
    fs->sb.generation = fs->trans_gen;

    int wrote = 0;

    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        u64 off = offsets[i];

        if (fs->bdev->block_count &&
            (off + 4096) / 512 > fs->bdev->block_count)
            break;
        if (i > 0) {
            /* Only refresh a mirror that is already there: writing one where
             * none existed would claim a filesystem over whatever is at that
             * offset. */
            u8 probe[512];

            if (blk_read_cached(fs->bdev, off / 512, 1, probe) < 0)
                continue;
            if (memcmp(((struct btrfs_super_block *)probe)->magic, BTRFS_MAGIC,
                       8) != 0)
                continue;
        }
        memset(buf, 0, 4096);
        fs->sb.bytenr = off;
        memcpy(buf, &fs->sb, sizeof(fs->sb));

        u32 csum = btrfs_crc32c(buf + 32, 4096 - 32);

        memset(buf, 0, 32);
        memcpy(buf, &csum, sizeof(csum));
        if (blk_write_cached(fs->bdev, off / 512, 8, buf) < 0) {
            kfree(buf);
            return -EIO;
        }
        wrote++;
    }
    kfree(buf);
    fs->sb.bytenr = 65536;
    return wrote ? 0 : -EIO;
}

/* The device's own record lives in the chunk tree, and the super block carries
 * a copy of it. Growing the filesystem changes bytes_used, and btrfs check
 * compares the two against the device extents it finds. */
static int update_device_item(struct btrfs_fs_info *fs) {
    struct btrfs_dev_item *di = (struct btrfs_dev_item *)fs->sb.dev_item;
    struct btrfs_root_ref cr = chunk_root_ref(fs);
    struct btrfs_path path;
    int rc = btrfs_search_path(fs, fs->chunk_root_bytenr,
                               BTRFS_DEV_ITEMS_OBJECTID, BTRFS_DEV_ITEM_KEY,
                               di->devid, &path);

    if (rc < 0)
        return rc;

    const struct btrfs_header *h = (const struct btrfs_header *)path.nodes[0];
    int found = path.slots[0] < h->nritems &&
                btrfs_key_cmp(&btrfs_leaf_item(path.nodes[0],
                                               path.slots[0])->key,
                              BTRFS_DEV_ITEMS_OBJECTID, BTRFS_DEV_ITEM_KEY,
                              di->devid) == 0;
    struct btrfs_dev_item on_disk;

    if (found)
        memcpy(&on_disk,
               btrfs_item_data(path.nodes[0],
                               btrfs_leaf_item(path.nodes[0], path.slots[0])),
               sizeof(on_disk));
    path_release(&path);
    if (!found || on_disk.bytes_used == di->bytes_used)
        return 0;
    on_disk.bytes_used = di->bytes_used;
    on_disk.generation = fs->trans_gen;
    return btrfs_update_item(fs, &cr, BTRFS_DEV_ITEMS_OBJECTID,
                             BTRFS_DEV_ITEM_KEY, di->devid, &on_disk,
                             sizeof(on_disk));
}

/* Structural check of one tree, for `b1nix.btrfs-verify`.
 *
 * Every parent key must equal its child's first key, every child's recorded
 * level must be one below its parent's, and keys must ascend within a node.
 * A B-tree that violates any of these still reads back correctly through the
 * paths the driver itself takes — which is precisely why it needs checking
 * from the outside. */
static int validate_tree(struct btrfs_fs_info *fs, u64 bytenr, u8 want_level,
                         const struct btrfs_disk_key *want_first,
                         const char *name, int depth) {
    if (depth > BTRFS_MAX_LEVEL)
        return -EIO;

    u8 *buf = btrfs_read_block(fs, bytenr);

    if (!buf) {
        char line[128];

        snprintf(line, sizeof(line), "btrfs-verify: %s block %llu unreadable",
                 name, (unsigned long long)bytenr);
        klog_info(line);
        return -EIO;
    }

    const struct btrfs_header *h = (const struct btrfs_header *)buf;
    int rc = 0;
    char line[192];

    if (want_level != (u8)-1 && h->level != want_level) {
        snprintf(line, sizeof(line),
                 "btrfs-verify: %s block %llu level %u, parent says %u", name,
                 (unsigned long long)bytenr, h->level, want_level);
        klog_info(line);
        rc = -EIO;
    }
    if (h->nritems) {
        struct btrfs_disk_key first = h->level == 0
                                          ? btrfs_leaf_item(buf, 0)->key
                                          : ((const struct btrfs_key_ptr *)
                                                 (buf + sizeof(*h)))[0].key;

        if (want_first &&
            btrfs_key_cmp(&first, want_first->objectid, want_first->type,
                          want_first->offset) != 0) {
            snprintf(line, sizeof(line),
                     "btrfs-verify: %s block %llu starts (%llu,%u,%llu), "
                     "parent says (%llu,%u,%llu)",
                     name, (unsigned long long)bytenr,
                     (unsigned long long)first.objectid, first.type,
                     (unsigned long long)first.offset,
                     (unsigned long long)want_first->objectid,
                     want_first->type, (unsigned long long)want_first->offset);
            klog_info(line);
            rc = -EIO;
        }
    }

    if (h->level == 0) {
        for (u32 i = 1; i < h->nritems; i++) {
            const struct btrfs_item *prev = btrfs_leaf_item(buf, i - 1);
            const struct btrfs_item *it = btrfs_leaf_item(buf, i);

            if (btrfs_key_cmp(&prev->key, it->key.objectid, it->key.type,
                              it->key.offset) >= 0) {
                snprintf(line, sizeof(line),
                         "btrfs-verify: %s leaf %llu keys out of order at %u",
                         name, (unsigned long long)bytenr, i);
                klog_info(line);
                rc = -EIO;
                break;
            }
            if (it->offset + it->size > fs->sb.nodesize - sizeof(*h)) {
                snprintf(line, sizeof(line),
                         "btrfs-verify: %s leaf %llu item %u runs past the "
                         "block", name, (unsigned long long)bytenr, i);
                klog_info(line);
                rc = -EIO;
                break;
            }
        }
        kfree(buf);
        return rc;
    }

    u32 n = h->nritems;
    struct btrfs_key_ptr *ptrs = kmalloc((usize)n * sizeof(*ptrs));

    if (!ptrs) {
        kfree(buf);
        return -ENOMEM;
    }
    memcpy(ptrs, buf + sizeof(*h), (usize)n * sizeof(*ptrs));
    kfree(buf);

    for (u32 i = 0; i < n; i++) {
        if (i && btrfs_key_cmp(&ptrs[i - 1].key, ptrs[i].key.objectid,
                               ptrs[i].key.type, ptrs[i].key.offset) >= 0) {
            snprintf(line, sizeof(line),
                     "btrfs-verify: %s node %llu keys out of order at %u",
                     name, (unsigned long long)bytenr, i);
            klog_info(line);
            rc = -EIO;
        }
        if (validate_tree(fs, ptrs[i].blockptr, (u8)(h->level - 1),
                          &ptrs[i].key, name, depth + 1) < 0)
            rc = -EIO;
    }
    kfree(ptrs);
    return rc;
}

int btrfs_commit(struct btrfs_fs_info *fs) {
    if (!fs->rw)
        return -EROFS;
    if (!fs->dirty)
        return 0;

    int drain = btrfs_run_delayed_refs(fs);

    if (drain < 0)
        return drain;

    /* Everything a commit has to record moves something else that has to be
     * recorded.
     *
     * Writing a block group's usage rewrites the extent tree; rewriting the
     * extent tree allocates, which changes a block group's usage and the
     * free-space records; naming the extent tree's new root rewrites the root
     * tree, which needs a root of its own recorded. Each step is small and
     * each one invalidates the last, so the whole set is applied in ONE loop
     * until a pass changes nothing — and only then is the super block
     * written. Split into separate loops, whichever tree is updated last is
     * left named by a stale address with its new blocks absent from the
     * extent tree, and btrfs check reports exactly one such block per tree. */
    for (int round = 0; round < 16; round++) {
        u64 before[7];
        int bg_changed = 0, fst_changed = 0;
        int rc2;

        before[0] = fs->extent_root_bytenr;
        before[1] = fs->csum_root_bytenr;
        before[2] = fs->fs_root_bytenr;
        before[3] = fs->fst_root_bytenr;
        before[4] = fs->bgt_root_bytenr;
        before[5] = fs->dev_root_bytenr;
        before[6] = fs->sb.root;

        rc2 = fst_rebuild(fs, &fst_changed);
        if (rc2 == 0)
            rc2 = btrfs_run_delayed_refs(fs);
        if (rc2 == 0)
            rc2 = update_block_group_items(fs, &bg_changed);
        if (rc2 == 0)
            rc2 = btrfs_run_delayed_refs(fs);
        if (rc2 == 0)
            rc2 = update_device_item(fs);
        if (rc2 == 0)
            rc2 = btrfs_run_delayed_refs(fs);
        if (rc2 == 0)
            rc2 = update_root_item(fs, BTRFS_EXTENT_TREE_OBJECTID,
                                   fs->extent_root_bytenr,
                                   fs->extent_root_level);
        if (rc2 == 0)
            rc2 = update_root_item(fs, BTRFS_CSUM_TREE_OBJECTID,
                                   fs->csum_root_bytenr, fs->csum_root_level);
        if (rc2 == 0)
            rc2 = update_root_item(fs, BTRFS_FS_TREE_OBJECTID,
                                   fs->fs_root_bytenr, fs->fs_root_level);
        if (rc2 == 0 && fs->has_fst)
            rc2 = update_root_item(fs, BTRFS_FREE_SPACE_TREE_OBJECTID,
                                   fs->fst_root_bytenr, fs->fst_root_level);
        if (rc2 == 0 && fs->has_bgt)
            rc2 = update_root_item(fs, BTRFS_BLOCK_GROUP_TREE_OBJECTID,
                                   fs->bgt_root_bytenr, fs->bgt_root_level);
        if (rc2 == 0)
            rc2 = update_root_item(fs, BTRFS_DEV_TREE_OBJECTID,
                                   fs->dev_root_bytenr, fs->dev_root_level);
        if (rc2 == 0)
            rc2 = btrfs_run_delayed_refs(fs);
        if (rc2 < 0)
            return rc2;

        if (!bg_changed && !fst_changed &&
            before[0] == fs->extent_root_bytenr &&
            before[1] == fs->csum_root_bytenr &&
            before[2] == fs->fs_root_bytenr &&
            before[3] == fs->fst_root_bytenr &&
            before[4] == fs->bgt_root_bytenr &&
            before[5] == fs->dev_root_bytenr && before[6] == fs->sb.root)
            break;
        if (round == 15) {
            klog_info("btrfs: commit did not reach a fixed point");
            return -EIO;
        }
    }

    /* The chunk tree is named by the super block rather than by a root item,
     * so growing the filesystem moves an address the super block carries.
     * Copied last, after every update that could still move it.
     *
     * The GENERATION moves only when the tree actually did. Stamping the new
     * transaction on an unchanged chunk root makes the super block promise a
     * generation the block does not carry, and btrfs-progs refuses the
     * filesystem outright: "parent transid verify failed ... wanted 10 found
     * 5", then "cannot read chunk root". */
    /* The generation moves whenever the chunk tree was WRITTEN, not only when
     * its root address changed. A root rewritten in place keeps its address
     * and takes the new transaction's generation, and a super block still
     * naming the old one makes btrfs-progs refuse the tree outright —
     * "parent transid verify failed ... wanted 5 found 10", after which it
     * finds no block groups and reports every extent as unreferenced. */
    if (fs->chunk_dirty || fs->sb.chunk_root != fs->chunk_root_bytenr) {
        fs->sb.chunk_root = fs->chunk_root_bytenr;
        fs->sb.chunk_root_level = fs->chunk_root_level;
        fs->sb.chunk_root_generation = fs->trans_gen;
        fs->chunk_dirty = 0;
    }

    /* Data and metadata first, then the super block that names them: the
     * order is the whole guarantee. A crash before the super block lands
     * leaves the previous transaction intact, which is exactly what a
     * copy-on-write filesystem is for. */
    blk_cache_flush(fs->bdev);
    if (fs->bdev->flush)
        fs->bdev->flush(fs->bdev);

    int wrc = write_super(fs);

    if (wrc < 0)
        return wrc;
    blk_cache_flush(fs->bdev);
    if (fs->bdev->flush)
        fs->bdev->flush(fs->bdev);
    /* The super block is down; nothing points at the freed ranges any more,
     * so they become ordinary free space. */
    /* `b1nix.btrfs-verify`: re-read every tree this commit touched. Each block
     * is checksum-verified on the way in, so a walk that completes says the
     * medium holds what the driver believes it wrote. */
    if (bootinfo_has_flag("b1nix.btrfs-verify")) {
        static const struct {
            const char *name;
            u64 *root;
        } trees[] = {{"fs", 0}, {"extent", 0}, {"csum", 0}, {"chunk", 0},
                     {"dev", 0}, {"root", 0}, {"fst", 0}, {"bgt", 0}};
        u64 roots[8] = {fs->fs_root_bytenr, fs->extent_root_bytenr,
                        fs->csum_root_bytenr, fs->chunk_root_bytenr,
                        fs->dev_root_bytenr, fs->sb.root,
                        fs->has_fst ? fs->fst_root_bytenr : 0,
                        fs->has_bgt ? fs->bgt_root_bytenr : 0};

        for (unsigned i = 0; i < 8; i++) {
            if (!roots[i])
                continue;
            validate_tree(fs, roots[i], (u8)-1, 0, trees[i].name, 0);
            if (walk_tree(fs, roots[i], 0, 0, 0) < 0) {
                char line[128];

                snprintf(line, sizeof(line),
                         "btrfs-verify: %s tree unreadable after commit",
                         trees[i].name);
                klog_info(line);
            }
        }
    }
    fs->npinned = 0;
    fs->dirty = 0;
    return 0;
}

/* ── The namespace ─────────────────────────────────────────────────────────
 *
 * A name in a directory is four items, not one: DIR_ITEM keyed by the hash of
 * the name (what a lookup by name finds), DIR_INDEX keyed by a sequence
 * number (what readdir walks, in creation order), INODE_REF on the child
 * naming its parent, and the child's own INODE_ITEM. The directory's `size`
 * is the sum of the name lengths over BOTH the DIR_ITEM and the DIR_INDEX
 * entries, which is why it grows by twice a name's length. */

/* btrfs hashes directory names with crc32c seeded ~1 and does NOT invert the
 * result. Verified against a filesystem mkfs.btrfs wrote rather than taken
 * from memory: the four names in the test image hash to exactly the four
 * DIR_ITEM offsets it recorded. */
static u64 name_hash(const char *name, usize len) {
    return btrfs_crc32c_seed(0xFFFFFFFEu, name, len);
}

/* The highest objectid the FS tree has used, so the next one is free. btrfs
 * keeps a counter in memory and rebuilds it exactly this way after a mount. */
static int highest_objectid(struct btrfs_fs_info *fs, u64 *out) {
    struct btrfs_path path;
    int rc = btrfs_search_path(fs, fs->fs_root_bytenr, (u64)-1, (u8)-1,
                               (u64)-1, &path);

    if (rc < 0)
        return rc;

    const struct btrfs_header *h = (const struct btrfs_header *)path.nodes[0];
    u64 best = BTRFS_FIRST_FREE_OBJECTID;

    for (u32 i = 0; i < h->nritems; i++) {
        const struct btrfs_item *it = btrfs_leaf_item(path.nodes[0], i);

        if (it->key.objectid >= BTRFS_FIRST_FREE_OBJECTID &&
            it->key.objectid != (u64)-1 && it->key.objectid > best)
            best = it->key.objectid;
    }
    path_release(&path);
    *out = best;
    return 0;
}

/* The next free DIR_INDEX in a directory. Indices 0 and 1 belong to "." and
 * "..", so a directory with no entries starts at 2. */
static int next_dir_index(struct btrfs_fs_info *fs, u64 dir, u64 *out) {
    struct btrfs_path path;
    int rc = btrfs_search_path(fs, fs->fs_root_bytenr, dir,
                               BTRFS_DIR_INDEX_KEY, (u64)-1, &path);

    if (rc < 0)
        return rc;

    const struct btrfs_header *h = (const struct btrfs_header *)path.nodes[0];
    u64 best = 1;

    for (u32 i = 0; i < h->nritems; i++) {
        const struct btrfs_item *it = btrfs_leaf_item(path.nodes[0], i);

        if (it->key.objectid == dir && it->key.type == BTRFS_DIR_INDEX_KEY &&
            it->key.offset > best)
            best = it->key.offset;
    }
    path_release(&path);
    *out = best + 1;
    return 0;
}

static int inode_store(struct btrfs_fs_info *fs, u64 objectid,
                       const struct btrfs_inode_item *in) {
    struct btrfs_root_ref fr = fs_root_ref(fs);

    return btrfs_update_item(fs, &fr, objectid, BTRFS_INODE_ITEM_KEY, 0, in,
                             sizeof(*in));
}

/* The directory's own size and times, after an entry came or went. */
static int dir_account(struct btrfs_fs_info *fs, u64 dir, isize name_delta) {
    struct btrfs_inode_item di;
    int rc = inode_load(fs, dir, &di);

    if (rc < 0)
        return rc;

    isize sz = (isize)di.size + name_delta;

    di.size = sz > 0 ? (u64)sz : 0;
    di.generation = fs->trans_gen;
    di.transid = fs->trans_gen;
    di.sequence++;

    u64 now = rtc_now_unix_seconds();

    di.mtime.sec = now;
    di.mtime.nsec = 0;
    di.ctime = di.mtime;
    return inode_store(fs, dir, &di);
}

/* One DIR_ITEM or DIR_INDEX: the fixed header, then the name. */
static int dir_entry_insert(struct btrfs_fs_info *fs, u64 dir, u8 type,
                            u64 key_offset, u64 child, u8 ftype,
                            const char *name, usize name_len) {
    struct btrfs_root_ref fr = fs_root_ref(fs);
    u32 size = (u32)(sizeof(struct btrfs_dir_item) + name_len);
    u8 *buf = kzalloc(size);

    if (!buf)
        return -ENOMEM;

    struct btrfs_dir_item *di = (struct btrfs_dir_item *)buf;

    di->location.objectid = child;
    di->location.type = BTRFS_INODE_ITEM_KEY;
    di->location.offset = 0;
    di->transid = fs->trans_gen;
    di->data_len = 0;
    di->name_len = (u16)name_len;
    di->type = ftype;
    memcpy(buf + sizeof(*di), name, name_len);

    int rc = btrfs_insert_item(fs, &fr, dir, type, key_offset, buf, size);

    kfree(buf);
    return rc;
}

static int inode_ref_insert(struct btrfs_fs_info *fs, u64 objectid, u64 dir,
                            u64 index, const char *name, usize name_len) {
    struct btrfs_root_ref fr = fs_root_ref(fs);
    u32 size = (u32)(sizeof(struct btrfs_inode_ref) + name_len);
    u8 *buf = kzalloc(size);

    if (!buf)
        return -ENOMEM;

    struct btrfs_inode_ref *ir = (struct btrfs_inode_ref *)buf;

    ir->index = index;
    ir->name_len = (u16)name_len;
    memcpy(buf + sizeof(*ir), name, name_len);

    int rc = btrfs_insert_item(fs, &fr, objectid, BTRFS_INODE_REF_KEY, dir,
                               buf, size);

    kfree(buf);
    return rc;
}

/* Find the DIR_INDEX that names `name` in `dir`, since unlinking has to
 * remove it and only the DIR_ITEM is reachable by hash.
 *
 * The walk continues past the end of a leaf. A directory whose entries span
 * two leaves is ordinary — writing to this filesystem is what splits them —
 * and a search that stopped at the first leaf boundary reported ENOENT for
 * files that were plainly there. */
static int find_dir_index(struct btrfs_fs_info *fs, u64 dir, const char *name,
                          usize name_len, u64 *index_out, u64 *child_out) {
    u64 from = 0;

    while (1) {
        struct btrfs_path path;
        int rc = btrfs_search_path(fs, fs->fs_root_bytenr, dir,
                                   BTRFS_DIR_INDEX_KEY, from, &path);

        if (rc < 0)
            return rc;

        const struct btrfs_header *h =
            (const struct btrfs_header *)path.nodes[0];
        u64 last = 0;
        int scanned = 0, done = 0, found = 0;

        for (u32 i = path.slots[0]; i < h->nritems; i++) {
            const struct btrfs_item *it = btrfs_leaf_item(path.nodes[0], i);

            if (it->key.objectid != dir) {
                done = 1;
                break;
            }
            if (it->key.type != BTRFS_DIR_INDEX_KEY) {
                if (it->key.type > BTRFS_DIR_INDEX_KEY)
                    done = 1;
                continue;
            }
            scanned = 1;
            last = it->key.offset;

            const struct btrfs_dir_item *di =
                (const struct btrfs_dir_item *)btrfs_item_data(path.nodes[0],
                                                               it);

            if (di->name_len != name_len)
                continue;
            if (memcmp((const char *)(di + 1), name, name_len) != 0)
                continue;
            *index_out = it->key.offset;
            *child_out = di->location.objectid;
            found = 1;
            break;
        }
        path_release(&path);
        if (found)
            return 0;
        if (done || !scanned)
            return -ENOENT;
        from = last + 1; /* continue in the next leaf */
    }
}

/* Is this directory empty? Only "." and ".." live below index 2, and neither
 * is stored, so any DIR_INDEX at all means it has a child. */
static int dir_is_empty(struct btrfs_fs_info *fs, u64 dir, int *empty) {
    struct btrfs_path path;
    int rc = btrfs_search_path(fs, fs->fs_root_bytenr, dir,
                               BTRFS_DIR_INDEX_KEY, 0, &path);

    if (rc < 0)
        return rc;

    const struct btrfs_header *h = (const struct btrfs_header *)path.nodes[0];

    *empty = 1;
    for (u32 i = path.slots[0]; i < h->nritems; i++) {
        const struct btrfs_item *it = btrfs_leaf_item(path.nodes[0], i);

        if (it->key.objectid != dir)
            break;
        if (it->key.type == BTRFS_DIR_INDEX_KEY) {
            *empty = 0;
            break;
        }
    }
    path_release(&path);
    return 0;
}

/* An inline extent: the file's bytes live in the leaf itself. Used for a
 * symlink's target, which is what btrfs stores a symlink as. */
static int inline_extent_insert(struct btrfs_fs_info *fs, u64 objectid,
                                const void *data, usize len) {
    struct btrfs_root_ref fr = fs_root_ref(fs);
    u32 size = (u32)(BTRFS_FILE_EXTENT_INLINE_HDR + len);
    u8 *buf = kzalloc(size);

    if (!buf)
        return -ENOMEM;

    struct btrfs_file_extent_item *fe = (struct btrfs_file_extent_item *)buf;

    fe->generation = fs->trans_gen;
    fe->ram_bytes = len;
    fe->compression = 0;
    fe->encryption = 0;
    fe->other_encoding = 0;
    fe->type = BTRFS_FILE_EXTENT_INLINE;
    memcpy(buf + BTRFS_FILE_EXTENT_INLINE_HDR, data, len);

    int rc = btrfs_insert_item(fs, &fr, objectid, BTRFS_EXTENT_DATA_KEY, 0,
                               buf, size);

    kfree(buf);
    return rc;
}

int btrfs_create_entry(struct btrfs_fs_info *fs, u64 dir_objectid,
                       const char *name, u32 mode, const char *symlink_target,
                       u64 *objectid_out) {
    if (!fs->rw)
        return -EROFS;

    usize name_len = strlen(name);

    if (!name_len || name_len > 255)
        return -ENAMETOOLONG;

    int reserve_rc = btrfs_reserve(fs, BTRFS_BLOCK_GROUP_METADATA, 0);

    if (reserve_rc < 0)
        return reserve_rc;

    /* A name that is already there must not become a second entry: btrfs
     * would then have two DIR_ITEMs for one name and readdir would show it
     * twice. */
    u64 dummy_index, dummy_child;

    if (find_dir_index(fs, dir_objectid, name, name_len, &dummy_index,
                       &dummy_child) == 0)
        return -EEXIST;

    u64 objectid, index;
    int rc = highest_objectid(fs, &objectid);

    if (rc < 0)
        return rc;
    objectid++;
    rc = next_dir_index(fs, dir_objectid, &index);
    if (rc < 0)
        return rc;

    u8 ftype = BTRFS_FT_REG_FILE;

    if ((mode & 0170000) == 0040000)
        ftype = BTRFS_FT_DIR;
    else if ((mode & 0170000) == 0120000)
        ftype = BTRFS_FT_SYMLINK;

    struct btrfs_inode_item ii;
    u64 now = rtc_now_unix_seconds();

    memset(&ii, 0, sizeof(ii));
    ii.generation = fs->trans_gen;
    ii.transid = fs->trans_gen;
    ii.size = symlink_target ? strlen(symlink_target) : 0;
    ii.nbytes = 0;
    ii.nlink = 1;
    ii.mode = mode;
    ii.atime.sec = now;
    ii.ctime.sec = now;
    ii.mtime.sec = now;
    ii.otime.sec = now;

    struct btrfs_root_ref fr = fs_root_ref(fs);

    rc = btrfs_insert_item(fs, &fr, objectid, BTRFS_INODE_ITEM_KEY, 0, &ii,
                           sizeof(ii));
    if (rc == 0)
        rc = inode_ref_insert(fs, objectid, dir_objectid, index, name,
                              name_len);
    if (rc == 0)
        rc = dir_entry_insert(fs, dir_objectid, BTRFS_DIR_ITEM_KEY,
                              name_hash(name, name_len), objectid, ftype, name,
                              name_len);
    if (rc == 0)
        rc = dir_entry_insert(fs, dir_objectid, BTRFS_DIR_INDEX_KEY, index,
                              objectid, ftype, name, name_len);
    if (rc == 0 && symlink_target)
        rc = inline_extent_insert(fs, objectid, symlink_target,
                                  strlen(symlink_target));
    if (rc == 0)
        rc = dir_account(fs, dir_objectid, (isize)name_len * 2);
    if (rc == 0)
        rc = btrfs_run_delayed_refs(fs);
    if (rc < 0)
        return rc;
    if (objectid_out)
        *objectid_out = objectid;
    return 0;
}

/* Free every extent of a file, and its checksums with them. */
static int file_drop_all(struct btrfs_fs_info *fs, u64 objectid) {
    return file_drop_range(fs, objectid, 0, (u64)-1, 0);
}

int btrfs_unlink_entry(struct btrfs_fs_info *fs, u64 dir_objectid,
                       const char *name, int is_dir) {
    if (!fs->rw)
        return -EROFS;

    usize name_len = strlen(name);
    u64 index, child;
    int rc = find_dir_index(fs, dir_objectid, name, name_len, &index, &child);

    if (rc < 0)
        return rc;

    if (is_dir) {
        int empty = 0;

        rc = dir_is_empty(fs, child, &empty);
        if (rc < 0)
            return rc;
        if (!empty)
            return -ENOTEMPTY;
    }

    struct btrfs_root_ref fr = fs_root_ref(fs);

    rc = btrfs_delete_item(fs, &fr, dir_objectid, BTRFS_DIR_INDEX_KEY, index);
    if (rc == 0)
        rc = btrfs_delete_item(fs, &fr, dir_objectid, BTRFS_DIR_ITEM_KEY,
                               name_hash(name, name_len));
    if (rc == 0)
        rc = btrfs_delete_item(fs, &fr, child, BTRFS_INODE_REF_KEY,
                               dir_objectid);
    if (rc < 0)
        return rc;

    struct btrfs_inode_item ci;

    rc = inode_load(fs, child, &ci);
    if (rc < 0)
        return rc;

    if (ci.nlink > 1) {
        /* A hard link went away; the file stays. */
        ci.nlink--;
        ci.ctime.sec = rtc_now_unix_seconds();
        ci.generation = fs->trans_gen;
        rc = inode_store(fs, child, &ci);
    } else {
        rc = file_drop_all(fs, child);
        if (rc == 0)
            rc = btrfs_delete_item(fs, &fr, child, BTRFS_INODE_ITEM_KEY, 0);
    }
    if (rc < 0)
        return rc;
    rc = dir_account(fs, dir_objectid, -(isize)(name_len * 2));
    if (rc < 0)
        return rc;
    return btrfs_run_delayed_refs(fs);
}

int btrfs_link_entry(struct btrfs_fs_info *fs, u64 dir_objectid,
                     const char *name, u64 objectid) {
    if (!fs->rw)
        return -EROFS;

    usize name_len = strlen(name);
    struct btrfs_inode_item ii;
    int rc = inode_load(fs, objectid, &ii);

    if (rc < 0)
        return rc;
    if ((ii.mode & 0170000) == 0040000)
        return -EPERM; /* no hard links to directories, as POSIX requires */

    u64 index;

    rc = next_dir_index(fs, dir_objectid, &index);
    if (rc < 0)
        return rc;

    u8 ftype = (ii.mode & 0170000) == 0120000 ? BTRFS_FT_SYMLINK
                                              : BTRFS_FT_REG_FILE;

    rc = inode_ref_insert(fs, objectid, dir_objectid, index, name, name_len);
    if (rc == 0)
        rc = dir_entry_insert(fs, dir_objectid, BTRFS_DIR_ITEM_KEY,
                              name_hash(name, name_len), objectid, ftype, name,
                              name_len);
    if (rc == 0)
        rc = dir_entry_insert(fs, dir_objectid, BTRFS_DIR_INDEX_KEY, index,
                              objectid, ftype, name, name_len);
    if (rc < 0)
        return rc;

    ii.nlink++;
    ii.ctime.sec = rtc_now_unix_seconds();
    ii.generation = fs->trans_gen;
    rc = inode_store(fs, objectid, &ii);
    if (rc < 0)
        return rc;
    rc = dir_account(fs, dir_objectid, (isize)name_len * 2);
    if (rc < 0)
        return rc;
    return btrfs_run_delayed_refs(fs);
}

int btrfs_rename_entry(struct btrfs_fs_info *fs, u64 old_dir,
                       const char *old_name, u64 new_dir,
                       const char *new_name) {
    if (!fs->rw)
        return -EROFS;

    usize old_len = strlen(old_name);
    usize new_len = strlen(new_name);
    u64 index, child;
    int rc = find_dir_index(fs, old_dir, old_name, old_len, &index, &child);

    if (rc < 0)
        return rc;

    struct btrfs_inode_item ci;

    rc = inode_load(fs, child, &ci);
    if (rc < 0)
        return rc;

    /* An existing destination is replaced, which is what rename(2) promises. */
    u64 victim_index, victim;

    if (find_dir_index(fs, new_dir, new_name, new_len, &victim_index,
                       &victim) == 0) {
        rc = btrfs_unlink_entry(fs, new_dir, new_name,
                                (ci.mode & 0170000) == 0040000);
        if (rc < 0)
            return rc;
    }

    u8 ftype = BTRFS_FT_REG_FILE;

    if ((ci.mode & 0170000) == 0040000)
        ftype = BTRFS_FT_DIR;
    else if ((ci.mode & 0170000) == 0120000)
        ftype = BTRFS_FT_SYMLINK;

    struct btrfs_root_ref fr = fs_root_ref(fs);
    u64 new_index;

    rc = next_dir_index(fs, new_dir, &new_index);
    if (rc == 0)
        rc = btrfs_delete_item(fs, &fr, old_dir, BTRFS_DIR_INDEX_KEY, index);
    if (rc == 0)
        rc = btrfs_delete_item(fs, &fr, old_dir, BTRFS_DIR_ITEM_KEY,
                               name_hash(old_name, old_len));
    if (rc == 0)
        rc = btrfs_delete_item(fs, &fr, child, BTRFS_INODE_REF_KEY, old_dir);
    if (rc == 0)
        rc = inode_ref_insert(fs, child, new_dir, new_index, new_name,
                              new_len);
    if (rc == 0)
        rc = dir_entry_insert(fs, new_dir, BTRFS_DIR_ITEM_KEY,
                              name_hash(new_name, new_len), child, ftype,
                              new_name, new_len);
    if (rc == 0)
        rc = dir_entry_insert(fs, new_dir, BTRFS_DIR_INDEX_KEY, new_index,
                              child, ftype, new_name, new_len);
    if (rc == 0)
        rc = dir_account(fs, old_dir, -(isize)(old_len * 2));
    if (rc == 0)
        rc = dir_account(fs, new_dir, (isize)new_len * 2);
    if (rc == 0)
        rc = btrfs_run_delayed_refs(fs);
    return rc;
}

int btrfs_truncate_file(struct btrfs_fs_info *fs, u64 objectid, u64 length) {
    if (!fs->rw)
        return -EROFS;

    struct btrfs_inode_item ii;
    int rc = inode_load(fs, objectid, &ii);

    if (rc < 0)
        return rc;

    u32 sector = fs->sb.sectorsize;
    u64 aligned = (length + sector - 1) & ~((u64)sector - 1);
    u64 freed = 0;

    /* Extents wholly past the new end go; one that straddles it stays, and
     * the bytes beyond i_size inside its last sector are simply not read.
     * file_drop_range refuses a partial overlap, which is what stops this
     * from silently dropping live data. */
    if (length < ii.size) {
        rc = file_drop_range(fs, objectid, aligned, (u64)-1, &freed);
        if (rc < 0)
            return rc;
    }

    ii.size = length;
    ii.nbytes = ii.nbytes > freed ? ii.nbytes - freed : 0;
    ii.generation = fs->trans_gen;
    ii.transid = fs->trans_gen;

    u64 now = rtc_now_unix_seconds();

    ii.mtime.sec = now;
    ii.mtime.nsec = 0;
    ii.ctime = ii.mtime;
    rc = inode_store(fs, objectid, &ii);
    if (rc < 0)
        return rc;
    return btrfs_run_delayed_refs(fs);
}

int btrfs_setattr_inode(struct btrfs_fs_info *fs, u64 objectid, u32 mode,
                        u32 uid, u32 gid) {
    if (!fs->rw)
        return -EROFS;

    struct btrfs_inode_item ii;
    int rc = inode_load(fs, objectid, &ii);

    if (rc < 0)
        return rc;
    ii.mode = (ii.mode & 0170000) | (mode & 07777);
    ii.uid = uid;
    ii.gid = gid;
    ii.ctime.sec = rtc_now_unix_seconds();
    ii.generation = fs->trans_gen;
    rc = inode_store(fs, objectid, &ii);
    if (rc < 0)
        return rc;
    return btrfs_run_delayed_refs(fs);
}
