#include <b1nix/ext2.h>
#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <string.h>
#include <b1nix/klog.h>

static struct ext2_fs *ext2_fs_list = NULL;

static int ext2_read_block(struct ext2_fs *fs, u32 block, void *buffer) {
	if (!fs || !fs->bdev) return -1;
	u64 lba = (u64)block * (fs->block_size / 512);
	return blk_read_cached(fs->bdev, lba, fs->block_size / 512, buffer);
}

static int ext2_write_block(struct ext2_fs *fs, u32 block, const void *buffer) {
	if (!fs || !fs->bdev) return -1;
	u64 lba = (u64)block * (fs->block_size / 512);
	return blk_write_cached(fs->bdev, lba, fs->block_size / 512, buffer);
}

static int ext2_vfs_fsync(struct vfs_node *node);
static int ext2_vfs_setattr(struct vfs_node *node);

static void ext2_read_bgd(struct ext2_fs *fs, u32 group, struct ext2_block_group_desc *bgd) {
	u32 bg_desc_block = (fs->block_size == 1024) ? 2 : 1;
	u32 bg_offset = group * sizeof(struct ext2_block_group_desc);
	u32 bg_block = bg_desc_block + (bg_offset / fs->block_size);
	
	u8 *bg_buffer = kmalloc(fs->block_size);
	ext2_read_block(fs, bg_block, bg_buffer);
  memcpy(bgd, bg_buffer + (bg_offset % fs->block_size),
         sizeof(struct ext2_block_group_desc));
	kfree(bg_buffer);
}

static void ext2_write_bgd(struct ext2_fs *fs, u32 group, struct ext2_block_group_desc *bgd) {
	u32 bg_desc_block = (fs->block_size == 1024) ? 2 : 1;
	u32 bg_offset = group * sizeof(struct ext2_block_group_desc);
	u32 bg_block = bg_desc_block + (bg_offset / fs->block_size);
	
	u8 *bg_buffer = kmalloc(fs->block_size);
	ext2_read_block(fs, bg_block, bg_buffer);
  memcpy(bg_buffer + (bg_offset % fs->block_size), bgd,
         sizeof(struct ext2_block_group_desc));
	ext2_write_block(fs, bg_block, bg_buffer);
	kfree(bg_buffer);
}

static void ext2_write_superblock(struct ext2_fs *fs) {
	u8 *sb_buffer = kmalloc(1024);
	if (blk_read_cached(fs->bdev, 2, 2, sb_buffer) >= 0) {
		memcpy(sb_buffer, &fs->sb, sizeof(struct ext2_superblock));
		blk_write_cached(fs->bdev, 2, 2, sb_buffer);
	}
	kfree(sb_buffer);
}

static int ext2_read_inode(struct ext2_fs *fs, u32 inode_num, struct ext2_inode *inode) {
  if (inode_num == 0)
    return -1;
	
	u32 group = (inode_num - 1) / fs->inodes_per_group;
	u32 index = (inode_num - 1) % fs->inodes_per_group;
	
	struct ext2_block_group_desc bgd;
	ext2_read_bgd(fs, group, &bgd);
	u32 inode_table_block = bgd.bg_inode_table;
	
	u32 inode_offset = index * fs->inode_size;
	u32 block_idx = inode_table_block + (inode_offset / fs->block_size);
	
	u8 *inode_buffer = kmalloc(fs->block_size);
	if (ext2_read_block(fs, block_idx, inode_buffer) < 0) {
		kfree(inode_buffer);
		return -1;
	}
	
  memcpy(inode, inode_buffer + (inode_offset % fs->block_size),
         sizeof(struct ext2_inode));
	kfree(inode_buffer);
	return 0;
}

static int ext2_write_inode(struct ext2_fs *fs, u32 inode_num, const struct ext2_inode *inode) {
  if (inode_num == 0)
    return -1;
	
	u32 group = (inode_num - 1) / fs->inodes_per_group;
	u32 index = (inode_num - 1) % fs->inodes_per_group;
	
	struct ext2_block_group_desc bgd;
	ext2_read_bgd(fs, group, &bgd);
	u32 inode_table_block = bgd.bg_inode_table;
	
	u32 inode_offset = index * fs->inode_size;
	u32 block_idx = inode_table_block + (inode_offset / fs->block_size);
	
	u8 *inode_buffer = kmalloc(fs->block_size);
	if (ext2_read_block(fs, block_idx, inode_buffer) < 0) {
		kfree(inode_buffer);
		return -1;
	}
	
  memcpy(inode_buffer + (inode_offset % fs->block_size), inode,
         sizeof(struct ext2_inode));
	int ret = ext2_write_block(fs, block_idx, inode_buffer);
	kfree(inode_buffer);
	return ret;
}

static u32 ext2_alloc_block(struct ext2_fs *fs) {
  /* Bitmap+superblock RMW must be atomic per fs — see ext4_alloc_block_tx. */
  vfs_meta_lock_acquire(&fs->alloc_lock);
  u32 groups = (fs->sb.s_blocks_count + fs->sb.s_blocks_per_group - 1) /
               fs->sb.s_blocks_per_group;
	for (u32 g = 0; g < groups; g++) {
		struct ext2_block_group_desc bgd;
		ext2_read_bgd(fs, g, &bgd);
		if (bgd.bg_free_blocks_count > 0) {
			u8 *bitmap = kmalloc(fs->block_size);
			if (ext2_read_block(fs, bgd.bg_block_bitmap, bitmap) < 0) {
				kfree(bitmap);
				continue;
			}
			for (u32 i = 0; i < fs->sb.s_blocks_per_group; i++) {
				if (!(bitmap[i / 8] & (1 << (i % 8)))) {
					bitmap[i / 8] |= (1 << (i % 8));
					ext2_write_block(fs, bgd.bg_block_bitmap, bitmap);
					kfree(bitmap);
					
					bgd.bg_free_blocks_count--;
					ext2_write_bgd(fs, g, &bgd);
					
					fs->sb.s_free_blocks_count--;
          fs->bitmaps_dirty = 1;
					
					u32 block_num = g * fs->sb.s_blocks_per_group + i +
					              (fs->sb.s_log_block_size == 0 ? 1 : 0);
					
					u8 *zero_block = kmalloc(fs->block_size);
					memset(zero_block, 0, fs->block_size);
					ext2_write_block(fs, block_num, zero_block);
					kfree(zero_block);

					vfs_meta_lock_release(&fs->alloc_lock);
					return block_num;
				}
			}
			kfree(bitmap);
		}
	}
	vfs_meta_lock_release(&fs->alloc_lock);
	return 0;
}

static void ext2_free_block(struct ext2_fs *fs, u32 block_num) {
  if (block_num == 0) return;
  vfs_meta_lock_acquire(&fs->alloc_lock);
  u32 g = (block_num - (fs->sb.s_log_block_size == 0 ? 1 : 0)) / fs->sb.s_blocks_per_group;
  u32 i = (block_num - (fs->sb.s_log_block_size == 0 ? 1 : 0)) % fs->sb.s_blocks_per_group;

  struct ext2_block_group_desc bgd;
  ext2_read_bgd(fs, g, &bgd);
  u8 *bitmap = kmalloc(fs->block_size);
  if (ext2_read_block(fs, bgd.bg_block_bitmap, bitmap) < 0) {
    kfree(bitmap);
    vfs_meta_lock_release(&fs->alloc_lock);
    return;
  }
  bitmap[i / 8] &= ~(1 << (i % 8));
  ext2_write_block(fs, bgd.bg_block_bitmap, bitmap);
  kfree(bitmap);

  bgd.bg_free_blocks_count++;
  ext2_write_bgd(fs, g, &bgd);
  fs->sb.s_free_blocks_count++;
  fs->bitmaps_dirty = 1;
  vfs_meta_lock_release(&fs->alloc_lock);
}

static void ext2_free_inode(struct ext2_fs *fs, u32 inode_num) {
  if (inode_num == 0) return;
  vfs_meta_lock_acquire(&fs->alloc_lock);
  u32 g = (inode_num - 1) / fs->inodes_per_group;
  u32 i = (inode_num - 1) % fs->inodes_per_group;

  struct ext2_block_group_desc bgd;
  ext2_read_bgd(fs, g, &bgd);
  u8 *bitmap = kmalloc(fs->block_size);
  if (ext2_read_block(fs, bgd.bg_inode_bitmap, bitmap) < 0) {
    kfree(bitmap);
    vfs_meta_lock_release(&fs->alloc_lock);
    return;
  }
  bitmap[i / 8] &= ~(1 << (i % 8));
  ext2_write_block(fs, bgd.bg_inode_bitmap, bitmap);
  kfree(bitmap);

  bgd.bg_free_inodes_count++;
  ext2_write_bgd(fs, g, &bgd);
  fs->sb.s_free_inodes_count++;
  fs->bitmaps_dirty = 1;
  vfs_meta_lock_release(&fs->alloc_lock);
}

static u32 ext2_alloc_inode(struct ext2_fs *fs) {
  vfs_meta_lock_acquire(&fs->alloc_lock);
  u32 groups = (fs->sb.s_inodes_count + fs->inodes_per_group - 1) /
               fs->inodes_per_group;
	for (u32 g = 0; g < groups; g++) {
		struct ext2_block_group_desc bgd;
		ext2_read_bgd(fs, g, &bgd);
		if (bgd.bg_free_inodes_count > 0) {
			u8 *bitmap = kmalloc(fs->block_size);
			if (ext2_read_block(fs, bgd.bg_inode_bitmap, bitmap) < 0) {
				kfree(bitmap);
				continue;
			}
			for (u32 i = 0; i < fs->inodes_per_group; i++) {
				if (!(bitmap[i / 8] & (1 << (i % 8)))) {
					bitmap[i / 8] |= (1 << (i % 8));
					ext2_write_block(fs, bgd.bg_inode_bitmap, bitmap);
					kfree(bitmap);
					
					bgd.bg_free_inodes_count--;
					ext2_write_bgd(fs, g, &bgd);
					
					fs->sb.s_free_inodes_count--;
          fs->bitmaps_dirty = 1;
					
					u32 inode_num = g * fs->inodes_per_group + i + 1;
					
					struct ext2_inode new_inode;
					memset(&new_inode, 0, sizeof(new_inode));
					ext2_write_inode(fs, inode_num, &new_inode);

					vfs_meta_lock_release(&fs->alloc_lock);
					return inode_num;
				}
			}
			kfree(bitmap);
		}
	}
	vfs_meta_lock_release(&fs->alloc_lock);
	return 0;
}

static int ext2_load_acls(struct ext2_fs *fs, struct ext2_inode *ei, struct vfs_inode *vi) {
    if (ei->i_file_acl == 0) return 0;
    u8 *buf = kmalloc(fs->block_size);
    if (ext2_read_block(fs, ei->i_file_acl, buf) < 0) { kfree(buf); return -1; }
    
    /* b1nix uses a simple flat storage: first 4 bytes is an entry count, then
     * the entries. A block written by a real Linux mkfs/setfacl instead begins
     * with the standard ext2 xattr header magic (0xEA020000) — reading that as
     * a count yields a huge/negative value, and the original unclamped
     * `count * sizeof` memcpy then scribbled gigabytes over the heap. Read the
     * count unsigned and treat anything out of range (foreign xattr blocks
     * included) as "no ACLs" so genuine on-disk filesystems mount safely. We do
     * not interpret Linux POSIX ACL xattrs here (read-only path doesn't need
     * them); ignoring them is correct, not a silent failure. */
    u32 count = *(u32 *)buf;
    if (count > ACL_MAX_ENTRIES) count = 0;
    vi->acl_count = count;
    if (count)
        memcpy(vi->acls, buf + 4, count * sizeof(struct acl_entry));
    kfree(buf);
    return 0;
}

static int ext2_save_acls(struct ext2_fs *fs, u32 inode_num, struct ext2_inode *ei, struct vfs_inode *vi) {
    (void)inode_num;
    if (vi->acl_count == 0) {
        if (ei->i_file_acl != 0) {
            /* Free the ACL block */
            ext2_free_block(fs, ei->i_file_acl);
            ei->i_file_acl = 0;
        }
        return 0;
    }
    
    if (ei->i_file_acl == 0) {
        ei->i_file_acl = ext2_alloc_block(fs);
        if (ei->i_file_acl == 0) return -ENOSPC;
    }
    
    u8 *buf = kzalloc(fs->block_size);
    *(int *)buf = vi->acl_count;
    memcpy(buf + 4, vi->acls, vi->acl_count * sizeof(struct acl_entry));
    ext2_write_block(fs, ei->i_file_acl, buf);
    kfree(buf);
    return 0;
}

/* Read path with single and double-indirect block support. */
static u32 ext2_get_inode_block(struct ext2_fs *fs, struct ext2_inode *inode, u32 block_idx) {
  u32 ptrs_per_block = fs->block_size / 4;

	if (block_idx < EXT2_NDIR_BLOCKS) {
		return inode->i_block[block_idx];
	}
  block_idx -= EXT2_NDIR_BLOCKS;
	
  if (block_idx < ptrs_per_block) {
		u32 ind_block = inode->i_block[EXT2_IND_BLOCK];
    if (ind_block == 0)
      return 0;
		u32 *ind_buffer = kmalloc(fs->block_size);
		if (ext2_read_block(fs, ind_block, ind_buffer) < 0) {
			kfree(ind_buffer);
			return 0;
		}
    u32 phys = ind_buffer[block_idx];
    kfree(ind_buffer);
    return phys;
  }
  block_idx -= ptrs_per_block;

  if (block_idx < ptrs_per_block * ptrs_per_block) {
    u32 dind_block = inode->i_block[EXT2_DIND_BLOCK];
    if (dind_block == 0)
      return 0;

    u32 *ind_buffer = kmalloc(fs->block_size);
    if (ext2_read_block(fs, dind_block, ind_buffer) < 0) {
      kfree(ind_buffer);
      return 0;
    }
    u32 ind1 = ind_buffer[block_idx / ptrs_per_block];
    if (ind1 == 0) {
      kfree(ind_buffer);
      return 0;
    }
    if (ext2_read_block(fs, ind1, ind_buffer) < 0) {
      kfree(ind_buffer);
      return 0;
    }
    u32 phys = ind_buffer[block_idx % ptrs_per_block];
		kfree(ind_buffer);
		return phys;
	}

	return 0;
}

/* Allocate data blocks for sparse files and double-indirect growth. */
static u32 ext2_allocate_block_for_inode(struct ext2_fs *fs, u32 inode_num,
                                          struct ext2_inode *inode, u32 b) {
  u32 ptrs_per_block = fs->block_size / 4;
  u32 pblk = 0;

  if (b < EXT2_NDIR_BLOCKS) {
    if (inode->i_block[b] == 0) {
      pblk = ext2_alloc_block(fs);
      if (!pblk)
        return 0;
      inode->i_block[b] = pblk;
      inode->i_blocks += (fs->block_size / 512);
      ext2_write_inode(fs, inode_num, inode);
    }
    return inode->i_block[b];
  }

  b -= EXT2_NDIR_BLOCKS;
  if (b < ptrs_per_block) {
    u32 ind_block = inode->i_block[EXT2_IND_BLOCK];
    if (ind_block == 0) {
      ind_block = ext2_alloc_block(fs);
      if (!ind_block)
        return 0;
      inode->i_block[EXT2_IND_BLOCK] = ind_block;
      inode->i_blocks += (fs->block_size / 512);
      ext2_write_inode(fs, inode_num, inode);
    }

    u32 *ind_buffer = kmalloc(fs->block_size);
    ext2_read_block(fs, ind_block, ind_buffer);
    if (ind_buffer[b] == 0) {
      pblk = ext2_alloc_block(fs);
      if (pblk) {
        ind_buffer[b] = pblk;
        ext2_write_block(fs, ind_block, ind_buffer);
        inode->i_blocks += (fs->block_size / 512);
        ext2_write_inode(fs, inode_num, inode);
      }
    } else {
      pblk = ind_buffer[b];
    }
    kfree(ind_buffer);
    return pblk;
  }

  b -= ptrs_per_block;
  if (b < ptrs_per_block * ptrs_per_block) {
    u32 dind_block = inode->i_block[EXT2_DIND_BLOCK];
    if (dind_block == 0) {
      dind_block = ext2_alloc_block(fs);
      if (!dind_block)
        return 0;
      inode->i_block[EXT2_DIND_BLOCK] = dind_block;
      inode->i_blocks += (fs->block_size / 512);
      ext2_write_inode(fs, inode_num, inode);
    }

    u32 *ind_buffer = kmalloc(fs->block_size);
    ext2_read_block(fs, dind_block, ind_buffer);

    u32 ind1_idx = b / ptrs_per_block;
    u32 ind1_block = ind_buffer[ind1_idx];
    if (ind1_block == 0) {
      ind1_block = ext2_alloc_block(fs);
      if (!ind1_block) {
        kfree(ind_buffer);
        return 0;
      }
      ind_buffer[ind1_idx] = ind1_block;
      ext2_write_block(fs, dind_block, ind_buffer);
      inode->i_blocks += (fs->block_size / 512);
      ext2_write_inode(fs, inode_num, inode);
    }

    ext2_read_block(fs, ind1_block, ind_buffer);
    u32 ind2_idx = b % ptrs_per_block;
    if (ind_buffer[ind2_idx] == 0) {
      pblk = ext2_alloc_block(fs);
      if (pblk) {
        ind_buffer[ind2_idx] = pblk;
        ext2_write_block(fs, ind1_block, ind_buffer);
        inode->i_blocks += (fs->block_size / 512);
        ext2_write_inode(fs, inode_num, inode);
      }
    } else {
      pblk = ind_buffer[ind2_idx];
    }
    kfree(ind_buffer);
    return pblk;
  }
  return 0;
}

static u32 get_ino(struct vfs_node *node) {
  if (!node || !node->inode || !node->inode->data) return 0;
  return ((struct ext2_inode_info *)node->inode->data)->inode_num;
}

static struct ext2_fs *get_fs(struct vfs_node *node) {
  if (!node || !node->inode || !node->inode->data) return NULL;
  return ((struct ext2_inode_info *)node->inode->data)->fs;
}

static isize ext2_vfs_read(struct vfs_node *node, u64 offset, char *buffer,
                           usize size, int flags) {
  (void)flags;
	u32 inode_num = get_ino(node);
  struct ext2_fs *fs = get_fs(node);
	struct ext2_inode inode;
  if (ext2_read_inode(fs, inode_num, &inode) < 0)
    return -EIO;
	
  if (offset >= inode.i_size)
    return 0;
	
	usize remaining = inode.i_size - offset;
	usize to_read = size < remaining ? size : remaining;
	usize bytes_read = 0;
	
	u8 *block_buf = kmalloc(fs->block_size);
  if (!block_buf)
    return -1;
	
	while (bytes_read < to_read) {
		u64 current_offset = offset + bytes_read;
		u32 block_idx = current_offset / fs->block_size;
		u32 block_offset = current_offset % fs->block_size;
		
		u32 phys_block = ext2_get_inode_block(fs, &inode, block_idx);
		if (phys_block == 0) {
			/* Clamp the hole zero-fill to the bytes actually requested — an
			 * unclamped block_size-block_offset overruns the caller buffer on a
			 * trailing partial block (R3-10). */
			usize hole = fs->block_size - block_offset;
			if (hole > to_read - bytes_read)
				hole = to_read - bytes_read;
			memset(buffer + bytes_read, 0, hole);
		} else {
			ext2_read_block(fs, phys_block, block_buf);
			usize chunk = fs->block_size - block_offset;
      if (chunk > to_read - bytes_read)
        chunk = to_read - bytes_read;
			memcpy(buffer + bytes_read, block_buf + block_offset, chunk);
		}
		
		usize chunk = fs->block_size - block_offset;
    if (chunk > to_read - bytes_read)
      chunk = to_read - bytes_read;
		bytes_read += chunk;
	}
	
	kfree(block_buf);
	if (bytes_read > 0) {
		node->inode->atime = vfs_get_unix_time();
		struct ext2_inode inode_to_update;
		if (ext2_read_inode(fs, inode_num, &inode_to_update) == 0) {
			inode_to_update.i_atime = node->inode->atime;
			ext2_write_inode(fs, inode_num, &inode_to_update);
		}
	}
	return bytes_read;
}

/* Sparse-file write path with file-size growth. */
static isize ext2_vfs_write(struct vfs_node *node, u64 offset,
                            const char *buffer, usize size, int flags) {
  (void)flags;
	u32 inode_num = get_ino(node);
  struct ext2_fs *fs = get_fs(node);
	struct ext2_inode inode;
  if (ext2_read_inode(fs, inode_num, &inode) < 0)
    return -EIO;
	
	usize bytes_written = 0;
	u8 *block_buf = kmalloc(fs->block_size);
  if (!block_buf)
    return -1;
	
	while (bytes_written < size) {
		u64 current_offset = offset + bytes_written;
		u32 block_idx = current_offset / fs->block_size;
		u32 block_offset = current_offset % fs->block_size;
		
    /* Resolve the block or allocate it when writing into a hole. */
		u32 phys_block = ext2_get_inode_block(fs, &inode, block_idx);
    if (!phys_block) {
      phys_block = ext2_allocate_block_for_inode(fs, inode_num, &inode, block_idx);
      if (!phys_block)
        break; /* Out of space. */
    }
		
		usize chunk = fs->block_size - block_offset;
    if (chunk > size - bytes_written)
      chunk = size - bytes_written;
		
		if (chunk < fs->block_size) {
			ext2_read_block(fs, phys_block, block_buf);
		}
		memcpy(block_buf + block_offset, buffer + bytes_written, chunk);
		ext2_write_block(fs, phys_block, block_buf);
		
		bytes_written += chunk;
	}
	
	kfree(block_buf);

  if (bytes_written > 0) {
    node->inode->mtime = vfs_get_unix_time();
    node->inode->ctime = vfs_get_unix_time();
    if (offset + bytes_written > inode.i_size) {
      inode.i_size = (u32)(offset + bytes_written);
    }
    if (offset + bytes_written > node->inode->size) {
      node->inode->size = (usize)(offset + bytes_written);
    }
    // Reuse our existing up-to-date inode rather than fetching a stale one from disk
    inode.i_mtime = node->inode->mtime;
    inode.i_ctime = node->inode->ctime;
    ext2_write_inode(fs, inode_num, &inode);
  }

	return bytes_written;
}

static void ext2_inode_blocks_sub(struct ext2_fs *fs, struct ext2_inode *inode,
                                  u32 blocks) {
  u32 sectors = blocks * (fs->block_size / 512);
  inode->i_blocks = inode->i_blocks > sectors ? inode->i_blocks - sectors : 0;
}

static int ext2_block_table_empty(u32 *table, u32 entries) {
  for (u32 i = 0; i < entries; i++) {
    if (table[i])
      return 0;
  }
  return 1;
}

static int ext2_vfs_truncate(struct vfs_node *node, u64 length) {
  u32 inode_num = get_ino(node);
  struct ext2_fs *fs = get_fs(node);
  struct ext2_inode inode;
  if (ext2_read_inode(fs, inode_num, &inode) < 0)
    return -EIO;

  u32 ptrs = fs->block_size / 4;
  u32 old_blocks = (inode.i_size + fs->block_size - 1) / fs->block_size;
  u32 new_blocks = (length + fs->block_size - 1) / fs->block_size;

  for (u32 b = new_blocks; b < old_blocks; b++) {
    if (b < EXT2_NDIR_BLOCKS) {
      if (inode.i_block[b]) {
        ext2_free_block(fs, inode.i_block[b]);
        ext2_inode_blocks_sub(fs, &inode, 1);
        inode.i_block[b] = 0;
      }
      continue;
    }

    u32 rel = b - EXT2_NDIR_BLOCKS;
    if (rel < ptrs) {
      if (!inode.i_block[EXT2_IND_BLOCK])
        continue;
      u32 *ind = kmalloc(fs->block_size);
      ext2_read_block(fs, inode.i_block[EXT2_IND_BLOCK], ind);
      if (ind[rel]) {
        ext2_free_block(fs, ind[rel]);
        ext2_inode_blocks_sub(fs, &inode, 1);
        ind[rel] = 0;
        ext2_write_block(fs, inode.i_block[EXT2_IND_BLOCK], ind);
      }
      if (ext2_block_table_empty(ind, ptrs)) {
        ext2_free_block(fs, inode.i_block[EXT2_IND_BLOCK]);
        ext2_inode_blocks_sub(fs, &inode, 1);
        inode.i_block[EXT2_IND_BLOCK] = 0;
      }
      kfree(ind);
      continue;
    }

    rel -= ptrs;
    if (rel < ptrs * ptrs && inode.i_block[EXT2_DIND_BLOCK]) {
      u32 *dind = kmalloc(fs->block_size);
      u32 *ind = kmalloc(fs->block_size);
      ext2_read_block(fs, inode.i_block[EXT2_DIND_BLOCK], dind);
      u32 idx1 = rel / ptrs;
      u32 idx2 = rel % ptrs;
      if (dind[idx1]) {
        ext2_read_block(fs, dind[idx1], ind);
        if (ind[idx2]) {
          ext2_free_block(fs, ind[idx2]);
          ext2_inode_blocks_sub(fs, &inode, 1);
          ind[idx2] = 0;
          ext2_write_block(fs, dind[idx1], ind);
        }
        if (ext2_block_table_empty(ind, ptrs)) {
          ext2_free_block(fs, dind[idx1]);
          ext2_inode_blocks_sub(fs, &inode, 1);
          dind[idx1] = 0;
          ext2_write_block(fs, inode.i_block[EXT2_DIND_BLOCK], dind);
        }
      }
      if (ext2_block_table_empty(dind, ptrs)) {
        ext2_free_block(fs, inode.i_block[EXT2_DIND_BLOCK]);
        ext2_inode_blocks_sub(fs, &inode, 1);
        inode.i_block[EXT2_DIND_BLOCK] = 0;
      }
      kfree(ind);
      kfree(dind);
    }
  }

  inode.i_size = (u32)length;
  node->inode->size = (usize)length;
  node->inode->mtime = vfs_get_unix_time();
  node->inode->ctime = node->inode->mtime;
  inode.i_mtime = node->inode->mtime;
  inode.i_ctime = node->inode->ctime;
  return ext2_write_inode(fs, inode_num, &inode);
}

static int ext2_add_dir_entry(struct ext2_fs *fs, u32 dir_inode_num, u32 inode_num,
                               const char *name, u8 type) {
	struct ext2_inode dir_inode;
  if (ext2_read_inode(fs, dir_inode_num, &dir_inode) < 0)
    return -1;
	
	usize name_len = strlen(name);
  if (name_len > 255)
    name_len = 255;
	
	u32 needed_len = 8 + ((name_len + 3) & ~3);
	u8 *dir_buf = kmalloc(fs->block_size);
  if (!dir_buf)
    return -1;
	
	u32 blocks = (dir_inode.i_size + fs->block_size - 1) / fs->block_size;
	if (blocks == 0) {
		u32 new_phys = ext2_alloc_block(fs);
    if (!new_phys) {
      kfree(dir_buf);
      return -1;
    }
		dir_inode.i_block[0] = new_phys;
		dir_inode.i_blocks += (fs->block_size / 512);
		dir_inode.i_size += fs->block_size;
		ext2_write_inode(fs, dir_inode_num, &dir_inode);
		
		memset(dir_buf, 0, fs->block_size);
		struct ext2_dir_entry *new_entry = (struct ext2_dir_entry *)dir_buf;
		new_entry->inode = inode_num;
		new_entry->rec_len = fs->block_size;
		new_entry->name_len = name_len;
		new_entry->file_type = type;
		memcpy(new_entry->name, name, name_len);
		ext2_write_block(fs, new_phys, dir_buf);
		kfree(dir_buf);
		return 0;
	}
	
	for (u32 b = 0; b < blocks; b++) {
		u32 phys_block = ext2_get_inode_block(fs, &dir_inode, b);
		ext2_read_block(fs, phys_block, dir_buf);
		
		usize offset = 0;
		while (offset < fs->block_size) {
      struct ext2_dir_entry *entry =
          (struct ext2_dir_entry *)(dir_buf + offset);
      if (entry->rec_len < 8 || offset + entry->rec_len > fs->block_size)
        break;
			
			u32 actual_len = 8 + ((entry->name_len + 3) & ~3);
			if (entry->rec_len >= actual_len + needed_len) {
				u32 original_rec_len = entry->rec_len;
				entry->rec_len = actual_len;
				
        struct ext2_dir_entry *new_entry =
            (struct ext2_dir_entry *)(dir_buf + offset + actual_len);
				new_entry->inode = inode_num;
				new_entry->rec_len = original_rec_len - actual_len;
				new_entry->name_len = name_len;
				new_entry->file_type = type;
				memcpy(new_entry->name, name, name_len);
				
				ext2_write_block(fs, phys_block, dir_buf);
				kfree(dir_buf);
				return 0;
			}
			offset += entry->rec_len;
		}
	}
	
	/* Use the indirect-aware set-block helper instead of writing
	 * i_block[blocks] directly: past 12 blocks a raw write clobbers the
	 * single/double-indirect pointer slots with a data block number, after
	 * which reads treat directory data as block pointers (arbitrary-block I/O).
	 * The helper allocates the block, places it (direct or indirect), updates
	 * i_blocks and writes the inode (R3-10). */
	u32 new_phys = ext2_allocate_block_for_inode(fs, dir_inode_num, &dir_inode, blocks);
	if (new_phys) {
		dir_inode.i_size += fs->block_size;
		ext2_write_inode(fs, dir_inode_num, &dir_inode);

		memset(dir_buf, 0, fs->block_size);
		struct ext2_dir_entry *new_entry = (struct ext2_dir_entry *)dir_buf;
		new_entry->inode = inode_num;
		new_entry->rec_len = fs->block_size;
		new_entry->name_len = name_len;
		new_entry->file_type = type;
		memcpy(new_entry->name, name, name_len);
		ext2_write_block(fs, new_phys, dir_buf);
		kfree(dir_buf);
		return 0;
	}
	
	kfree(dir_buf);
	return -1;
}

static u32 ext2_find_dir_entry(struct ext2_fs *fs, u32 dir_inode_num, const char *name, u8 *out_type) {
  struct ext2_inode dir_inode;
  if (ext2_read_inode(fs, dir_inode_num, &dir_inode) < 0) return 0;
  
  u8 *dir_buf = kmalloc(fs->block_size);
  if (!dir_buf) return 0;
  u32 blocks = (dir_inode.i_size + fs->block_size - 1) / fs->block_size;
  for (u32 b = 0; b < blocks; b++) {
    u32 phys_block = ext2_get_inode_block(fs, &dir_inode, b);
    if (!phys_block) continue;
    ext2_read_block(fs, phys_block, dir_buf);
    usize offset = 0;
    while (offset < fs->block_size) {
      struct ext2_dir_entry *entry = (struct ext2_dir_entry *)(dir_buf + offset);
      if (entry->rec_len < 8 || offset + entry->rec_len > fs->block_size) break;
      if (entry->inode != 0 && strlen(name) == entry->name_len &&
          memcmp(entry->name, name, entry->name_len) == 0) {
        u32 ino = entry->inode;
        if (out_type) *out_type = entry->file_type;
        kfree(dir_buf);
        return ino;
      }
      offset += entry->rec_len;
    }
  }
  kfree(dir_buf);
  return 0;
}

static int ext2_is_dir_empty(struct ext2_fs *fs, u32 inode_num) {
    struct ext2_inode inode;
    if (ext2_read_inode(fs, inode_num, &inode) < 0) return -EIO;
    
    u8 *buf = kmalloc(fs->block_size);
    if (!buf) return -ENOMEM;
    u32 blocks = (inode.i_size + fs->block_size - 1) / fs->block_size;
    for (u32 b = 0; b < blocks; b++) {
        u32 phys = ext2_get_inode_block(fs, &inode, b);
        if (!phys) continue;
        ext2_read_block(fs, phys, buf);
        usize offset = 0;
        while (offset < fs->block_size) {
            struct ext2_dir_entry *entry = (struct ext2_dir_entry *)(buf + offset);
            if (entry->rec_len < 8 || offset + entry->rec_len > fs->block_size) break;
            if (entry->inode != 0) {
                if (entry->name_len > 2 || (entry->name_len == 1 && entry->name[0] != '.') ||
                    (entry->name_len == 2 && (entry->name[0] != '.' || entry->name[1] != '.'))) {
                    kfree(buf);
                    return 0; // Not empty
                }
            }
            offset += entry->rec_len;
        }
    }
    kfree(buf);
    return 1; // Empty
}

static int ext2_remove_dir_entry(struct ext2_fs *fs, u32 dir_inode_num, const char *name) {
  struct ext2_inode dir_inode;
  if (ext2_read_inode(fs, dir_inode_num, &dir_inode) < 0) return -1;
  
  u8 *dir_buf = kmalloc(fs->block_size);
  if (!dir_buf) return -ENOMEM;
  
  u32 blocks = (dir_inode.i_size + fs->block_size - 1) / fs->block_size;
  for (u32 b = 0; b < blocks; b++) {
    u32 phys_block = ext2_get_inode_block(fs, &dir_inode, b);
    if (!phys_block) continue;
    ext2_read_block(fs, phys_block, dir_buf);
    
    usize offset = 0;
    struct ext2_dir_entry *prev_entry = 0;
    while (offset < fs->block_size) {
      struct ext2_dir_entry *entry = (struct ext2_dir_entry *)(dir_buf + offset);
      if (entry->rec_len < 8 || offset + entry->rec_len > fs->block_size) break;

      if (entry->inode != 0 && strlen(name) == entry->name_len &&
          memcmp(entry->name, name, entry->name_len) == 0) {
        if (prev_entry) {
          prev_entry->rec_len += entry->rec_len;
        } else {
          entry->inode = 0;
        }
        ext2_write_block(fs, phys_block, dir_buf);
        kfree(dir_buf);
        return 0;
      }
      
      offset += entry->rec_len;
      prev_entry = entry;
    }
  }
  
  kfree(dir_buf);
  return -ENOENT;
}

/* mknod: a FIFO is an ordinary ext2 inode with S_IFIFO in i_mode and no data
 * blocks — the pipe buffer is in RAM, only the name, mode and ownership are
 * persistent. Only S_IFIFO is accepted (see ext4_vfs_mknod). */
static int ext2_vfs_mknod(struct vfs_node *dir, const char *name, u32 mode) {
  u32 dir_inode_num = get_ino(dir);
  struct ext2_fs *fs = get_fs(dir);

  u32 new_inode_num = ext2_alloc_inode(fs);
  if (!new_inode_num)
    return -ENOSPC;

  struct ext2_inode new_inode;
  memset(&new_inode, 0, sizeof(new_inode));
  new_inode.i_mode = EXT2_S_IFIFO | (mode & 07777);
  new_inode.i_links_count = 1;
  ext2_write_inode(fs, new_inode_num, &new_inode);

  if (ext2_add_dir_entry(fs, dir_inode_num, new_inode_num, name,
                         EXT2_FT_FIFO) < 0)
    return -EIO;

  struct vfs_node *node = find_child(dir, name);
  if (node) {
    node->inode->blk_dev = dir->inode->blk_dev;
    node->inode->setattr_cb = ext2_vfs_setattr;
    struct ext2_inode_info *info = kmalloc(sizeof(struct ext2_inode_info));
    if (info) {
      info->fs = fs;
      info->inode_num = new_inode_num;
      node->inode->ino = new_inode_num; /* st_ino == d_ino */
      node->inode->data = info;
    }
    vfs_node_put(node);
  }
  return 0;
}

static int ext2_vfs_create(struct vfs_node *dir, const char *name,
                           const char *full_path, u32 mode) {
	(void)full_path; /* part of the vfs create-op signature; this impl uses name+dir */
	u32 dir_inode_num = get_ino(dir);
  struct ext2_fs *fs = get_fs(dir);
	
	u32 new_inode_num = ext2_alloc_inode(fs);
  if (!new_inode_num)
    return -ENOSPC;
	
	struct ext2_inode new_inode;
	memset(&new_inode, 0, sizeof(new_inode));
	new_inode.i_mode = EXT2_S_IFREG | (mode & 07777);
	new_inode.i_links_count = 1;
	ext2_write_inode(fs, new_inode_num, &new_inode);
	
  if (ext2_add_dir_entry(fs, dir_inode_num, new_inode_num, name, EXT2_FT_REG_FILE) <
      0) {
		return -1;
	}
	
  struct vfs_node *node = find_child(dir, name);
	if (node) {
		node->inode->blk_dev = dir->inode->blk_dev;
		node->inode->read_cb = ext2_vfs_read;
		node->inode->write_cb = ext2_vfs_write;
    node->inode->truncate_cb = ext2_vfs_truncate;
    node->inode->setattr_cb = ext2_vfs_setattr;
    node->inode->fsync_cb = ext2_vfs_fsync;
    
    struct ext2_inode_info *info = kmalloc(sizeof(struct ext2_inode_info));
    info->fs = fs;
    info->inode_num = new_inode_num;
    node->inode->ino = new_inode_num; /* st_ino == d_ino */
    node->inode->data = info;
    vfs_node_put(node);
	}
	
	return 0;
}

static int ext2_vfs_unlink(struct vfs_node *dir, const char *name) {
  u32 dir_inode_num = get_ino(dir);
  struct ext2_fs *fs = get_fs(dir);
  struct ext2_inode dir_inode;
  if (ext2_read_inode(fs, dir_inode_num, &dir_inode) < 0) return -EIO;

  u32 inode_num = ext2_find_dir_entry(fs, dir_inode_num, name, NULL);
  if (!inode_num) return -ENOENT;

  /* Remove directory entry */
  if (ext2_remove_dir_entry(fs, dir_inode_num, name) < 0) return -EIO;

  /* Update target inode link count */
  struct ext2_inode inode;
  if (ext2_read_inode(fs, inode_num, &inode) == 0) {
    if (inode.i_links_count > 0) {
      inode.i_links_count--;
      ext2_write_inode(fs, inode_num, &inode);
    }
  }
  return 0;
}

static void ext2_vfs_release(struct vfs_node *node) {
  u32 inode_num = get_ino(node);
  struct ext2_fs *fs = get_fs(node);
  struct ext2_inode inode;
  if (ext2_read_inode(fs, inode_num, &inode) == 0) {
    if (inode.i_links_count == 0) {
      /* Free blocks */
      u32 total_blocks = (inode.i_size + fs->block_size - 1) / fs->block_size;
      for (u32 b = 0; b < total_blocks; b++) {
        u32 phys = ext2_get_inode_block(fs, &inode, b);
        if (phys) ext2_free_block(fs, phys);
      }
      if (inode.i_block[EXT2_IND_BLOCK]) {
        ext2_free_block(fs, inode.i_block[EXT2_IND_BLOCK]);
      }
      if (inode.i_block[EXT2_DIND_BLOCK]) {
        u32 ptrs = fs->block_size / 4;
        u32 *dind_buf = kmalloc(fs->block_size);
        if (dind_buf) {
          if (ext2_read_block(fs, inode.i_block[EXT2_DIND_BLOCK], dind_buf) == 0) {
            for (u32 i = 0; i < ptrs; i++) {
              if (dind_buf[i]) {
                ext2_free_block(fs, dind_buf[i]);
              }
            }
          }
          kfree(dind_buf);
        }
        ext2_free_block(fs, inode.i_block[EXT2_DIND_BLOCK]);
      }
      if (inode.i_file_acl) {
        ext2_free_block(fs, inode.i_file_acl);
      }
      ext2_free_inode(fs, inode_num);
    }
  }
}

static int ext2_vfs_setattr(struct vfs_node *node) {
  u32 inode_num = get_ino(node);
  struct ext2_fs *fs = get_fs(node);
  struct ext2_inode inode;
  if (ext2_read_inode(fs, inode_num, &inode) < 0) return -EIO;
  
  node->inode->ctime = vfs_get_unix_time();
  inode.i_mode = (inode.i_mode & ~07777) | (node->inode->mode & 07777);
  inode.i_uid = node->inode->uid;
  inode.i_gid = node->inode->gid;
  inode.i_size = (u32)node->inode->size;
  inode.i_atime = node->inode->atime;
  inode.i_mtime = node->inode->mtime;
  inode.i_ctime = node->inode->ctime;
  inode.i_links_count = node->inode->nlink;
  
  ext2_save_acls(fs, inode_num, &inode, node->inode);
  
  return ext2_write_inode(fs, inode_num, &inode);
}

static int ext2_vfs_symlink(struct vfs_node *dir, const char *name, const char *target) {
  struct ext2_fs *fs = get_fs(dir);
  u32 new_inode_num = ext2_alloc_inode(fs);
  if (!new_inode_num) return -ENOSPC;

  struct ext2_inode inode;
  memset(&inode, 0, sizeof(inode));
  inode.i_mode = EXT2_S_IFLNK | 0777;
  inode.i_links_count = 1;
  inode.i_size = strlen(target);
  inode.i_atime = inode.i_mtime = inode.i_ctime = vfs_get_unix_time();

  if (inode.i_size < 60) {
    memcpy(inode.i_block, target, inode.i_size);
  } else {
    u32 block = ext2_alloc_block(fs);
    if (!block) {
      ext2_free_inode(fs, new_inode_num);
      return -ENOSPC;
    }
    inode.i_block[0] = block;
    ext2_write_block(fs, block, target);
  }
  
  ext2_write_inode(fs, new_inode_num, &inode);
  return ext2_add_dir_entry(fs, get_ino(dir), new_inode_num, name, EXT2_FT_SYMLINK);
}

static int ext2_vfs_link(struct vfs_node *target_node, struct vfs_node *dir, const char *name) {
  u32 inode_num = get_ino(target_node);
  struct ext2_fs *fs = get_fs(dir);
  struct ext2_inode inode;
  if (ext2_read_inode(fs, inode_num, &inode) < 0) return -EIO;
  
  inode.i_links_count++;
  ext2_write_inode(fs, inode_num, &inode);
  
  u8 type = (target_node->inode->type == VFS_DIRECTORY) ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
  return ext2_add_dir_entry(fs, get_ino(dir), inode_num, name, type);
}

static int ext2_vfs_statfs(struct vfs_node *node, struct b1nix_statfs *st);

void ext2_sync_inode(struct ext2_fs *fs, u32 inode_num) {
  (void)inode_num;
  /* In this driver, ext2_write_inode uses blk_write_cached, which marks the block dirty.
     To ensure durability, we just need to flush the block cache for this device. */
  blk_cache_flush(fs->bdev);
}

void ext2_sync_bitmaps(struct ext2_fs *fs) {
  /* Write the superblock and then flush all cached blocks for this device */
  ext2_write_superblock(fs);
  /* In a more optimized version, we'd also track which BGD blocks are dirty.
     But here ext2_write_bgd also uses blk_write_cached. */
  blk_cache_flush(fs->bdev);
}

void ext2_sync_all_fs(void) {
  struct ext2_fs *fs = ext2_fs_list;
  while (fs) {
    if (fs->bitmaps_dirty) {
      ext2_write_superblock(fs);
      fs->bitmaps_dirty = 0;
    }
    fs = fs->next;
  }
}

static int ext2_vfs_fsync(struct vfs_node *node) {
    if (!node || !node->inode || !node->inode->data) return -EINVAL;
    
    struct ext2_inode_info *info = (struct ext2_inode_info *)node->inode->data;
    struct ext2_fs *fs = info->fs;
    
    /* 1. Sync the inode structure itself (size, timestamps, mode) */
    ext2_vfs_setattr(node);
    
    /* 2. Sync superblock and bitmaps if allocations happened */
    if (fs->bitmaps_dirty) {
        ext2_write_superblock(fs);
        fs->bitmaps_dirty = 0;
    }
    
    /* 3. Force flush all dirty blocks for this device to physical storage */
    blk_cache_flush(node->inode->blk_dev);
    
    return 0;
}

static int ext2_vfs_statfs(struct vfs_node *node, struct b1nix_statfs *st) {
  struct ext2_fs *fs = get_fs(node);
  if (!fs) return -EINVAL;
  memset(st, 0, sizeof(*st));
  st->f_type = EXT2_SUPER_MAGIC;
  st->f_bsize = fs->block_size;
  st->f_blocks = fs->sb.s_blocks_count;
  st->f_bfree = fs->sb.s_free_blocks_count;
  st->f_bavail = fs->sb.s_free_blocks_count;
  st->f_files = fs->sb.s_inodes_count;
  st->f_ffree = fs->sb.s_free_inodes_count;
  st->f_namelen = 255;
  return 0;
}

static int ext2_vfs_rmdir(struct vfs_node *dir, const char *name) {
  struct ext2_fs *fs = get_fs(dir);
  u32 dir_inode_num = get_ino(dir);
  
  u8 type = 0;
  u32 target_inode_num = ext2_find_dir_entry(fs, dir_inode_num, name, &type);
  if (!target_inode_num) return -ENOENT;
  if (type != EXT2_FT_DIR) return -ENOTDIR;

  if (!ext2_is_dir_empty(fs, target_inode_num)) return -ENOTEMPTY;

  int err = ext2_vfs_unlink(dir, name);
  if (err == 0) {
    struct ext2_inode dir_inode;
    if (ext2_read_inode(fs, dir_inode_num, &dir_inode) == 0) {
      if (dir_inode.i_links_count > 2) {
        dir_inode.i_links_count--;
        ext2_write_inode(fs, dir_inode_num, &dir_inode);
      }
    }
  }
  return err;
}

static int ext2_vfs_rename(struct vfs_node *old_dir, const char *old_name,
                            struct vfs_node *new_dir, const char *new_name) {
  u32 old_dir_inode_num = get_ino(old_dir);
  u32 new_dir_inode_num = get_ino(new_dir);
  struct ext2_fs *fs = get_fs(old_dir);
  
  u8 type = 0;
  u32 inode_num = ext2_find_dir_entry(fs, old_dir_inode_num, old_name, &type);
  if (!inode_num) return -ENOENT;

  /* Check if target exists */
  u8 target_type = 0;
  u32 target_ino = ext2_find_dir_entry(fs, new_dir_inode_num, new_name, &target_type);
  if (target_ino) {
      if (target_type == EXT2_FT_DIR) {
          if (!ext2_is_dir_empty(fs, target_ino)) return -ENOTEMPTY;
          ext2_vfs_rmdir(new_dir, new_name);
      } else {
          ext2_vfs_unlink(new_dir, new_name);
      }
  }
 
  if (ext2_add_dir_entry(fs, new_dir_inode_num, inode_num, new_name, type) < 0)
    return -EIO;

  ext2_remove_dir_entry(fs, old_dir_inode_num, old_name);

  if (type == EXT2_FT_DIR && old_dir_inode_num != new_dir_inode_num) {
    /* Directory moved to a different parent: rewrite its ".." entry and move
     * the back-link between the parents' link counts (see ext4_vfs_rename). */
    struct ext2_inode mdi;
    if (ext2_read_inode(fs, inode_num, &mdi) == 0) {
      u8 *db = kmalloc(fs->block_size);
      u32 mblocks = (mdi.i_size + fs->block_size - 1) / fs->block_size;
      int rewrote = 0;
      for (u32 b = 0; b < mblocks && !rewrote && db; b++) {
        u32 phys = ext2_get_inode_block(fs, &mdi, b);
        if (!phys) continue;
        ext2_read_block(fs, phys, db);
        usize off = 0;
        while (off < fs->block_size) {
          struct ext2_dir_entry *e = (struct ext2_dir_entry *)(db + off);
          if (e->rec_len < 8 || off + e->rec_len > fs->block_size) break;
          if (e->inode != 0 && e->name_len == 2 && e->name[0] == '.' &&
              e->name[1] == '.') {
            e->inode = new_dir_inode_num;
            ext2_write_block(fs, phys, db);
            rewrote = 1;
            break;
          }
          off += e->rec_len;
        }
      }
      if (db) kfree(db);
    }
    struct ext2_inode pdi;
    if (ext2_read_inode(fs, old_dir_inode_num, &pdi) == 0 &&
        pdi.i_links_count > 2) {
      pdi.i_links_count--;
      ext2_write_inode(fs, old_dir_inode_num, &pdi);
    }
    if (ext2_read_inode(fs, new_dir_inode_num, &pdi) == 0) {
      pdi.i_links_count++;
      ext2_write_inode(fs, new_dir_inode_num, &pdi);
    }
  }
  return 0;
}

static isize ext2_vfs_readdir(struct vfs_node *dir, usize offset, struct dirent *buf, usize max_entries) {
    u32 inode_num = get_ino(dir);
    struct ext2_fs *fs = get_fs(dir);
    struct ext2_inode inode;
    if (ext2_read_inode(fs, inode_num, &inode) < 0) return -EIO;

    u8 *dir_buf = kmalloc(fs->block_size);
    usize count = 0;
    usize entry_idx = 0;
    u32 blocks = (inode.i_size + fs->block_size - 1) / fs->block_size;

    for (u32 b = 0; b < blocks && count < max_entries; b++) {
        u32 phys = ext2_get_inode_block(fs, &inode, b);
        if (!phys) continue;
        ext2_read_block(fs, phys, dir_buf);
        usize off = 0;
        while (off < fs->block_size && count < max_entries) {
            struct ext2_dir_entry *e = (struct ext2_dir_entry *)(dir_buf + off);
            if (e->rec_len < 8 || off + e->rec_len > fs->block_size) break;
            if (e->inode != 0) {
                if (entry_idx >= offset) {
                    usize name_len = e->name_len > 63 ? 63 : e->name_len;
                    memcpy(buf[count].name, e->name, name_len);
                    buf[count].name[name_len] = '\0';
                    buf[count].type = (u32)VFS_FILE; // Default
                    if (e->file_type == EXT2_FT_DIR) buf[count].type = (u32)VFS_DIRECTORY;
                    buf[count].is_dir = (e->file_type == EXT2_FT_DIR);
                    buf[count].size = 0; // We don't easily know child size here
                    buf[count].ino = e->inode;
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

static int ext2_vfs_mkdir(struct vfs_node *dir, const char *name, u32 mode) {
    u32 dir_inode_num = get_ino(dir);
    struct ext2_fs *fs = get_fs(dir);
    u32 new_inode_num = ext2_alloc_inode(fs);
    if (!new_inode_num) return -ENOSPC;

    struct ext2_inode new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.i_mode = EXT2_S_IFDIR | (mode & 07777);
    new_inode.i_links_count = 2; // . and parent
    ext2_write_inode(fs, new_inode_num, &new_inode);

    if (ext2_add_dir_entry(fs, dir_inode_num, new_inode_num, name, EXT2_FT_DIR) < 0) {
        ext2_free_inode(fs, new_inode_num);
        return -1;
    }

    /* Add . and .. entries */
    ext2_add_dir_entry(fs, new_inode_num, new_inode_num, ".", EXT2_FT_DIR);
    ext2_add_dir_entry(fs, new_inode_num, dir_inode_num, "..", EXT2_FT_DIR);

    /* Wire the freshly-linked VFS node to its on-disk inode, exactly like
     * ext2_vfs_create does for files. Without this the node had NO
     * ext2_inode_info (get_ino() garbage) and NO readdir_cb: every child
     * created inside a runtime-mkdir'd directory went to a bogus on-disk
     * inode (memory-only in practice), readdir through a re-looked-up node
     * showed the dir empty, and rm -rf then failed ENOTEMPTY against the
     * in-memory child it could never enumerate (the M22 cp -r regression). */
    struct vfs_node *node = find_child(dir, name);
    if (node) {
        struct ext2_inode_info *info = kmalloc(sizeof(struct ext2_inode_info));
        if (info) {
            info->fs = fs;
            info->inode_num = new_inode_num;
            node->inode->data = info;
        }
        node->inode->ino = new_inode_num; /* st_ino == d_ino */
        node->inode->blk_dev = dir->inode->blk_dev;
        node->inode->create_cb = ext2_vfs_create;
        node->inode->mknod_cb = ext2_vfs_mknod;
        node->inode->mkdir_cb = ext2_vfs_mkdir;
        node->inode->unlink_cb = ext2_vfs_unlink;
        node->inode->rmdir_cb = ext2_vfs_rmdir;
        node->inode->rename_cb = ext2_vfs_rename;
        node->inode->symlink_cb = ext2_vfs_symlink;
        node->inode->link_cb = ext2_vfs_link;
        node->inode->release_cb = ext2_vfs_release;
        node->inode->setattr_cb = ext2_vfs_setattr;
        node->inode->statfs_cb = ext2_vfs_statfs;
        node->inode->readdir_cb = ext2_vfs_readdir;
        node->inode->fsync_cb = ext2_vfs_fsync;
        vfs_node_put(node);
    }

    return 0;
}

static void ext2_populate_vfs(struct ext2_fs *fs, u32 inode_num, const char *base_path) {
	struct ext2_inode inode;
  if (ext2_read_inode(fs, inode_num, &inode) < 0)
    return;

  if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
    return;

	u8 *dir_buf = kmalloc(inode.i_size);
  if (!dir_buf)
    return;
	
  for (u32 i = 0; i < (inode.i_size + fs->block_size - 1) / fs->block_size;
       i++) {
		u32 phys_block = ext2_get_inode_block(fs, &inode, i);
		if (phys_block) {
			ext2_read_block(fs, phys_block, dir_buf + i * fs->block_size);
		}
	}
	
	usize offset = 0;
	while (offset < inode.i_size) {
		struct ext2_dir_entry *entry = (struct ext2_dir_entry *)(dir_buf + offset);
    if (entry->rec_len < 8 || offset + entry->rec_len > inode.i_size)
      break;
		
		if (entry->inode != 0) {
			char name[256];
			memcpy(name, entry->name, entry->name_len);
			name[entry->name_len] = '\0';
			
			if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
				char full_path[256];
				usize len = strlen(base_path);
				memcpy(full_path, base_path, len);
        if (full_path[len - 1] != '/')
					full_path[len++] = '/';
				memcpy(full_path + len, name, entry->name_len + 1);
				
				struct ext2_inode child_inode;
				if (ext2_read_inode(fs, entry->inode, &child_inode) == 0) {
					if ((child_inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
            struct vfs_node *dir_node = vfs_add_node(
                full_path, VFS_DIRECTORY, 0, 0, 0);
            if (dir_node) {
              struct ext2_inode_info *info = kmalloc(sizeof(struct ext2_inode_info));
              info->fs = fs;
              info->inode_num = entry->inode;
              dir_node->inode->data = info;
              dir_node->inode->ino = entry->inode; /* st_ino == d_ino */
              dir_node->inode->blk_dev = fs->bdev;
              dir_node->inode->mode = child_inode.i_mode & 0xFFFF;
              dir_node->inode->uid = child_inode.i_uid;
              dir_node->inode->gid = child_inode.i_gid;
              dir_node->inode->atime = child_inode.i_atime;
              dir_node->inode->mtime = child_inode.i_mtime;
              dir_node->inode->ctime = child_inode.i_ctime;
              dir_node->inode->nlink = child_inode.i_links_count;
              dir_node->inode->fs_id = dir_node->parent->inode->fs_id;
              dir_node->inode->create_cb = ext2_vfs_create;
              dir_node->inode->mknod_cb = ext2_vfs_mknod;
              dir_node->inode->mkdir_cb = ext2_vfs_mkdir;
              dir_node->inode->unlink_cb = ext2_vfs_unlink;
              dir_node->inode->rmdir_cb = ext2_vfs_rmdir;
              dir_node->inode->rename_cb = ext2_vfs_rename;
              dir_node->inode->symlink_cb = ext2_vfs_symlink;
              dir_node->inode->link_cb = ext2_vfs_link;
              dir_node->inode->release_cb = ext2_vfs_release;
              dir_node->inode->setattr_cb = ext2_vfs_setattr;
              dir_node->inode->statfs_cb = ext2_vfs_statfs;
              dir_node->inode->readdir_cb = ext2_vfs_readdir;
              dir_node->inode->fsync_cb = ext2_vfs_fsync;
              ext2_load_acls(fs, &child_inode, dir_node->inode);
            }
						ext2_populate_vfs(fs, entry->inode, full_path);
					} else {
            enum vfs_node_type ntype =
                ((child_inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFIFO)
                    ? VFS_FIFO
                    : VFS_FILE;
            struct vfs_node *node =
                vfs_add_node(full_path, ntype, 0,
                             child_inode.i_size, 0);
						if (node) {
              struct ext2_inode_info *info = kmalloc(sizeof(struct ext2_inode_info));
              info->fs = fs;
              info->inode_num = entry->inode;
              node->inode->data = info;
              node->inode->ino = entry->inode; /* st_ino == d_ino */
							node->inode->blk_dev = fs->bdev;
							node->inode->read_cb = ext2_vfs_read;
							node->inode->write_cb = ext2_vfs_write;
              node->inode->truncate_cb = ext2_vfs_truncate;
              node->inode->mode = child_inode.i_mode & 0xFFFF;
              node->inode->uid = child_inode.i_uid;
              node->inode->gid = child_inode.i_gid;
              node->inode->atime = child_inode.i_atime;
              node->inode->mtime = child_inode.i_mtime;
              node->inode->ctime = child_inode.i_ctime;
              node->inode->nlink = child_inode.i_links_count;
              node->inode->fs_id = node->parent->inode->fs_id;
              node->inode->release_cb = ext2_vfs_release;
              node->inode->setattr_cb = ext2_vfs_setattr;
              node->inode->fsync_cb = ext2_vfs_fsync;
              ext2_load_acls(fs, &child_inode, node->inode);
						}
					}
				}
			}
		}
		offset += entry->rec_len;
	}
	
	kfree(dir_buf);
}

static struct vfs_node *ext2_vfs_mount_cb(const char *source, u64 flags, void *data) {
  (void)flags; (void)data;
  struct block_device *dev = blk_get(source);
  if (!dev) return ERR_PTR(-ENODEV);
  
  u8 *sb_buffer = kmalloc(1024);
  if (blk_read_cached(dev, 2, 2, sb_buffer) < 0) {
    kfree(sb_buffer);
    return ERR_PTR(-EIO);
  }
  
  struct ext2_fs *fs = kmalloc(sizeof(struct ext2_fs));
  memset(fs, 0, sizeof(struct ext2_fs));
  fs->bdev = dev;
  memcpy(&fs->sb, sb_buffer, sizeof(struct ext2_superblock));
  kfree(sb_buffer);
  
  if (fs->sb.s_magic != EXT2_SUPER_MAGIC) {
    kfree(fs);
    return ERR_PTR(-EINVAL);
  }
  
  fs->block_size = 1024 << fs->sb.s_log_block_size;
  fs->inodes_per_group = fs->sb.s_inodes_per_group;
  fs->inode_size = (fs->sb.s_rev_level == 0) ? 128 : fs->sb.s_inode_size;
  
  /* Add to global list */
  fs->next = ext2_fs_list;
  ext2_fs_list = fs;
  
  struct vfs_node *root = vfs_create_node(VFS_DIRECTORY);
  if (!root) return ERR_PTR(-ENOMEM);
  
  struct ext2_inode_info *info = kmalloc(sizeof(struct ext2_inode_info));
  info->fs = fs;
  info->inode_num = 2;
  root->inode->data = info;
  root->inode->blk_dev = dev;
  root->inode->create_cb = ext2_vfs_create;
  root->inode->mknod_cb = ext2_vfs_mknod;
  root->inode->mkdir_cb = ext2_vfs_mkdir;
  root->inode->unlink_cb = ext2_vfs_unlink;
  root->inode->rmdir_cb = ext2_vfs_rmdir;
  root->inode->rename_cb = ext2_vfs_rename;
  root->inode->symlink_cb = ext2_vfs_symlink;
  root->inode->link_cb = ext2_vfs_link;
  root->inode->release_cb = ext2_vfs_release;
  root->inode->truncate_cb = ext2_vfs_truncate;
  root->inode->setattr_cb = ext2_vfs_setattr;
  root->inode->statfs_cb = ext2_vfs_statfs;
  root->inode->readdir_cb = ext2_vfs_readdir;
  root->inode->fsync_cb = ext2_vfs_fsync;
  
  struct ext2_inode ri;
  if (ext2_read_inode(fs, 2, &ri) == 0) {
      root->inode->mode = ri.i_mode & 07777;
      root->inode->uid = ri.i_uid;
      root->inode->gid = ri.i_gid;
      ext2_load_acls(fs, &ri, root->inode);
  }
  
  vfs_set_currently_mounting_root(root);
  if (data) {
    ext2_populate_vfs(fs, 2, (const char *)data);
  }
  return root;
}

static struct vfs_fs ext2_fs = {
  .name = "ext2",
  .mount = ext2_vfs_mount_cb,
};

int ext2_mount_root(const char *device_name, const char *mount_point) {
    vfs_mount(device_name, mount_point, "ext2", 0);
	return 0;
}

void ext2_init(void) {
  vfs_register_fs(&ext2_fs);
  /* The vfs_mount calls will trigger ext2_vfs_mount_cb which handles allocation */
  if (vfs_mount("virtio-blk0", "/", "ext2", 0) == 0) return;
  if (vfs_mount("virtio-blk1", "/mnt/ext2", "ext2", 0) == 0) return;
}
