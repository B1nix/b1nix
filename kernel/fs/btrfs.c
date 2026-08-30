/* btrfs — read path.
 *
 * Everything btrfs keeps is in B-trees keyed by (objectid, type, offset), and
 * reading the filesystem is walking them:
 *
 *   the superblock  names the chunk tree and the root tree by LOGICAL address
 *   the chunk tree  turns a logical address into a place on a disk
 *   the root tree   names every other tree, the FS tree among them
 *   the FS tree     holds inodes, directory entries and file extents
 *
 * The one ordering that matters is the first two: nothing can be read until
 * logical addresses can be resolved, and the chunk tree is itself at a logical
 * address. btrfs solves that by copying the chunks that cover its own chunk
 * tree into the superblock (the "system chunk array"), which is the bootstrap
 * this driver starts from.
 *
 * What is deliberately refused rather than guessed at, because reading it
 * wrongly would hand a caller bytes that are not its file:
 *
 *   - compressed and encrypted extents (the data is not the bytes on disk)
 *   - multi-device and striped/mirrored profiles (a stripe is not the file)
 *   - checksum types other than crc32c
 *
 * Each refusal names itself at mount. A filesystem this driver cannot read in
 * full is not mounted at all.
 */

#include <b1nix/btrfs.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <b1nix/errno.h>
#include <b1nix/klog.h>
#include <stdio.h>
#include <string.h>

/* ── crc32c ────────────────────────────────────────────────────────────────
 *
 * btrfs checksums every tree block with crc32c (the Castagnoli polynomial,
 * reflected), seeded with ~0 and finalised by inversion. The table is built
 * once on first use rather than stored: 1 KiB of .data for something computed
 * in a few hundred instructions is a poor trade in a kernel image. */
static u32 crc32c_table[256];
static int crc32c_ready;

static void crc32c_init(void) {
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;

        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        crc32c_table[i] = c;
    }
    crc32c_ready = 1;
}

static u32 crc32c(const void *data, usize len) {
    if (!crc32c_ready)
        crc32c_init();

    const u8 *p = data;
    u32 crc = 0xFFFFFFFFu;

    while (len--)
        crc = crc32c_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* ── Logical addressing ────────────────────────────────────────────────────*/

/* Where a logical address lives on the device, or 0 when nothing maps it.
 * Zero is not a valid answer for real data — the first megabyte of a btrfs
 * device is never mapped — so the caller can treat it as "no mapping". */
static u64 btrfs_map(struct btrfs_fs_info *fs, u64 logical) {
    for (u32 i = 0; i < fs->nchunks; i++) {
        struct btrfs_chunk_map *m = &fs->chunks[i];

        if (logical >= m->logical && logical < m->logical + m->length)
            return m->physical + (logical - m->logical);
    }
    return 0;
}

/* Record one chunk, refusing the profiles this driver cannot read. A striped
 * or mirrored chunk maps one logical range onto SEVERAL places, and reading
 * the first stripe of a striped chunk returns a sixteenth of the file with the
 * rest silently wrong. */
static int btrfs_add_chunk(struct btrfs_fs_info *fs, u64 logical,
                           const struct btrfs_chunk *chunk) {
    if (chunk->num_stripes != 1) {
        char line[96];

        snprintf(line, sizeof(line),
                 "btrfs: chunk at %llu has %u stripes; only single is read",
                 (unsigned long long)logical, (unsigned)chunk->num_stripes);
        klog_info(line);
        return -EOPNOTSUPP;
    }
    if (fs->nchunks >= BTRFS_MAX_CHUNKS)
        return -ENOMEM;

    fs->chunks[fs->nchunks].logical = logical;
    fs->chunks[fs->nchunks].length = chunk->length;
    fs->chunks[fs->nchunks].physical = chunk->stripes[0].physical;
    fs->nchunks++;
    return 0;
}

/* The bootstrap map: the chunks that cover the chunk tree itself, copied into
 * the superblock precisely so this is possible. */
static int btrfs_read_sys_chunks(struct btrfs_fs_info *fs) {
    const u8 *ptr = fs->sb.sys_chunk_array;
    const u8 *end = ptr + fs->sb.sys_chunk_array_size;

    if (fs->sb.sys_chunk_array_size > sizeof(fs->sb.sys_chunk_array))
        return -EINVAL;

    while (ptr + sizeof(struct btrfs_disk_key) + sizeof(struct btrfs_chunk) <=
           end) {
        const struct btrfs_disk_key *key = (const struct btrfs_disk_key *)ptr;
        const struct btrfs_chunk *chunk =
            (const struct btrfs_chunk *)(ptr + sizeof(*key));

        if (key->type != BTRFS_CHUNK_ITEM_KEY)
            return -EINVAL;

        usize span = sizeof(*key) + sizeof(*chunk) +
                     (usize)chunk->num_stripes * sizeof(struct btrfs_stripe);

        if (ptr + span > end)
            return -EINVAL;
        int rc = btrfs_add_chunk(fs, key->offset, chunk);

        if (rc < 0)
            return rc;
        ptr += span;
    }
    return fs->nchunks ? 0 : -EINVAL;
}

/* ── Tree blocks ───────────────────────────────────────────────────────────*/

/* Read one tree block and verify it is the block that was asked for.
 *
 * The checksum alone is not the check that matters: a stale block from a
 * previous transaction has a perfectly good checksum. What says "this is the
 * block you asked for" is its own recorded bytenr, which is why btrfs stores
 * it inside the block. The caller owns the returned buffer. */
static u8 *btrfs_read_block(struct btrfs_fs_info *fs, u64 logical) {
    u64 physical = btrfs_map(fs, logical);

    if (!physical)
        return 0;

    u32 nodesize = fs->sb.nodesize;
    u8 *buf = kmalloc(nodesize);

    if (!buf)
        return 0;
    if (blk_read_cached(fs->bdev, physical / 512, nodesize / 512, buf) < 0) {
        kfree(buf);
        return 0;
    }

    const struct btrfs_header *h = (const struct btrfs_header *)buf;

    if (h->bytenr != logical) {
        kfree(buf);
        return 0;
    }
    /* The checksum covers everything after the CSUM FIELD — all 32 bytes of
     * it, not just the four that crc32c fills. The value itself lives in the
     * first four; the rest is room for the wider checksums btrfs also
     * supports. Starting the sum four bytes in instead of thirty-two produces
     * a number that never matches, and every tree block reads as corrupt. */
    u32 want;

    memcpy(&want, h->csum, sizeof(want));
    if (crc32c(buf + sizeof(h->csum), nodesize - sizeof(h->csum)) != want) {
        kfree(buf);
        return 0;
    }
    if (memcmp(h->fsid, fs->sb.fsid, sizeof(h->fsid)) != 0) {
        kfree(buf);
        return 0;
    }
    return buf;
}

/* btrfs key order: objectid, then type, then offset. Returns <0, 0, >0. */
static int btrfs_key_cmp(const struct btrfs_disk_key *a, u64 objectid, u8 type,
                         u64 offset) {
    if (a->objectid != objectid)
        return a->objectid < objectid ? -1 : 1;
    if (a->type != type)
        return a->type < type ? -1 : 1;
    if (a->offset != offset)
        return a->offset < offset ? -1 : 1;
    return 0;
}

static const struct btrfs_item *btrfs_leaf_item(const u8 *leaf, u32 i) {
    return (const struct btrfs_item *)(leaf + sizeof(struct btrfs_header) +
                                       (usize)i * sizeof(struct btrfs_item));
}

static const u8 *btrfs_item_data(const u8 *leaf, const struct btrfs_item *it) {
    return leaf + sizeof(struct btrfs_header) + it->offset;
}

/* Descend to the leaf that would hold this key.
 *
 * Returns the leaf (caller frees) and the index of the first item at or after
 * the key. The index may equal nritems, which means "past the end of this
 * leaf" — a caller walking a range stops there rather than reading a
 * neighbouring key that belongs to a different object. */
static u8 *btrfs_search(struct btrfs_fs_info *fs, u64 root_bytenr, u8 level,
                        u64 objectid, u8 type, u64 offset, u32 *slot_out) {
    u64 block = root_bytenr;

    while (1) {
        u8 *buf = btrfs_read_block(fs, block);

        if (!buf)
            return 0;

        const struct btrfs_header *h = (const struct btrfs_header *)buf;
        u32 n = h->nritems;

        if (h->level == 0) {
            /* Leaf: first item >= the key. */
            u32 i = 0;

            while (i < n &&
                   btrfs_key_cmp(&btrfs_leaf_item(buf, i)->key, objectid, type,
                                 offset) < 0)
                i++;
            *slot_out = i;
            return buf;
        }

        /* Internal node: the LAST child whose key is <= the target, because a
         * child holds every key from its own up to the next child's. Taking
         * the first key >= target instead descends one subtree too far and
         * misses items that are there. */
        if (n == 0) {
            kfree(buf);
            return 0;
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
        block = ptrs[pick].blockptr;
        kfree(buf);
        (void)level;
    }
}

/* ── Trees ─────────────────────────────────────────────────────────────────*/

/* Walk the chunk tree and record every mapping, so addresses outside the
 * bootstrap range resolve too. */
static int btrfs_read_chunk_tree(struct btrfs_fs_info *fs) {
    u32 slot = 0;
    u8 *leaf = btrfs_search(fs, fs->sb.chunk_root, fs->sb.chunk_root_level, 0,
                            0, 0, &slot);

    if (!leaf)
        return -EIO;

    /* One leaf is enough for the sizes this driver mounts: a chunk item is
     * ~80 bytes and a 16 KiB leaf holds hundreds, which is thousands of
     * gigabytes of chunks. A filesystem whose chunk tree does not fit says so
     * rather than mapping half of itself. */
    const struct btrfs_header *h = (const struct btrfs_header *)leaf;

    if (h->level != 0) {
        kfree(leaf);
        klog_info("btrfs: chunk tree deeper than one leaf; refusing the mount");
        return -EOPNOTSUPP;
    }

    for (u32 i = 0; i < h->nritems; i++) {
        const struct btrfs_item *it = btrfs_leaf_item(leaf, i);

        if (it->key.type != BTRFS_CHUNK_ITEM_KEY)
            continue;

        const struct btrfs_chunk *chunk =
            (const struct btrfs_chunk *)btrfs_item_data(leaf, it);
        int have = 0;

        for (u32 c = 0; c < fs->nchunks; c++)
            if (fs->chunks[c].logical == it->key.offset)
                have = 1;
        if (have)
            continue; /* already known from the bootstrap array */

        int rc = btrfs_add_chunk(fs, it->key.offset, chunk);

        if (rc < 0) {
            kfree(leaf);
            return rc;
        }
    }
    kfree(leaf);
    return 0;
}

/* The FS tree's root, from the root tree. */
static int btrfs_find_fs_root(struct btrfs_fs_info *fs) {
    u32 slot = 0;
    u8 *leaf = btrfs_search(fs, fs->sb.root, fs->sb.root_level,
                            BTRFS_FS_TREE_OBJECTID, BTRFS_ROOT_ITEM_KEY, 0,
                            &slot);

    if (!leaf)
        return -EIO;

    const struct btrfs_header *h = (const struct btrfs_header *)leaf;

    if (slot >= h->nritems) {
        kfree(leaf);
        return -ENOENT;
    }

    const struct btrfs_item *it = btrfs_leaf_item(leaf, slot);

    if (it->key.objectid != BTRFS_FS_TREE_OBJECTID ||
        it->key.type != BTRFS_ROOT_ITEM_KEY ||
        it->size < sizeof(struct btrfs_root_item)) {
        kfree(leaf);
        return -ENOENT;
    }

    const struct btrfs_root_item *ri =
        (const struct btrfs_root_item *)btrfs_item_data(leaf, it);

    fs->fs_root_bytenr = ri->bytenr;
    fs->fs_root_level = ri->level;
    kfree(leaf);
    return 0;
}

/* ── Reading a file ────────────────────────────────────────────────────────*/

/* Copy one extent's contribution to the caller's range.
 *
 * `file_off` is where this extent starts in the file. Returns the number of
 * bytes placed, or a negative errno for an extent this driver will not
 * pretend to read. */
static isize btrfs_read_extent(struct btrfs_fs_info *fs,
                               const struct btrfs_file_extent_item *fi,
                               const u8 *inline_data, u32 item_size,
                               u64 file_off, u64 want_off, usize want_len,
                               char *out) {
    if (fi->compression != 0 || fi->encryption != 0) {
        klog_info("btrfs: compressed or encrypted extent; read refused");
        return -EOPNOTSUPP;
    }

    if (fi->type == BTRFS_FILE_EXTENT_INLINE) {
        u64 len = item_size > BTRFS_FILE_EXTENT_INLINE_HDR
                      ? item_size - BTRFS_FILE_EXTENT_INLINE_HDR
                      : 0;

        if (want_off < file_off || want_off >= file_off + len)
            return 0;

        u64 skip = want_off - file_off;
        usize n = (usize)(len - skip);

        if (n > want_len)
            n = want_len;
        memcpy(out, inline_data + BTRFS_FILE_EXTENT_INLINE_HDR + skip, n);
        return (isize)n;
    }

    if (fi->type != BTRFS_FILE_EXTENT_REG &&
        fi->type != BTRFS_FILE_EXTENT_PREALLOC)
        return -EOPNOTSUPP;

    /* A hole: recorded as an extent pointing nowhere. Its bytes are zeroes,
     * and saying so is part of reading the file correctly. */
    if (fi->disk_bytenr == 0) {
        if (want_off < file_off || want_off >= file_off + fi->num_bytes)
            return 0;

        u64 skip = want_off - file_off;
        usize n = (usize)(fi->num_bytes - skip);

        if (n > want_len)
            n = want_len;
        memset(out, 0, n);
        return (isize)n;
    }

    if (want_off < file_off || want_off >= file_off + fi->num_bytes)
        return 0;

    u64 skip = want_off - file_off;
    usize n = (usize)(fi->num_bytes - skip);

    if (n > want_len)
        n = want_len;

    /* The extent may be a slice of a larger allocation: `offset` is where this
     * file's data begins inside it. */
    u64 logical = fi->disk_bytenr + fi->offset + skip;
    u64 physical = btrfs_map(fs, logical);

    if (!physical)
        return -EIO;

    /* Sector-aligned read into a bounce buffer, because a file offset is not
     * a sector boundary. */
    u64 sector = physical / 512;
    u64 head = physical - sector * 512;
    usize total = (usize)(head + n);
    usize sectors = (total + 511) / 512;
    u8 *bounce = kmalloc(sectors * 512);

    if (!bounce)
        return -ENOMEM;
    if (blk_read_cached(fs->bdev, sector, sectors, bounce) < 0) {
        kfree(bounce);
        return -EIO;
    }
    memcpy(out, bounce + head, n);
    kfree(bounce);
    return (isize)n;
}

static isize btrfs_vfs_read(struct vfs_node *node, u64 offset, char *buffer,
                            usize size, int flags) {
    (void)flags;
    struct btrfs_file_info *fi =
        node && node->inode ? (struct btrfs_file_info *)node->inode->data : 0;

    if (!fi)
        return -EIO;
    if (offset >= fi->size)
        return 0;
    if (offset + size > fi->size)
        size = (usize)(fi->size - offset);

    usize done = 0;

    while (done < size) {
        u64 want = offset + done;
        u32 slot = 0;
        /* The extent that CONTAINS this offset starts at or before it, so the
         * search is for the last EXTENT_DATA at or below `want`. */
        u8 *leaf = btrfs_search(fi->fs, fi->fs->fs_root_bytenr,
                                fi->fs->fs_root_level, fi->objectid,
                                BTRFS_EXTENT_DATA_KEY, want, &slot);

        if (!leaf)
            return done ? (isize)done : -EIO;

        const struct btrfs_header *h = (const struct btrfs_header *)leaf;
        const struct btrfs_item *it = 0;

        /* btrfs_search lands on the first key >= want; the extent covering
         * `want` is that one only if it starts exactly there, otherwise it is
         * the one before. */
        if (slot < h->nritems &&
            btrfs_key_cmp(&btrfs_leaf_item(leaf, slot)->key, fi->objectid,
                          BTRFS_EXTENT_DATA_KEY, want) == 0) {
            it = btrfs_leaf_item(leaf, slot);
        } else if (slot > 0) {
            const struct btrfs_item *prev = btrfs_leaf_item(leaf, slot - 1);

            if (prev->key.objectid == fi->objectid &&
                prev->key.type == BTRFS_EXTENT_DATA_KEY)
                it = prev;
        }

        if (!it) {
            /* NO_HOLES: a range with no extent at all is a hole. */
            memset(buffer + done, 0, size - done);
            done = size;
            kfree(leaf);
            break;
        }

        const struct btrfs_file_extent_item *fe =
            (const struct btrfs_file_extent_item *)btrfs_item_data(leaf, it);
        isize n = btrfs_read_extent(fi->fs, fe, btrfs_item_data(leaf, it),
                                    it->size, it->key.offset, want,
                                    size - done, buffer + done);

        kfree(leaf);
        if (n < 0)
            return done ? (isize)done : n;
        if (n == 0) {
            /* Nothing covers this offset: the rest of the range is a hole. */
            memset(buffer + done, 0, size - done);
            done = size;
            break;
        }
        done += (usize)n;
    }
    return (isize)done;
}

/* ── Building the tree the VFS shows ───────────────────────────────────────*/

static int btrfs_inode_of(struct btrfs_fs_info *fs, u64 objectid,
                          struct btrfs_inode_item *out) {
    u32 slot = 0;
    u8 *leaf = btrfs_search(fs, fs->fs_root_bytenr, fs->fs_root_level, objectid,
                            BTRFS_INODE_ITEM_KEY, 0, &slot);

    if (!leaf)
        return -EIO;

    const struct btrfs_header *h = (const struct btrfs_header *)leaf;

    if (slot >= h->nritems) {
        kfree(leaf);
        return -ENOENT;
    }

    const struct btrfs_item *it = btrfs_leaf_item(leaf, slot);

    if (it->key.objectid != objectid || it->key.type != BTRFS_INODE_ITEM_KEY ||
        it->size < sizeof(*out)) {
        kfree(leaf);
        return -ENOENT;
    }
    memcpy(out, btrfs_item_data(leaf, it), sizeof(*out));
    kfree(leaf);
    return 0;
}

static void btrfs_populate_dir(struct btrfs_fs_info *fs, u64 dir_objectid,
                               const char *dir_path, int depth);

/* One directory entry becomes one VFS node. */
static void btrfs_add_entry(struct btrfs_fs_info *fs, const char *dir_path,
                            const char *name, u8 ftype, u64 objectid,
                            int depth) {
    char path[VFS_MAX_PATH];
    usize plen = strlen(dir_path);
    usize nlen = strlen(name);

    if (plen + 1 + nlen + 1 > sizeof(path))
        return;
    memcpy(path, dir_path, plen);
    if (plen == 0 || path[plen - 1] != '/')
        path[plen++] = '/';
    memcpy(path + plen, name, nlen + 1);

    struct btrfs_inode_item inode;

    if (btrfs_inode_of(fs, objectid, &inode) < 0)
        return;

    if (ftype == BTRFS_FT_DIR) {
        struct vfs_node *node = vfs_add_node(path, VFS_DIRECTORY, 0, 0, 0);

        if (node) {
            node->inode->mode = (u16)(inode.mode & 07777);
            node->inode->uid = inode.uid;
            node->inode->gid = inode.gid;
            node->inode->mtime = inode.mtime.sec;
        }
        btrfs_populate_dir(fs, objectid, path, depth + 1);
        return;
    }

    if (ftype == BTRFS_FT_SYMLINK) {
        /* A symlink's target is its file content — an inline extent — so it is
         * read the same way anything else is. */
        struct btrfs_file_info tmp = {.fs = fs,
                                      .objectid = objectid,
                                      .size = inode.size};
        char target[VFS_MAX_PATH];
        usize want = inode.size < sizeof(target) - 1 ? (usize)inode.size
                                                     : sizeof(target) - 1;
        struct vfs_inode fake_inode = {0};
        struct vfs_node fake_node = {0};

        fake_inode.data = &tmp;
        fake_node.inode = &fake_inode;

        isize n = btrfs_vfs_read(&fake_node, 0, target, want, 0);

        if (n <= 0)
            return;
        target[n] = '\0';
        (void)vfs_symlink(target, path);
        return;
    }

    struct vfs_node *node =
        vfs_add_node(path, VFS_FILE, 0, (usize)inode.size, 0);

    if (!node)
        return;

    struct btrfs_file_info *fi = kzalloc(sizeof(*fi));

    if (!fi)
        return;
    fi->fs = fs;
    fi->objectid = objectid;
    fi->size = inode.size;
    node->inode->data = fi;
    node->inode->read_cb = btrfs_vfs_read;
    node->inode->mode = (u16)(inode.mode & 07777);
    node->inode->uid = inode.uid;
    node->inode->gid = inode.gid;
    node->inode->mtime = inode.mtime.sec;
}

/* Walk one directory's DIR_INDEX items, which are its entries in the order
 * they were created. DIR_ITEM would do as well; the index is used because its
 * keys are sequential, so one search finds the first and the rest follow. */
static void btrfs_populate_dir(struct btrfs_fs_info *fs, u64 dir_objectid,
                               const char *dir_path, int depth) {
    if (depth > 16) {
        klog_info("btrfs: directory nesting past 16 levels; not descending");
        return;
    }

    u64 index = 0;

    while (1) {
        u32 slot = 0;
        u8 *leaf = btrfs_search(fs, fs->fs_root_bytenr, fs->fs_root_level,
                                dir_objectid, BTRFS_DIR_INDEX_KEY, index,
                                &slot);

        if (!leaf)
            return;

        const struct btrfs_header *h = (const struct btrfs_header *)leaf;

        if (slot >= h->nritems) {
            kfree(leaf);
            return; /* no more entries in this directory */
        }

        u64 next_index = 0;
        int found = 0;

        for (u32 i = slot; i < h->nritems; i++) {
            const struct btrfs_item *it = btrfs_leaf_item(leaf, i);

            if (it->key.objectid != dir_objectid ||
                it->key.type != BTRFS_DIR_INDEX_KEY)
                break;

            const struct btrfs_dir_item *di =
                (const struct btrfs_dir_item *)btrfs_item_data(leaf, it);
            const char *name = (const char *)(di + 1);
            char namebuf[256];
            usize nlen = di->name_len < sizeof(namebuf) - 1
                             ? di->name_len
                             : sizeof(namebuf) - 1;

            memcpy(namebuf, name, nlen);
            namebuf[nlen] = '\0';

            /* Entries that name another TREE (a subvolume) are not files in
             * this one. Reading them as inodes of this tree would report
             * whatever inode happens to carry that number here. */
            if (di->location.type == BTRFS_INODE_ITEM_KEY)
                btrfs_add_entry(fs, dir_path, namebuf, di->type,
                                di->location.objectid, depth);

            next_index = it->key.offset + 1;
            found = 1;
        }
        kfree(leaf);
        if (!found)
            return;
        index = next_index;
    }
}

/* ── Mount ─────────────────────────────────────────────────────────────────*/

static int btrfs_check_features(struct btrfs_fs_info *fs) {
    if (fs->sb.csum_type != 0) {
        klog_info("btrfs: checksum type is not crc32c; refusing the mount");
        return -EOPNOTSUPP;
    }
    if (fs->sb.num_devices != 1) {
        klog_info("btrfs: multi-device filesystem; refusing the mount");
        return -EOPNOTSUPP;
    }
    if (fs->sb.nodesize == 0 || fs->sb.nodesize % 512 ||
        fs->sb.nodesize > 65536) {
        klog_info("btrfs: unusable node size; refusing the mount");
        return -EINVAL;
    }
    return 0;
}

static struct vfs_node *btrfs_vfs_mount_cb(const char *source, u64 flags,
                                           void *data) {
    (void)flags;
    const char *target = (const char *)data;
    struct block_device *dev = blk_get(source);

    if (!dev)
        return ERR_PTR(-ENODEV);

    u8 *sb_buf = kmalloc(4096);

    if (!sb_buf)
        return ERR_PTR(-ENOMEM);
    if (blk_read_cached(dev, BTRFS_SUPER_INFO_OFFSET / 512, 8, sb_buf) < 0) {
        kfree(sb_buf);
        return ERR_PTR(-EIO);
    }

    struct btrfs_super_block *sb = (struct btrfs_super_block *)sb_buf;

    if (memcmp(sb->magic, BTRFS_MAGIC, 8) != 0) {
        kfree(sb_buf);
        return ERR_PTR(-EINVAL);
    }

    struct btrfs_fs_info *fs = kzalloc(sizeof(*fs));

    if (!fs) {
        kfree(sb_buf);
        return ERR_PTR(-ENOMEM);
    }
    fs->bdev = dev;
    memcpy(&fs->sb, sb, sizeof(fs->sb));
    kfree(sb_buf);

    int rc = btrfs_check_features(fs);

    if (rc == 0)
        rc = btrfs_read_sys_chunks(fs);
    if (rc == 0)
        rc = btrfs_read_chunk_tree(fs);
    if (rc == 0)
        rc = btrfs_find_fs_root(fs);
    if (rc < 0) {
        kfree(fs);
        return ERR_PTR(rc);
    }

    {
        char line[160];

        snprintf(line, sizeof(line),
                 "btrfs: %s label=\"%s\" nodesize=%u chunks=%u fs-root=%llu "
                 "level=%u", source, fs->sb.label[0] ? fs->sb.label : "(none)",
                 (unsigned)fs->sb.nodesize, (unsigned)fs->nchunks,
                 (unsigned long long)fs->fs_root_bytenr,
                 (unsigned)fs->fs_root_level);
        klog_info(line);
    }

    struct vfs_node *root = vfs_create_node(VFS_DIRECTORY);

    if (!root) {
        kfree(fs);
        return ERR_PTR(-ENOMEM);
    }
    root->inode->data = fs;
    root->inode->blk_dev = dev;
    root->inode->mode = 0755;

    /* The tree is built now rather than on demand: the mount point has to be
     * published before paths under it resolve, which is what
     * vfs_set_currently_mounting_root does, and every node below is created by
     * absolute path. */
    vfs_set_currently_mounting_root(root);
    btrfs_populate_dir(fs, BTRFS_FIRST_FREE_OBJECTID,
                       target && target[0] ? target : "/", 0);

    return root;
}

static struct vfs_fs btrfs_vfs = {
    .name = "btrfs",
    .mount = btrfs_vfs_mount_cb,
};

int btrfs_mount_root(const char *device_name, const char *mount_point) {
    return vfs_mount(device_name, mount_point, "btrfs", 0);
}

void btrfs_init(void) {
    vfs_register_fs(&btrfs_vfs);
}

/* ── M95: btrfs is a loadable module ─────────────────────────────────────── */
#include <b1nix/module.h>

MODULE_NAME("btrfs");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("b1nix");
MODULE_DESCRIPTION("btrfs read support: chunk map, B-tree walk, files and dirs");
MODULE_ALIAS("fs-btrfs");

static int btrfs_module_init(void) {
    btrfs_init();
    return 0;
}

static void btrfs_module_exit(void) { vfs_unregister_fs(&btrfs_vfs); }

module_init(btrfs_module_init);
module_exit(btrfs_module_exit);
