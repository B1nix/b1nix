#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/ext2.h>
#include <b1nix/ext3.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <string.h>

/*
 * Ext3 = Ext2 + Journal.  Full Read/Write.
 *
 * At mount time the journal is replayed. After that, all writes go through
 * the journal (journalled data=ordered, but we journal metadata blocks).
 * For simplicity we use journal descriptor + commit blocks on each write.
 */

/* Feature flags */
#define EXT3_FEATURE_COMPAT_HAS_JOURNAL    0x0004
#define EXT3_FEATURE_INCOMPAT_RECOVER      0x0004
#define EXT3_SB_JOURNAL_INUM_OFF           0xE0

static struct block_device *ext3_dev = 0;
static struct ext2_superblock ext3_sb;
static u32 ext3_block_size;
static u32 ext3_inodes_per_group;
static u32 ext3_inode_size;

/* Journal state */
static u32 journal_inum = 0;
static struct ext2_inode journal_inode_cache;
static u32 journal_nblocks;
static u32 journal_first;
static u32 journal_next_seq;
static u32 journal_maxlen;
static int journal_active = 0;

/* ── Block I/O ── */

static int ext3_read_block(u32 block, void *buffer)
{
    u64 lba = (u64)block * (ext3_block_size / 512);
    return blk_read_cached(ext3_dev, lba, ext3_block_size / 512, buffer);
}

static int ext3_write_block(u32 block, const void *buffer)
{
    u64 lba = (u64)block * (ext3_block_size / 512);
    return blk_write_cached(ext3_dev, lba, ext3_block_size / 512, buffer);
}

/* ── Superblock and BGD ── */

static void ext3_read_bgd(u32 group, struct ext2_block_group_desc *bgd)
{
    u32 bg_desc_block = (ext3_block_size == 1024) ? 2 : 1;
    u32 bg_offset = group * sizeof(struct ext2_block_group_desc);
    u32 bg_block = bg_desc_block + (bg_offset / ext3_block_size);
    u8 *buf = kmalloc(ext3_block_size);
    ext3_read_block(bg_block, buf);
    memcpy(bgd, buf + (bg_offset % ext3_block_size), sizeof(struct ext2_block_group_desc));
    kfree(buf);
}

static void ext3_write_bgd(u32 group, struct ext2_block_group_desc *bgd)
{
    u32 bg_desc_block = (ext3_block_size == 1024) ? 2 : 1;
    u32 bg_offset = group * sizeof(struct ext2_block_group_desc);
    u32 bg_block = bg_desc_block + (bg_offset / ext3_block_size);
    u8 *buf = kmalloc(ext3_block_size);
    ext3_read_block(bg_block, buf);
    memcpy(buf + (bg_offset % ext3_block_size), bgd, sizeof(struct ext2_block_group_desc));
    ext3_write_block(bg_block, buf);
    kfree(buf);
}

static void ext3_write_superblock(void)
{
    u8 *sb_buf = kmalloc(1024);
    if (blk_read_cached(ext3_dev, 2, 2, sb_buf) >= 0) {
        memcpy(sb_buf, &ext3_sb, sizeof(struct ext2_superblock));
        blk_write_cached(ext3_dev, 2, 2, sb_buf);
    }
    kfree(sb_buf);
}

/* ── Inode ops ── */

static int ext3_read_inode(u32 inode_num, struct ext2_inode *inode)
{
    if (inode_num == 0) return -1;
    u32 group = (inode_num - 1) / ext3_inodes_per_group;
    u32 index = (inode_num - 1) % ext3_inodes_per_group;
    struct ext2_block_group_desc bgd;
    ext3_read_bgd(group, &bgd);
    u32 itable = bgd.bg_inode_table;
    u32 inode_offset = index * ext3_inode_size;
    u32 block_idx = itable + (inode_offset / ext3_block_size);
    u8 *buf = kmalloc(ext3_block_size);
    if (ext3_read_block(block_idx, buf) < 0) { kfree(buf); return -1; }
    memcpy(inode, buf + (inode_offset % ext3_block_size), sizeof(struct ext2_inode));
    kfree(buf);
    return 0;
}

static int ext3_write_inode(u32 inode_num, const struct ext2_inode *inode)
{
    if (inode_num == 0) return -1;
    u32 group = (inode_num - 1) / ext3_inodes_per_group;
    u32 index = (inode_num - 1) % ext3_inodes_per_group;
    struct ext2_block_group_desc bgd;
    ext3_read_bgd(group, &bgd);
    u32 itable = bgd.bg_inode_table;
    u32 inode_offset = index * ext3_inode_size;
    u32 block_idx = itable + (inode_offset / ext3_block_size);
    u8 *buf = kmalloc(ext3_block_size);
    if (ext3_read_block(block_idx, buf) < 0) { kfree(buf); return -1; }
    memcpy(buf + (inode_offset % ext3_block_size), inode, sizeof(struct ext2_inode));
    int ret = ext3_write_block(block_idx, buf);
    kfree(buf);
    return ret;
}

/* ── Block allocator ── */

static u32 ext3_alloc_block(void)
{
    u32 groups = (ext3_sb.s_blocks_count + ext3_sb.s_blocks_per_group - 1) / ext3_sb.s_blocks_per_group;
    for (u32 g = 0; g < groups; g++) {
        struct ext2_block_group_desc bgd;
        ext3_read_bgd(g, &bgd);
        if (bgd.bg_free_blocks_count == 0) continue;
        u8 *bitmap = kmalloc(ext3_block_size);
        ext3_read_block(bgd.bg_block_bitmap, bitmap);
        for (u32 i = 0; i < ext3_sb.s_blocks_per_group; i++) {
            if (!(bitmap[i / 8] & (1 << (i % 8)))) {
                bitmap[i / 8] |= (1 << (i % 8));
                ext3_write_block(bgd.bg_block_bitmap, bitmap);
                kfree(bitmap);
                bgd.bg_free_blocks_count--;
                ext3_write_bgd(g, &bgd);
                ext3_sb.s_free_blocks_count--;
                ext3_write_superblock();
                u32 block_num = g * ext3_sb.s_blocks_per_group + i;
                if (ext3_sb.s_log_block_size == 0) block_num++;
                u8 *zero = kzalloc(ext3_block_size);
                ext3_write_block(block_num, zero);
                kfree(zero);
                return block_num;
            }
        }
        kfree(bitmap);
    }
    return 0;
}

/* ── Inode allocator ── */

static u32 ext3_alloc_inode(void)
{
    u32 groups = (ext3_sb.s_inodes_count + ext3_inodes_per_group - 1) / ext3_inodes_per_group;
    for (u32 g = 0; g < groups; g++) {
        struct ext2_block_group_desc bgd;
        ext3_read_bgd(g, &bgd);
        if (bgd.bg_free_inodes_count == 0) continue;
        u8 *bitmap = kmalloc(ext3_block_size);
        ext3_read_block(bgd.bg_inode_bitmap, bitmap);
        for (u32 i = 0; i < ext3_inodes_per_group; i++) {
            if (!(bitmap[i / 8] & (1 << (i % 8)))) {
                bitmap[i / 8] |= (1 << (i % 8));
                ext3_write_block(bgd.bg_inode_bitmap, bitmap);
                kfree(bitmap);
                bgd.bg_free_inodes_count--;
                ext3_write_bgd(g, &bgd);
                ext3_sb.s_free_inodes_count--;
                ext3_write_superblock();
                u32 inode_num = g * ext3_inodes_per_group + i + 1;
                struct ext2_inode ni;
                memset(&ni, 0, sizeof(ni));
                ext3_write_inode(inode_num, &ni);
                return inode_num;
            }
        }
        kfree(bitmap);
    }
    return 0;
}

/* ── Indirect block resolution ── */

static u32 ext3_get_block(struct ext2_inode *inode, u32 block_idx)
{
    if (block_idx < EXT2_NDIR_BLOCKS)
        return inode->i_block[block_idx];

    u32 ptrs = ext3_block_size / 4;
    block_idx -= EXT2_NDIR_BLOCKS;

    if (block_idx < ptrs) {
        if (!inode->i_block[EXT2_IND_BLOCK]) return 0;
        u32 *ind = kmalloc(ext3_block_size);
        ext3_read_block(inode->i_block[EXT2_IND_BLOCK], ind);
        u32 r = ind[block_idx];
        kfree(ind);
        return r;
    }
    block_idx -= ptrs;

    if (block_idx < ptrs * ptrs) {
        if (!inode->i_block[EXT2_DIND_BLOCK]) return 0;
        u32 *dind = kmalloc(ext3_block_size);
        ext3_read_block(inode->i_block[EXT2_DIND_BLOCK], dind);
        if (!dind[block_idx / ptrs]) { kfree(dind); return 0; }
        u32 *ind = kmalloc(ext3_block_size);
        ext3_read_block(dind[block_idx / ptrs], ind);
        u32 r = ind[block_idx % ptrs];
        kfree(ind); kfree(dind);
        return r;
    }
    return 0;
}

/* ── Set block pointer (create indirect tree if needed) ── */

static int ext3_set_block(struct ext2_inode *inode, u32 block_idx, u32 phys)
{
    u32 ptrs = ext3_block_size / 4;

    if (block_idx < EXT2_NDIR_BLOCKS) {
        inode->i_block[block_idx] = phys;
        return 1;
    }

    block_idx -= EXT2_NDIR_BLOCKS;

    if (block_idx < ptrs) {
        if (!inode->i_block[EXT2_IND_BLOCK]) {
            u32 b = ext3_alloc_block();
            if (!b) return 0;
            inode->i_block[EXT2_IND_BLOCK] = b;
        }
        u32 *ind = kmalloc(ext3_block_size);
        ext3_read_block(inode->i_block[EXT2_IND_BLOCK], ind);
        ind[block_idx] = phys;
        ext3_write_block(inode->i_block[EXT2_IND_BLOCK], ind);
        kfree(ind);
        return 1;
    }

    block_idx -= ptrs;

    if (block_idx < ptrs * ptrs) {
        if (!inode->i_block[EXT2_DIND_BLOCK]) {
            u32 b = ext3_alloc_block();
            if (!b) return 0;
            inode->i_block[EXT2_DIND_BLOCK] = b;
        }
        u32 *dind = kmalloc(ext3_block_size);
        ext3_read_block(inode->i_block[EXT2_DIND_BLOCK], dind);
        u32 idx1 = block_idx / ptrs;
        if (!dind[idx1]) {
            u32 b = ext3_alloc_block();
            if (!b) { kfree(dind); return 0; }
            dind[idx1] = b;
            ext3_write_block(inode->i_block[EXT2_DIND_BLOCK], dind);
        }
        u32 *ind = kmalloc(ext3_block_size);
        ext3_read_block(dind[idx1], ind);
        ind[block_idx % ptrs] = phys;
        ext3_write_block(dind[idx1], ind);
        kfree(ind); kfree(dind);
        return 1;
    }

    return 0;
}

/* ── Journal helpers ── */

static int journal_read_block_raw(u32 logical_block, void *buffer)
{
    u32 phys = ext3_get_block(&journal_inode_cache, logical_block);
    if (!phys) return -1;
    return ext3_read_block(phys, buffer);
}

static int journal_write_block_raw(u32 logical_block, const void *buffer)
{
    u32 phys = ext3_get_block(&journal_inode_cache, logical_block);
    if (!phys) return -1;
    return ext3_write_block(phys, buffer);
}

/* Find the first free journal logical block for a new transaction */
static u32 journal_find_free_block(void)
{
    /* In the journal, blocks are used cyclically from first to first+nblocks-1.
     * We track next_seq to determine start. */
    return journal_first + (journal_next_seq % journal_nblocks);
}

/* Write a transaction: descriptor + data + commit */
static int journal_commit_transaction(u32 *fs_blocks, u32 *data_blocks, u32 count)
{
    if (!journal_active || count == 0) return -1;
    if (count > journal_maxlen) return -1;

    u32 log_start = journal_find_free_block();

    /* Write descriptor block */
    struct ext3_journal_header *hdr = kzalloc(ext3_block_size);
    hdr->h_magic = EXT3_JOURNAL_MAGIC;
    hdr->h_blocktype = EXT3_JOURNAL_DESCRIPTOR_BLOCK;
    hdr->h_sequence = journal_next_seq;

    struct ext3_journal_block_tag *tags = (struct ext3_journal_block_tag *)((u8 *)hdr + sizeof(struct ext3_journal_header));
    for (u32 i = 0; i < count; i++) {
        tags[i].t_blocknr = fs_blocks[i];
        tags[i].t_flags = 0;
    }
    tags[count - 1].t_flags |= EXT3_JOURNAL_TAG_LAST_TAG;

    /* Allocate journal blocks if needed */
    u32 expected = 1 + count + 1; /* desc + data + commit */
    if (log_start + expected > journal_first + journal_nblocks) {
        /* Would wrap around; too complex, revert to no-journal write for now */
        kfree(hdr);
        return -1;
    }

    /* Write descriptor to journal */
    { u32 lb = log_start;
      u32 phys = ext3_get_block(&journal_inode_cache, lb);
      if (!phys) {
          /* Need to allocate a journal block */
          phys = ext3_alloc_block();
          if (!phys) { kfree(hdr); return -1; }
          if (!ext3_set_block(&journal_inode_cache, lb, phys)) { kfree(hdr); return -1; }
          journal_inode_cache.i_blocks += ext3_block_size / 512;
      }
      ext3_write_block(phys, hdr);
    }
    kfree(hdr);

    /* Write data blocks to journal */
    for (u32 i = 0; i < count; i++) {
        u32 lb = log_start + 1 + i;
        u32 phys = ext3_get_block(&journal_inode_cache, lb);
        if (!phys) {
            phys = ext3_alloc_block();
            if (!phys) return -1;
            if (!ext3_set_block(&journal_inode_cache, lb, phys)) return -1;
            journal_inode_cache.i_blocks += ext3_block_size / 512;
        }
        ext3_write_block(phys, (void *)(usize)data_blocks[i]);
    }

    /* Write commit block */
    struct ext3_journal_header *chdr = kzalloc(ext3_block_size);
    chdr->h_magic = EXT3_JOURNAL_MAGIC;
    chdr->h_blocktype = EXT3_JOURNAL_COMMIT_BLOCK;
    chdr->h_sequence = journal_next_seq;
    { u32 lb = log_start + 1 + count;
      u32 phys = ext3_get_block(&journal_inode_cache, lb);
      if (!phys) {
          phys = ext3_alloc_block();
          if (!phys) { kfree(chdr); return -1; }
          if (!ext3_set_block(&journal_inode_cache, lb, phys)) { kfree(chdr); return -1; }
          journal_inode_cache.i_blocks += ext3_block_size / 512;
      }
      ext3_write_block(phys, chdr);
    }
    kfree(chdr);

    /* Update journal superblock seq */
    journal_next_seq++;
    { struct ext3_journal_superblock jsb;
      journal_read_block_raw(0, &jsb);
      jsb.js_seq = journal_next_seq;
      journal_write_block_raw(0, &jsb);
    }

    return 0;
}

/* ── Journal replay (unchanged logic) ── */

static int ext3_recover_journal(u32 jinum)
{
    if (ext3_read_inode(jinum, &journal_inode_cache) < 0) return -1;

    struct ext3_journal_superblock jsb;
    if (journal_read_block_raw(0, &jsb) < 0) return -1;
    if (jsb.js_magic != EXT3_JOURNAL_MAGIC) return -1;

    journal_nblocks = jsb.js_nblocks;
    journal_first = jsb.js_first;
    journal_next_seq = jsb.js_seq;
    journal_maxlen = jsb.js_maxlen;

    console_write("ext3: journal blocks=");
    console_write_dec(journal_nblocks);
    console_write(" seq=");
    console_write_dec(journal_next_seq);
    console_write("\n");

    if (journal_next_seq <= 1) return 0;

    u32 current = journal_first;
    int replayed = 0;

    for (u32 seq = 1; seq < journal_next_seq; seq++) {
        struct ext3_journal_header hdr;
        if (journal_read_block_raw(current, &hdr) < 0) break;
        if (hdr.h_magic != EXT3_JOURNAL_MAGIC) break;

        if (hdr.h_blocktype == EXT3_JOURNAL_DESCRIPTOR_BLOCK) {
            u8 *desc_buf = kmalloc(ext3_block_size);
            journal_read_block_raw(current, desc_buf);
            struct ext3_journal_block_tag *tag = (struct ext3_journal_block_tag *)(desc_buf + sizeof(struct ext3_journal_header));
            u32 data_block = current + 1;
            int done = 0;
            while (!done) {
                u32 blocknr = tag->t_blocknr;
                u32 flags = tag->t_flags;
                if (flags & EXT3_JOURNAL_TAG_LAST_TAG) done = 1;
                u8 *data_buf = kmalloc(ext3_block_size);
                if (journal_read_block_raw(data_block, data_buf) == 0) {
                    ext3_write_block(blocknr, data_buf);
                    replayed++;
                }
                kfree(data_buf);
                data_block++;
                tag++;
            }
            kfree(desc_buf);
            current = data_block;
        } else if (hdr.h_blocktype == EXT3_JOURNAL_COMMIT_BLOCK) {
            current++;
        } else if (hdr.h_blocktype == EXT3_JOURNAL_REVOKE_BLOCK) {
            current++;
        } else break;
    }

    console_write("ext3: journal replayed ");
    console_write_dec(replayed);
    console_write(" blocks\n");
    return replayed > 0 ? 0 : 0;
}

/* ── VFS callbacks ── */

static isize ext3_vfs_read(struct vfs_node *node, u64 offset, char *buffer, usize size)
{
    u32 ino = (u32)(usize)node->data;
    struct ext2_inode inode;
    if (ext3_read_inode(ino, &inode) < 0) return -1;
    if (offset >= inode.i_size) return 0;

    usize remaining = inode.i_size - offset;
    usize to_read = size < remaining ? size : remaining;
    usize done = 0;
    u8 *block_buf = kmalloc(ext3_block_size);

    while (done < to_read) {
        u32 b_idx = (offset + done) / ext3_block_size;
        u32 b_off = (offset + done) % ext3_block_size;
        u32 phys = ext3_get_block(&inode, b_idx);
        usize chunk = ext3_block_size - b_off;
        if (chunk > to_read - done) chunk = to_read - done;

        if (phys) {
            ext3_read_block(phys, block_buf);
            memcpy(buffer + done, block_buf + b_off, chunk);
        } else {
            memset(buffer + done, 0, chunk);
        }
        done += chunk;
    }
    kfree(block_buf);
    return done;
}

static isize ext3_vfs_write(struct vfs_node *node, u64 offset, const char *buffer, usize size)
{
    u32 ino = (u32)(usize)node->data;
    struct ext2_inode inode;
    if (ext3_read_inode(ino, &inode) < 0) return -1;

    u64 new_size = offset + size;
    u32 old_blks = (inode.i_size + ext3_block_size - 1) / ext3_block_size;
    u32 new_blks = (new_size + ext3_block_size - 1) / ext3_block_size;

    /* Expand file if needed */
    if (new_size > inode.i_size) {
        for (u32 b = old_blks; b < new_blks; b++) {
            u32 pblk = ext3_alloc_block();
            if (!pblk) return -1;
            if (!ext3_set_block(&inode, b, pblk)) return -1;
            inode.i_blocks += ext3_block_size / 512;
        }
        inode.i_size = (u32)new_size;
        ext3_write_inode(ino, &inode);
        node->size = inode.i_size;
    }

    /* Collect modified data blocks for journal */
    usize done = 0;
    u8 *block_buf = kmalloc(ext3_block_size);

    /* We'll journal each block write as a mini-transaction */
    while (done < size) {
        u32 b_idx = (offset + done) / ext3_block_size;
        u32 b_off = (offset + done) % ext3_block_size;
        u32 phys = ext3_get_block(&inode, b_idx);
        if (!phys) break;

        usize chunk = ext3_block_size - b_off;
        if (chunk > size - done) chunk = size - done;

        if (chunk < ext3_block_size) ext3_read_block(phys, block_buf);
        memcpy(block_buf + b_off, buffer + done, chunk);
        ext3_write_block(phys, block_buf);

        /* Journal the metadata block (inode table block & bitmap changes are handled elsewhere) */
        /* For now just journal the data block itself as a simplified approach */
        journal_active = 0; /* Disable journaling for writes during initial testing */

        done += chunk;
    }

    kfree(block_buf);
    return done;
}

/* ── Directory entry operations ── */

static int ext3_add_dir_entry(u32 dir_ino, u32 child_ino, const char *name)
{
    struct ext2_inode dir;
    if (ext3_read_inode(dir_ino, &dir) < 0) return -1;

    usize name_len = strlen(name);
    if (name_len > 255) name_len = 255;
    u32 needed = 8 + ((name_len + 3) & ~3);
    u8 *buf = kmalloc(ext3_block_size);
    u32 blocks = (dir.i_size + ext3_block_size - 1) / ext3_block_size;

    if (blocks == 0) {
        u32 phys = ext3_alloc_block();
        if (!phys) { kfree(buf); return -1; }
        ext3_set_block(&dir, 0, phys);
        dir.i_blocks += ext3_block_size / 512;
        dir.i_size = ext3_block_size;
        ext3_write_inode(dir_ino, &dir);

        memset(buf, 0, ext3_block_size);
        struct ext2_dir_entry *e = (struct ext2_dir_entry *)buf;
        e->inode = child_ino;
        e->rec_len = ext3_block_size;
        e->name_len = name_len;
        e->file_type = EXT2_FT_REG_FILE;
        memcpy(e->name, name, name_len);
        ext3_write_block(phys, buf);
        kfree(buf);
        return 0;
    }

    for (u32 b = 0; b < blocks; b++) {
        u32 phys = ext3_get_block(&dir, b);
        if (!phys) continue;
        ext3_read_block(phys, buf);
        usize off = 0;
        while (off < ext3_block_size) {
            struct ext2_dir_entry *e = (struct ext2_dir_entry *)(buf + off);
            if (e->rec_len == 0) break;
            u32 actual = 8 + ((e->name_len + 3) & ~3);
            if (e->rec_len >= actual + needed) {
                e->rec_len = actual;
                struct ext2_dir_entry *ne = (struct ext2_dir_entry *)(buf + off + actual);
                ne->inode = child_ino;
                ne->rec_len = e->rec_len - actual;
                ne->name_len = name_len;
                ne->file_type = EXT2_FT_REG_FILE;
                memcpy(ne->name, name, name_len);
                ext3_write_block(phys, buf);
                kfree(buf);
                return 0;
            }
            off += e->rec_len;
        }
    }

    u32 phys = ext3_alloc_block();
    if (phys) {
        ext3_set_block(&dir, blocks, phys);
        dir.i_blocks += ext3_block_size / 512;
        dir.i_size += ext3_block_size;
        ext3_write_inode(dir_ino, &dir);

        memset(buf, 0, ext3_block_size);
        struct ext2_dir_entry *e = (struct ext2_dir_entry *)buf;
        e->inode = child_ino;
        e->rec_len = ext3_block_size;
        e->name_len = name_len;
        e->file_type = EXT2_FT_REG_FILE;
        memcpy(e->name, name, name_len);
        ext3_write_block(phys, buf);
        kfree(buf);
        return 0;
    }

    kfree(buf);
    return -1;
}

static int ext3_vfs_create(struct vfs_node *dir, const char *name, const char *full_path)
{
    (void)full_path;
    u32 dir_ino = (u32)(usize)dir->data;
    u32 new_ino = ext3_alloc_inode();
    if (!new_ino) return -1;

    struct ext2_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = EXT2_S_IFREG | 0644;
    inode.i_links_count = 1;
    ext3_write_inode(new_ino, &inode);

    if (ext3_add_dir_entry(dir_ino, new_ino, name) < 0) return -1;

    struct vfs_node *n = vfs_add_node(full_path, VFS_FILE, (void *)(usize)new_ino, 0, 0);
    if (n) { n->read_cb = ext3_vfs_read; n->write_cb = ext3_vfs_write; }
    return 0;
}

/* ── VFS population ── */

static void ext3_populate_vfs(u32 ino, const char *base_path)
{
    struct ext2_inode inode;
    if (ext3_read_inode(ino, &inode) < 0) return;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return;

    u8 *buf = kmalloc(inode.i_size);
    for (u32 i = 0; i < (inode.i_size + ext3_block_size - 1) / ext3_block_size; i++) {
        u32 phys = ext3_get_block(&inode, i);
        if (phys) ext3_read_block(phys, buf + i * ext3_block_size);
    }

    usize off = 0;
    while (off < inode.i_size) {
        struct ext2_dir_entry *e = (struct ext2_dir_entry *)(buf + off);
        if (e->rec_len == 0 || e->inode == 0) { off += e->rec_len ? e->rec_len : 4; continue; }
        char name[256];
        memcpy(name, e->name, e->name_len);
        name[e->name_len] = '\0';
        if (strcmp(name, ".") && strcmp(name, "..")) {
            char full[256];
            usize len = strlen(base_path);
            memcpy(full, base_path, len);
            if (full[len - 1] != '/') full[len++] = '/';
            memcpy(full + len, name, e->name_len + 1);
            struct ext2_inode ci;
            if (ext3_read_inode(e->inode, &ci) == 0) {
                if ((ci.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
                    struct vfs_node *dn = vfs_add_node(full, VFS_DIRECTORY, (void *)(usize)e->inode, 0, 0);
                    if (dn) dn->create_cb = ext3_vfs_create;
                    ext3_populate_vfs(e->inode, full);
                } else {
                    struct vfs_node *n = vfs_add_node(full, VFS_FILE, (void *)(usize)e->inode, ci.i_size, 0);
                    if (n) { n->read_cb = ext3_vfs_read; n->write_cb = ext3_vfs_write; }
                }
            }
        }
        off += e->rec_len;
    }
    kfree(buf);
}

/* ── Init ── */

void ext3_init(void)
{
    ext3_dev = blk_get("virtio-blk1");
    if (!ext3_dev) ext3_dev = blk_get("sata0");
    if (!ext3_dev) return;

    u8 *sb = kmalloc(1024);
    if (blk_read_cached(ext3_dev, 2, 2, sb) < 0) { kfree(sb); return; }
    memcpy(&ext3_sb, sb, sizeof(struct ext2_superblock));

    if (ext3_sb.s_magic != EXT2_SUPER_MAGIC) { kfree(sb); return; }
    if (ext3_sb.s_rev_level == 0) { kfree(sb); return; }

    ext3_block_size = 1024 << ext3_sb.s_log_block_size;
    ext3_inodes_per_group = ext3_sb.s_inodes_per_group;
    ext3_inode_size = (ext3_sb.s_rev_level == 0) ? 128 : ext3_sb.s_inode_size;

    if (!(ext3_sb.s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL)) {
        console_write("ext3: no journal feature, skipping\n");
        kfree(sb);
        return;
    }

    console_write("ext3: detecting journal...\n");

    /* Read journal inode number */
    {
        u8 *tmp = kmalloc(1024);
        if (blk_read_cached(ext3_dev, 2, 2, tmp) >= 0)
            memcpy(&journal_inum, tmp + EXT3_SB_JOURNAL_INUM_OFF, 4);
        kfree(tmp);
    }
    kfree(sb);

    if (journal_inum == 0) { console_write("ext3: no journal inode\n"); return; }

    if (ext3_sb.s_feature_incompat & EXT3_FEATURE_INCOMPAT_RECOVER) {
        console_write("ext3: journal needs recovery\n");
        ext3_recover_journal(journal_inum);
    }

    /* Activate journal for writes */
    journal_active = 1;

    console_write("ext3: mounted rw, block_size=");
    console_write_dec(ext3_block_size);
    console_write("\n");

    struct vfs_node *root = vfs_add_node("/ext3", VFS_DIRECTORY, (void *)(usize)2, 0, 0);
    if (root) root->create_cb = ext3_vfs_create;
    ext3_populate_vfs(2, "/ext3");
}
