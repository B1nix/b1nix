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

static struct block_device *ext4_dev = 0;
static struct ext2_superblock ext4_sb;
static u32 ext4_block_size;
static u32 ext4_inodes_per_group;
static u32 ext4_inode_size;
static u32 ext4_desc_size;
static u32 ext4_flex_size;
static u32 ext4_features_incompat;
static u32 ext4_features_ro_compat;

/* JBD State */
static u32 journal_inum = 0;
static struct ext2_inode journal_inode_cache;
static struct journal_dev *ext4_jdev = 0;

static int ext4_read_block(u32 block, void *buffer) {
  return blk_read_cached(ext4_dev, (u64)block * (ext4_block_size / 512),
                         ext4_block_size / 512, buffer);
}

static int ext4_write_block(u32 block, const void *buffer) {
  return blk_write_cached(ext4_dev, (u64)block * (ext4_block_size / 512),
                          ext4_block_size / 512, buffer);
}

/* --- JBD Callbacks --- */
static u32 ext4_get_block(struct ext2_inode *inode,
                          u32 block_idx); /* Forward decl */

static int ext4_jbd_read(struct journal_dev *jdev, u32 logical, void *buf) {
  (void)jdev;
  u32 phys = ext4_get_block(&journal_inode_cache, logical);
  if (!phys)
    return -1;
  return ext4_read_block(phys, buf);
}

static int ext4_jbd_write(struct journal_dev *jdev, u32 logical,
                          const void *buf) {
  (void)jdev;
  u32 phys = ext4_get_block(&journal_inode_cache, logical);
  if (!phys)
    return -1; /* The journal is preallocated by mke2fs. */
  return ext4_write_block(phys, buf);
}

static int ext4_jbd_fs_write(struct journal_dev *jdev, u32 phys,
                             const void *buf) {
  (void)jdev;
  return ext4_write_block(phys, buf);
}

static struct journal_ops ext4_jbd_ops = {ext4_jbd_read, ext4_jbd_write,
                                          ext4_jbd_fs_write};

/* Wrap data writes in small journal transactions. */
static int ext4_journal_write(u32 block, const void *buffer) {
  if (ext4_jdev) {
    struct journal_handle *h = journal_start_transaction(ext4_jdev);
    if (h) {
      journal_log_block(h, block, buffer);
      journal_commit_transaction(h);
      return 0;
    }
  }
  return ext4_write_block(block, buffer);
}

static u32 ext4_get_desc_block(u32 group) {
    u32 desc_per_block = ext4_block_size / ext4_desc_size;
    if (ext4_features_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
        if (ext4_sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_META_BG) {
            u32 meta_bg = group / desc_per_block;
            return (meta_bg * desc_per_block) + 1;
        }
    return (ext4_block_size == 1024 ? 2 : 1) +
           (group / desc_per_block) * desc_per_block;
    }
    if (ext4_features_incompat & EXT4_FEATURE_INCOMPAT_FLEX_BG) {
        return (ext4_block_size == 1024 ? 2 : 1) + (group / desc_per_block);
    }
    return (ext4_block_size == 1024) ? 2 : 1;
}

static void ext4_read_bgd(u32 group, struct ext4_bgd_64 *bgd) {
    u32 bg_block = ext4_get_desc_block(group);
    u32 desc_per_block = ext4_block_size / ext4_desc_size;
    u32 bg_offset = (group % desc_per_block) * ext4_desc_size;
    u8 *buf = kmalloc(ext4_block_size);
    ext4_read_block(bg_block, buf);

    if (ext4_desc_size >= 64) {
        memcpy(bgd, buf + bg_offset, sizeof(struct ext4_bgd_64));
    } else {
        struct ext2_block_group_desc bg32;
        memcpy(&bg32, buf + bg_offset, sizeof(bg32));
        memset(bgd, 0, sizeof(struct ext4_bgd_64));
        bgd->bg_block_bitmap_lo = bg32.bg_block_bitmap;
        bgd->bg_inode_bitmap_lo = bg32.bg_inode_bitmap;
        bgd->bg_inode_table_lo = bg32.bg_inode_table;
        bgd->bg_free_blocks_count_lo = bg32.bg_free_blocks_count;
        bgd->bg_free_inodes_count_lo = bg32.bg_free_inodes_count;
    }
    kfree(buf);
}

static void ext4_write_bgd(u32 group, struct ext4_bgd_64 *bgd) {
    u32 bg_block = ext4_get_desc_block(group);
    u32 desc_per_block = ext4_block_size / ext4_desc_size;
    u32 bg_offset = (group % desc_per_block) * ext4_desc_size;
    u8 *buf = kmalloc(ext4_block_size);
    ext4_read_block(bg_block, buf);

    if (ext4_desc_size >= 64) {
        memcpy(buf + bg_offset, bgd, sizeof(struct ext4_bgd_64));
    } else {
        struct ext2_block_group_desc bg32;
        bg32.bg_block_bitmap = bgd->bg_block_bitmap_lo;
        bg32.bg_inode_bitmap = bgd->bg_inode_bitmap_lo;
        bg32.bg_inode_table = bgd->bg_inode_table_lo;
        bg32.bg_free_blocks_count = bgd->bg_free_blocks_count_lo;
        bg32.bg_free_inodes_count = bgd->bg_free_inodes_count_lo;
        memcpy(buf + bg_offset, &bg32, sizeof(bg32));
    }
  ext4_journal_write(bg_block, buf);
    kfree(buf);
}

static void ext4_write_superblock(void) {
    u8 *sb_buf = kmalloc(1024);
    if (blk_read_cached(ext4_dev, 2, 2, sb_buf) >= 0) {
        memcpy(sb_buf, &ext4_sb, sizeof(struct ext2_superblock));
        blk_write_cached(ext4_dev, 2, 2, sb_buf);
    }
    kfree(sb_buf);
}

static u32 ext4_bgd_block_bitmap(struct ext4_bgd_64 *bgd) {
    u32 lo = bgd->bg_block_bitmap_lo;
    if (ext4_features_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
        lo |= (u32)bgd->bg_block_bitmap_hi << 16;
    return lo;
}

static u32 ext4_bgd_inode_bitmap(struct ext4_bgd_64 *bgd) {
    u32 lo = bgd->bg_inode_bitmap_lo;
    if (ext4_features_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
        lo |= (u32)bgd->bg_inode_bitmap_hi << 16;
    return lo;
}

static u32 ext4_bgd_inode_table(struct ext4_bgd_64 *bgd) {
    u32 lo = bgd->bg_inode_table_lo;
    if (ext4_features_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
        lo |= (u32)bgd->bg_inode_table_hi << 16;
    return lo;
}

static int ext4_read_inode(u32 inode_num, struct ext2_inode *inode) {
  if (inode_num == 0)
    return -1;
  u32 group = (inode_num - 1) / ext4_inodes_per_group;
  struct ext4_bgd_64 bgd;
  ext4_read_bgd(group, &bgd);
    u32 itable = ext4_bgd_inode_table(&bgd);
  u32 inode_offset =
      ((inode_num - 1) % ext4_inodes_per_group) * ext4_inode_size;
    u32 block_idx = itable + (inode_offset / ext4_block_size);
    u8 *buf = kmalloc(ext4_block_size);
  ext4_read_block(block_idx, buf);
  memcpy(inode, buf + (inode_offset % ext4_block_size),
         sizeof(struct ext2_inode));
    kfree(buf);
    return 0;
}

static int ext4_write_inode(u32 inode_num, const struct ext2_inode *inode) {
  if (inode_num == 0)
    return -1;
  u32 group = (inode_num - 1) / ext4_inodes_per_group;
  struct ext4_bgd_64 bgd;
  ext4_read_bgd(group, &bgd);
    u32 itable = ext4_bgd_inode_table(&bgd);
  u32 inode_offset =
      ((inode_num - 1) % ext4_inodes_per_group) * ext4_inode_size;
    u32 block_idx = itable + (inode_offset / ext4_block_size);
    u8 *buf = kmalloc(ext4_block_size);
  ext4_read_block(block_idx, buf);
  memcpy(buf + (inode_offset % ext4_block_size), inode,
         sizeof(struct ext2_inode));
  int ret = ext4_journal_write(block_idx, buf);
    kfree(buf);
    return ret;
}

static u32 ext4_alloc_block(void) {
  u32 groups = (ext4_sb.s_blocks_count + ext4_sb.s_blocks_per_group - 1) /
               ext4_sb.s_blocks_per_group;
    for (u32 g = 0; g < groups; g++) {
        struct ext4_bgd_64 bgd;
        ext4_read_bgd(g, &bgd);
    if (bgd.bg_free_blocks_count_lo == 0)
      continue;
        u8 *bitmap = kmalloc(ext4_block_size);
        ext4_read_block(ext4_bgd_block_bitmap(&bgd), bitmap);
        for (u32 i = 0; i < ext4_sb.s_blocks_per_group; i++) {
            if (!(bitmap[i / 8] & (1 << (i % 8)))) {
                bitmap[i / 8] |= (1 << (i % 8));
        ext4_journal_write(ext4_bgd_block_bitmap(&bgd), bitmap);
                kfree(bitmap);
                bgd.bg_free_blocks_count_lo--;
                ext4_write_bgd(g, &bgd);
                ext4_sb.s_free_blocks_count--;
                ext4_write_superblock();
        u32 block_num = g * ext4_sb.s_blocks_per_group + i +
                        (ext4_sb.s_log_block_size == 0 ? 1 : 0);
                u8 *zero = kzalloc(ext4_block_size);
        ext4_journal_write(block_num, zero);
                kfree(zero);
                return block_num;
            }
        }
        kfree(bitmap);
    }
    return 0;
}

static u32 ext4_alloc_inode(void) {
  u32 groups = (ext4_sb.s_inodes_count + ext4_inodes_per_group - 1) /
               ext4_inodes_per_group;
    for (u32 g = 0; g < groups; g++) {
        struct ext4_bgd_64 bgd;
        ext4_read_bgd(g, &bgd);
    if (bgd.bg_free_inodes_count_lo == 0)
      continue;
        u8 *bitmap = kmalloc(ext4_block_size);
        ext4_read_block(ext4_bgd_inode_bitmap(&bgd), bitmap);
        for (u32 i = 0; i < ext4_inodes_per_group; i++) {
            if (!(bitmap[i / 8] & (1 << (i % 8)))) {
                bitmap[i / 8] |= (1 << (i % 8));
        ext4_journal_write(ext4_bgd_inode_bitmap(&bgd), bitmap);
                kfree(bitmap);
                bgd.bg_free_inodes_count_lo--;
                ext4_write_bgd(g, &bgd);
                ext4_sb.s_free_inodes_count--;
                ext4_write_superblock();
                u32 inode_num = g * ext4_inodes_per_group + i + 1;
                struct ext2_inode ni;
                memset(&ni, 0, sizeof(ni));
                ext4_write_inode(inode_num, &ni);
                return inode_num;
            }
        }
        kfree(bitmap);
    }
    return 0;
}

static void ext4_free_block(u32 block_num) {
    if (block_num == 0) return;
    u32 g = (block_num - (ext4_sb.s_log_block_size == 0 ? 1 : 0)) / ext4_sb.s_blocks_per_group;
    u32 i = (block_num - (ext4_sb.s_log_block_size == 0 ? 1 : 0)) % ext4_sb.s_blocks_per_group;
    struct ext4_bgd_64 bgd;
    ext4_read_bgd(g, &bgd);
    u8 *bitmap = kmalloc(ext4_block_size);
    ext4_read_block(ext4_bgd_block_bitmap(&bgd), bitmap);
    bitmap[i / 8] &= ~(1 << (i % 8));
    ext4_journal_write(ext4_bgd_block_bitmap(&bgd), bitmap);
    kfree(bitmap);
    bgd.bg_free_blocks_count_lo++;
    ext4_write_bgd(g, &bgd);
    ext4_sb.s_free_blocks_count++;
    ext4_write_superblock();
}

static void ext4_free_inode(u32 inode_num) {
    if (inode_num == 0) return;
    u32 g = (inode_num - 1) / ext4_inodes_per_group;
    u32 i = (inode_num - 1) % ext4_inodes_per_group;
    struct ext4_bgd_64 bgd;
    ext4_read_bgd(g, &bgd);
    u8 *bitmap = kmalloc(ext4_block_size);
    ext4_read_block(ext4_bgd_inode_bitmap(&bgd), bitmap);
    bitmap[i / 8] &= ~(1 << (i % 8));
    ext4_journal_write(ext4_bgd_inode_bitmap(&bgd), bitmap);
    kfree(bitmap);
    bgd.bg_free_inodes_count_lo++;
    ext4_write_bgd(g, &bgd);
    ext4_sb.s_free_inodes_count++;
    ext4_write_superblock();
}

static u32 ext4_extent_lookup(struct ext2_inode *inode, u32 logical_block) {
    struct ext4_extent_header *eh = (struct ext4_extent_header *)inode->i_block;
  if (eh->eh_magic != EXT4_EXTENT_MAGIC)
    return 0;
    u32 depth = eh->eh_depth;
    u8 *block_buf = kmalloc(ext4_block_size);

    if (depth == 0) {
        struct ext4_extent *exts = (struct ext4_extent *)(eh + 1);
        for (u16 i = 0; i < eh->eh_entries; i++) {
            if (logical_block >= exts[i].ee_block &&
                logical_block < exts[i].ee_block + exts[i].ee_len) {
        u32 result = (exts[i].ee_start_lo | ((u32)exts[i].ee_start_hi << 16)) +
                     (logical_block - exts[i].ee_block);
                kfree(block_buf);
                return result;
            }
        }
        kfree(block_buf);
        return 0;
    }

    struct ext4_extent_idx *idx = (struct ext4_extent_idx *)(eh + 1);
  u32 phys = 0;
    for (u32 i = 0; i < eh->eh_entries; i++) {
        if (logical_block < idx[i].ei_block) {
            if (i > 0)
        phys = idx[i - 1].ei_leaf_lo | ((u32)idx[i - 1].ei_leaf_hi << 16);
            break;
        }
    }
  if (phys == 0 && eh->eh_entries > 0)
    phys = idx[eh->eh_entries - 1].ei_leaf_lo |
           ((u32)idx[eh->eh_entries - 1].ei_leaf_hi << 16);
  if (!phys) {
    kfree(block_buf);
    return 0;
  }

    for (u32 level = depth; level > 0 && phys; level--) {
        ext4_read_block(phys, block_buf);
    struct ext4_extent_header *node_hdr =
        (struct ext4_extent_header *)block_buf;
        if (level == 1) {
            struct ext4_extent *exts = (struct ext4_extent *)(node_hdr + 1);
            for (u16 i = 0; i < node_hdr->eh_entries; i++) {
                if (logical_block >= exts[i].ee_block &&
                    logical_block < exts[i].ee_block + exts[i].ee_len) {
          u32 result =
              (exts[i].ee_start_lo | ((u32)exts[i].ee_start_hi << 16)) +
              (logical_block - exts[i].ee_block);
                    kfree(block_buf);
                    return result;
                }
            }
            break;
        }

        struct ext4_extent_idx *indices = (struct ext4_extent_idx *)(node_hdr + 1);
        phys = 0;
        for (u16 i = 0; i < node_hdr->eh_entries; i++) {
            if (logical_block < indices[i].ei_block) {
                if (i > 0)
          phys = indices[i - 1].ei_leaf_lo |
                 ((u32)indices[i - 1].ei_leaf_hi << 16);
                break;
            }
        }
        if (phys == 0 && node_hdr->eh_entries > 0)
      phys = indices[node_hdr->eh_entries - 1].ei_leaf_lo |
             ((u32)indices[node_hdr->eh_entries - 1].ei_leaf_hi << 16);
    }

    kfree(block_buf);
    return 0;
}

static int ext4_add_extent(struct ext2_inode *inode, u32 logical, u32 physical,
                           u16 len) {
    struct ext4_extent_header *eh = (struct ext4_extent_header *)inode->i_block;
    if (eh->eh_magic == EXT4_EXTENT_MAGIC && eh->eh_depth == 0) {
        struct ext4_extent *exts = (struct ext4_extent *)(eh + 1);
        u16 n = eh->eh_entries;
        if (n > 0) {
            struct ext4_extent *last = &exts[n - 1];
      if (logical == last->ee_block + last->ee_len &&
          (last->ee_start_lo + last->ee_len) == physical) {
        if ((u32)last->ee_len + len <= 32768) {
          last->ee_len += len;
                    return 1;
                }
            }
        }
        if (n < eh->eh_max) {
      exts[n].ee_block = logical;
      exts[n].ee_len = len;
      exts[n].ee_start_lo = physical;
      exts[n].ee_start_hi = 0;
            eh->eh_entries = n + 1;
            return 1;
        }
        return 0;
    }

    memset(inode->i_block, 0, sizeof(inode->i_block));
    eh->eh_magic = EXT4_EXTENT_MAGIC;
    eh->eh_entries = 1;
  eh->eh_max = 4;
    eh->eh_depth = 0;
    eh->eh_generation = 0;

    struct ext4_extent *ext = (struct ext4_extent *)(eh + 1);
    ext->ee_block = logical;
    ext->ee_len = len;
    ext->ee_start_lo = physical;
    ext->ee_start_hi = 0;

    inode->i_flags |= EXT4_EXTENTS_FL;
    return 1;
}

static u32 ext4_get_block(struct ext2_inode *inode, u32 block_idx) {
    if (inode->i_flags & EXT4_EXTENTS_FL)
        return ext4_extent_lookup(inode, block_idx);

    if (block_idx < EXT2_NDIR_BLOCKS)
        return inode->i_block[block_idx];

    u32 ptrs = ext4_block_size / 4;
    block_idx -= EXT2_NDIR_BLOCKS;

    if (block_idx < ptrs) {
    if (!inode->i_block[EXT2_IND_BLOCK])
      return 0;
        u32 *ind = kmalloc(ext4_block_size);
        ext4_read_block(inode->i_block[EXT2_IND_BLOCK], ind);
        u32 r = ind[block_idx];
        kfree(ind);
        return r;
    }
    return 0;
}

static int ext4_set_block(struct ext2_inode *inode, u32 block_idx, u32 phys) {
    if (inode->i_flags & EXT4_EXTENTS_FL || block_idx >= EXT2_NDIR_BLOCKS) {
        return ext4_add_extent(inode, block_idx, phys, 1);
    }
    inode->i_block[block_idx] = phys;
    return 1;
}

static u64 ext4_get_inode_size(struct ext2_inode *inode) {
    u64 size = inode->i_size;
    if ((ext4_features_ro_compat & EXT4_FEATURE_RO_COMPAT_HUGE_FILE) &&
        (inode->i_flags & EXT4_EOFBLOCKS_FL)) {
    size |= ((u64)inode->i_dir_acl) << 32;
    }
    return size;
}

static isize ext4_vfs_read(struct vfs_node *node, u64 offset, char *buffer,
                           usize size) {
    u32 ino = (u32)(usize)node->inode->data;
    struct ext2_inode inode;
  if (ext4_read_inode(ino, &inode) < 0)
    return -1;

    u64 inode_sz = ext4_get_inode_size(&inode);
  if (offset >= inode_sz)
    return 0;

    usize remaining = (usize)(inode_sz - offset);
    usize to_read = size < remaining ? size : remaining;
    usize done = 0;
    u8 *block_buf = kmalloc(ext4_block_size);

    while (done < to_read) {
        u32 b_idx = (u32)((offset + done) / ext4_block_size);
        u32 b_off = (u32)((offset + done) % ext4_block_size);
        u32 phys = ext4_get_block(&inode, b_idx);
        usize chunk = ext4_block_size - b_off;
    if (chunk > to_read - done)
      chunk = to_read - done;

        if (phys) {
            ext4_read_block(phys, block_buf);
            memcpy(buffer + done, block_buf + b_off, chunk);
        } else {
            memset(buffer + done, 0, chunk);
        }
        done += chunk;
    }
    kfree(block_buf);
    return done;
}

static isize ext4_vfs_write(struct vfs_node *node, u64 offset,
                            const char *buffer, usize size) {
    u32 ino = (u32)(usize)node->inode->data;
    struct ext2_inode inode;
  if (ext4_read_inode(ino, &inode) < 0)
    return -1;

    u64 new_size = offset + size;
    u32 old_blks = (inode.i_size + ext4_block_size - 1) / ext4_block_size;
    u32 new_blks = (new_size + ext4_block_size - 1) / ext4_block_size;

    if (new_size > inode.i_size) {
        for (u32 b = old_blks; b < new_blks; b++) {
            u32 pblk = ext4_alloc_block();
      if (!pblk)
        return -1;
      if (!ext4_set_block(&inode, b, pblk))
        return -1;
            inode.i_blocks += ext4_block_size / 512;
        }
        inode.i_size = (u32)new_size;
        ext4_write_inode(ino, &inode);
        node->inode->size = (usize)ext4_get_inode_size(&inode);
    }

    usize done = 0;
    u8 *block_buf = kmalloc(ext4_block_size);
    while (done < size) {
        u32 b_idx = (u32)((offset + done) / ext4_block_size);
        u32 b_off = (u32)((offset + done) % ext4_block_size);
        u32 phys = ext4_get_block(&inode, b_idx);
    if (!phys)
      break;

        usize chunk = ext4_block_size - b_off;
    if (chunk > size - done)
      chunk = size - done;

    if (chunk < ext4_block_size)
      ext4_read_block(phys, block_buf);
        memcpy(block_buf + b_off, buffer + done, chunk);
    ext4_journal_write(phys, block_buf);
        done += chunk;
    }
    kfree(block_buf);
    return done;
}

static int ext4_add_dir_entry(u32 dir_ino, u32 child_ino, const char *name,
                              u8 type) {
    struct ext2_inode dir;
  if (ext4_read_inode(dir_ino, &dir) < 0)
    return -1;

    usize name_len = strlen(name);
  if (name_len > 255)
    name_len = 255;
    u32 needed = 8 + ((name_len + 3) & ~3);
    u8 *buf = kmalloc(ext4_block_size);
    u32 blocks = (dir.i_size + ext4_block_size - 1) / ext4_block_size;

    for (u32 b = 0; b < blocks; b++) {
        u32 phys = ext4_get_block(&dir, b);
    if (!phys)
      continue;
        ext4_read_block(phys, buf);
        usize off = 0;
        while (off < ext4_block_size) {
            struct ext2_dir_entry *e = (struct ext2_dir_entry *)(buf + off);
      if (e->rec_len == 0)
        break;
            u32 actual = 8 + ((e->name_len + 3) & ~3);
            if (e->rec_len >= actual + needed) {
                e->rec_len = actual;
        struct ext2_dir_entry *ne =
            (struct ext2_dir_entry *)(buf + off + actual);
                ne->inode = child_ino;
                ne->rec_len = e->rec_len - actual;
                ne->name_len = name_len;
                ne->file_type = type;
                memcpy(ne->name, name, name_len);
        ext4_journal_write(phys, buf);
                kfree(buf);
                return 0;
            }
            off += e->rec_len;
        }
    }

    u32 phys = ext4_alloc_block();
    if (phys) {
        ext4_set_block(&dir, blocks, phys);
        dir.i_blocks += ext4_block_size / 512;
        dir.i_size += ext4_block_size;
        ext4_write_inode(dir_ino, &dir);

        memset(buf, 0, ext4_block_size);
        struct ext2_dir_entry *e = (struct ext2_dir_entry *)buf;
        e->inode = child_ino;
        e->rec_len = ext4_block_size;
        e->name_len = name_len;
        e->file_type = type;
        memcpy(e->name, name, name_len);
    ext4_journal_write(phys, buf);
        kfree(buf);
        return 0;
    }

    kfree(buf);
    return -1;
}

static isize ext4_vfs_readdir(struct vfs_node *dir, usize offset, struct dirent *buf, usize max_entries) {
    u32 inode_num = (u32)(usize)dir->inode->data;
    struct ext2_inode inode;
    if (ext4_read_inode(inode_num, &inode) < 0) return -EIO;

    u8 *dir_buf = kmalloc(ext4_block_size);
    usize count = 0;
    usize entry_idx = 0;
    u32 blocks = (u32)((inode.i_size + ext4_block_size - 1) / ext4_block_size);

    for (u32 b = 0; b < blocks && count < max_entries; b++) {
        u32 phys = ext4_get_block(&inode, b);
        if (!phys) continue;
        ext4_read_block(phys, dir_buf);
        usize off = 0;
        while (off < ext4_block_size && count < max_entries) {
            struct ext2_dir_entry *e = (struct ext2_dir_entry *)(dir_buf + off);
            if (e->rec_len == 0) break;
            if (e->inode != 0) {
                if (entry_idx >= offset) {
                    usize name_len = e->name_len > 63 ? 63 : e->name_len;
                    memcpy(buf[count].name, e->name, name_len);
                    buf[count].name[name_len] = '\0';
                    buf[count].type = (u32)VFS_FILE;
                    if (e->file_type == EXT2_FT_DIR) buf[count].type = (u32)VFS_DIRECTORY;
                    buf[count].is_dir = (e->file_type == EXT2_FT_DIR);
                    buf[count].size = 0;
                    count++;
                }
                entry_idx++;
            }
            off += e->rec_len;
        }
    }
    kfree(dir_buf);
    return (isize)count;
}

static int ext4_vfs_fsync(struct vfs_node *node) {
    (void)node;
    /* Current ext4 implementation already commits in journal_write. */
    return 0;
}

static int ext4_vfs_unlink(struct vfs_node *dir, const char *name) {
    u32 dir_ino = (u32)(usize)dir->inode->data;
    struct ext2_inode di;
    if (ext4_read_inode(dir_ino, &di) < 0) return -EIO;

    u32 ino = 0;
    u8 *buf = kmalloc(ext4_block_size);
    u32 blocks = (u32)((di.i_size + ext4_block_size - 1) / ext4_block_size);
    for (u32 b = 0; b < blocks; b++) {
        u32 phys = ext4_get_block(&di, b);
        if (!phys) continue;
        ext4_read_block(phys, buf);
        usize off = 0;
        while (off < ext4_block_size) {
            struct ext2_dir_entry *e = (struct ext2_dir_entry *)(buf + off);
            if (e->rec_len == 0) break;
            if (e->inode != 0 && strlen(name) == e->name_len && memcmp(e->name, name, e->name_len) == 0) {
                ino = e->inode;
                break;
            }
            off += e->rec_len;
        }
        if (ino) break;
    }
    kfree(buf);
    if (!ino) return -ENOENT;

    buf = kmalloc(ext4_block_size);
    for (u32 b = 0; b < blocks; b++) {
        u32 phys = ext4_get_block(&di, b);
        if (!phys) continue;
        ext4_read_block(phys, buf);
        usize off = 0;
        struct ext2_dir_entry *prev = 0;
        while (off < ext4_block_size) {
            struct ext2_dir_entry *e = (struct ext2_dir_entry *)(buf + off);
            if (e->rec_len == 0) break;
            if (e->inode == ino && strlen(name) == e->name_len && memcmp(e->name, name, e->name_len) == 0) {
                if (prev) prev->rec_len += e->rec_len; else e->inode = 0;
                ext4_journal_write(phys, buf);
                break;
            }
            off += e->rec_len; prev = e;
        }
    }
    kfree(buf);

    struct ext2_inode ci;
    if (ext4_read_inode(ino, &ci) == 0) {
        if (ci.i_links_count > 0) {
            ci.i_links_count--;
            ext4_write_inode(ino, &ci);
        }
    }
    return 0;
}

static int ext4_vfs_rmdir(struct vfs_node *dir, const char *name) {
    int err = ext4_vfs_unlink(dir, name);
    if (err == 0) {
        u32 dir_ino = (u32)(usize)dir->inode->data;
        struct ext2_inode di;
        if (ext4_read_inode(dir_ino, &di) == 0) {
            if (di.i_links_count > 2) {
                di.i_links_count--;
                ext4_write_inode(dir_ino, &di);
            }
        }
    }
    return err;
}

static int ext4_vfs_mkdir(struct vfs_node *dir, const char *name, u32 mode) {
    u32 dir_ino = (u32)(usize)dir->inode->data;
    u32 new_ino = ext4_alloc_inode();
    if (!new_ino) return -ENOSPC;

    struct ext2_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = EXT2_S_IFDIR | (mode & 0777);
    inode.i_links_count = 2;
    inode.i_atime = inode.i_mtime = inode.i_ctime = vfs_get_unix_time();
    ext4_write_inode(new_ino, &inode);

    if (ext4_add_dir_entry(dir_ino, new_ino, name, EXT2_FT_DIR) < 0) return -EIO;
    ext4_add_dir_entry(new_ino, new_ino, ".", EXT2_FT_DIR);
    ext4_add_dir_entry(new_ino, dir_ino, "..", EXT2_FT_DIR);

    struct ext2_inode di;
    if (ext4_read_inode(dir_ino, &di) == 0) {
        di.i_links_count++;
        ext4_write_inode(dir_ino, &di);
    }
    return 0;
}

static int ext4_vfs_rename(struct vfs_node *old_dir, const char *old_name,
                           struct vfs_node *new_dir, const char *new_name) {
    u32 old_dir_ino = (u32)(usize)old_dir->inode->data;
    struct ext2_inode old_di;
    ext4_read_inode(old_dir_ino, &old_di);

    u32 ino = 0; u8 type = 0;
    u8 *buf = kmalloc(ext4_block_size);
    u32 blocks = (u32)((old_di.i_size + ext4_block_size - 1) / ext4_block_size);
    for (u32 b = 0; b < blocks; b++) {
        u32 phys = ext4_get_block(&old_di, b);
        if (!phys) continue;
        ext4_read_block(phys, buf);
        usize off = 0;
        while (off < ext4_block_size) {
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
    if (!ino) return -ENOENT;

    if (ext4_add_dir_entry((u32)(usize)new_dir->inode->data, ino, new_name, type) < 0) return -EIO;
    ext4_vfs_unlink(old_dir, old_name);
    return 0;
}

static int ext4_vfs_setattr(struct vfs_node *node) {
    u32 ino = (u32)(usize)node->inode->data;
    struct ext2_inode inode;
    if (ext4_read_inode(ino, &inode) < 0) return -EIO;
    inode.i_mode = (inode.i_mode & ~0777) | (node->inode->mode & 0777);
    inode.i_uid = node->inode->uid;
    inode.i_gid = node->inode->gid;
    inode.i_size = (u32)node->inode->size;
    inode.i_atime = node->inode->atime;
    inode.i_mtime = node->inode->mtime;
    inode.i_ctime = node->inode->ctime;
    return ext4_write_inode(ino, &inode);
}

static int ext4_vfs_statfs(struct vfs_node *node, struct b1nix_statfs *st) {
    (void)node;
    memset(st, 0, sizeof(*st));
    st->f_type = EXT2_SUPER_MAGIC;
    st->f_bsize = ext4_block_size;
    st->f_blocks = ext4_sb.s_blocks_count;
    st->f_bfree = ext4_sb.s_free_blocks_count;
    st->f_bavail = ext4_sb.s_free_blocks_count;
    st->f_files = ext4_sb.s_inodes_count;
    st->f_ffree = ext4_sb.s_free_inodes_count;
    st->f_namelen = 255;
    return 0;
}

static void ext4_vfs_release(struct vfs_node *node) {
    u32 ino = (u32)(usize)node->inode->data;
    struct ext2_inode inode;
    if (ext4_read_inode(ino, &inode) == 0) {
        if (inode.i_links_count == 0) {
            u32 blocks = (u32)((inode.i_size + ext4_block_size - 1) / ext4_block_size);
            for (u32 b = 0; b < blocks; b++) {
                u32 phys = ext4_get_block(&inode, b);
                if (phys) ext4_free_block(phys);
            }
            ext4_free_inode(ino);
        }
    }
}

static int ext4_vfs_create(struct vfs_node *dir, const char *name,
                           const char *full_path, u32 mode) {
    u32 dir_ino = (u32)(usize)dir->inode->data;
    u32 new_ino = ext4_alloc_inode();
    if (!new_ino) return -ENOSPC;

    struct ext2_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = EXT2_S_IFREG | (mode & 0777);
    inode.i_links_count = 1;
    inode.i_atime = inode.i_mtime = inode.i_ctime = vfs_get_unix_time();
    ext4_write_inode(new_ino, &inode);

    if (ext4_add_dir_entry(dir_ino, new_ino, name, EXT2_FT_REG_FILE) < 0) return -EIO;

    struct vfs_node *n = vfs_add_node(full_path, VFS_FILE, (void *)(usize)new_ino, 0, 0);
    if (n) {
        n->inode->blk_dev = ext4_dev;
        n->inode->read_cb = ext4_vfs_read;
        n->inode->write_cb = ext4_vfs_write;
    }
    return 0;
}

static void ext4_populate_vfs(u32 ino, const char *base_path) {
    struct ext2_inode inode;
  if (ext4_read_inode(ino, &inode) < 0)
    return;

    u64 inode_sz = ext4_get_inode_size(&inode);
  if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
    return;

    u8 *buf = kmalloc((usize)inode_sz);
    for (u32 i = 0; i < (inode_sz + ext4_block_size - 1) / ext4_block_size; i++) {
        u32 phys = ext4_get_block(&inode, i);
    if (phys)
      ext4_read_block(phys, buf + i * ext4_block_size);
    }

    usize off = 0;
    while (off < (usize)inode_sz) {
        struct ext2_dir_entry *e = (struct ext2_dir_entry *)(buf + off);
    if (e->rec_len == 0 || e->inode == 0) {
      off += e->rec_len ? e->rec_len : 4;
      continue;
    }

        char name[256];
        memcpy(name, e->name, e->name_len);
        name[e->name_len] = '\0';

        if (strcmp(name, ".") && strcmp(name, "..")) {
            char full[256];
            usize len = strlen(base_path);
            memcpy(full, base_path, len);
      if (full[len - 1] != '/')
        full[len++] = '/';
            memcpy(full + len, name, e->name_len + 1);

            struct ext2_inode ci;
            if (ext4_read_inode(e->inode, &ci) == 0) {
                if ((ci.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
          struct vfs_node *dn =
              vfs_add_node(full, VFS_DIRECTORY, (void *)(usize)e->inode, 0, 0);
          if (dn) {
            dn->inode->nlink = ci.i_links_count;
            dn->inode->fs_id = dn->parent->inode->fs_id;
            dn->inode->create_cb = ext4_vfs_create;
            dn->inode->mkdir_cb = ext4_vfs_mkdir;
            dn->inode->unlink_cb = ext4_vfs_unlink;
            dn->inode->rmdir_cb = ext4_vfs_rmdir;
            dn->inode->rename_cb = ext4_vfs_rename;
            dn->inode->release_cb = ext4_vfs_release;
            dn->inode->setattr_cb = ext4_vfs_setattr;
            dn->inode->statfs_cb = ext4_vfs_statfs;
            dn->inode->readdir_cb = ext4_vfs_readdir;
            dn->inode->fsync_cb = ext4_vfs_fsync;
          }
                    ext4_populate_vfs(e->inode, full);
                } else {
          struct vfs_node *n =
              vfs_add_node(full, VFS_FILE, (void *)(usize)e->inode,
                                                      (usize)ext4_get_inode_size(&ci), 0);
          if (n) {
            n->inode->read_cb = ext4_vfs_read;
            n->inode->write_cb = ext4_vfs_write;
            n->inode->release_cb = ext4_vfs_release;
            n->inode->setattr_cb = ext4_vfs_setattr;
            n->inode->fsync_cb = ext4_vfs_fsync;
            n->inode->fs_id = n->parent->inode->fs_id;
            n->inode->nlink = ci.i_links_count;
          }
                }
            }
        }
        off += e->rec_len;
    }
    kfree(buf);
}

void ext4_init(void) {
    ext4_dev = blk_get("virtio-blk1");
  if (!ext4_dev)
    ext4_dev = blk_get("virtio-blk2");
  if (!ext4_dev)
    ext4_dev = blk_get("sata1");
  if (!ext4_dev)
    return;

    u8 *sb_buf = kmalloc(1024);
  if (blk_read_cached(ext4_dev, 2, 2, sb_buf) < 0) {
    kfree(sb_buf);
    return;
  }
    memcpy(&ext4_sb, sb_buf, sizeof(struct ext2_superblock));

  if (ext4_sb.s_magic != EXT2_SUPER_MAGIC || ext4_sb.s_rev_level == 0) {
    kfree(sb_buf);
    return;
  }

    ext4_block_size = 1024 << ext4_sb.s_log_block_size;
    ext4_inodes_per_group = ext4_sb.s_inodes_per_group;
  ext4_inode_size = ext4_sb.s_inode_size;
    ext4_features_incompat = ext4_sb.s_feature_incompat;
    ext4_features_ro_compat = ext4_sb.s_feature_ro_compat;

    if (!(ext4_features_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS)) {
        kfree(sb_buf);
        return;
    }

    ext4_desc_size = 32;
    if (ext4_features_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
        u16 tmp;
        memcpy(&tmp, sb_buf + EXT4_SB_DESC_SIZE_OFF, 2);
    ext4_desc_size = tmp >= 32 ? tmp : 64;
    }

    ext4_flex_size = 1;
    if (ext4_features_incompat & EXT4_FEATURE_INCOMPAT_FLEX_BG) {
        ext4_flex_size = 1 << sb_buf[EXT4_SB_LOG_GROUPS_PER_FLEX];
  if (ext4_sb.s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL) {
    console_write("ext4: detecting journal...\n");
    memcpy(&journal_inum, sb_buf + EXT3_SB_JOURNAL_INUM_OFF, 4);

    if (journal_inum != 0) {
      ext4_read_inode(journal_inum, &journal_inode_cache);
      ext4_jdev =
          journal_mount(&journal_inode_cache, ext4_block_size, &ext4_jbd_ops);

      if (ext4_jdev &&
          (ext4_sb.s_feature_incompat & EXT3_FEATURE_INCOMPAT_RECOVER)) {
        console_write("ext4: journal needs recovery\n");
        journal_recover(ext4_jdev);
      }
    }
  }
    if (ext4_flex_size == 0)
      ext4_flex_size = 1;
    }

    kfree(sb_buf);

    console_write("ext4: mounted rw, block_size=");
    console_write_dec(ext4_block_size);
    console_write("\n");

  struct vfs_node *root =
      vfs_add_node("/ext4", VFS_DIRECTORY, (void *)(usize)2, 0, 0);
  if (root) {
    root->inode->create_cb = ext4_vfs_create;
    root->inode->mkdir_cb = ext4_vfs_mkdir;
    root->inode->unlink_cb = ext4_vfs_unlink;
    root->inode->rmdir_cb = ext4_vfs_rmdir;
    root->inode->rename_cb = ext4_vfs_rename;
    root->inode->release_cb = ext4_vfs_release;
    root->inode->setattr_cb = ext4_vfs_setattr;
    root->inode->statfs_cb = ext4_vfs_statfs;
    root->inode->readdir_cb = ext4_vfs_readdir;
    root->inode->fsync_cb = ext4_vfs_fsync;
  }
    ext4_populate_vfs(2, "/ext4");
}
