#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/ext2.h>
#include <b1nix/ext3.h>
#include <b1nix/journal.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <string.h>

#define EXT3_FEATURE_COMPAT_HAS_JOURNAL    0x0004
#define EXT3_FEATURE_INCOMPAT_RECOVER      0x0004
#define EXT3_SB_JOURNAL_INUM_OFF           0xE0

static struct block_device *ext3_dev = 0;
static struct ext2_superblock ext3_sb;
static u32 ext3_block_size;
static u32 ext3_inodes_per_group;
static u32 ext3_inode_size;

/* JBD State */
static u32 journal_inum = 0;
static struct ext2_inode journal_inode_cache;
static struct journal_dev *ext3_jdev = 0;

static int ext3_read_block(u32 block, void *buffer) {
  return blk_read_cached(ext3_dev, (u64)block * (ext3_block_size / 512),
                         ext3_block_size / 512, buffer);
}

static int ext3_write_block(u32 block, const void *buffer) {
  return blk_write_cached(ext3_dev, (u64)block * (ext3_block_size / 512),
                          ext3_block_size / 512, buffer);
}

/* --- JBD Callbacks --- */
static u32 ext3_get_block(struct ext2_inode *inode,
                          u32 block_idx); /* Forward decl */

static int ext3_jbd_read(struct journal_dev *jdev, u32 logical, void *buf) {
  (void)jdev;
  u32 phys = ext3_get_block(&journal_inode_cache, logical);
  if (!phys)
    return -1;
  return ext3_read_block(phys, buf);
}

static int ext3_jbd_write(struct journal_dev *jdev, u32 logical,
                          const void *buf) {
  (void)jdev;
  u32 phys = ext3_get_block(&journal_inode_cache, logical);
  if (!phys)
    return -1; /* The journal is preallocated by mke2fs. */
  return ext3_write_block(phys, buf);
}

static int ext3_jbd_fs_write(struct journal_dev *jdev, u32 phys,
                             const void *buf) {
  (void)jdev;
  return ext3_write_block(phys, buf);
}

static struct journal_ops ext3_jbd_ops = {ext3_jbd_read, ext3_jbd_write,
                                          ext3_jbd_fs_write};

/* Wrap data writes in small journal transactions. */
static int ext3_journal_write(u32 block, const void *buffer) {
  if (ext3_jdev) {
    struct journal_handle *h = journal_start_transaction(ext3_jdev);
    if (h) {
      journal_log_block(h, block, buffer);
      journal_commit_transaction(h);
      return 0;
    }
  }
  return ext3_write_block(block, buffer);
}

static void ext3_read_bgd(u32 group, struct ext2_block_group_desc *bgd) {
  u32 bg_block =
      (ext3_block_size == 1024 ? 2 : 1) +
      (group * sizeof(struct ext2_block_group_desc)) / ext3_block_size;
    u8 *buf = kmalloc(ext3_block_size);
    ext3_read_block(bg_block, buf);
  memcpy(bgd,
         buf +
             ((group * sizeof(struct ext2_block_group_desc)) % ext3_block_size),
         sizeof(struct ext2_block_group_desc));
    kfree(buf);
}

static void ext3_write_bgd(u32 group, struct ext2_block_group_desc *bgd) {
  u32 bg_block =
      (ext3_block_size == 1024 ? 2 : 1) +
      (group * sizeof(struct ext2_block_group_desc)) / ext3_block_size;
    u8 *buf = kmalloc(ext3_block_size);
    ext3_read_block(bg_block, buf);
  memcpy(buf +
             ((group * sizeof(struct ext2_block_group_desc)) % ext3_block_size),
         bgd, sizeof(struct ext2_block_group_desc));
  ext3_journal_write(bg_block, buf);
    kfree(buf);
}

static void ext3_write_superblock(void) {
    u8 *sb_buf = kmalloc(1024);
    if (blk_read_cached(ext3_dev, 2, 2, sb_buf) >= 0) {
        memcpy(sb_buf, &ext3_sb, sizeof(struct ext2_superblock));
        blk_write_cached(ext3_dev, 2, 2, sb_buf);
    }
    kfree(sb_buf);
}

static int ext3_read_inode(u32 inode_num, struct ext2_inode *inode) {
  if (inode_num == 0)
    return -1;
    u32 group = (inode_num - 1) / ext3_inodes_per_group;
    struct ext2_block_group_desc bgd;
    ext3_read_bgd(group, &bgd);
  u32 inode_offset =
      ((inode_num - 1) % ext3_inodes_per_group) * ext3_inode_size;
  u32 block_idx = bgd.bg_inode_table + (inode_offset / ext3_block_size);
    u8 *buf = kmalloc(ext3_block_size);
  ext3_read_block(block_idx, buf);
  memcpy(inode, buf + (inode_offset % ext3_block_size),
         sizeof(struct ext2_inode));
    kfree(buf);
    return 0;
}

static int ext3_write_inode(u32 inode_num, const struct ext2_inode *inode) {
  if (inode_num == 0)
    return -1;
    u32 group = (inode_num - 1) / ext3_inodes_per_group;
    struct ext2_block_group_desc bgd;
    ext3_read_bgd(group, &bgd);
  u32 inode_offset =
      ((inode_num - 1) % ext3_inodes_per_group) * ext3_inode_size;
  u32 block_idx = bgd.bg_inode_table + (inode_offset / ext3_block_size);
    u8 *buf = kmalloc(ext3_block_size);
  ext3_read_block(block_idx, buf);
  memcpy(buf + (inode_offset % ext3_block_size), inode,
         sizeof(struct ext2_inode));
  int ret = ext3_journal_write(block_idx, buf);
    kfree(buf);
    return ret;
}

static u32 ext3_alloc_block(void) {
  u32 groups = (ext3_sb.s_blocks_count + ext3_sb.s_blocks_per_group - 1) /
               ext3_sb.s_blocks_per_group;
    for (u32 g = 0; g < groups; g++) {
        struct ext2_block_group_desc bgd;
        ext3_read_bgd(g, &bgd);
    if (bgd.bg_free_blocks_count == 0)
      continue;
        u8 *bitmap = kmalloc(ext3_block_size);
        ext3_read_block(bgd.bg_block_bitmap, bitmap);
        for (u32 i = 0; i < ext3_sb.s_blocks_per_group; i++) {
            if (!(bitmap[i / 8] & (1 << (i % 8)))) {
                bitmap[i / 8] |= (1 << (i % 8));
        ext3_journal_write(bgd.bg_block_bitmap, bitmap);
                kfree(bitmap);
                bgd.bg_free_blocks_count--;
                ext3_write_bgd(g, &bgd);
                ext3_sb.s_free_blocks_count--;
                ext3_write_superblock();
        u32 block_num = g * ext3_sb.s_blocks_per_group + i +
                        (ext3_sb.s_log_block_size == 0 ? 1 : 0);
                u8 *zero = kzalloc(ext3_block_size);
        ext3_journal_write(block_num, zero);
                kfree(zero);
                return block_num;
            }
        }
        kfree(bitmap);
    }
    return 0;
}

static u32 ext3_alloc_inode(void) {
  u32 groups = (ext3_sb.s_inodes_count + ext3_inodes_per_group - 1) /
               ext3_inodes_per_group;
    for (u32 g = 0; g < groups; g++) {
        struct ext2_block_group_desc bgd;
        ext3_read_bgd(g, &bgd);
    if (bgd.bg_free_inodes_count == 0)
      continue;
        u8 *bitmap = kmalloc(ext3_block_size);
        ext3_read_block(bgd.bg_inode_bitmap, bitmap);
        for (u32 i = 0; i < ext3_inodes_per_group; i++) {
            if (!(bitmap[i / 8] & (1 << (i % 8)))) {
                bitmap[i / 8] |= (1 << (i % 8));
        ext3_journal_write(bgd.bg_inode_bitmap, bitmap);
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

static u32 ext3_get_block(struct ext2_inode *inode, u32 block_idx) {
    if (block_idx < EXT2_NDIR_BLOCKS)
        return inode->i_block[block_idx];

    u32 ptrs = ext3_block_size / 4;
    block_idx -= EXT2_NDIR_BLOCKS;

    if (block_idx < ptrs) {
    if (!inode->i_block[EXT2_IND_BLOCK])
      return 0;
        u32 *ind = kmalloc(ext3_block_size);
        ext3_read_block(inode->i_block[EXT2_IND_BLOCK], ind);
        u32 r = ind[block_idx];
        kfree(ind);
        return r;
    }
    block_idx -= ptrs;

    if (block_idx < ptrs * ptrs) {
    if (!inode->i_block[EXT2_DIND_BLOCK])
      return 0;
        u32 *dind = kmalloc(ext3_block_size);
        ext3_read_block(inode->i_block[EXT2_DIND_BLOCK], dind);
    if (!dind[block_idx / ptrs]) {
      kfree(dind);
      return 0;
    }
        u32 *ind = kmalloc(ext3_block_size);
        ext3_read_block(dind[block_idx / ptrs], ind);
        u32 r = ind[block_idx % ptrs];
    kfree(ind);
    kfree(dind);
        return r;
    }
    return 0;
}

static int ext3_set_block(struct ext2_inode *inode, u32 block_idx, u32 phys) {
    u32 ptrs = ext3_block_size / 4;

    if (block_idx < EXT2_NDIR_BLOCKS) {
        inode->i_block[block_idx] = phys;
        return 1;
    }

    block_idx -= EXT2_NDIR_BLOCKS;

    if (block_idx < ptrs) {
        if (!inode->i_block[EXT2_IND_BLOCK]) {
            u32 b = ext3_alloc_block();
      if (!b)
        return 0;
            inode->i_block[EXT2_IND_BLOCK] = b;
        }
        u32 *ind = kmalloc(ext3_block_size);
        ext3_read_block(inode->i_block[EXT2_IND_BLOCK], ind);
        ind[block_idx] = phys;
    ext3_journal_write(inode->i_block[EXT2_IND_BLOCK], ind);
        kfree(ind);
        return 1;
    }

    block_idx -= ptrs;

    if (block_idx < ptrs * ptrs) {
        if (!inode->i_block[EXT2_DIND_BLOCK]) {
            u32 b = ext3_alloc_block();
      if (!b)
        return 0;
            inode->i_block[EXT2_DIND_BLOCK] = b;
        }
        u32 *dind = kmalloc(ext3_block_size);
        ext3_read_block(inode->i_block[EXT2_DIND_BLOCK], dind);
        u32 idx1 = block_idx / ptrs;
        if (!dind[idx1]) {
            u32 b = ext3_alloc_block();
      if (!b) {
        kfree(dind);
        return 0;
      }
            dind[idx1] = b;
      ext3_journal_write(inode->i_block[EXT2_DIND_BLOCK], dind);
        }
        u32 *ind = kmalloc(ext3_block_size);
        ext3_read_block(dind[idx1], ind);
        ind[block_idx % ptrs] = phys;
    ext3_journal_write(dind[idx1], ind);
    kfree(ind);
    kfree(dind);
        return 1;
    }
    return 0;
}

static isize ext3_vfs_read(struct vfs_node *node, u64 offset, char *buffer,
                           usize size) {
    u32 ino = (u32)(usize)node->data;
    struct ext2_inode inode;
  if (ext3_read_inode(ino, &inode) < 0)
    return -1;
  if (offset >= inode.i_size)
    return 0;

    usize remaining = inode.i_size - offset;
    usize to_read = size < remaining ? size : remaining;
    usize done = 0;
    u8 *block_buf = kmalloc(ext3_block_size);

    while (done < to_read) {
        u32 b_idx = (offset + done) / ext3_block_size;
        u32 b_off = (offset + done) % ext3_block_size;
        u32 phys = ext3_get_block(&inode, b_idx);
        usize chunk = ext3_block_size - b_off;
    if (chunk > to_read - done)
      chunk = to_read - done;

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

static isize ext3_vfs_write(struct vfs_node *node, u64 offset,
                            const char *buffer, usize size) {
    u32 ino = (u32)(usize)node->data;
    struct ext2_inode inode;
  if (ext3_read_inode(ino, &inode) < 0)
    return -1;

    u64 new_size = offset + size;
    u32 old_blks = (inode.i_size + ext3_block_size - 1) / ext3_block_size;
    u32 new_blks = (new_size + ext3_block_size - 1) / ext3_block_size;

    if (new_size > inode.i_size) {
        for (u32 b = old_blks; b < new_blks; b++) {
            u32 pblk = ext3_alloc_block();
      if (!pblk)
        return -1;
      if (!ext3_set_block(&inode, b, pblk))
        return -1;
            inode.i_blocks += ext3_block_size / 512;
        }
        inode.i_size = (u32)new_size;
        ext3_write_inode(ino, &inode);
        node->size = inode.i_size;
    }

    usize done = 0;
    u8 *block_buf = kmalloc(ext3_block_size);
    while (done < size) {
        u32 b_idx = (offset + done) / ext3_block_size;
        u32 b_off = (offset + done) % ext3_block_size;
        u32 phys = ext3_get_block(&inode, b_idx);
    if (!phys)
      break;

        usize chunk = ext3_block_size - b_off;
    if (chunk > size - done)
      chunk = size - done;

    if (chunk < ext3_block_size)
      ext3_read_block(phys, block_buf);
        memcpy(block_buf + b_off, buffer + done, chunk);
  ext3_journal_write(phys, block_buf); /* Automatic journaling. */
        done += chunk;
    }

    kfree(block_buf);
    return done;
}

static int ext3_add_dir_entry(u32 dir_ino, u32 child_ino, const char *name) {
    struct ext2_inode dir;
  if (ext3_read_inode(dir_ino, &dir) < 0)
    return -1;

    usize name_len = strlen(name);
  if (name_len > 255)
    name_len = 255;
    u32 needed = 8 + ((name_len + 3) & ~3);
    u8 *buf = kmalloc(ext3_block_size);
    u32 blocks = (dir.i_size + ext3_block_size - 1) / ext3_block_size;

    for (u32 b = 0; b < blocks; b++) {
        u32 phys = ext3_get_block(&dir, b);
    if (!phys)
      continue;
        ext3_read_block(phys, buf);
        usize off = 0;
        while (off < ext3_block_size) {
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
                ne->file_type = EXT2_FT_REG_FILE;
                memcpy(ne->name, name, name_len);
        ext3_journal_write(phys, buf);
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
    ext3_journal_write(phys, buf);
        kfree(buf);
        return 0;
    }

    kfree(buf);
    return -1;
}

static int ext3_vfs_create(struct vfs_node *dir, const char *name,
                           const char *full_path) {
    u32 dir_ino = (u32)(usize)dir->data;
    u32 new_ino = ext3_alloc_inode();
  if (!new_ino)
    return -1;

    struct ext2_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = EXT2_S_IFREG | 0644;
    inode.i_links_count = 1;
    ext3_write_inode(new_ino, &inode);

  if (ext3_add_dir_entry(dir_ino, new_ino, name) < 0)
    return -1;

  struct vfs_node *n =
      vfs_add_node(full_path, VFS_FILE, (void *)(usize)new_ino, 0, 0);
  if (n) {
    n->read_cb = ext3_vfs_read;
    n->write_cb = ext3_vfs_write;
  }
    return 0;
}

static void ext3_populate_vfs(u32 ino, const char *base_path) {
    struct ext2_inode inode;
  if (ext3_read_inode(ino, &inode) < 0)
    return;
  if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
    return;

    u8 *buf = kmalloc(inode.i_size);
  for (u32 i = 0; i < (inode.i_size + ext3_block_size - 1) / ext3_block_size;
       i++) {
        u32 phys = ext3_get_block(&inode, i);
    if (phys)
      ext3_read_block(phys, buf + i * ext3_block_size);
    }

    usize off = 0;
    while (off < inode.i_size) {
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
            if (ext3_read_inode(e->inode, &ci) == 0) {
                if ((ci.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
          struct vfs_node *dn =
              vfs_add_node(full, VFS_DIRECTORY, (void *)(usize)e->inode, 0, 0);
          if (dn)
            dn->create_cb = ext3_vfs_create;
                    ext3_populate_vfs(e->inode, full);
                } else {
          struct vfs_node *n = vfs_add_node(
              full, VFS_FILE, (void *)(usize)e->inode, ci.i_size, 0);
          if (n) {
            n->read_cb = ext3_vfs_read;
            n->write_cb = ext3_vfs_write;
          }
                }
            }
        }
        off += e->rec_len;
    }
    kfree(buf);
}

void ext3_init(void) {
    ext3_dev = blk_get("virtio-blk1");
  if (!ext3_dev)
    ext3_dev = blk_get("sata0");
  if (!ext3_dev)
    return;

    u8 *sb = kmalloc(1024);
  if (blk_read_cached(ext3_dev, 2, 2, sb) < 0) {
    kfree(sb);
    return;
  }
    memcpy(&ext3_sb, sb, sizeof(struct ext2_superblock));

  if (ext3_sb.s_magic != EXT2_SUPER_MAGIC || ext3_sb.s_rev_level == 0) {
        kfree(sb);
        return;
    }

  ext3_block_size = 1024 << ext3_sb.s_log_block_size;
  ext3_inodes_per_group = ext3_sb.s_inodes_per_group;
  ext3_inode_size = ext3_sb.s_inode_size;

  if (ext3_sb.s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL) {
    console_write("ext3: detecting journal...\n");
    memcpy(&journal_inum, sb + EXT3_SB_JOURNAL_INUM_OFF, 4);

    if (journal_inum != 0) {
      ext3_read_inode(journal_inum, &journal_inode_cache);
      ext3_jdev =
          journal_mount(&journal_inode_cache, ext3_block_size, &ext3_jbd_ops);

      if (ext3_jdev &&
          (ext3_sb.s_feature_incompat & EXT3_FEATURE_INCOMPAT_RECOVER)) {
        console_write("ext3: journal needs recovery\n");
        journal_recover(ext3_jdev);
    }
    }
  }
  kfree(sb);

    console_write("ext3: mounted rw, block_size=");
    console_write_dec(ext3_block_size);
    console_write("\n");

  struct vfs_node *root =
      vfs_add_node("/ext3", VFS_DIRECTORY, (void *)(usize)2, 0, 0);
  if (root)
    root->create_cb = ext3_vfs_create;
    ext3_populate_vfs(2, "/ext3");
}
