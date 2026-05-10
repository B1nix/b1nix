#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/ext2.h>
#include <b1nix/ext3.h>
#include <b1nix/journal.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <string.h>

#define EXT3_FEATURE_COMPAT_HAS_JOURNAL    0x0004
#define EXT3_FEATURE_INCOMPAT_RECOVER      0x0004
#define EXT3_SB_JOURNAL_INUM_OFF           0xE0

static int ext3_read_block(struct ext3_fs *fs, u32 block, void *buffer) {
  return blk_read_cached(fs->bdev, (u64)block * (fs->block_size / 512), fs->block_size / 512, buffer);
}

static int ext3_write_block(struct ext3_fs *fs, u32 block, const void *buffer) {
  return blk_write_cached(fs->bdev, (u64)block * (fs->block_size / 512), fs->block_size / 512, buffer);
}

static u32 ext3_get_block(struct ext3_fs *fs, struct ext2_inode *inode, u32 block_idx);

static int ext3_jbd_read(struct journal_dev *jdev, u32 logical, void *buf) {
  struct ext3_fs *fs = (struct ext3_fs *)jdev->fs_priv;
  u32 phys = ext3_get_block(fs, &fs->journal_inode_cache, logical);
  if (!phys) return -1;
  return ext3_read_block(fs, phys, buf);
}

static int ext3_jbd_write(struct journal_dev *jdev, u32 logical, const void *buf) {
  struct ext3_fs *fs = (struct ext3_fs *)jdev->fs_priv;
  u32 phys = ext3_get_block(fs, &fs->journal_inode_cache, logical);
  if (!phys) return -1;
  return ext3_write_block(fs, phys, buf);
}

static int ext3_jbd_fs_write(struct journal_dev *jdev, u32 phys, const void *buf) {
  struct ext3_fs *fs = (struct ext3_fs *)jdev->fs_priv;
  return ext3_write_block(fs, phys, buf);
}

static struct journal_ops ext3_jbd_ops = {ext3_jbd_read, ext3_jbd_write, ext3_jbd_fs_write};

static int ext3_journal_write(struct ext3_fs *fs, u32 block, const void *buffer) {
  if (fs->jdev) {
    struct journal_handle *h = journal_start_transaction(fs->jdev);
    if (h) { journal_log_block(h, block, buffer); journal_commit_transaction(h); return 0; }
  }
  return ext3_write_block(fs, block, buffer);
}

static void ext3_read_bgd(struct ext3_fs *fs, u32 group, struct ext2_block_group_desc *bgd) {
  u32 bg_block = (fs->block_size == 1024 ? 2 : 1) + (group * sizeof(struct ext2_block_group_desc)) / fs->block_size;
  u8 *buf = kmalloc(fs->block_size);
  ext3_read_block(fs, bg_block, buf);
  memcpy(bgd, buf + ((group * sizeof(struct ext2_block_group_desc)) % fs->block_size), sizeof(struct ext2_block_group_desc));
  kfree(buf);
}

static void ext3_write_bgd(struct ext3_fs *fs, u32 group, struct ext2_block_group_desc *bgd) {
  u32 bg_block = (fs->block_size == 1024 ? 2 : 1) + (group * sizeof(struct ext2_block_group_desc)) / fs->block_size;
  u8 *buf = kmalloc(fs->block_size);
  ext3_read_block(fs, bg_block, buf);
  memcpy(buf + ((group * sizeof(struct ext2_block_group_desc)) % fs->block_size), bgd, sizeof(struct ext2_block_group_desc));
  ext3_journal_write(fs, bg_block, buf);
  kfree(buf);
}

static void ext3_write_superblock(struct ext3_fs *fs) {
    u8 *sb_buf = kmalloc(1024);
    if (blk_read_cached(fs->bdev, 2, 2, sb_buf) >= 0) { memcpy(sb_buf, &fs->sb, sizeof(struct ext2_superblock)); blk_write_cached(fs->bdev, 2, 2, sb_buf); }
    kfree(sb_buf);
}

static int ext3_read_inode(struct ext3_fs *fs, u32 inode_num, struct ext2_inode *inode) {
  if (inode_num == 0) return -1;
  u32 group = (inode_num - 1) / fs->inodes_per_group;
  struct ext2_block_group_desc bgd;
  ext3_read_bgd(fs, group, &bgd);
  u32 inode_offset = ((inode_num - 1) % fs->inodes_per_group) * fs->inode_size;
  u32 block_idx = bgd.bg_inode_table + (inode_offset / fs->block_size);
  u8 *buf = kmalloc(fs->block_size);
  ext3_read_block(fs, block_idx, buf);
  memcpy(inode, buf + (inode_offset % fs->block_size), sizeof(struct ext2_inode));
  kfree(buf);
  return 0;
}

static int ext3_write_inode(struct ext3_fs *fs, u32 inode_num, const struct ext2_inode *inode) {
  if (inode_num == 0) return -1;
  u32 group = (inode_num - 1) / fs->inodes_per_group;
  struct ext2_block_group_desc bgd;
  ext3_read_bgd(fs, group, &bgd);
  u32 inode_offset = ((inode_num - 1) % fs->inodes_per_group) * fs->inode_size;
  u32 block_idx = bgd.bg_inode_table + (inode_offset / fs->block_size);
  u8 *buf = kmalloc(fs->block_size);
  ext3_read_block(fs, block_idx, buf);
  memcpy(buf + (inode_offset % fs->block_size), inode, sizeof(struct ext2_inode));
  int ret = ext3_journal_write(fs, block_idx, buf);
  kfree(buf);
  return ret;
}

static u32 ext3_alloc_block(struct ext3_fs *fs) {
  u32 groups = (fs->sb.s_blocks_count + fs->sb.s_blocks_per_group - 1) / fs->sb.s_blocks_per_group;
  for (u32 g = 0; g < groups; g++) {
    struct ext2_block_group_desc bgd;
    ext3_read_bgd(fs, g, &bgd);
    if (bgd.bg_free_blocks_count == 0) continue;
    u8 *bitmap = kmalloc(fs->block_size);
    ext3_read_block(fs, bgd.bg_block_bitmap, bitmap);
    for (u32 i = 0; i < fs->sb.s_blocks_per_group; i++) {
      if (!(bitmap[i / 8] & (1 << (i % 8)))) {
        bitmap[i / 8] |= (1 << (i % 8));
        ext3_journal_write(fs, bgd.bg_block_bitmap, bitmap);
        kfree(bitmap);
        bgd.bg_free_blocks_count--; ext3_write_bgd(fs, g, &bgd);
        fs->sb.s_free_blocks_count--; ext3_write_superblock(fs);
        u32 block_num = g * fs->sb.s_blocks_per_group + i + (fs->sb.s_log_block_size == 0 ? 1 : 0);
        u8 *zero = kzalloc(fs->block_size);
        ext3_journal_write(fs, block_num, zero);
        kfree(zero); return block_num;
      }
    }
    kfree(bitmap);
  }
  return 0;
}

static u32 ext3_alloc_inode(struct ext3_fs *fs) {
  u32 groups = (fs->sb.s_inodes_count + fs->inodes_per_group - 1) / fs->inodes_per_group;
  for (u32 g = 0; g < groups; g++) {
    struct ext2_block_group_desc bgd;
    ext3_read_bgd(fs, g, &bgd);
    if (bgd.bg_free_inodes_count == 0) continue;
    u8 *bitmap = kmalloc(fs->block_size);
    ext3_read_block(fs, bgd.bg_inode_bitmap, bitmap);
    for (u32 i = 0; i < fs->inodes_per_group; i++) {
      if (!(bitmap[i / 8] & (1 << (i % 8)))) {
        bitmap[i / 8] |= (1 << (i % 8));
        ext3_journal_write(fs, bgd.bg_inode_bitmap, bitmap);
        kfree(bitmap);
        bgd.bg_free_inodes_count--; ext3_write_bgd(fs, g, &bgd);
        fs->sb.s_free_inodes_count--; ext3_write_superblock(fs);
        u32 inode_num = g * fs->inodes_per_group + i + 1;
        struct ext2_inode ni; memset(&ni, 0, sizeof(ni)); ext3_write_inode(fs, inode_num, &ni);
        return inode_num;
      }
    }
    kfree(bitmap);
  }
  return 0;
}

static void ext3_free_block(struct ext3_fs *fs, u32 block_num) {
  if (block_num == 0) return;
  u32 g = (block_num - (fs->sb.s_log_block_size == 0 ? 1 : 0)) / fs->sb.s_blocks_per_group;
  u32 i = (block_num - (fs->sb.s_log_block_size == 0 ? 1 : 0)) % fs->sb.s_blocks_per_group;
  struct ext2_block_group_desc bgd; ext3_read_bgd(fs, g, &bgd);
  u8 *bitmap = kmalloc(fs->block_size); ext3_read_block(fs, bgd.bg_block_bitmap, bitmap);
  bitmap[i / 8] &= ~(1 << (i % 8)); ext3_journal_write(fs, bgd.bg_block_bitmap, bitmap);
  kfree(bitmap); bgd.bg_free_blocks_count++; ext3_write_bgd(fs, g, &bgd);
  fs->sb.s_free_blocks_count++; ext3_write_superblock(fs);
}

static void ext3_free_inode(struct ext3_fs *fs, u32 inode_num) {
  if (inode_num == 0) return;
  u32 g = (inode_num - 1) / fs->inodes_per_group;
  u32 i = (inode_num - 1) % fs->inodes_per_group;
  struct ext2_block_group_desc bgd; ext3_read_bgd(fs, g, &bgd);
  u8 *bitmap = kmalloc(fs->block_size); ext3_read_block(fs, bgd.bg_inode_bitmap, bitmap);
  bitmap[i / 8] &= ~(1 << (i % 8)); ext3_journal_write(fs, bgd.bg_inode_bitmap, bitmap);
  kfree(bitmap); bgd.bg_free_inodes_count++; ext3_write_bgd(fs, g, &bgd);
  fs->sb.s_free_inodes_count++; ext3_write_superblock(fs);
}

static u32 ext3_get_block(struct ext3_fs *fs, struct ext2_inode *inode, u32 block_idx) {
  if (block_idx < EXT2_NDIR_BLOCKS) return inode->i_block[block_idx];
  u32 ptrs = fs->block_size / 4;
  block_idx -= EXT2_NDIR_BLOCKS;
  if (block_idx < ptrs) {
    if (!inode->i_block[EXT2_IND_BLOCK]) return 0;
    u32 *ind = kmalloc(fs->block_size); ext3_read_block(fs, inode->i_block[EXT2_IND_BLOCK], ind);
    u32 r = ind[block_idx]; kfree(ind); return r;
  }
  block_idx -= ptrs;
  if (block_idx < ptrs * ptrs) {
    if (!inode->i_block[EXT2_DIND_BLOCK]) return 0;
    u32 *dind = kmalloc(fs->block_size); ext3_read_block(fs, inode->i_block[EXT2_DIND_BLOCK], dind);
    if (!dind[block_idx / ptrs]) { kfree(dind); return 0; }
    u32 *ind = kmalloc(fs->block_size); ext3_read_block(fs, dind[block_idx / ptrs], ind);
    u32 r = ind[block_idx % ptrs]; kfree(ind); kfree(dind); return r;
  }
  return 0;
}

static int ext3_set_block(struct ext3_fs *fs, struct ext2_inode *inode, u32 block_idx, u32 phys) {
  u32 ptrs = fs->block_size / 4;
  if (block_idx < EXT2_NDIR_BLOCKS) { inode->i_block[block_idx] = phys; return 1; }
  block_idx -= EXT2_NDIR_BLOCKS;
  if (block_idx < ptrs) {
    if (!inode->i_block[EXT2_IND_BLOCK]) {
      u32 b = ext3_alloc_block(fs); if (!b) return 0;
      inode->i_block[EXT2_IND_BLOCK] = b;
    }
    u32 *ind = kmalloc(fs->block_size); ext3_read_block(fs, inode->i_block[EXT2_IND_BLOCK], ind);
    ind[block_idx] = phys; ext3_journal_write(fs, inode->i_block[EXT2_IND_BLOCK], ind);
    kfree(ind); return 1;
  }
  block_idx -= ptrs;
  if (block_idx < ptrs * ptrs) {
    if (!inode->i_block[EXT2_DIND_BLOCK]) {
      u32 b = ext3_alloc_block(fs); if (!b) return 0;
      inode->i_block[EXT2_DIND_BLOCK] = b;
    }
    u32 *dind = kmalloc(fs->block_size); ext3_read_block(fs, inode->i_block[EXT2_DIND_BLOCK], dind);
    u32 idx1 = block_idx / ptrs;
    if (!dind[idx1]) {
      u32 b = ext3_alloc_block(fs); if (!b) { kfree(dind); return 0; }
      dind[idx1] = b; ext3_journal_write(fs, inode->i_block[EXT2_DIND_BLOCK], dind);
    }
    u32 *ind = kmalloc(fs->block_size); ext3_read_block(fs, dind[idx1], ind);
    ind[block_idx % ptrs] = phys; ext3_journal_write(fs, dind[idx1], ind);
    kfree(ind); kfree(dind); return 1;
  }
  return 0;
}

static isize ext3_vfs_read(struct vfs_node *node, u64 offset, char *buffer, usize size, int flags) {
  (void)flags; struct ext3_inode_info *info = (struct ext3_inode_info *)node->inode->data;
  struct ext3_fs *fs = info->fs; struct ext2_inode inode;
  if (ext3_read_inode(fs, info->inode_num, &inode) < 0) return -1;
  if (offset >= inode.i_size) return 0;
  usize remaining = inode.i_size - offset; usize to_read = size < remaining ? size : remaining;
  usize done = 0; u8 *block_buf = kmalloc(fs->block_size);
  while (done < to_read) {
    u32 b_idx = (offset + done) / fs->block_size; u32 b_off = (offset + done) % fs->block_size;
    u32 phys = ext3_get_block(fs, &inode, b_idx); usize chunk = fs->block_size - b_off;
    if (chunk > to_read - done) chunk = to_read - done;
    if (phys) { ext3_read_block(fs, phys, block_buf); memcpy(buffer + done, block_buf + b_off, chunk); }
    else memset(buffer + done, 0, chunk);
    done += chunk;
  }
  kfree(block_buf); return done;
}

static isize ext3_vfs_write(struct vfs_node *node, u64 offset, const char *buffer, usize size, int flags) {
  (void)flags; struct ext3_inode_info *info = (struct ext3_inode_info *)node->inode->data;
  struct ext3_fs *fs = info->fs; struct ext2_inode inode;
  if (ext3_read_inode(fs, info->inode_num, &inode) < 0) return -1;
  u64 new_size = offset + size;
  if (new_size > inode.i_size) {
    u32 old_blks = (inode.i_size + fs->block_size - 1) / fs->block_size;
    u32 new_blks = (new_size + fs->block_size - 1) / fs->block_size;
    for (u32 b = old_blks; b < new_blks; b++) {
      u32 pblk = ext3_alloc_block(fs); if (!pblk || !ext3_set_block(fs, &inode, b, pblk)) return -1;
      inode.i_blocks += fs->block_size / 512;
    }
    inode.i_size = (u32)new_size; ext3_write_inode(fs, info->inode_num, &inode);
    node->inode->size = inode.i_size;
  }
  usize done = 0; u8 *block_buf = kmalloc(fs->block_size);
  while (done < size) {
    u32 b_idx = (offset + done) / fs->block_size; u32 b_off = (offset + done) % fs->block_size;
    u32 phys = ext3_get_block(fs, &inode, b_idx); if (!phys) break;
    usize chunk = fs->block_size - b_off; if (chunk > size - done) chunk = size - done;
    if (chunk < fs->block_size) ext3_read_block(fs, phys, block_buf);
    memcpy(block_buf + b_off, buffer + done, chunk); ext3_journal_write(fs, phys, block_buf);
    done += chunk;
  }
  kfree(block_buf); return done;
}

static int ext3_add_dir_entry(struct ext3_fs *fs, u32 dir_ino, u32 child_ino, const char *name, u8 type) {
  struct ext2_inode dir; if (ext3_read_inode(fs, dir_ino, &dir) < 0) return -1;
  usize name_len = strlen(name); if (name_len > 255) name_len = 255;
  u32 needed = 8 + ((name_len + 3) & ~3);
  u8 *buf = kmalloc(fs->block_size); u32 blocks = (dir.i_size + fs->block_size - 1) / fs->block_size;
  for (u32 b = 0; b < blocks; b++) {
    u32 phys = ext3_get_block(fs, &dir, b); if (!phys) continue;
    ext3_read_block(fs, phys, buf); usize off = 0;
    while (off < fs->block_size) {
      struct ext2_dir_entry *e = (struct ext2_dir_entry *)(buf + off);
      if (e->rec_len == 0) break;
      u32 actual = 8 + ((e->name_len + 3) & ~3);
      if (e->rec_len >= actual + needed) {
        e->rec_len = actual;
        struct ext2_dir_entry *ne = (struct ext2_dir_entry *)(buf + off + actual);
        ne->inode = child_ino; ne->rec_len = e->rec_len - actual; ne->name_len = name_len; ne->file_type = type;
        memcpy(ne->name, name, name_len); ext3_journal_write(fs, phys, buf);
        kfree(buf); return 0;
      }
      off += e->rec_len;
    }
  }
  u32 phys = ext3_alloc_block(fs);
  if (phys) {
    ext3_set_block(fs, &dir, blocks, phys);
    dir.i_blocks += fs->block_size / 512; dir.i_size += fs->block_size;
    ext3_write_inode(fs, dir_ino, &dir);
    memset(buf, 0, fs->block_size);
    struct ext2_dir_entry *e = (struct ext2_dir_entry *)buf;
    e->inode = child_ino; e->rec_len = fs->block_size; e->name_len = name_len; e->file_type = type;
    memcpy(e->name, name, name_len); ext3_journal_write(fs, phys, buf);
    kfree(buf); return 0;
  }
  kfree(buf); return -1;
}

static void ext3_populate_vfs(struct ext3_fs *fs, u32 ino, const char *base_path);
static void ext3_setup_node(struct vfs_node *n, struct ext3_fs *fs, u32 ino, u32 mode);

static int ext3_vfs_create(struct vfs_node *dir, const char *name, const char *full_path, u32 mode) {
  struct ext3_inode_info *dir_info = (struct ext3_inode_info *)dir->inode->data;
  struct ext3_fs *fs = dir_info->fs; u32 new_ino = ext3_alloc_inode(fs); if (!new_ino) return -ENOSPC;
  struct ext2_inode inode; memset(&inode, 0, sizeof(inode));
  inode.i_mode = EXT2_S_IFREG | (mode & 0777); inode.i_links_count = 1;
  inode.i_atime = inode.i_mtime = inode.i_ctime = vfs_get_unix_time();
  ext3_write_inode(fs, new_ino, &inode);
  if (ext3_add_dir_entry(fs, dir_info->inode_num, new_ino, name, EXT2_FT_REG_FILE) < 0) return -EIO;
  struct vfs_node *n = vfs_add_node(full_path, VFS_FILE, 0, 0, 0);
  if (n) ext3_setup_node(n, fs, new_ino, inode.i_mode);
  return 0;
}

static int ext3_vfs_mkdir(struct vfs_node *dir, const char *name, u32 mode) {
  struct ext3_inode_info *dir_info = (struct ext3_inode_info *)dir->inode->data;
  struct ext3_fs *fs = dir_info->fs; u32 new_ino = ext3_alloc_inode(fs); if (!new_ino) return -ENOSPC;
  struct ext2_inode inode; memset(&inode, 0, sizeof(inode));
  inode.i_mode = EXT2_S_IFDIR | (mode & 0777); inode.i_links_count = 2;
  inode.i_atime = inode.i_mtime = inode.i_ctime = vfs_get_unix_time();
  ext3_write_inode(fs, new_ino, &inode);
  if (ext3_add_dir_entry(fs, dir_info->inode_num, new_ino, name, EXT2_FT_DIR) < 0) return -EIO;
  ext3_add_dir_entry(fs, new_ino, new_ino, ".", EXT2_FT_DIR);
  ext3_add_dir_entry(fs, new_ino, dir_info->inode_num, "..", EXT2_FT_DIR);
  struct ext2_inode di; if (ext3_read_inode(fs, dir_info->inode_num, &di) == 0) { di.i_links_count++; ext3_write_inode(fs, dir_info->inode_num, &di); }
  return 0;
}

static int ext3_vfs_setattr(struct vfs_node *node) {
  struct ext3_inode_info *info = (struct ext3_inode_info *)node->inode->data;
  struct ext3_fs *fs = info->fs; struct ext2_inode inode;
  if (ext3_read_inode(fs, info->inode_num, &inode) < 0) return -EIO;
  inode.i_mode = (inode.i_mode & ~0777) | (node->inode->mode & 0777);
  inode.i_uid = node->inode->uid; inode.i_gid = node->inode->gid;
  inode.i_size = (u32)node->inode->size;
  inode.i_atime = node->inode->atime; inode.i_mtime = node->inode->mtime; inode.i_ctime = node->inode->ctime;
  return ext3_write_inode(fs, info->inode_num, &inode);
}

static int ext3_vfs_statfs(struct vfs_node *node, struct b1nix_statfs *st) {
  struct ext3_inode_info *info = (struct ext3_inode_info *)node->inode->data;
  struct ext3_fs *fs = info->fs; memset(st, 0, sizeof(*st));
  st->f_type = EXT2_SUPER_MAGIC; st->f_bsize = fs->block_size;
  st->f_blocks = fs->sb.s_blocks_count; st->f_bfree = fs->sb.s_free_blocks_count;
  st->f_bavail = fs->sb.s_free_blocks_count; st->f_files = fs->sb.s_inodes_count;
  st->f_ffree = fs->sb.s_free_inodes_count; st->f_namelen = 255;
  return 0;
}

static isize ext3_vfs_readdir(struct vfs_node *dir, usize offset, struct dirent *buf, usize max_entries) {
  struct ext3_inode_info *info = (struct ext3_inode_info *)dir->inode->data;
  struct ext3_fs *fs = info->fs; struct ext2_inode inode;
  if (ext3_read_inode(fs, info->inode_num, &inode) < 0) return -EIO;
  u8 *dir_buf = kmalloc(fs->block_size); usize count = 0; usize entry_idx = 0;
  u32 blocks = (inode.i_size + fs->block_size - 1) / fs->block_size;
  for (u32 b = 0; b < blocks && count < max_entries; b++) {
    u32 phys = ext3_get_block(fs, &inode, b); if (!phys) continue;
    ext3_read_block(fs, phys, dir_buf); usize off = 0;
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

static int ext3_vfs_unlink(struct vfs_node *dir, const char *name) {
  struct ext3_inode_info *dir_info = (struct ext3_inode_info *)dir->inode->data;
  struct ext3_fs *fs = dir_info->fs; struct ext2_inode di;
  if (ext3_read_inode(fs, dir_info->inode_num, &di) < 0) return -EIO;
  u32 ino = 0; u8 *buf = kmalloc(fs->block_size);
  u32 blocks = (di.i_size + fs->block_size - 1) / fs->block_size;
  for (u32 b = 0; b < blocks; b++) {
    u32 phys = ext3_get_block(fs, &di, b); if (!phys) continue;
    ext3_read_block(fs, phys, buf); usize off = 0;
    while (off < fs->block_size) {
      struct ext2_dir_entry *e = (struct ext2_dir_entry *)(buf + off);
      if (e->rec_len == 0) break;
      if (e->inode != 0 && strlen(name) == e->name_len && memcmp(e->name, name, e->name_len) == 0) { ino = e->inode; break; }
      off += e->rec_len;
    }
    if (ino) break;
  }
  kfree(buf); if (!ino) return -ENOENT;
  buf = kmalloc(fs->block_size);
  for (u32 b = 0; b < blocks; b++) {
    u32 phys = ext3_get_block(fs, &di, b); if (!phys) continue;
    ext3_read_block(fs, phys, buf); usize off = 0; struct ext2_dir_entry *prev = 0;
    while (off < fs->block_size) {
      struct ext2_dir_entry *e = (struct ext2_dir_entry *)(buf + off);
      if (e->rec_len == 0) break;
      if (e->inode == ino && strlen(name) == e->name_len && memcmp(e->name, name, e->name_len) == 0) {
        if (prev) prev->rec_len += e->rec_len; else e->inode = 0;
        ext3_journal_write(fs, phys, buf); break;
      }
      off += e->rec_len; prev = e;
    }
  }
  kfree(buf);
  struct ext2_inode ci; if (ext3_read_inode(fs, ino, &ci) == 0) { if (ci.i_links_count > 0) { ci.i_links_count--; ext3_write_inode(fs, ino, &ci); } }
  return 0;
}

static int ext3_vfs_rmdir(struct vfs_node *dir, const char *name) {
  int err = ext3_vfs_unlink(dir, name);
  if (err == 0) {
    struct ext3_inode_info *dir_info = (struct ext3_inode_info *)dir->inode->data;
    struct ext3_fs *fs = dir_info->fs; struct ext2_inode di;
    if (ext3_read_inode(fs, dir_info->inode_num, &di) == 0) { if (di.i_links_count > 2) { di.i_links_count--; ext3_write_inode(fs, dir_info->inode_num, &di); } }
  }
  return err;
}

static int ext3_vfs_rename(struct vfs_node *old_dir, const char *old_name, struct vfs_node *new_dir, const char *new_name) {
  struct ext3_inode_info *old_dir_info = (struct ext3_inode_info *)old_dir->inode->data;
  struct ext3_fs *fs = old_dir_info->fs; struct ext2_inode old_di;
  ext3_read_inode(fs, old_dir_info->inode_num, &old_di);
  u32 ino = 0; u8 type = 0; u8 *buf = kmalloc(fs->block_size);
  u32 blocks = (old_di.i_size + fs->block_size - 1) / fs->block_size;
  for (u32 b = 0; b < blocks; b++) {
    u32 phys = ext3_get_block(fs, &old_di, b); if (!phys) continue;
    ext3_read_block(fs, phys, buf); usize off = 0;
    while (off < fs->block_size) {
      struct ext2_dir_entry *e = (struct ext2_dir_entry *)(buf + off);
      if (e->rec_len == 0) break;
      if (e->inode != 0 && strlen(old_name) == e->name_len && memcmp(e->name, old_name, e->name_len) == 0) { ino = e->inode; type = e->file_type; break; }
      off += e->rec_len;
    }
    if (ino) break;
  }
  kfree(buf); if (!ino) return -ENOENT;
  struct ext3_inode_info *new_dir_info = (struct ext3_inode_info *)new_dir->inode->data;
  if (ext3_add_dir_entry(fs, new_dir_info->inode_num, ino, new_name, type) < 0) return -EIO;
  ext3_vfs_unlink(old_dir, old_name); return 0;
}

static int ext3_vfs_symlink(struct vfs_node *dir, const char *name, const char *target) {
  struct ext3_inode_info *dir_info = (struct ext3_inode_info *)dir->inode->data;
  struct ext3_fs *fs = dir_info->fs; u32 new_ino = ext3_alloc_inode(fs); if (!new_ino) return -ENOSPC;
  struct ext2_inode inode; memset(&inode, 0, sizeof(inode));
  inode.i_mode = EXT2_S_IFLNK | 0777; inode.i_links_count = 1;
  inode.i_size = strlen(target); inode.i_atime = inode.i_mtime = inode.i_ctime = vfs_get_unix_time();
  if (inode.i_size < 60) memcpy(inode.i_block, target, inode.i_size);
  else {
    u32 block = ext3_alloc_block(fs); if (!block) { ext3_free_inode(fs, new_ino); return -ENOSPC; }
    inode.i_block[0] = block; ext3_journal_write(fs, block, target);
  }
  ext3_write_inode(fs, new_ino, &inode);
  return ext3_add_dir_entry(fs, dir_info->inode_num, new_ino, name, EXT2_FT_SYMLINK);
}

static int ext3_vfs_link(struct vfs_node *target_node, struct vfs_node *dir, const char *name) {
  struct ext3_inode_info *target_info = (struct ext3_inode_info *)target_node->inode->data;
  struct ext3_fs *fs = target_info->fs; struct ext2_inode inode;
  if (ext3_read_inode(fs, target_info->inode_num, &inode) < 0) return -EIO;
  inode.i_links_count++; ext3_write_inode(fs, target_info->inode_num, &inode);
  struct ext3_inode_info *dir_info = (struct ext3_inode_info *)dir->inode->data;
  return ext3_add_dir_entry(fs, dir_info->inode_num, target_info->inode_num, name, EXT2_FT_REG_FILE);
}

static int ext3_vfs_fsync(struct vfs_node *node) { (void)node; return 0; }

static void ext3_vfs_release(struct vfs_node *node) {
  struct ext3_inode_info *info = (struct ext3_inode_info *)node->inode->data;
  struct ext3_fs *fs = info->fs; struct ext2_inode inode;
  if (ext3_read_inode(fs, info->inode_num, &inode) == 0) {
    if (inode.i_links_count == 0) {
      u32 blocks = (inode.i_size + fs->block_size - 1) / fs->block_size;
      for (u32 b = 0; b < blocks; b++) { u32 phys = ext3_get_block(fs, &inode, b); if (phys) ext3_free_block(fs, phys); }
      ext3_free_inode(fs, info->inode_num);
    }
  }
}

static void ext3_setup_node(struct vfs_node *n, struct ext3_fs *fs, u32 ino, u32 mode) {
  struct ext3_inode_info *ni = kmalloc(sizeof(struct ext3_inode_info));
  ni->fs = fs; ni->inode_num = ino;
  n->inode->data = ni; n->inode->blk_dev = fs->bdev;
  n->inode->read_cb = ext3_vfs_read; n->inode->write_cb = ext3_vfs_write;
  n->inode->setattr_cb = ext3_vfs_setattr; n->inode->statfs_cb = ext3_vfs_statfs;
  n->inode->unlink_cb = ext3_vfs_unlink; n->inode->release_cb = ext3_vfs_release;
  n->inode->fsync_cb = ext3_vfs_fsync;
  if ((mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
    n->inode->create_cb = ext3_vfs_create; n->inode->mkdir_cb = ext3_vfs_mkdir;
    n->inode->readdir_cb = ext3_vfs_readdir; n->inode->rmdir_cb = ext3_vfs_rmdir;
    n->inode->rename_cb = ext3_vfs_rename; n->inode->symlink_cb = ext3_vfs_symlink;
    n->inode->link_cb = ext3_vfs_link;
  }
}

static void ext3_populate_vfs(struct ext3_fs *fs, u32 ino, const char *base_path) {
  struct ext2_inode inode; if (ext3_read_inode(fs, ino, &inode) < 0) return;
  if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return;
  u8 *buf = kmalloc(inode.i_size);
  for (u32 i = 0; i < (inode.i_size + fs->block_size - 1) / fs->block_size; i++) {
    u32 phys = ext3_get_block(fs, &inode, i); if (phys) ext3_read_block(fs, phys, buf + i * fs->block_size);
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
      if (ext3_read_inode(fs, e->inode, &ci) == 0) {
        struct vfs_node *n = vfs_add_node(full, ((ci.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) ? VFS_DIRECTORY : VFS_FILE, 0, ci.i_size, 0);
        if (n) {
          ext3_setup_node(n, fs, e->inode, ci.i_mode);
          if ((ci.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) ext3_populate_vfs(fs, e->inode, full);
        }
      }
    }
    off += e->rec_len;
  }
  kfree(buf);
}

static struct vfs_node *ext3_vfs_mount_cb(const char *source, u64 flags, void *data) {
  (void)flags; struct block_device *dev = blk_get(source); if (!dev) return ERR_PTR(-ENODEV);
  u8 *sb_buf = kmalloc(1024); if (blk_read_cached(dev, 2, 2, sb_buf) < 0) { kfree(sb_buf); return ERR_PTR(-EIO); }
  struct ext2_superblock *sb = (struct ext2_superblock *)sb_buf;
  if (sb->s_magic != EXT2_SUPER_MAGIC) { kfree(sb_buf); return ERR_PTR(-EINVAL); }
  struct ext3_fs *fs = kmalloc(sizeof(struct ext3_fs)); memset(fs, 0, sizeof(struct ext3_fs));
  fs->bdev = dev; memcpy(&fs->sb, sb, sizeof(struct ext2_superblock));
  fs->block_size = 1024 << fs->sb.s_log_block_size;
  fs->inodes_per_group = fs->sb.s_inodes_per_group;
  fs->inode_size = (fs->sb.s_rev_level == 0) ? 128 : fs->sb.s_inode_size;
  if (fs->sb.s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL) {
    memcpy(&fs->journal_inum, sb_buf + EXT3_SB_JOURNAL_INUM_OFF, 4);
    if (fs->journal_inum != 0) {
      ext3_read_inode(fs, fs->journal_inum, &fs->journal_inode_cache);
      fs->jdev = journal_mount(&fs->journal_inode_cache, fs->block_size, &ext3_jbd_ops);
      if (fs->jdev) {
        fs->jdev->fs_priv = fs;
        if (fs->sb.s_feature_incompat & EXT3_FEATURE_INCOMPAT_RECOVER) {
          console_write("ext3: journal needs recovery\n"); journal_recover(fs->jdev);
        }
      }
    }
  }
  kfree(sb_buf);
  struct vfs_node *root = vfs_create_node(VFS_DIRECTORY);
  ext3_setup_node(root, fs, 2, EXT2_S_IFDIR);
  if (data) ext3_populate_vfs(fs, 2, (const char *)data);
  return root;
}

static struct vfs_fs ext3_vfs = { .name = "ext3", .mount = ext3_vfs_mount_cb };

void ext3_init(void) {
  vfs_register_fs(&ext3_vfs);
  vfs_mount("virtio-blk0", "/ext3", "ext3", 0);
}
