#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/ext2.h>
#include <b1nix/ext4.h>
#include <b1nix/journal.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <string.h>

#define EXT4_FEATURE_INCOMPAT_64BIT    0x0080
#define EXT4_FEATURE_INCOMPAT_EXTENTS  0x0040
#define EXT4_FEATURE_INCOMPAT_FLEX_BG  0x0200
#define EXT4_FEATURE_INCOMPAT_INLINE_DATA 0x8000
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE   0x0008
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE 0x0040
#define EXT2_FEATURE_INCOMPAT_META_BG  0x0010

#define EXT3_FEATURE_COMPAT_HAS_JOURNAL 0x0004
#define EXT3_FEATURE_INCOMPAT_RECOVER 0x0004
#define EXT3_SB_JOURNAL_INUM_OFF 0xE0

#define EXT4_SB_DESC_SIZE_OFF         0x106
#define EXT4_SB_LOG_GROUPS_PER_FLEX   0x15E

static int ext4_read_block(struct ext4_fs *fs, u32 block, void *buffer) {
  return blk_read_cached(fs->bdev, (u64)block * (fs->block_size / 512), fs->block_size / 512, buffer);
}

static int ext4_write_block(struct ext4_fs *fs, u32 block, const void *buffer) {
  return blk_write_cached(fs->bdev, (u64)block * (fs->block_size / 512), fs->block_size / 512, buffer);
}

static u32 ext4_get_block(struct ext4_fs *fs, struct ext2_inode *inode, u32 block_idx);

static int ext4_jbd_read(struct journal_dev *jdev, u32 logical, void *buf) {
  struct ext4_fs *fs = (struct ext4_fs *)jdev->fs_priv;
  u32 phys = ext4_get_block(fs, &fs->journal_inode_cache, logical);
  if (!phys) return -1;
  return ext4_read_block(fs, phys, buf);
}

static int ext4_jbd_write(struct journal_dev *jdev, u32 logical, const void *buf) {
  struct ext4_fs *fs = (struct ext4_fs *)jdev->fs_priv;
  u32 phys = ext4_get_block(fs, &fs->journal_inode_cache, logical);
  if (!phys) return -1;
  return ext4_write_block(fs, phys, buf);
}

static int ext4_jbd_fs_write(struct journal_dev *jdev, u32 phys, const void *buf) {
  struct ext4_fs *fs = (struct ext4_fs *)jdev->fs_priv;
  return ext4_write_block(fs, phys, buf);
}

static struct journal_ops ext4_jbd_ops = {ext4_jbd_read, ext4_jbd_write, ext4_jbd_fs_write};
static int ext4_journal_write(struct ext4_fs *fs, u32 block, const void *buffer);

static int ext4_journal_write_tx(struct ext4_fs *fs, struct journal_handle *h,
                                 u32 block, const void *buffer) {
  if (h) {
    journal_log_block(h, block, buffer);
  }
  // Always update the block cache so subsequent reads see the changes
  return ext4_write_block(fs, block, buffer);
}

static int ext4_journal_write(struct ext4_fs *fs, u32 block, const void *buffer) {
  if (fs->jdev) {
    struct journal_handle *h = journal_start_transaction(fs->jdev);
    if (h) { 
      journal_log_block(h, block, buffer); 
      journal_commit_transaction(h); 
    }
  }
  // Always update the block cache so subsequent reads see the changes
  return ext4_write_block(fs, block, buffer);
}

static u32 ext4_get_desc_block(struct ext4_fs *fs, u32 group) {
    u32 desc_per_block = fs->block_size / fs->desc_size;
    if (fs->features_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
        if (fs->sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_META_BG) {
            u32 meta_bg = group / desc_per_block; return (meta_bg * desc_per_block) + 1;
        }
        return (fs->block_size == 1024 ? 2 : 1) + (group / desc_per_block) * desc_per_block;
    }
    if (fs->features_incompat & EXT4_FEATURE_INCOMPAT_FLEX_BG) return (fs->block_size == 1024 ? 2 : 1) + (group / desc_per_block);
    return (fs->block_size == 1024) ? 2 : 1;
}

static void ext4_read_bgd(struct ext4_fs *fs, u32 group, struct ext4_bgd_64 *bgd) {
    u32 bg_block = ext4_get_desc_block(fs, group);
    u32 desc_per_block = fs->block_size / fs->desc_size;
    u32 bg_offset = (group % desc_per_block) * fs->desc_size;
    u8 *buf = kmalloc(fs->block_size); ext4_read_block(fs, bg_block, buf);
    if (fs->desc_size >= 64) memcpy(bgd, buf + bg_offset, sizeof(struct ext4_bgd_64));
    else {
        struct ext2_block_group_desc bg32; memcpy(&bg32, buf + bg_offset, sizeof(bg32));
        memset(bgd, 0, sizeof(struct ext4_bgd_64));
        bgd->bg_block_bitmap_lo = bg32.bg_block_bitmap; bgd->bg_inode_bitmap_lo = bg32.bg_inode_bitmap;
        bgd->bg_inode_table_lo = bg32.bg_inode_table; bgd->bg_free_blocks_count_lo = bg32.bg_free_blocks_count;
        bgd->bg_free_inodes_count_lo = bg32.bg_free_inodes_count;
    }
    kfree(buf);
}

static void ext4_write_bgd_tx(struct ext4_fs *fs, u32 group, struct ext4_bgd_64 *bgd, struct journal_handle *h) {
    u32 bg_block = ext4_get_desc_block(fs, group);
    u32 desc_per_block = fs->block_size / fs->desc_size;
    u32 bg_offset = (group % desc_per_block) * fs->desc_size;
    u8 *buf = kmalloc(fs->block_size); ext4_read_block(fs, bg_block, buf);
    if (fs->desc_size >= 64) memcpy(buf + bg_offset, bgd, sizeof(struct ext4_bgd_64));
    else {
        struct ext2_block_group_desc bg32;
        bg32.bg_block_bitmap = bgd->bg_block_bitmap_lo; bg32.bg_inode_bitmap = bgd->bg_inode_bitmap_lo;
        bg32.bg_inode_table = bgd->bg_inode_table_lo; bg32.bg_free_blocks_count = bgd->bg_free_blocks_count_lo;
        bg32.bg_free_inodes_count = bgd->bg_free_inodes_count_lo; memcpy(buf + bg_offset, &bg32, sizeof(bg32));
    }
    ext4_journal_write_tx(fs, h, bg_block, buf); kfree(buf);
}

static void ext4_write_superblock(struct ext4_fs *fs) {
    u8 *sb_buf = kmalloc(1024);
    if (blk_read_cached(fs->bdev, 2, 2, sb_buf) >= 0) { memcpy(sb_buf, &fs->sb, sizeof(struct ext2_superblock)); blk_write_cached(fs->bdev, 2, 2, sb_buf); }
    kfree(sb_buf);
}

static u32 ext4_bgd_block_bitmap(struct ext4_fs *fs, struct ext4_bgd_64 *bgd) {
    u32 lo = bgd->bg_block_bitmap_lo; if (fs->features_incompat & EXT4_FEATURE_INCOMPAT_64BIT) lo |= (u32)bgd->bg_block_bitmap_hi << 16;
    return lo;
}

static u32 ext4_bgd_inode_bitmap(struct ext4_fs *fs, struct ext4_bgd_64 *bgd) {
    u32 lo = bgd->bg_inode_bitmap_lo; if (fs->features_incompat & EXT4_FEATURE_INCOMPAT_64BIT) lo |= (u32)bgd->bg_inode_bitmap_hi << 16;
    return lo;
}

static u32 ext4_bgd_inode_table(struct ext4_fs *fs, struct ext4_bgd_64 *bgd) {
    u32 lo = bgd->bg_inode_table_lo; if (fs->features_incompat & EXT4_FEATURE_INCOMPAT_64BIT) lo |= (u32)bgd->bg_inode_table_hi << 16;
    return lo;
}

static int ext4_read_inode(struct ext4_fs *fs, u32 inode_num, struct ext2_inode *inode) {
  if (inode_num == 0) return -1;
  u32 group = (inode_num - 1) / fs->inodes_per_group; struct ext4_bgd_64 bgd; ext4_read_bgd(fs, group, &bgd);
  u32 itable = ext4_bgd_inode_table(fs, &bgd);
  u32 inode_offset = ((inode_num - 1) % fs->inodes_per_group) * fs->inode_size;
  u32 block_idx = itable + (inode_offset / fs->block_size);
  u8 *buf = kmalloc(fs->block_size); ext4_read_block(fs, block_idx, buf);
  memcpy(inode, buf + (inode_offset % fs->block_size), sizeof(struct ext2_inode)); kfree(buf);
  return 0;
}

static int ext4_write_inode_tx(struct ext4_fs *fs, struct journal_handle *h,
                               u32 inode_num, const struct ext2_inode *inode) {
  if (inode_num == 0)
    return -1;
  u32 group = (inode_num - 1) / fs->inodes_per_group;
  struct ext4_bgd_64 bgd;
  ext4_read_bgd(fs, group, &bgd);
  u32 itable = ext4_bgd_inode_table(fs, &bgd);
  u32 inode_offset = ((inode_num - 1) % fs->inodes_per_group) * fs->inode_size;
  u32 block_idx = itable + (inode_offset / fs->block_size);
  u8 *buf = kmalloc(fs->block_size);
  ext4_read_block(fs, block_idx, buf);
  memcpy(buf + (inode_offset % fs->block_size), inode, sizeof(struct ext2_inode));
  int ret = ext4_journal_write_tx(fs, h, block_idx, buf);
  kfree(buf);
  return ret;
}

static int ext4_write_inode(struct ext4_fs *fs, u32 inode_num, const struct ext2_inode *inode) {
  return ext4_write_inode_tx(fs, 0, inode_num, inode);
}

static u32 ext4_alloc_block_tx(struct ext4_fs *fs, struct journal_handle *h) {
  u32 groups = (fs->sb.s_blocks_count + fs->sb.s_blocks_per_group - 1) / fs->sb.s_blocks_per_group;
  for (u32 g = 0; g < groups; g++) {
    struct ext4_bgd_64 bgd; ext4_read_bgd(fs, g, &bgd); if (bgd.bg_free_blocks_count_lo == 0) continue;
    u8 *bitmap = kmalloc(fs->block_size); ext4_read_block(fs, ext4_bgd_block_bitmap(fs, &bgd), bitmap);
    for (u32 i = 0; i < fs->sb.s_blocks_per_group; i++) {
      if (!(bitmap[i / 8] & (1 << (i % 8)))) {
        bitmap[i / 8] |= (1 << (i % 8)); ext4_journal_write_tx(fs, h, ext4_bgd_block_bitmap(fs, &bgd), bitmap);
        kfree(bitmap); bgd.bg_free_blocks_count_lo--; ext4_write_bgd_tx(fs, g, &bgd, h);
        fs->sb.s_free_blocks_count--; ext4_write_superblock(fs);
        u32 block_num = g * fs->sb.s_blocks_per_group + i + (fs->sb.s_log_block_size == 0 ? 1 : 0);
        u8 *zero = kzalloc(fs->block_size); ext4_journal_write_tx(fs, h, block_num, zero);
        kfree(zero); return block_num;
      }
    }
    kfree(bitmap);
  }
  return 0;
}

static u32 ext4_alloc_block(struct ext4_fs *fs) {
  return ext4_alloc_block_tx(fs, 0);
}

static u32 ext4_alloc_inode_tx(struct ext4_fs *fs, struct journal_handle *h) {
  u32 groups = (fs->sb.s_inodes_count + fs->inodes_per_group - 1) / fs->inodes_per_group;
  for (u32 g = 0; g < groups; g++) {
    struct ext4_bgd_64 bgd; ext4_read_bgd(fs, g, &bgd); if (bgd.bg_free_inodes_count_lo == 0) continue;
    u8 *bitmap = kmalloc(fs->block_size); ext4_read_block(fs, ext4_bgd_inode_bitmap(fs, &bgd), bitmap);
    for (u32 i = 0; i < fs->inodes_per_group; i++) {
      if (!(bitmap[i / 8] & (1 << (i % 8)))) {
        bitmap[i / 8] |= (1 << (i % 8)); ext4_journal_write_tx(fs, h, ext4_bgd_inode_bitmap(fs, &bgd), bitmap);
        kfree(bitmap); bgd.bg_free_inodes_count_lo--; ext4_write_bgd_tx(fs, g, &bgd, h);
        fs->sb.s_free_inodes_count--; ext4_write_superblock(fs);
        u32 inode_num = g * fs->inodes_per_group + i + 1;
        struct ext2_inode ni; memset(&ni, 0, sizeof(ni)); ext4_write_inode_tx(fs, h, inode_num, &ni);
        return inode_num;
      }
    }
    kfree(bitmap);
  }
  return 0;
}

static void ext4_free_block_tx(struct ext4_fs *fs, u32 block_num, struct journal_handle *h) {
  if (block_num == 0) return;
  u32 g = (block_num - (fs->sb.s_log_block_size == 0 ? 1 : 0)) / fs->sb.s_blocks_per_group;
  u32 i = (block_num - (fs->sb.s_log_block_size == 0 ? 1 : 0)) % fs->sb.s_blocks_per_group;
  struct ext4_bgd_64 bgd; ext4_read_bgd(fs, g, &bgd);
  u8 *bitmap = kmalloc(fs->block_size); ext4_read_block(fs, ext4_bgd_block_bitmap(fs, &bgd), bitmap);
  bitmap[i / 8] &= ~(1 << (i % 8)); ext4_journal_write_tx(fs, h, ext4_bgd_block_bitmap(fs, &bgd), bitmap);
  kfree(bitmap); bgd.bg_free_blocks_count_lo++; ext4_write_bgd_tx(fs, g, &bgd, h);
  fs->sb.s_free_blocks_count++; ext4_write_superblock(fs);
}

static void ext4_free_block(struct ext4_fs *fs, u32 block_num) {
  ext4_free_block_tx(fs, block_num, 0);
}

static void ext4_free_inode_tx(struct ext4_fs *fs, u32 inode_num, struct journal_handle *h) {
  if (inode_num == 0) return;
  u32 g = (inode_num - 1) / fs->inodes_per_group;
  u32 i = (inode_num - 1) % fs->inodes_per_group;
  struct ext4_bgd_64 bgd; ext4_read_bgd(fs, g, &bgd);
  u8 *bitmap = kmalloc(fs->block_size); ext4_read_block(fs, ext4_bgd_inode_bitmap(fs, &bgd), bitmap);
  bitmap[i / 8] &= ~(1 << (i % 8)); ext4_journal_write_tx(fs, h, ext4_bgd_inode_bitmap(fs, &bgd), bitmap);
  kfree(bitmap); bgd.bg_free_inodes_count_lo++; ext4_write_bgd_tx(fs, g, &bgd, h);
  fs->sb.s_free_inodes_count++; ext4_write_superblock(fs);
}

static void ext4_free_inode(struct ext4_fs *fs, u32 inode_num) {
  ext4_free_inode_tx(fs, inode_num, 0);
}

static u32 ext4_extent_lookup(struct ext4_fs *fs, struct ext2_inode *inode, u32 logical_block) {
    struct ext4_extent_header *eh = (struct ext4_extent_header *)inode->i_block;
  if (eh->eh_magic != EXT4_EXTENT_MAGIC) return 0;
    u8 *block_buf = kmalloc(fs->block_size);
    if (eh->eh_depth == 0) {
        struct ext4_extent *exts = (struct ext4_extent *)(eh + 1);
        for (u16 i = 0; i < eh->eh_entries; i++) {
            if (logical_block >= exts[i].ee_block && logical_block < exts[i].ee_block + exts[i].ee_len) {
                u32 result = (exts[i].ee_start_lo | ((u32)exts[i].ee_start_hi << 16)) + (logical_block - exts[i].ee_block);
                kfree(block_buf); return result;
            }
        }
        kfree(block_buf); return 0;
    }
    struct ext4_extent_idx *idx = (struct ext4_extent_idx *)(eh + 1); u32 phys = 0;
    for (u32 i = 0; i < eh->eh_entries; i++) { if (logical_block < idx[i].ei_block) { if (i > 0) phys = idx[i - 1].ei_leaf_lo | ((u32)idx[i - 1].ei_leaf_hi << 16); break; } }
  if (phys == 0 && eh->eh_entries > 0) phys = idx[eh->eh_entries - 1].ei_leaf_lo | ((u32)idx[eh->eh_entries - 1].ei_leaf_hi << 16);
  if (!phys) { kfree(block_buf); return 0; }
    for (u32 level = eh->eh_depth; level > 0 && phys; level--) {
        ext4_read_block(fs, phys, block_buf); struct ext4_extent_header *node_hdr = (struct ext4_extent_header *)block_buf;
        if (level == 1) {
            struct ext4_extent *exts = (struct ext4_extent *)(node_hdr + 1);
            for (u16 i = 0; i < node_hdr->eh_entries; i++) {
                if (logical_block >= exts[i].ee_block && logical_block < exts[i].ee_block + exts[i].ee_len) {
                    u32 result = (exts[i].ee_start_lo | ((u32)exts[i].ee_start_hi << 16)) + (logical_block - exts[i].ee_block);
                    kfree(block_buf); return result;
                }
            }
            break;
        }
        struct ext4_extent_idx *indices = (struct ext4_extent_idx *)(node_hdr + 1); phys = 0;
        for (u16 i = 0; i < node_hdr->eh_entries; i++) { if (logical_block < indices[i].ei_block) { if (i > 0) phys = indices[i - 1].ei_leaf_lo | ((u32)indices[i - 1].ei_leaf_hi << 16); break; } }
        if (phys == 0 && node_hdr->eh_entries > 0) phys = indices[node_hdr->eh_entries - 1].ei_leaf_lo | ((u32)indices[node_hdr->eh_entries - 1].ei_leaf_hi << 16);
    }
    kfree(block_buf); return 0;
}

static int ext4_add_extent(struct ext2_inode *inode, u32 logical, u32 physical, u16 len) {
    struct ext4_extent_header *eh = (struct ext4_extent_header *)inode->i_block;
    if (eh->eh_magic == EXT4_EXTENT_MAGIC && eh->eh_depth == 0) {
        struct ext4_extent *exts = (struct ext4_extent *)(eh + 1); u16 n = eh->eh_entries;
        if (n > 0) {
            struct ext4_extent *last = &exts[n - 1];
      if (logical == last->ee_block + last->ee_len && (last->ee_start_lo + last->ee_len) == physical) { if ((u32)last->ee_len + len <= 32768) { last->ee_len += len; return 1; } }
        }
        if (n < eh->eh_max) { exts[n].ee_block = logical; exts[n].ee_len = len; exts[n].ee_start_lo = physical; exts[n].ee_start_hi = 0; eh->eh_entries = n + 1; return 1; }
        return 0;
    }
    memset(inode->i_block, 0, sizeof(inode->i_block));
    eh->eh_magic = EXT4_EXTENT_MAGIC; eh->eh_entries = 1; eh->eh_max = 4; eh->eh_depth = 0; eh->eh_generation = 0;
    struct ext4_extent *ext = (struct ext4_extent *)(eh + 1); ext->ee_block = logical; ext->ee_len = len; ext->ee_start_lo = physical; ext->ee_start_hi = 0;
    inode->i_flags |= EXT4_EXTENTS_FL; return 1;
}

static u32 ext4_get_block(struct ext4_fs *fs, struct ext2_inode *inode, u32 block_idx) {
    if (inode->i_flags & EXT4_EXTENTS_FL) return ext4_extent_lookup(fs, inode, block_idx);
    if (block_idx < EXT2_NDIR_BLOCKS) return inode->i_block[block_idx];
    u32 ptrs = fs->block_size / 4; block_idx -= EXT2_NDIR_BLOCKS;
    if (block_idx < ptrs) {
        if (!inode->i_block[EXT2_IND_BLOCK]) return 0;
        u32 *ind = kmalloc(fs->block_size); ext4_read_block(fs, inode->i_block[EXT2_IND_BLOCK], ind);
        u32 r = ind[block_idx]; kfree(ind); return r;
    }
    return 0;
}

static int ext4_set_block(struct ext2_inode *inode, u32 block_idx, u32 phys) {
    if (inode->i_flags & EXT4_EXTENTS_FL || block_idx >= EXT2_NDIR_BLOCKS) return ext4_add_extent(inode, block_idx, phys, 1);
    inode->i_block[block_idx] = phys; return 1;
}

static u64 ext4_get_inode_size(struct ext4_fs *fs, struct ext2_inode *inode) {
    u64 size = inode->i_size; if ((fs->features_ro_compat & EXT4_FEATURE_RO_COMPAT_HUGE_FILE) && (inode->i_flags & EXT4_EOFBLOCKS_FL)) size |= ((u64)inode->i_dir_acl) << 32;
    return size;
}

static isize ext4_vfs_read(struct vfs_node *node, u64 offset, char *buffer, usize size, int flags) {
    (void)flags; struct ext4_inode_info *info = (struct ext4_inode_info *)node->inode->data;
    struct ext4_fs *fs = info->fs; struct ext2_inode inode;
    if (ext4_read_inode(fs, info->inode_num, &inode) < 0) return -1;
    u64 inode_sz = ext4_get_inode_size(fs, &inode); if (offset >= inode_sz) return 0;
    usize remaining = (usize)(inode_sz - offset); usize to_read = size < remaining ? size : remaining;
    usize done = 0; u8 *block_buf = kmalloc(fs->block_size);
    while (done < to_read) {
        u32 b_idx = (u32)((offset + done) / fs->block_size); u32 b_off = (u32)((offset + done) % fs->block_size);
        u32 phys = ext4_get_block(fs, &inode, b_idx); usize chunk = fs->block_size - b_off;
        if (chunk > to_read - done) chunk = to_read - done;
        if (phys) { ext4_read_block(fs, phys, block_buf); memcpy(buffer + done, block_buf + b_off, chunk); }
        else memset(buffer + done, 0, chunk);
        done += chunk;
    }
    kfree(block_buf);
    if (done > 0) {
        node->inode->atime = vfs_get_unix_time();
        struct ext2_inode inode_to_update;
        if (ext4_read_inode(fs, info->inode_num, &inode_to_update) == 0) {
            inode_to_update.i_atime = node->inode->atime;
            ext4_write_inode(fs, info->inode_num, &inode_to_update);
        }
    }
    return (isize)done;
}

static isize ext4_vfs_write(struct vfs_node *node, u64 offset, const char *buffer, usize size, int flags) {
    (void)flags; struct ext4_inode_info *info = (struct ext4_inode_info *)node->inode->data;
    struct ext4_fs *fs = info->fs; struct ext2_inode inode;
    if (ext4_read_inode(fs, info->inode_num, &inode) < 0) return -1;
    u64 new_size = offset + size; u32 old_blks = (inode.i_size + fs->block_size - 1) / fs->block_size;
    u32 new_blks = (new_size + fs->block_size - 1) / fs->block_size;
    if (new_size > inode.i_size) {
        for (u32 b = old_blks; b < new_blks; b++) {
            u32 pblk = ext4_alloc_block(fs); if (!pblk || !ext4_set_block(&inode, b, pblk)) return -1;
            inode.i_blocks += fs->block_size / 512;
        }
        inode.i_size = (u32)new_size; ext4_write_inode(fs, info->inode_num, &inode);
        u64 cur_sz = ext4_get_inode_size(fs, &inode);
        if (cur_sz > node->inode->size) {
            node->inode->size = (usize)cur_sz;
        }
    }
    usize done = 0; u8 *block_buf = kmalloc(fs->block_size);
    while (done < size) {
        u32 b_idx = (u32)((offset + done) / fs->block_size); u32 b_off = (u32)((offset + done) % fs->block_size);
        u32 phys = ext4_get_block(fs, &inode, b_idx); if (!phys) break;
        usize chunk = fs->block_size - b_off; if (chunk > size - done) chunk = size - done;
        if (chunk < fs->block_size) ext4_read_block(fs, phys, block_buf);
        memcpy(block_buf + b_off, buffer + done, chunk); ext4_journal_write(fs, phys, block_buf);
        done += chunk;
    }
    kfree(block_buf);
    if (done > 0) {
        node->inode->mtime = vfs_get_unix_time();
        node->inode->ctime = vfs_get_unix_time();
        
        u64 disk_sz = ext4_get_inode_size(fs, &inode);
        if (offset + done > disk_sz) {
            disk_sz = offset + done;
            inode.i_size = (u32)disk_sz;
            if (fs->features_ro_compat & EXT4_FEATURE_RO_COMPAT_HUGE_FILE) {
                inode.i_dir_acl = (u32)(disk_sz >> 32);
            }
        }
        if (offset + done > node->inode->size) {
            node->inode->size = (usize)(offset + done);
        }
        
        // Reuse our existing up-to-date inode rather than fetching a stale one from disk
        inode.i_mtime = node->inode->mtime;
        inode.i_ctime = node->inode->ctime;
        ext4_write_inode(fs, info->inode_num, &inode);
    }
    return (isize)done;
}

static void ext4_inode_blocks_sub(struct ext4_fs *fs, struct ext2_inode *inode,
                                  u32 blocks) {
    u32 sectors = blocks * (fs->block_size / 512);
    inode->i_blocks = inode->i_blocks > sectors ? inode->i_blocks - sectors : 0;
}

static int ext4_block_table_empty(u32 *table, u32 entries) {
    for (u32 i = 0; i < entries; i++) {
        if (table[i]) return 0;
    }
    return 1;
}

static u32 ext4_extent_phys(const struct ext4_extent *ext) {
    return ext->ee_start_lo | ((u32)ext->ee_start_hi << 16);
}

static int ext4_vfs_truncate(struct vfs_node *node, u64 length) {
    struct ext4_inode_info *info = (struct ext4_inode_info *)node->inode->data;
    struct ext4_fs *fs = info->fs;
    struct ext2_inode inode;
    if (ext4_read_inode(fs, info->inode_num, &inode) < 0) return -EIO;

    u64 old_size = ext4_get_inode_size(fs, &inode);
    u32 old_blocks = (old_size + fs->block_size - 1) / fs->block_size;
    u32 new_blocks = (length + fs->block_size - 1) / fs->block_size;

    if (new_blocks < old_blocks) {
        if (inode.i_flags & EXT4_EXTENTS_FL) {
            struct ext4_extent_header *eh = (struct ext4_extent_header *)inode.i_block;
            if (eh->eh_magic == EXT4_EXTENT_MAGIC && eh->eh_depth == 0) {
                struct ext4_extent *exts = (struct ext4_extent *)(eh + 1);
                u16 out = 0;
                for (u16 i = 0; i < eh->eh_entries; i++) {
                    u32 start = exts[i].ee_block;
                    u32 len = exts[i].ee_len;
                    u32 end = start + len;
                    u32 phys = ext4_extent_phys(&exts[i]);
                    if (end <= new_blocks) {
                        exts[out++] = exts[i];
                        continue;
                    }
                    u32 free_from = new_blocks > start ? new_blocks : start;
                    for (u32 b = free_from; b < end; b++) {
                        ext4_free_block(fs, phys + (b - start));
                        ext4_inode_blocks_sub(fs, &inode, 1);
                    }
                    if (start < new_blocks) {
                        exts[i].ee_len = (u16)(new_blocks - start);
                        exts[out++] = exts[i];
                    }
                }
                eh->eh_entries = out;
            }
        } else {
            u32 ptrs = fs->block_size / 4;
            for (u32 b = new_blocks; b < old_blocks; b++) {
                if (b < EXT2_NDIR_BLOCKS) {
                    if (inode.i_block[b]) {
                        ext4_free_block(fs, inode.i_block[b]);
                        ext4_inode_blocks_sub(fs, &inode, 1);
                        inode.i_block[b] = 0;
                    }
                    continue;
                }
                u32 rel = b - EXT2_NDIR_BLOCKS;
                if (rel < ptrs && inode.i_block[EXT2_IND_BLOCK]) {
                    u32 *ind = kmalloc(fs->block_size);
                    ext4_read_block(fs, inode.i_block[EXT2_IND_BLOCK], ind);
                    if (ind[rel]) {
                        ext4_free_block(fs, ind[rel]);
                        ext4_inode_blocks_sub(fs, &inode, 1);
                        ind[rel] = 0;
                        ext4_journal_write(fs, inode.i_block[EXT2_IND_BLOCK], ind);
                    }
                    if (ext4_block_table_empty(ind, ptrs)) {
                        ext4_free_block(fs, inode.i_block[EXT2_IND_BLOCK]);
                        ext4_inode_blocks_sub(fs, &inode, 1);
                        inode.i_block[EXT2_IND_BLOCK] = 0;
                    }
                    kfree(ind);
                }
            }
        }
    }

    inode.i_size = (u32)length;
    if (fs->features_ro_compat & EXT4_FEATURE_RO_COMPAT_HUGE_FILE) {
        inode.i_dir_acl = (u32)(length >> 32);
    }
    node->inode->size = (usize)length;
    node->inode->mtime = vfs_get_unix_time();
    node->inode->ctime = node->inode->mtime;
    inode.i_mtime = node->inode->mtime;
    inode.i_ctime = node->inode->ctime;
    return ext4_write_inode(fs, info->inode_num, &inode);
}

static int ext4_add_dir_entry_tx(struct ext4_fs *fs, u32 dir_ino, u32 child_ino, const char *name, u8 type, struct journal_handle *h) {
    struct ext2_inode dir; if (ext4_read_inode(fs, dir_ino, &dir) < 0) return -1;
    usize name_len = strlen(name); if (name_len > 255) name_len = 255;
    u32 needed = 8 + ((name_len + 3) & ~3);
    u8 *buf = kmalloc(fs->block_size); u32 blocks = (dir.i_size + fs->block_size - 1) / fs->block_size;
    for (u32 b = 0; b < blocks; b++) {
        u32 phys = ext4_get_block(fs, &dir, b); if (!phys) continue;
        ext4_read_block(fs, phys, buf); usize off = 0;
        while (off < fs->block_size) {
            struct ext2_dir_entry *e = (struct ext2_dir_entry *)(buf + off);
            if (e->rec_len == 0) break;
            u32 actual = 8 + ((e->name_len + 3) & ~3);
            if (e->rec_len >= actual + needed) {
                u32 old_rec_len = e->rec_len;
                e->rec_len = actual;
                struct ext2_dir_entry *ne = (struct ext2_dir_entry *)(buf + off + actual);
                ne->inode = child_ino; ne->rec_len = old_rec_len - actual; ne->name_len = name_len; ne->file_type = type;
                memcpy(ne->name, name, name_len); ext4_journal_write_tx(fs, h, phys, buf);
                kfree(buf); return 0;
            }
            off += e->rec_len;
        }
    }
    u32 phys = ext4_alloc_block_tx(fs, h);
    if (phys) {
        ext4_set_block(&dir, blocks, phys);
        dir.i_blocks += fs->block_size / 512; dir.i_size += fs->block_size;
        ext4_write_inode_tx(fs, h, dir_ino, &dir);
        memset(buf, 0, fs->block_size);
        struct ext2_dir_entry *e = (struct ext2_dir_entry *)buf;
        e->inode = child_ino; e->rec_len = fs->block_size; e->name_len = name_len; e->file_type = type;
        memcpy(e->name, name, name_len); ext4_journal_write_tx(fs, h, phys, buf);
        kfree(buf); return 0;
    }
    kfree(buf); return -1;
}

static void ext4_setup_node(struct vfs_node *n, struct ext4_fs *fs, u32 ino, u32 mode);

static isize ext4_vfs_readdir(struct vfs_node *dir, usize offset, struct dirent *buf, usize max_entries) {
    struct ext4_inode_info *info = (struct ext4_inode_info *)dir->inode->data;
    struct ext4_fs *fs = info->fs; struct ext2_inode inode;
    if (ext4_read_inode(fs, info->inode_num, &inode) < 0) return -EIO;
    u8 *dir_buf = kmalloc(fs->block_size); usize count = 0; usize entry_idx = 0;
    u32 blocks = (u32)((inode.i_size + fs->block_size - 1) / fs->block_size);
    for (u32 b = 0; b < blocks && count < max_entries; b++) {
        u32 phys = ext4_get_block(fs, &inode, b); if (!phys) continue;
        ext4_read_block(fs, phys, dir_buf); usize off = 0;
        while (off < fs->block_size && count < max_entries) {
            struct ext2_dir_entry *e = (struct ext2_dir_entry *)(dir_buf + off);
            if (e->rec_len == 0) break;
            if (e->inode != 0) {
                if (entry_idx >= offset) {
                    usize name_len = e->name_len > 63 ? 63 : e->name_len;
                    memcpy(buf[count].name, e->name, name_len); buf[count].name[name_len] = '\0';
                    buf[count].type = (u32)VFS_FILE; if (e->file_type == EXT2_FT_DIR) buf[count].type = (u32)VFS_DIRECTORY;
                    buf[count].is_dir = (e->file_type == EXT2_FT_DIR);
                    buf[count].size = 0; count++;
                }
                entry_idx++;
            }
            off += e->rec_len;
        }
    }
    kfree(dir_buf); return (isize)count;
}

static int ext4_vfs_create(struct vfs_node *dir, const char *name, const char *full_path, u32 mode) {
  (void)full_path; /* part of the vfs create-op signature; this impl uses name+dir */
  struct ext4_inode_info *dir_info = (struct ext4_inode_info *)dir->inode->data;
  struct ext4_fs *fs = dir_info->fs;
  struct journal_handle *h = fs->jdev ? journal_start_transaction(fs->jdev) : 0;
  u32 new_ino = ext4_alloc_inode_tx(fs, h);
  if (!new_ino) {
    if (h) journal_abort_transaction(h);
    return -ENOSPC;
  }
  struct ext2_inode inode; memset(&inode, 0, sizeof(inode));
  inode.i_mode = EXT2_S_IFREG | (mode & 07777); inode.i_links_count = 1;
  inode.i_atime = inode.i_mtime = inode.i_ctime = vfs_get_unix_time();
  struct ext4_extent_header *eh = (struct ext4_extent_header *)inode.i_block;
  eh->eh_magic = EXT4_EXTENT_MAGIC; eh->eh_entries = 0; eh->eh_max = 4; eh->eh_depth = 0; eh->eh_generation = 0;
  inode.i_flags |= EXT4_EXTENTS_FL;

  if (ext4_write_inode_tx(fs, h, new_ino, &inode) < 0) {
    if (h) journal_abort_transaction(h);
    return -EIO;
  }
  if (ext4_add_dir_entry_tx(fs, dir_info->inode_num, new_ino, name, EXT2_FT_REG_FILE, h) < 0) {
    if (h) journal_abort_transaction(h);
    return -EIO;
  }
  if (h) journal_commit_transaction(h);
  struct vfs_node *n = find_child(dir, name);
  if (n) {
    ext4_setup_node(n, fs, new_ino, inode.i_mode);
    vfs_node_put(n);
  }
  return 0;
}

static int ext4_vfs_mkdir(struct vfs_node *dir, const char *name, u32 mode) {
  struct ext4_inode_info *dir_info = (struct ext4_inode_info *)dir->inode->data;
  struct ext4_fs *fs = dir_info->fs;
  struct journal_handle *h = fs->jdev ? journal_start_transaction(fs->jdev) : 0;
  u32 new_ino = ext4_alloc_inode_tx(fs, h);
  if (!new_ino) {
    if (h) journal_abort_transaction(h);
    return -ENOSPC;
  }
  struct ext2_inode inode; memset(&inode, 0, sizeof(inode));
  inode.i_mode = EXT2_S_IFDIR | (mode & 07777); inode.i_links_count = 2;
  inode.i_atime = inode.i_mtime = inode.i_ctime = vfs_get_unix_time();
  if (ext4_write_inode_tx(fs, h, new_ino, &inode) < 0) {
    if (h) journal_abort_transaction(h);
    return -EIO;
  }
  if (ext4_add_dir_entry_tx(fs, dir_info->inode_num, new_ino, name, EXT2_FT_DIR, h) < 0) {
    if (h) journal_abort_transaction(h);
    return -EIO;
  }
  if (ext4_add_dir_entry_tx(fs, new_ino, new_ino, ".", EXT2_FT_DIR, h) < 0 ||
      ext4_add_dir_entry_tx(fs, new_ino, dir_info->inode_num, "..", EXT2_FT_DIR, h) < 0) {
    if (h) journal_abort_transaction(h);
    return -EIO;
  }
  struct ext2_inode di;
  if (ext4_read_inode(fs, dir_info->inode_num, &di) == 0) {
    di.i_links_count++;
    ext4_write_inode_tx(fs, h, dir_info->inode_num, &di);
  }
  if (h) journal_commit_transaction(h);
  /* Attach the ext4 inode_info to the freshly-created VFS directory node, the
   * same way ext4_vfs_create does for files. Without this the node's
   * inode->data stays NULL and any later op on the directory (e.g. creating a
   * file inside it) dereferences NULL. */
  struct vfs_node *n = find_child(dir, name);
  if (n) {
    ext4_setup_node(n, fs, new_ino, inode.i_mode);
    vfs_node_put(n);
  }
  return 0;
}

static int ext4_vfs_unlink_tx(struct vfs_node *dir, const char *name, struct journal_handle *h) {
  struct ext4_inode_info *dir_info = (struct ext4_inode_info *)dir->inode->data;
  struct ext4_fs *fs = dir_info->fs; struct ext2_inode di;
  if (ext4_read_inode(fs, dir_info->inode_num, &di) < 0) return -EIO;
  u32 ino = 0; u32 target_phys = 0; usize target_off = 0; usize prev_off = (usize)-1;
  u8 *buf = kmalloc(fs->block_size);
  u32 blocks = (di.i_size + fs->block_size - 1) / fs->block_size;
  for (u32 b = 0; b < blocks; b++) {
    u32 phys = ext4_get_block(fs, &di, b); if (!phys) continue;
    ext4_read_block(fs, phys, buf); usize off = 0; usize local_prev = (usize)-1;
    while (off < fs->block_size) {
      struct ext2_dir_entry *e = (struct ext2_dir_entry *)(buf + off);
      if (e->rec_len == 0) break;
      if (e->inode != 0 && strlen(name) == e->name_len && memcmp(e->name, name, e->name_len) == 0) {
        ino = e->inode; target_phys = phys; target_off = off; prev_off = local_prev; break;
      }
      local_prev = off;
      off += e->rec_len;
    }
    if (ino) break;
  }
  kfree(buf); if (!ino) return -ENOENT;

  buf = kmalloc(fs->block_size);
  ext4_read_block(fs, target_phys, buf);

  struct ext2_inode ci;
  if (ext4_read_inode(fs, ino, &ci) == 0) {
    if (ci.i_links_count > 0)
      ci.i_links_count--;
    ci.i_ctime = vfs_get_unix_time();
    if (ext4_write_inode_tx(fs, h, ino, &ci) < 0) {
      kfree(buf);
      return -EIO;
    }
  }

  struct ext2_dir_entry *entry = (struct ext2_dir_entry *)(buf + target_off);
  if (prev_off != (usize)-1) {
    struct ext2_dir_entry *prev = (struct ext2_dir_entry *)(buf + prev_off);
    prev->rec_len += entry->rec_len;
  } else {
    entry->inode = 0;
  }
  if (ext4_journal_write_tx(fs, h, target_phys, buf) < 0) {
    kfree(buf);
    return -EIO;
  }

  di.i_mtime = di.i_ctime = vfs_get_unix_time();
  if (ext4_write_inode_tx(fs, h, dir_info->inode_num, &di) < 0) {
    kfree(buf);
    return -EIO;
  }
  kfree(buf);
  return 0;
}

static int ext4_vfs_unlink(struct vfs_node *dir, const char *name) {
  struct ext4_inode_info *dir_info = (struct ext4_inode_info *)dir->inode->data;
  struct ext4_fs *fs = dir_info->fs;
  struct journal_handle *h = fs->jdev ? journal_start_transaction(fs->jdev) : 0;
  int err = ext4_vfs_unlink_tx(dir, name, h);
  if (err < 0) {
    if (h) journal_abort_transaction(h);
    return err;
  }
  if (h) journal_commit_transaction(h);
  return 0;
}

static int ext4_vfs_rmdir(struct vfs_node *dir, const char *name) {
  struct ext4_inode_info *dir_info = (struct ext4_inode_info *)dir->inode->data;
  struct ext4_fs *fs = dir_info->fs;
  struct journal_handle *h = fs->jdev ? journal_start_transaction(fs->jdev) : 0;
  int err = ext4_vfs_unlink_tx(dir, name, h);
  if (err < 0) {
    if (h) journal_abort_transaction(h);
    return err;
  }
  struct ext2_inode di;
  if (ext4_read_inode(fs, dir_info->inode_num, &di) == 0) {
    if (di.i_links_count > 2)
      di.i_links_count--;
    di.i_mtime = di.i_ctime = vfs_get_unix_time();
    ext4_write_inode_tx(fs, h, dir_info->inode_num, &di);
  }
  if (h) journal_commit_transaction(h);
  return 0;
}

static int ext4_vfs_rename(struct vfs_node *old_dir, const char *old_name,
                           struct vfs_node *new_dir, const char *new_name) {
  struct ext4_inode_info *old_dir_info = (struct ext4_inode_info *)old_dir->inode->data;
  struct ext4_fs *fs = old_dir_info->fs;
  struct journal_handle *h = fs->jdev ? journal_start_transaction(fs->jdev) : 0;
  struct ext2_inode old_di;
  if (ext4_read_inode(fs, old_dir_info->inode_num, &old_di) < 0) {
    if (h) journal_abort_transaction(h);
    return -EIO;
  }
  u32 ino = 0; u8 type = 0; u8 *buf = kmalloc(fs->block_size);
  u32 blocks = (old_di.i_size + fs->block_size - 1) / fs->block_size;
  for (u32 b = 0; b < blocks; b++) {
    u32 phys = ext4_get_block(fs, &old_di, b); if (!phys) continue;
    ext4_read_block(fs, phys, buf); usize off = 0;
    while (off < fs->block_size) {
      struct ext2_dir_entry *e = (struct ext2_dir_entry *)(buf + off);
      if (e->rec_len == 0) break;
      if (e->inode != 0 && strlen(old_name) == e->name_len && memcmp(e->name, old_name, e->name_len) == 0) {
        ino = e->inode; type = e->file_type; break;
      }
      off += e->rec_len;
    }
    if (ino) break;
  }
  kfree(buf);
  if (!ino) {
    if (h) journal_abort_transaction(h);
    return -ENOENT;
  }
  struct ext4_inode_info *new_dir_info = (struct ext4_inode_info *)new_dir->inode->data;
  if (ext4_add_dir_entry_tx(fs, new_dir_info->inode_num, ino, new_name, type, h) < 0) {
    if (h) journal_abort_transaction(h);
    return -EIO;
  }
  if (ext4_vfs_unlink_tx(old_dir, old_name, h) < 0) {
    if (h) journal_abort_transaction(h);
    return -EIO;
  }
  struct ext2_inode moved;
  if (ext4_read_inode(fs, ino, &moved) == 0) {
    moved.i_ctime = vfs_get_unix_time();
    ext4_write_inode_tx(fs, h, ino, &moved);
  }
  if (h) journal_commit_transaction(h);
  return 0;
}

static int ext4_vfs_symlink(struct vfs_node *dir, const char *name, const char *target) {
  struct ext4_inode_info *dir_info = (struct ext4_inode_info *)dir->inode->data;
  struct ext4_fs *fs = dir_info->fs;
  struct journal_handle *h = fs->jdev ? journal_start_transaction(fs->jdev) : 0;
  u32 new_ino = ext4_alloc_inode_tx(fs, h);
  if (!new_ino) {
    if (h) journal_abort_transaction(h);
    return -ENOSPC;
  }
  struct ext2_inode inode; memset(&inode, 0, sizeof(inode));
  inode.i_mode = EXT2_S_IFLNK | 0777; inode.i_links_count = 1;
  inode.i_size = strlen(target); inode.i_atime = inode.i_mtime = inode.i_ctime = vfs_get_unix_time();
  if (inode.i_size < 60) memcpy(inode.i_block, target, inode.i_size);
  else {
    u32 block = ext4_alloc_block_tx(fs, h);
    if (!block) {
      ext4_free_inode_tx(fs, new_ino, h);
      if (h) journal_abort_transaction(h);
      return -ENOSPC;
    }
    inode.i_block[0] = block;
    u8 *tmp_buf = kmalloc(fs->block_size);
    memset(tmp_buf, 0, fs->block_size);
    memcpy(tmp_buf, target, inode.i_size < fs->block_size ? inode.i_size : fs->block_size);
    ext4_journal_write_tx(fs, h, block, tmp_buf);
    kfree(tmp_buf);
  }
  if (ext4_write_inode_tx(fs, h, new_ino, &inode) < 0) {
    if (h) journal_abort_transaction(h);
    return -EIO;
  }
  if (ext4_add_dir_entry_tx(fs, dir_info->inode_num, new_ino, name, EXT2_FT_SYMLINK, h) < 0) {
    if (h) journal_abort_transaction(h);
    return -EIO;
  }
  if (h) journal_commit_transaction(h);
  return 0;
}

static int ext4_vfs_link(struct vfs_node *target_node, struct vfs_node *dir, const char *name) {
  struct ext4_inode_info *target_info = (struct ext4_inode_info *)target_node->inode->data;
  struct ext4_fs *fs = target_info->fs;
  struct journal_handle *h = fs->jdev ? journal_start_transaction(fs->jdev) : 0;
  struct ext2_inode inode;
  if (ext4_read_inode(fs, target_info->inode_num, &inode) < 0) {
    if (h) journal_abort_transaction(h);
    return -EIO;
  }
  inode.i_links_count++;
  if (ext4_write_inode_tx(fs, h, target_info->inode_num, &inode) < 0) {
    if (h) journal_abort_transaction(h);
    return -EIO;
  }
  struct ext4_inode_info *dir_info = (struct ext4_inode_info *)dir->inode->data;
  if (ext4_add_dir_entry_tx(fs, dir_info->inode_num, target_info->inode_num, name, EXT2_FT_REG_FILE, h) < 0) {
    if (h) journal_abort_transaction(h);
    return -EIO;
  }
  if (h) journal_commit_transaction(h);
  return 0;
}

static int ext4_vfs_setattr(struct vfs_node *node) {
  struct ext4_inode_info *info = (struct ext4_inode_info *)node->inode->data;
  struct ext4_fs *fs = info->fs; struct ext2_inode inode;
  if (ext4_read_inode(fs, info->inode_num, &inode) < 0) return -EIO;
  node->inode->ctime = vfs_get_unix_time();
  inode.i_mode = (inode.i_mode & ~07777) | (node->inode->mode & 07777);
  inode.i_uid = node->inode->uid; inode.i_gid = node->inode->gid;
  inode.i_size = (u32)node->inode->size;
  inode.i_atime = node->inode->atime; inode.i_mtime = node->inode->mtime; inode.i_ctime = node->inode->ctime;
  if (fs->features_ro_compat & EXT4_FEATURE_RO_COMPAT_HUGE_FILE) {
    inode.i_dir_acl = (u32)(node->inode->size >> 32);
  }
  return ext4_write_inode(fs, info->inode_num, &inode);
}

static int ext4_vfs_statfs(struct vfs_node *node, struct b1nix_statfs *st) {
  struct ext4_inode_info *info = (struct ext4_inode_info *)node->inode->data;
  struct ext4_fs *fs = info->fs; memset(st, 0, sizeof(*st));
  st->f_type = EXT2_SUPER_MAGIC; st->f_bsize = fs->block_size;
  st->f_blocks = fs->sb.s_blocks_count; st->f_bfree = fs->sb.s_free_blocks_count;
  st->f_bavail = fs->sb.s_free_blocks_count; st->f_files = fs->sb.s_inodes_count;
  st->f_ffree = fs->sb.s_free_inodes_count; st->f_namelen = 255;
  return 0;
}

static int ext4_vfs_fsync(struct vfs_node *node) {
  if (node && node->inode && node->inode->blk_dev) {
    blk_cache_flush(node->inode->blk_dev);
  }
  return 0;
}

static void ext4_vfs_release(struct vfs_node *node) {
  struct ext4_inode_info *info = (struct ext4_inode_info *)node->inode->data;
  struct ext4_fs *fs = info->fs; struct ext2_inode inode;
  if (ext4_read_inode(fs, info->inode_num, &inode) == 0) {
    if (inode.i_links_count == 0) {
      u32 blocks = (inode.i_size + fs->block_size - 1) / fs->block_size;
      for (u32 b = 0; b < blocks; b++) { u32 phys = ext4_get_block(fs, &inode, b); if (phys) ext4_free_block(fs, phys); }
      if (!(inode.i_flags & EXT4_EXTENTS_FL)) {
        if (inode.i_block[EXT2_IND_BLOCK]) {
          ext4_free_block(fs, inode.i_block[EXT2_IND_BLOCK]);
        }
      }
      ext4_free_inode(fs, info->inode_num);
    }
  }
}

static void ext4_setup_node(struct vfs_node *n, struct ext4_fs *fs, u32 ino, u32 mode) {
  struct ext4_inode_info *ni = kmalloc(sizeof(struct ext4_inode_info));
  ni->fs = fs; ni->inode_num = ino;
  n->inode->data = ni; n->inode->blk_dev = fs->bdev;
  n->inode->read_cb = ext4_vfs_read; n->inode->write_cb = ext4_vfs_write;
  n->inode->truncate_cb = ext4_vfs_truncate;
  n->inode->setattr_cb = ext4_vfs_setattr; n->inode->statfs_cb = ext4_vfs_statfs;
  n->inode->unlink_cb = ext4_vfs_unlink; n->inode->release_cb = ext4_vfs_release;
  n->inode->fsync_cb = ext4_vfs_fsync;
  if ((mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
    n->inode->create_cb = ext4_vfs_create; n->inode->mkdir_cb = ext4_vfs_mkdir;
    n->inode->readdir_cb = ext4_vfs_readdir; n->inode->rmdir_cb = ext4_vfs_rmdir;
    n->inode->rename_cb = ext4_vfs_rename; n->inode->symlink_cb = ext4_vfs_symlink;
    n->inode->link_cb = ext4_vfs_link;
  }
}

static void ext4_populate_vfs(struct ext4_fs *fs, u32 ino, const char *base_path) {
    struct ext2_inode inode; if (ext4_read_inode(fs, ino, &inode) < 0) return;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return;
    u8 *buf = kmalloc(inode.i_size);
    for (u32 i = 0; i < (inode.i_size + fs->block_size - 1) / fs->block_size; i++) {
        u32 phys = ext4_get_block(fs, &inode, i); if (phys) ext4_read_block(fs, phys, buf + i * fs->block_size);
    }
    usize off = 0;
    while (off < inode.i_size) {
        struct ext2_dir_entry *e = (struct ext2_dir_entry *)(buf + off);
        if (e->rec_len == 0 || e->inode == 0) { off += e->rec_len ? e->rec_len : 4; continue; }
        char name[256]; memcpy(name, e->name, e->name_len); name[e->name_len] = '\0';
        if (strcmp(name, ".") && strcmp(name, "..")) {
            char full[256]; usize len = strlen(base_path);
            memcpy(full, base_path, len); if (full[len - 1] != '/') full[len++] = '/';
            memcpy(full + len, name, e->name_len + 1);
            struct ext2_inode ci;
            if (ext4_read_inode(fs, e->inode, &ci) == 0) {
                u32 fmt = ci.i_mode & EXT2_S_IFMT;
                if (fmt == EXT2_S_IFLNK) {
                    /* Symlink: read its target and create a VFS_SYMLINK node so
                     * path resolution and readlink work. Do NOT call
                     * ext4_setup_node here — it overwrites inode->data (where the
                     * link target lives) with its ext4 inode_info. */
                    u32 tlen = ci.i_size;
                    if (tlen > 4095) tlen = 4095;
                    char *tgt = kmalloc(tlen + 1);
                    int ok = 0;
                    if (ci.i_size < 60) {
                        memcpy(tgt, (const char *)ci.i_block, tlen); ok = 1; /* fast symlink */
                    } else {
                        u32 phys = ext4_get_block(fs, &ci, 0);
                        if (phys) {
                            u8 *blk = kmalloc(fs->block_size);
                            ext4_read_block(fs, phys, blk);
                            memcpy(tgt, blk, tlen < fs->block_size ? tlen : fs->block_size);
                            kfree(blk); ok = 1;
                        }
                    }
                    if (ok) {
                        tgt[tlen] = '\0';
                        vfs_add_node(full, VFS_SYMLINK, tgt, tlen, VFS_NODE_OWNS_DATA);
                    } else {
                        kfree(tgt);
                    }
                } else {
                    struct vfs_node *n = vfs_add_node(full, (fmt == EXT2_S_IFDIR) ? VFS_DIRECTORY : VFS_FILE, 0, ci.i_size, 0);
                    if (n) {
                        ext4_setup_node(n, fs, e->inode, ci.i_mode);
                        n->inode->mode = ci.i_mode & 07777;
                        n->inode->uid = ci.i_uid;
                        n->inode->gid = ci.i_gid;
                        n->inode->atime = ci.i_atime;
                        n->inode->mtime = ci.i_mtime;
                        n->inode->ctime = ci.i_ctime;
                        n->inode->nlink = ci.i_links_count;
                        n->inode->fs_id = n->parent->inode->fs_id;
                        if (fmt == EXT2_S_IFDIR) ext4_populate_vfs(fs, e->inode, full);
                    }
                }
            }
        }
        off += e->rec_len;
    }
    kfree(buf);
}

static struct vfs_node *ext4_vfs_mount_cb(const char *source, u64 flags, void *data) {
    (void)flags; struct block_device *dev = blk_get(source); if (!dev) return ERR_PTR(-ENODEV);
    u8 *sb_buf = kmalloc(1024); if (blk_read_cached(dev, 2, 2, sb_buf) < 0) { kfree(sb_buf); return ERR_PTR(-EIO); }
    struct ext2_superblock *sb = (struct ext2_superblock *)sb_buf;
    if (sb->s_magic != EXT2_SUPER_MAGIC) { kfree(sb_buf); return ERR_PTR(-EINVAL); }
    struct ext4_fs *fs = kmalloc(sizeof(struct ext4_fs)); memset(fs, 0, sizeof(struct ext4_fs));
    fs->bdev = dev; memcpy(&fs->sb, sb, sizeof(struct ext2_superblock));
    fs->block_size = 1024 << fs->sb.s_log_block_size;
    fs->inodes_per_group = fs->sb.s_inodes_per_group;
    fs->inode_size = (fs->sb.s_rev_level == 0) ? 128 : fs->sb.s_inode_size;
    fs->features_incompat = fs->sb.s_feature_incompat;
    fs->features_ro_compat = fs->sb.s_feature_ro_compat;
    if (!(fs->features_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS)) { kfree(sb_buf); kfree(fs); return ERR_PTR(-EOPNOTSUPP); }
    fs->desc_size = 32;
    if (fs->features_incompat & EXT4_FEATURE_INCOMPAT_64BIT) { u16 tmp; memcpy(&tmp, sb_buf + EXT4_SB_DESC_SIZE_OFF, 2); fs->desc_size = tmp >= 32 ? tmp : 64; }
    fs->flex_size = 1;
    if (fs->features_incompat & EXT4_FEATURE_INCOMPAT_FLEX_BG) { fs->flex_size = 1 << sb_buf[EXT4_SB_LOG_GROUPS_PER_FLEX]; if (fs->flex_size == 0) fs->flex_size = 1; }
    if (fs->sb.s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL) {
        memcpy(&fs->journal_inum, sb_buf + EXT3_SB_JOURNAL_INUM_OFF, 4);
        if (fs->journal_inum != 0) {
            ext4_read_inode(fs, fs->journal_inum, &fs->journal_inode_cache);
            fs->jdev = journal_mount(fs, fs->block_size, &ext4_jbd_ops);
            if (fs->jdev) {
                if (fs->sb.s_feature_incompat & EXT3_FEATURE_INCOMPAT_RECOVER) {
                    console_write("ext4: journal needs recovery\n");
                    if (journal_recover(fs->jdev) == 0) {
                        console_write("ext4: journal recovery complete\n");
                    }
                }
                /* Mark as needing recovery (cleared on clean umount).
                 * Must be AFTER recovery so next mount re-replays on crash. */
                fs->sb.s_feature_incompat |= EXT3_FEATURE_INCOMPAT_RECOVER;
                ext4_write_superblock(fs);
            }
        }
    }
    kfree(sb_buf);
    struct vfs_node *root = vfs_create_node(VFS_DIRECTORY);
    ext4_setup_node(root, fs, 2, EXT2_S_IFDIR);
    struct ext2_inode ri;
    if (ext4_read_inode(fs, 2, &ri) == 0) {
        root->inode->mode = ri.i_mode & 07777;
        root->inode->uid = ri.i_uid;
        root->inode->gid = ri.i_gid;
    }
    vfs_set_currently_mounting_root(root);
    if (data) ext4_populate_vfs(fs, 2, (const char *)data);
    return root;
}

static int ext4_vfs_umount_cb(struct vfs_node *root_node) {
    struct ext4_inode_info *ni = (struct ext4_inode_info *)root_node->inode->data;
    struct ext4_fs *fs = ni->fs;
    if (fs->sb.s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL) {
        if (fs->jdev) {
            /* Clear RECOVER flag: filesystem was cleanly unmounted */
            fs->sb.s_feature_incompat &= ~EXT3_FEATURE_INCOMPAT_RECOVER;
            ext4_write_superblock(fs);
            console_write("ext4: clean umount, RECOVER flag cleared\n");
        }
    }
    return 0;
}

static struct vfs_fs ext4_vfs = { .name = "ext4", .mount = ext4_vfs_mount_cb, .umount = ext4_vfs_umount_cb };

void ext4_init(void) {
    vfs_register_fs(&ext4_vfs);
    vfs_mount("virtio-blk0", "/mnt/ext4", "ext4", 0);
}
