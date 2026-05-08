#include <b1nix/ext2.h>
#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <string.h>

static struct block_device *ext2_dev = 0;
static struct ext2_superblock ext2_sb;
static u32 ext2_block_size;
static u32 ext2_inodes_per_group;
static u32 ext2_inode_size;

static int ext2_read_block(struct block_device *dev, u32 block, void *buffer) {
	if (!dev) return -1;
	u64 lba = (u64)block * (ext2_block_size / 512);
	return blk_read_cached(dev, lba, ext2_block_size / 512, buffer);
}

static int ext2_write_block(struct block_device *dev, u32 block, const void *buffer) {
	if (!dev) return -1;
	u64 lba = (u64)block * (ext2_block_size / 512);
	return blk_write_cached(dev, lba, ext2_block_size / 512, buffer);
}

static void ext2_read_bgd(struct block_device *dev, u32 group, struct ext2_block_group_desc *bgd) {
	u32 bg_desc_block = (ext2_block_size == 1024) ? 2 : 1;
	u32 bg_offset = group * sizeof(struct ext2_block_group_desc);
	u32 bg_block = bg_desc_block + (bg_offset / ext2_block_size);
	
	u8 *bg_buffer = kmalloc(ext2_block_size);
	ext2_read_block(dev, bg_block, bg_buffer);
  memcpy(bgd, bg_buffer + (bg_offset % ext2_block_size),
         sizeof(struct ext2_block_group_desc));
	kfree(bg_buffer);
}

static void ext2_write_bgd(struct block_device *dev, u32 group, struct ext2_block_group_desc *bgd) {
	u32 bg_desc_block = (ext2_block_size == 1024) ? 2 : 1;
	u32 bg_offset = group * sizeof(struct ext2_block_group_desc);
	u32 bg_block = bg_desc_block + (bg_offset / ext2_block_size);
	
	u8 *bg_buffer = kmalloc(ext2_block_size);
	ext2_read_block(dev, bg_block, bg_buffer);
  memcpy(bg_buffer + (bg_offset % ext2_block_size), bgd,
         sizeof(struct ext2_block_group_desc));
	ext2_write_block(dev, bg_block, bg_buffer);
	kfree(bg_buffer);
}

static void ext2_write_superblock(void) {
	u8 *sb_buffer = kmalloc(1024);
	if (blk_read_cached(ext2_dev, 2, 2, sb_buffer) >= 0) {
		memcpy(sb_buffer, &ext2_sb, sizeof(struct ext2_superblock));
		blk_write_cached(ext2_dev, 2, 2, sb_buffer);
	}
	kfree(sb_buffer);
}

static int ext2_read_inode(struct block_device *dev, u32 inode_num, struct ext2_inode *inode) {
  if (inode_num == 0)
    return -1;
	
	u32 group = (inode_num - 1) / ext2_inodes_per_group;
	u32 index = (inode_num - 1) % ext2_inodes_per_group;
	
	struct ext2_block_group_desc bgd;
	ext2_read_bgd(dev, group, &bgd);
	u32 inode_table_block = bgd.bg_inode_table;
	
	u32 inode_offset = index * ext2_inode_size;
	u32 block_idx = inode_table_block + (inode_offset / ext2_block_size);
	
	u8 *inode_buffer = kmalloc(ext2_block_size);
	if (ext2_read_block(dev, block_idx, inode_buffer) < 0) {
		kfree(inode_buffer);
		return -1;
	}
	
  memcpy(inode, inode_buffer + (inode_offset % ext2_block_size),
         sizeof(struct ext2_inode));
	kfree(inode_buffer);
	return 0;
}

static int ext2_write_inode(struct block_device *dev, u32 inode_num, const struct ext2_inode *inode) {
  if (inode_num == 0)
    return -1;
	
	u32 group = (inode_num - 1) / ext2_inodes_per_group;
	u32 index = (inode_num - 1) % ext2_inodes_per_group;
	
	struct ext2_block_group_desc bgd;
	ext2_read_bgd(dev, group, &bgd);
	u32 inode_table_block = bgd.bg_inode_table;
	
	u32 inode_offset = index * ext2_inode_size;
	u32 block_idx = inode_table_block + (inode_offset / ext2_block_size);
	
	u8 *inode_buffer = kmalloc(ext2_block_size);
	if (ext2_read_block(dev, block_idx, inode_buffer) < 0) {
		kfree(inode_buffer);
		return -1;
	}
	
  memcpy(inode_buffer + (inode_offset % ext2_block_size), inode,
         sizeof(struct ext2_inode));
	int ret = ext2_write_block(dev, block_idx, inode_buffer);
	kfree(inode_buffer);
	return ret;
}

static u32 ext2_alloc_block(struct block_device *dev) {
  u32 groups = (ext2_sb.s_blocks_count + ext2_sb.s_blocks_per_group - 1) /
               ext2_sb.s_blocks_per_group;
	for (u32 g = 0; g < groups; g++) {
		struct ext2_block_group_desc bgd;
		ext2_read_bgd(dev, g, &bgd);
		if (bgd.bg_free_blocks_count > 0) {
			u8 *bitmap = kmalloc(ext2_block_size);
			if (ext2_read_block(dev, bgd.bg_block_bitmap, bitmap) < 0) {
				kfree(bitmap);
				continue;
			}
			for (u32 i = 0; i < ext2_sb.s_blocks_per_group; i++) {
				if (!(bitmap[i / 8] & (1 << (i % 8)))) {
					bitmap[i / 8] |= (1 << (i % 8));
					ext2_write_block(dev, bgd.bg_block_bitmap, bitmap);
					kfree(bitmap);
					
					bgd.bg_free_blocks_count--;
					ext2_write_bgd(dev, g, &bgd);
					
					ext2_sb.s_free_blocks_count--;
					ext2_write_superblock();
					
					u32 block_num = g * ext2_sb.s_blocks_per_group + i +
					              (ext2_sb.s_log_block_size == 0 ? 1 : 0);
					
					u8 *zero_block = kmalloc(ext2_block_size);
					memset(zero_block, 0, ext2_block_size);
					ext2_write_block(dev, block_num, zero_block);
					kfree(zero_block);
					
					return block_num;
				}
			}
			kfree(bitmap);
		}
	}
	return 0;
}

static void ext2_free_block(struct block_device *dev, u32 block_num) {
  if (block_num == 0) return;
  u32 g = (block_num - (ext2_sb.s_log_block_size == 0 ? 1 : 0)) / ext2_sb.s_blocks_per_group;
  u32 i = (block_num - (ext2_sb.s_log_block_size == 0 ? 1 : 0)) % ext2_sb.s_blocks_per_group;

  struct ext2_block_group_desc bgd;
  ext2_read_bgd(dev, g, &bgd);
  u8 *bitmap = kmalloc(ext2_block_size);
  if (ext2_read_block(dev, bgd.bg_block_bitmap, bitmap) < 0) {
    kfree(bitmap);
    return;
  }
  bitmap[i / 8] &= ~(1 << (i % 8));
  ext2_write_block(dev, bgd.bg_block_bitmap, bitmap);
  kfree(bitmap);

  bgd.bg_free_blocks_count++;
  ext2_write_bgd(dev, g, &bgd);
  ext2_sb.s_free_blocks_count++;
  ext2_write_superblock();
}

static void ext2_free_inode(struct block_device *dev, u32 inode_num) {
  if (inode_num == 0) return;
  u32 g = (inode_num - 1) / ext2_inodes_per_group;
  u32 i = (inode_num - 1) % ext2_inodes_per_group;

  struct ext2_block_group_desc bgd;
  ext2_read_bgd(dev, g, &bgd);
  u8 *bitmap = kmalloc(ext2_block_size);
  if (ext2_read_block(dev, bgd.bg_inode_bitmap, bitmap) < 0) {
    kfree(bitmap);
    return;
  }
  bitmap[i / 8] &= ~(1 << (i % 8));
  ext2_write_block(dev, bgd.bg_inode_bitmap, bitmap);
  kfree(bitmap);

  bgd.bg_free_inodes_count++;
  ext2_write_bgd(dev, g, &bgd);
  ext2_sb.s_free_inodes_count++;
  ext2_write_superblock();
}

static u32 ext2_alloc_inode(struct block_device *dev) {
  u32 groups = (ext2_sb.s_inodes_count + ext2_inodes_per_group - 1) /
               ext2_inodes_per_group;
	for (u32 g = 0; g < groups; g++) {
		struct ext2_block_group_desc bgd;
		ext2_read_bgd(dev, g, &bgd);
		if (bgd.bg_free_inodes_count > 0) {
			u8 *bitmap = kmalloc(ext2_block_size);
			if (ext2_read_block(dev, bgd.bg_inode_bitmap, bitmap) < 0) {
				kfree(bitmap);
				continue;
			}
			for (u32 i = 0; i < ext2_inodes_per_group; i++) {
				if (!(bitmap[i / 8] & (1 << (i % 8)))) {
					bitmap[i / 8] |= (1 << (i % 8));
					ext2_write_block(dev, bgd.bg_inode_bitmap, bitmap);
					kfree(bitmap);
					
					bgd.bg_free_inodes_count--;
					ext2_write_bgd(dev, g, &bgd);
					
					ext2_sb.s_free_inodes_count--;
					ext2_write_superblock();
					
					u32 inode_num = g * ext2_inodes_per_group + i + 1;
					
					struct ext2_inode new_inode;
					memset(&new_inode, 0, sizeof(new_inode));
					ext2_write_inode(dev, inode_num, &new_inode);
					
					return inode_num;
				}
			}
			kfree(bitmap);
		}
	}
	return 0;
}

/* Read path with single and double-indirect block support. */
static u32 ext2_get_inode_block(struct block_device *dev, struct ext2_inode *inode, u32 block_idx) {
  u32 ptrs_per_block = ext2_block_size / 4;

	if (block_idx < EXT2_NDIR_BLOCKS) {
		return inode->i_block[block_idx];
	}
  block_idx -= EXT2_NDIR_BLOCKS;
	
  if (block_idx < ptrs_per_block) {
		u32 ind_block = inode->i_block[EXT2_IND_BLOCK];
    if (ind_block == 0)
      return 0;
		u32 *ind_buffer = kmalloc(ext2_block_size);
		if (ext2_read_block(dev, ind_block, ind_buffer) < 0) {
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

    u32 *ind_buffer = kmalloc(ext2_block_size);
    if (ext2_read_block(dev, dind_block, ind_buffer) < 0) {
      kfree(ind_buffer);
      return 0;
    }
    u32 ind1 = ind_buffer[block_idx / ptrs_per_block];
    if (ind1 == 0) {
      kfree(ind_buffer);
      return 0;
    }
    if (ext2_read_block(dev, ind1, ind_buffer) < 0) {
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
static u32 ext2_allocate_block_for_inode(struct block_device *dev, u32 inode_num,
                                         struct ext2_inode *inode, u32 b) {
  u32 ptrs_per_block = ext2_block_size / 4;
  u32 pblk = 0;

  if (b < EXT2_NDIR_BLOCKS) {
    if (inode->i_block[b] == 0) {
      pblk = ext2_alloc_block(dev);
      if (!pblk)
        return 0;
      inode->i_block[b] = pblk;
      inode->i_blocks += (ext2_block_size / 512);
      ext2_write_inode(dev, inode_num, inode);
    }
    return inode->i_block[b];
  }

  b -= EXT2_NDIR_BLOCKS;
  if (b < ptrs_per_block) {
    u32 ind_block = inode->i_block[EXT2_IND_BLOCK];
    if (ind_block == 0) {
      ind_block = ext2_alloc_block(dev);
      if (!ind_block)
        return 0;
      inode->i_block[EXT2_IND_BLOCK] = ind_block;
      inode->i_blocks += (ext2_block_size / 512);
      ext2_write_inode(dev, inode_num, inode);
    }

    u32 *ind_buffer = kmalloc(ext2_block_size);
    ext2_read_block(dev, ind_block, ind_buffer);
    if (ind_buffer[b] == 0) {
      pblk = ext2_alloc_block(dev);
      if (pblk) {
        ind_buffer[b] = pblk;
        ext2_write_block(dev, ind_block, ind_buffer);
        inode->i_blocks += (ext2_block_size / 512);
        ext2_write_inode(dev, inode_num, inode);
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
      dind_block = ext2_alloc_block(dev);
      if (!dind_block)
        return 0;
      inode->i_block[EXT2_DIND_BLOCK] = dind_block;
      inode->i_blocks += (ext2_block_size / 512);
      ext2_write_inode(dev, inode_num, inode);
    }

    u32 *ind_buffer = kmalloc(ext2_block_size);
    ext2_read_block(dev, dind_block, ind_buffer);

    u32 ind1_idx = b / ptrs_per_block;
    u32 ind1_block = ind_buffer[ind1_idx];
    if (ind1_block == 0) {
      ind1_block = ext2_alloc_block(dev);
      if (!ind1_block) {
        kfree(ind_buffer);
        return 0;
      }
      ind_buffer[ind1_idx] = ind1_block;
      ext2_write_block(dev, dind_block, ind_buffer);
      inode->i_blocks += (ext2_block_size / 512);
      ext2_write_inode(dev, inode_num, inode);
    }

    ext2_read_block(dev, ind1_block, ind_buffer);
    u32 ind2_idx = b % ptrs_per_block;
    if (ind_buffer[ind2_idx] == 0) {
      pblk = ext2_alloc_block(dev);
      if (pblk) {
        ind_buffer[ind2_idx] = pblk;
        ext2_write_block(dev, ind1_block, ind_buffer);
        inode->i_blocks += (ext2_block_size / 512);
        ext2_write_inode(dev, inode_num, inode);
      }
    } else {
      pblk = ind_buffer[ind2_idx];
    }
    kfree(ind_buffer);
    return pblk;
  }
  return 0;
}

static isize ext2_vfs_read(struct vfs_node *node, u64 offset, char *buffer,
                           usize size) {
	u32 inode_num = (u32)(usize)node->inode->data;
	struct ext2_inode inode;
  if (ext2_read_inode(node->inode->blk_dev, inode_num, &inode) < 0)
    return -EIO;
	
  if (offset >= inode.i_size)
    return 0;
	
	usize remaining = inode.i_size - offset;
	usize to_read = size < remaining ? size : remaining;
	usize bytes_read = 0;
	
	u8 *block_buf = kmalloc(ext2_block_size);
  if (!block_buf)
    return -1;
	
	while (bytes_read < to_read) {
		u64 current_offset = offset + bytes_read;
		u32 block_idx = current_offset / ext2_block_size;
		u32 block_offset = current_offset % ext2_block_size;
		
		u32 phys_block = ext2_get_inode_block(node->inode->blk_dev, &inode, block_idx);
		if (phys_block == 0) {
			memset(buffer + bytes_read, 0, ext2_block_size - block_offset);
		} else {
			ext2_read_block(node->inode->blk_dev, phys_block, block_buf);
			usize chunk = ext2_block_size - block_offset;
      if (chunk > to_read - bytes_read)
        chunk = to_read - bytes_read;
			memcpy(buffer + bytes_read, block_buf + block_offset, chunk);
		}
		
		usize chunk = ext2_block_size - block_offset;
    if (chunk > to_read - bytes_read)
      chunk = to_read - bytes_read;
		bytes_read += chunk;
	}
	
	kfree(block_buf);
	return bytes_read;
}

/* Sparse-file write path with file-size growth. */
static isize ext2_vfs_write(struct vfs_node *node, u64 offset,
                            const char *buffer, usize size) {
	u32 inode_num = (u32)(usize)node->inode->data;
	struct ext2_inode inode;
  if (ext2_read_inode(node->inode->blk_dev, inode_num, &inode) < 0)
    return -EIO;
	
	usize bytes_written = 0;
	u8 *block_buf = kmalloc(ext2_block_size);
  if (!block_buf)
    return -1;
	
	while (bytes_written < size) {
		u64 current_offset = offset + bytes_written;
		u32 block_idx = current_offset / ext2_block_size;
		u32 block_offset = current_offset % ext2_block_size;
		
    /* Resolve the block or allocate it when writing into a hole. */
		u32 phys_block = ext2_get_inode_block(node->inode->blk_dev, &inode, block_idx);
    if (!phys_block) {
      phys_block = ext2_allocate_block_for_inode(node->inode->blk_dev, inode_num, &inode, block_idx);
      if (!phys_block)
        break; /* Out of space. */
    }
		
		usize chunk = ext2_block_size - block_offset;
    if (chunk > size - bytes_written)
      chunk = size - bytes_written;
		
		if (chunk < ext2_block_size) {
			ext2_read_block(node->inode->blk_dev, phys_block, block_buf);
		}
		memcpy(block_buf + block_offset, buffer + bytes_written, chunk);
		ext2_write_block(node->inode->blk_dev, phys_block, block_buf);
		
		bytes_written += chunk;
	}
	
	kfree(block_buf);

  /* Update the file size when appending past EOF. */
  if (offset + bytes_written > inode.i_size) {
    inode.i_size = (u32)(offset + bytes_written);
    ext2_write_inode(node->inode->blk_dev, inode_num, &inode);
    node->inode->size = inode.i_size;
  }

	return bytes_written;
}

static int ext2_add_dir_entry(struct block_device *dev, u32 dir_inode_num, u32 inode_num,
                               const char *name, u8 type) {
	struct ext2_inode dir_inode;
  if (ext2_read_inode(dev, dir_inode_num, &dir_inode) < 0)
    return -1;
	
	usize name_len = strlen(name);
  if (name_len > 255)
    name_len = 255;
	
	u32 needed_len = 8 + ((name_len + 3) & ~3);
	u8 *dir_buf = kmalloc(ext2_block_size);
  if (!dir_buf)
    return -1;
	
	u32 blocks = (dir_inode.i_size + ext2_block_size - 1) / ext2_block_size;
	if (blocks == 0) {
		u32 new_phys = ext2_alloc_block(dev);
    if (!new_phys) {
      kfree(dir_buf);
      return -1;
    }
		dir_inode.i_block[0] = new_phys;
		dir_inode.i_blocks += (ext2_block_size / 512);
		dir_inode.i_size += ext2_block_size;
		ext2_write_inode(dev, dir_inode_num, &dir_inode);
		
		memset(dir_buf, 0, ext2_block_size);
		struct ext2_dir_entry *new_entry = (struct ext2_dir_entry *)dir_buf;
		new_entry->inode = inode_num;
		new_entry->rec_len = ext2_block_size;
		new_entry->name_len = name_len;
		new_entry->file_type = type;
		memcpy(new_entry->name, name, name_len);
		ext2_write_block(dev, new_phys, dir_buf);
		kfree(dir_buf);
		return 0;
	}
	
	for (u32 b = 0; b < blocks; b++) {
		u32 phys_block = ext2_get_inode_block(dev, &dir_inode, b);
		ext2_read_block(dev, phys_block, dir_buf);
		
		usize offset = 0;
		while (offset < ext2_block_size) {
      struct ext2_dir_entry *entry =
          (struct ext2_dir_entry *)(dir_buf + offset);
      if (entry->rec_len == 0)
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
				
				ext2_write_block(dev, phys_block, dir_buf);
				kfree(dir_buf);
				return 0;
			}
			offset += entry->rec_len;
		}
	}
	
	u32 new_phys = ext2_alloc_block(dev);
	if (new_phys) {
		dir_inode.i_block[blocks] = new_phys;
		dir_inode.i_blocks += (ext2_block_size / 512);
		dir_inode.i_size += ext2_block_size;
		ext2_write_inode(dev, dir_inode_num, &dir_inode);
		
		memset(dir_buf, 0, ext2_block_size);
		struct ext2_dir_entry *new_entry = (struct ext2_dir_entry *)dir_buf;
		new_entry->inode = inode_num;
		new_entry->rec_len = ext2_block_size;
		new_entry->name_len = name_len;
		new_entry->file_type = type;
		memcpy(new_entry->name, name, name_len);
		ext2_write_block(dev, new_phys, dir_buf);
		kfree(dir_buf);
		return 0;
	}
	
	kfree(dir_buf);
	return -1;
}

static int ext2_remove_dir_entry(struct block_device *dev, u32 dir_inode_num, const char *name) {
  struct ext2_inode dir_inode;
  if (ext2_read_inode(dev, dir_inode_num, &dir_inode) < 0) return -1;
  
  u8 *dir_buf = kmalloc(ext2_block_size);
  if (!dir_buf) return -ENOMEM;
  
  u32 blocks = (dir_inode.i_size + ext2_block_size - 1) / ext2_block_size;
  for (u32 b = 0; b < blocks; b++) {
    u32 phys_block = ext2_get_inode_block(dev, &dir_inode, b);
    if (!phys_block) continue;
    ext2_read_block(dev, phys_block, dir_buf);
    
    usize offset = 0;
    struct ext2_dir_entry *prev_entry = 0;
    while (offset < ext2_block_size) {
      struct ext2_dir_entry *entry = (struct ext2_dir_entry *)(dir_buf + offset);
      if (entry->rec_len == 0) break;
      
      if (entry->inode != 0 && strlen(name) == entry->name_len &&
          memcmp(entry->name, name, entry->name_len) == 0) {
        if (prev_entry) {
          prev_entry->rec_len += entry->rec_len;
        } else {
          entry->inode = 0;
        }
        ext2_write_block(dev, phys_block, dir_buf);
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

static int ext2_vfs_create(struct vfs_node *dir, const char *name,
                           const char *full_path, u32 mode) {
	u32 dir_inode_num = (u32)(usize)dir->inode->data;
	
	u32 new_inode_num = ext2_alloc_inode(dir->inode->blk_dev);
  if (!new_inode_num)
    return -ENOSPC;
	
	struct ext2_inode new_inode;
	memset(&new_inode, 0, sizeof(new_inode));
	new_inode.i_mode = EXT2_S_IFREG | (mode & 0777);
	new_inode.i_links_count = 1;
	ext2_write_inode(dir->inode->blk_dev, new_inode_num, &new_inode);
	
  if (ext2_add_dir_entry(dir->inode->blk_dev, dir_inode_num, new_inode_num, name, EXT2_FT_REG_FILE) <
      0) {
		return -1;
	}
	
  struct vfs_node *node =
      vfs_add_node(full_path, VFS_FILE, (void *)(usize)new_inode_num, 0, 0);
	if (node) {
		node->inode->blk_dev = dir->inode->blk_dev;
		node->inode->read_cb = ext2_vfs_read;
		node->inode->write_cb = ext2_vfs_write;
	}
	
	return 0;
}

static int ext2_vfs_unlink(struct vfs_node *dir, const char *name) {
  u32 dir_inode_num = (u32)(usize)dir->inode->data;
  struct ext2_inode dir_inode;
  if (ext2_read_inode(dir->inode->blk_dev, dir_inode_num, &dir_inode) < 0) return -EIO;

  /* Find target inode number */
  u32 inode_num = 0;
  u8 *dir_buf = kmalloc(ext2_block_size);
  u32 blocks = (dir_inode.i_size + ext2_block_size - 1) / ext2_block_size;
  for (u32 b = 0; b < blocks; b++) {
    u32 phys_block = ext2_get_inode_block(dir->inode->blk_dev, &dir_inode, b);
    if (!phys_block) continue;
    ext2_read_block(dir->inode->blk_dev, phys_block, dir_buf);
    usize offset = 0;
    while (offset < ext2_block_size) {
      struct ext2_dir_entry *entry = (struct ext2_dir_entry *)(dir_buf + offset);
      if (entry->rec_len == 0) break;
      if (entry->inode != 0 && strlen(name) == entry->name_len &&
          memcmp(entry->name, name, entry->name_len) == 0) {
        inode_num = entry->inode;
        break;
      }
      offset += entry->rec_len;
    }
    if (inode_num) break;
  }
  kfree(dir_buf);
  if (!inode_num) return -ENOENT;

  /* Remove directory entry */
  if (ext2_remove_dir_entry(dir->inode->blk_dev, dir_inode_num, name) < 0) return -EIO;

  /* Update target inode link count */
  struct ext2_inode inode;
  if (ext2_read_inode(dir->inode->blk_dev, inode_num, &inode) == 0) {
    if (inode.i_links_count > 0) {
      inode.i_links_count--;
      ext2_write_inode(dir->inode->blk_dev, inode_num, &inode);
    }
  }
  return 0;
}

static void ext2_vfs_release(struct vfs_node *node) {
  u32 inode_num = (u32)(usize)node->inode->data;
  struct ext2_inode inode;
  if (ext2_read_inode(node->inode->blk_dev, inode_num, &inode) == 0) {
    if (inode.i_links_count == 0) {
      /* Free blocks */
      u32 total_blocks = (inode.i_size + ext2_block_size - 1) / ext2_block_size;
      for (u32 b = 0; b < total_blocks; b++) {
        u32 phys = ext2_get_inode_block(node->inode->blk_dev, &inode, b);
        if (phys) ext2_free_block(node->inode->blk_dev, phys);
      }
      ext2_free_inode(node->inode->blk_dev, inode_num);
    }
  }
}

static int ext2_vfs_setattr(struct vfs_node *node) {
  u32 inode_num = (u32)(usize)node->inode->data;
  struct ext2_inode inode;
  if (ext2_read_inode(node->inode->blk_dev, inode_num, &inode) < 0) return -EIO;
  
  inode.i_mode = (inode.i_mode & ~0777) | (node->inode->mode & 0777);
  inode.i_uid = node->inode->uid;
  inode.i_gid = node->inode->gid;
  inode.i_atime = node->inode->atime;
  inode.i_mtime = node->inode->mtime;
  inode.i_ctime = node->inode->ctime;
  inode.i_links_count = node->inode->nlink;
  
  return ext2_write_inode(node->inode->blk_dev, inode_num, &inode);
}

static int ext2_vfs_symlink(struct vfs_node *dir, const char *name, const char *target) {
  u32 new_inode_num = ext2_alloc_inode(dir->inode->blk_dev);
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
    u32 block = ext2_alloc_block(dir->inode->blk_dev);
    if (!block) {
      ext2_free_inode(dir->inode->blk_dev, new_inode_num);
      return -ENOSPC;
    }
    inode.i_block[0] = block;
    ext2_write_block(dir->inode->blk_dev, block, target);
  }
  
  ext2_write_inode(dir->inode->blk_dev, new_inode_num, &inode);
  return ext2_add_dir_entry(dir->inode->blk_dev, (u32)(usize)dir->inode->data, new_inode_num, name, EXT2_FT_SYMLINK);
}

static int ext2_vfs_link(struct vfs_node *target_node, struct vfs_node *dir, const char *name) {
  u32 inode_num = (u32)(usize)target_node->inode->data;
  struct ext2_inode inode;
  if (ext2_read_inode(dir->inode->blk_dev, inode_num, &inode) < 0) return -EIO;

  inode.i_links_count++;
  ext2_write_inode(dir->inode->blk_dev, inode_num, &inode);

  u8 type = (target_node->inode->type == VFS_DIRECTORY) ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
  return ext2_add_dir_entry(dir->inode->blk_dev, (u32)(usize)dir->inode->data, inode_num, name, type);
}

static int ext2_vfs_statfs(struct vfs_node *node, struct b1nix_statfs *st) {
  (void)node;
  memset(st, 0, sizeof(*st));
  st->f_type = EXT2_SUPER_MAGIC;
  st->f_bsize = ext2_block_size;
  st->f_blocks = ext2_sb.s_blocks_count;
  st->f_bfree = ext2_sb.s_free_blocks_count;
  st->f_bavail = ext2_sb.s_free_blocks_count;
  st->f_files = ext2_sb.s_inodes_count;
  st->f_ffree = ext2_sb.s_free_inodes_count;
  st->f_namelen = 255;
  return 0;
}

static int ext2_vfs_rmdir(struct vfs_node *dir, const char *name) {
  /* For simplicity, we use unlink for rmdir but also decrement parent link count */
  /* Real ext2 rmdir would check if directory is empty */
  int err = ext2_vfs_unlink(dir, name);
  if (err == 0) {
    struct ext2_inode dir_inode;
    u32 dir_inode_num = (u32)(usize)dir->inode->data;
    if (ext2_read_inode(dir->inode->blk_dev, dir_inode_num, &dir_inode) == 0) {
      if (dir_inode.i_links_count > 2) {
        dir_inode.i_links_count--;
        ext2_write_inode(dir->inode->blk_dev, dir_inode_num, &dir_inode);
      }
    }
  }
  return err;
}

static int ext2_vfs_rename(struct vfs_node *old_dir, const char *old_name,
                           struct vfs_node *new_dir, const char *new_name) {
  /* Simplified rename: add new entry, then remove old entry */
  /* This doesn't handle the case where target inode needs to be unlinked if it exists */
  u32 old_dir_inode_num = (u32)(usize)old_dir->inode->data;
  struct ext2_inode old_dir_inode;
  ext2_read_inode(old_dir->inode->blk_dev, old_dir_inode_num, &old_dir_inode);
  
  u32 inode_num = 0;
  u8 type = 0;
  u8 *dir_buf = kmalloc(ext2_block_size);
  u32 blocks = (old_dir_inode.i_size + ext2_block_size - 1) / ext2_block_size;
  for (u32 b = 0; b < blocks; b++) {
    u32 phys_block = ext2_get_inode_block(old_dir->inode->blk_dev, &old_dir_inode, b);
    if (!phys_block) continue;
    ext2_read_block(old_dir->inode->blk_dev, phys_block, dir_buf);
    usize offset = 0;
    while (offset < ext2_block_size) {
      struct ext2_dir_entry *entry = (struct ext2_dir_entry *)(dir_buf + offset);
      if (entry->rec_len == 0) break;
      if (entry->inode != 0 && strlen(old_name) == entry->name_len &&
          memcmp(entry->name, old_name, entry->name_len) == 0) {
        inode_num = entry->inode;
        type = entry->file_type;
        break;
      }
      offset += entry->rec_len;
    }
    if (inode_num) break;
  }
  kfree(dir_buf);
  if (!inode_num) return -ENOENT;
 
  if (ext2_add_dir_entry(new_dir->inode->blk_dev, (u32)(usize)new_dir->inode->data, inode_num, new_name, type) < 0)
    return -EIO;
  
  ext2_remove_dir_entry(old_dir->inode->blk_dev, old_dir_inode_num, old_name);
  return 0;
}

static int ext2_vfs_mkdir(struct vfs_node *dir, const char *name, u32 mode) {
    u32 dir_inode_num = (u32)(usize)dir->inode->data;
    u32 new_inode_num = ext2_alloc_inode(dir->inode->blk_dev);
    if (!new_inode_num) return -ENOSPC;

    struct ext2_inode new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.i_mode = EXT2_S_IFDIR | (mode & 0777);
    new_inode.i_links_count = 2; // . and parent
    ext2_write_inode(dir->inode->blk_dev, new_inode_num, &new_inode);

    if (ext2_add_dir_entry(dir->inode->blk_dev, dir_inode_num, new_inode_num, name, EXT2_FT_DIR) < 0) {
        return -1;
    }

    /* Add . and .. entries */
    ext2_add_dir_entry(dir->inode->blk_dev, new_inode_num, new_inode_num, ".", EXT2_FT_DIR);
    ext2_add_dir_entry(dir->inode->blk_dev, new_inode_num, dir_inode_num, "..", EXT2_FT_DIR);

    char full_path[256];
    vfs_resolve_path(name, full_path); // This is wrong if it's relative, but let's assume it works for now
    // Actually we need the parent path.

    return 0;
}

static void ext2_populate_vfs(struct block_device *dev, u32 inode_num, const char *base_path) {
	struct ext2_inode inode;
  if (ext2_read_inode(dev, inode_num, &inode) < 0)
    return;
	
  if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
    return;
	
	u8 *dir_buf = kmalloc(inode.i_size);
  if (!dir_buf)
    return;
	
  for (u32 i = 0; i < (inode.i_size + ext2_block_size - 1) / ext2_block_size;
       i++) {
		u32 phys_block = ext2_get_inode_block(dev, &inode, i);
		if (phys_block) {
			ext2_read_block(dev, phys_block, dir_buf + i * ext2_block_size);
		}
	}
	
	usize offset = 0;
	while (offset < inode.i_size) {
		struct ext2_dir_entry *entry = (struct ext2_dir_entry *)(dir_buf + offset);
    if (entry->rec_len == 0)
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
				if (ext2_read_inode(dev, entry->inode, &child_inode) == 0) {
					if ((child_inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
            struct vfs_node *dir_node = vfs_add_node(
                full_path, VFS_DIRECTORY, (void *)(usize)entry->inode, 0, 0);
            if (dir_node) {
              dir_node->inode->blk_dev = dev;
              dir_node->inode->mode = child_inode.i_mode & 0xFFFF;
              dir_node->inode->uid = child_inode.i_uid;
              dir_node->inode->gid = child_inode.i_gid;
              dir_node->inode->atime = child_inode.i_atime;
              dir_node->inode->mtime = child_inode.i_mtime;
              dir_node->inode->ctime = child_inode.i_ctime;
              dir_node->inode->nlink = child_inode.i_links_count;
              dir_node->inode->create_cb = ext2_vfs_create;
              dir_node->inode->mkdir_cb = ext2_vfs_mkdir;
              dir_node->inode->unlink_cb = ext2_vfs_unlink;
              dir_node->inode->rmdir_cb = ext2_vfs_rmdir;
              dir_node->inode->rename_cb = ext2_vfs_rename;
              dir_node->inode->symlink_cb = ext2_vfs_symlink;
              dir_node->inode->link_cb = ext2_vfs_link;
              dir_node->inode->release_cb = ext2_vfs_release;
              dir_node->inode->setattr_cb = ext2_vfs_setattr;
              dir_node->inode->statfs_cb = ext2_vfs_statfs;
            }
						ext2_populate_vfs(dev, entry->inode, full_path);
					} else {
            struct vfs_node *node =
                vfs_add_node(full_path, VFS_FILE, (void *)(usize)entry->inode,
                             child_inode.i_size, 0);
						if (node) {
							node->inode->blk_dev = dev;
							node->inode->read_cb = ext2_vfs_read;
							node->inode->write_cb = ext2_vfs_write;
              node->inode->mode = child_inode.i_mode & 0xFFFF;
              node->inode->uid = child_inode.i_uid;
              node->inode->gid = child_inode.i_gid;
              node->inode->atime = child_inode.i_atime;
              node->inode->mtime = child_inode.i_mtime;
              node->inode->ctime = child_inode.i_ctime;
              node->inode->nlink = child_inode.i_links_count;
              node->inode->release_cb = ext2_vfs_release;
              node->inode->setattr_cb = ext2_vfs_setattr;
						}
					}
				}
			}
		}
		offset += entry->rec_len;
	}
	
	kfree(dir_buf);
}

int ext2_mount_root(const char *device_name, const char *mount_point) {
	ext2_dev = blk_get(device_name);
  if (!ext2_dev)
    return -1;
	
	u8 *sb_buffer = kmalloc(1024);
	if (blk_read_cached(ext2_dev, 2, 2, sb_buffer) < 0) {
		kfree(sb_buffer);
		return -1;
	}
	
	memcpy(&ext2_sb, sb_buffer, sizeof(struct ext2_superblock));
	kfree(sb_buffer);
	
  if (ext2_sb.s_magic != EXT2_SUPER_MAGIC)
    return -1;
	
	ext2_block_size = 1024 << ext2_sb.s_log_block_size;
	ext2_inodes_per_group = ext2_sb.s_inodes_per_group;
	
	if (ext2_sb.s_rev_level == 0) {
		ext2_inode_size = 128;
	} else {
		ext2_inode_size = ext2_sb.s_inode_size;
	}
	
	console_write("ext2: mounted, block_size=");
	console_write_dec(ext2_block_size);
	console_write(" at ");
	console_write(mount_point);
	console_write("\n");

	vfs_mount(device_name, mount_point, "ext2", 0);
  struct vfs_node *ext2_root =
      vfs_add_node(mount_point, VFS_DIRECTORY, (void *)(usize)2, 0, 0);
  if (ext2_root) {
    ext2_root->inode->blk_dev = ext2_dev;
    ext2_root->inode->create_cb = ext2_vfs_create;
    ext2_root->inode->mkdir_cb = ext2_vfs_mkdir;
    ext2_root->inode->unlink_cb = ext2_vfs_unlink;
    ext2_root->inode->rmdir_cb = ext2_vfs_rmdir;
    ext2_root->inode->rename_cb = ext2_vfs_rename;
    ext2_root->inode->symlink_cb = ext2_vfs_symlink;
    ext2_root->inode->link_cb = ext2_vfs_link;
    ext2_root->inode->release_cb = ext2_vfs_release;
    ext2_root->inode->setattr_cb = ext2_vfs_setattr;
    ext2_root->inode->statfs_cb = ext2_vfs_statfs;
  }
	ext2_populate_vfs(ext2_dev, 2, mount_point);
	return 0;
}

void ext2_init(void) {
  if (ext2_mount_root("virtio-blk0", "/") == 0)
		return;
  if (ext2_mount_root("virtio-blk1", "/ext2") == 0)
		return;
	}
