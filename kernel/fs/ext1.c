#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/ext1.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <b1nix/errno.h>
#include <string.h>

/* ── Block I/O ── */

static int ext1_read_block(struct ext1_fs *fs, u32 block, void *buffer) {
    u64 lba = (u64)block * (fs->block_size / 512);
    return blk_read_cached(fs->bdev, lba, fs->block_size / 512, buffer);
}

static int ext1_write_block(struct ext1_fs *fs, u32 block, const void *buffer) {
    u64 lba = (u64)block * (fs->block_size / 512);
    return blk_write_cached(fs->bdev, lba, fs->block_size / 512, buffer);
}

/* ── Block Group Descriptor helpers ── */

static void ext1_read_bgd(struct ext1_fs *fs, u32 group, struct ext1_bgd *bgd) {
    u32 bg_desc_block = (fs->block_size == 1024) ? 2 : 1;
    u32 bg_offset = group * sizeof(struct ext1_bgd);
    u32 bg_block = bg_desc_block + (bg_offset / fs->block_size);

    u8 *buf = kmalloc(fs->block_size);
    ext1_read_block(fs, bg_block, buf);
    memcpy(bgd, buf + (bg_offset % fs->block_size), sizeof(struct ext1_bgd));
    kfree(buf);
}

static void ext1_write_bgd(struct ext1_fs *fs, u32 group, struct ext1_bgd *bgd) {
    u32 bg_desc_block = (fs->block_size == 1024) ? 2 : 1;
    u32 bg_offset = group * sizeof(struct ext1_bgd);
    u32 bg_block = bg_desc_block + (bg_offset / fs->block_size);

    u8 *buf = kmalloc(fs->block_size);
    ext1_read_block(fs, bg_block, buf);
    memcpy(buf + (bg_offset % fs->block_size), bgd, sizeof(struct ext1_bgd));
    ext1_write_block(fs, bg_block, buf);
    kfree(buf);
}

static void ext1_write_superblock(struct ext1_fs *fs) {
    u8 *sb = kmalloc(1024);
    if (blk_read_cached(fs->bdev, 2, 2, sb) >= 0) {
        memcpy(sb, &fs->sb, sizeof(struct ext1_superblock));
        blk_write_cached(fs->bdev, 2, 2, sb);
    }
    kfree(sb);
}

/* ── Inode operations ── */

static int ext1_read_inode(struct ext1_fs *fs, u32 inode_num, struct ext1_inode *inode) {
    if (inode_num == 0) return -1;
    u32 group = (inode_num - 1) / fs->inodes_per_group;
    u32 index = (inode_num - 1) % fs->inodes_per_group;
    struct ext1_bgd bgd;
    ext1_read_bgd(fs, group, &bgd);
    u32 inode_offset = index * 128;
    u32 block_idx = bgd.bg_inode_table + (inode_offset / fs->block_size);
    u8 *buf = kmalloc(fs->block_size);
    if (ext1_read_block(fs, block_idx, buf) < 0) { kfree(buf); return -1; }
    memcpy(inode, buf + (inode_offset % fs->block_size), sizeof(struct ext1_inode));
    kfree(buf);
    return 0;
}

static int ext1_write_inode(struct ext1_fs *fs, u32 inode_num, const struct ext1_inode *inode) {
    if (inode_num == 0) return -1;
    u32 group = (inode_num - 1) / fs->inodes_per_group;
    u32 index = (inode_num - 1) % fs->inodes_per_group;
    struct ext1_bgd bgd;
    ext1_read_bgd(fs, group, &bgd);
    u32 inode_offset = index * 128;
    u32 block_idx = bgd.bg_inode_table + (inode_offset / fs->block_size);
    u8 *buf = kmalloc(fs->block_size);
    if (ext1_read_block(fs, block_idx, buf) < 0) { kfree(buf); return -1; }
    memcpy(buf + (inode_offset % fs->block_size), inode, sizeof(struct ext1_inode));
    int ret = ext1_write_block(fs, block_idx, buf);
    kfree(buf);
    return ret;
}

/* ── Block allocation ── */

static u32 ext1_alloc_block(struct ext1_fs *fs) {
    u32 groups = (fs->sb.s_blocks_count + fs->sb.s_blocks_per_group - 1) / fs->sb.s_blocks_per_group;
    for (u32 g = 0; g < groups; g++) {
        struct ext1_bgd bgd;
        ext1_read_bgd(fs, g, &bgd);
        if (bgd.bg_free_blocks_count == 0) continue;
        u8 *bitmap = kmalloc(fs->block_size);
        ext1_read_block(fs, bgd.bg_block_bitmap, bitmap);
        for (u32 i = 0; i < fs->sb.s_blocks_per_group; i++) {
            if (!(bitmap[i / 8] & (1 << (i % 8)))) {
                bitmap[i / 8] |= (1 << (i % 8));
                ext1_write_block(fs, bgd.bg_block_bitmap, bitmap);
                kfree(bitmap);
                bgd.bg_free_blocks_count--;
                ext1_write_bgd(fs, g, &bgd);
                fs->sb.s_free_blocks_count--;
                ext1_write_superblock(fs);
                u32 block_num = g * fs->sb.s_blocks_per_group + i;
                if (fs->sb.s_first_data_block == 1 && fs->sb.s_log_block_size == 0) block_num++;
                u8 *zero = kzalloc(fs->block_size);
                ext1_write_block(fs, block_num, zero);
                kfree(zero);
                return block_num;
            }
        }
        kfree(bitmap);
    }
    return 0;
}

static u32 ext1_alloc_inode(struct ext1_fs *fs) {
    u32 groups = (fs->sb.s_inodes_count + fs->inodes_per_group - 1) / fs->inodes_per_group;
    for (u32 g = 0; g < groups; g++) {
        struct ext1_bgd bgd;
        ext1_read_bgd(fs, g, &bgd);
        if (bgd.bg_free_inodes_count == 0) continue;
        u8 *bitmap = kmalloc(fs->block_size);
        ext1_read_block(fs, bgd.bg_inode_bitmap, bitmap);
        for (u32 i = 0; i < fs->inodes_per_group; i++) {
            if (!(bitmap[i / 8] & (1 << (i % 8)))) {
                bitmap[i / 8] |= (1 << (i % 8));
                ext1_write_block(fs, bgd.bg_inode_bitmap, bitmap);
                kfree(bitmap);
                bgd.bg_free_inodes_count--;
                ext1_write_bgd(fs, g, &bgd);
                fs->sb.s_free_inodes_count--;
                ext1_write_superblock(fs);
                u32 inode_num = g * fs->inodes_per_group + i + 1;
                struct ext1_inode ni;
                memset(&ni, 0, sizeof(ni));
                ext1_write_inode(fs, inode_num, &ni);
                return inode_num;
            }
        }
        kfree(bitmap);
    }
    return 0;
}

static u32 ext1_get_block(struct ext1_fs *fs, struct ext1_inode *inode, u32 block_idx) {
    if (block_idx < EXT1_NDIR_BLOCKS) return inode->i_block[block_idx];
    u32 ptrs = fs->block_size / 4;
    block_idx -= EXT1_NDIR_BLOCKS;
    if (block_idx < ptrs) {
        if (!inode->i_block[EXT1_IND_BLOCK]) return 0;
        u32 *ind = kmalloc(fs->block_size);
        ext1_read_block(fs, inode->i_block[EXT1_IND_BLOCK], ind);
        u32 r = ind[block_idx];
        kfree(ind);
        return r;
    }
    block_idx -= ptrs;
    if (block_idx < ptrs * ptrs) {
        if (!inode->i_block[EXT1_DIND_BLOCK]) return 0;
        u32 *dind = kmalloc(fs->block_size);
        ext1_read_block(fs, inode->i_block[EXT1_DIND_BLOCK], dind);
        if (!dind[block_idx / ptrs]) { kfree(dind); return 0; }
        u32 *ind = kmalloc(fs->block_size);
        ext1_read_block(fs, dind[block_idx / ptrs], ind);
        u32 r = ind[block_idx % ptrs];
        kfree(ind); kfree(dind);
        return r;
    }
    return 0;
}

static u32 ext1_set_block(struct ext1_fs *fs, struct ext1_inode *inode, u32 block_idx, u32 phys) {
    u32 ptrs = fs->block_size / 4;
    if (block_idx < EXT1_NDIR_BLOCKS) {
        inode->i_block[block_idx] = phys;
        return 1;
    }
    block_idx -= EXT1_NDIR_BLOCKS;
    if (block_idx < ptrs) {
        if (!inode->i_block[EXT1_IND_BLOCK]) {
            u32 b = ext1_alloc_block(fs);
            if (!b) return 0;
            inode->i_block[EXT1_IND_BLOCK] = b;
        }
        u32 *ind = kmalloc(fs->block_size);
        ext1_read_block(fs, inode->i_block[EXT1_IND_BLOCK], ind);
        ind[block_idx] = phys;
        ext1_write_block(fs, inode->i_block[EXT1_IND_BLOCK], ind);
        kfree(ind);
        return 1;
    }
    block_idx -= ptrs;
    if (block_idx < ptrs * ptrs) {
        if (!inode->i_block[EXT1_DIND_BLOCK]) {
            u32 b = ext1_alloc_block(fs);
            if (!b) return 0;
            inode->i_block[EXT1_DIND_BLOCK] = b;
        }
        u32 *dind = kmalloc(fs->block_size);
        ext1_read_block(fs, inode->i_block[EXT1_DIND_BLOCK], dind);
        u32 idx1 = block_idx / ptrs;
        if (!dind[idx1]) {
            u32 b = ext1_alloc_block(fs);
            if (!b) { kfree(dind); return 0; }
            dind[idx1] = b;
            ext1_write_block(fs, inode->i_block[EXT1_DIND_BLOCK], dind);
        }
        u32 *ind = kmalloc(fs->block_size);
        ext1_read_block(fs, dind[idx1], ind);
        ind[block_idx % ptrs] = phys;
        ext1_write_block(fs, dind[idx1], ind);
        kfree(ind); kfree(dind);
        return 1;
    }
    return 0;
}

static isize ext1_vfs_read(struct vfs_node *node, u64 offset, char *buffer, usize size, int flags) {
    (void)flags;
    struct ext1_inode_info *info = (struct ext1_inode_info *)node->inode->data;
    struct ext1_fs *fs = info->fs;
    struct ext1_inode inode;
    if (ext1_read_inode(fs, info->inode_num, &inode) < 0) return -1;
    if (offset >= inode.i_size) return 0;
    if ((inode.i_mode & EXT1_S_IFMT) == EXT1_S_IFLNK && inode.i_size < 60) {
        usize to_copy = size < (usize)inode.i_size ? size : (usize)inode.i_size;
        memcpy(buffer, (char *)inode.i_block, to_copy);
        return (isize)to_copy;
    }
    usize remaining = inode.i_size - offset;
    usize to_read = size < remaining ? size : remaining;
    usize done = 0;
    u8 *block_buf = kmalloc(fs->block_size);
    while (done < to_read) {
        u32 b_idx = (offset + done) / fs->block_size;
        u32 b_off = (offset + done) % fs->block_size;
        u32 phys = ext1_get_block(fs, &inode, b_idx);
        usize chunk = fs->block_size - b_off;
        if (chunk > to_read - done) chunk = to_read - done;
        if (phys) {
            ext1_read_block(fs, phys, block_buf);
            memcpy(buffer + done, block_buf + b_off, chunk);
        } else memset(buffer + done, 0, chunk);
        done += chunk;
    }
    kfree(block_buf);
    return done;
}

static isize ext1_vfs_write(struct vfs_node *node, u64 offset, const char *buffer, usize size, int flags) {
    (void)flags;
    struct ext1_inode_info *info = (struct ext1_inode_info *)node->inode->data;
    struct ext1_fs *fs = info->fs;
    struct ext1_inode inode;
    if (ext1_read_inode(fs, info->inode_num, &inode) < 0) return -1;
    u64 new_size = offset + size;
    if (new_size > inode.i_size) {
        u32 old_blks = (inode.i_size + fs->block_size - 1) / fs->block_size;
        u32 new_blks = (new_size + fs->block_size - 1) / fs->block_size;
        for (u32 b = old_blks; b < new_blks; b++) {
            u32 pblk = ext1_alloc_block(fs);
            if (!pblk || !ext1_set_block(fs, &inode, b, pblk)) return -1;
            inode.i_blocks += fs->block_size / 512;
        }
        inode.i_size = (u32)new_size;
        ext1_write_inode(fs, info->inode_num, &inode);
        if (inode.i_size > node->inode->size) {
            node->inode->size = inode.i_size;
        }
    }
    usize done = 0;
    u8 *block_buf = kmalloc(fs->block_size);
    while (done < size) {
        u32 b_idx = (offset + done) / fs->block_size;
        u32 b_off = (offset + done) % fs->block_size;
        u32 phys = ext1_get_block(fs, &inode, b_idx);
        if (!phys) break;
        usize chunk = fs->block_size - b_off;
        if (chunk > size - done) chunk = size - done;
        if (chunk < fs->block_size) ext1_read_block(fs, phys, block_buf);
        memcpy(block_buf + b_off, buffer + done, chunk);
        ext1_write_block(fs, phys, block_buf);
        done += chunk;
    }
    kfree(block_buf);
    return done;
}

static int ext1_add_dir_entry(struct ext1_fs *fs, u32 dir_ino, u32 inode_num, const char *name) {
    struct ext1_inode dir;
    if (ext1_read_inode(fs, dir_ino, &dir) < 0) return -1;
    usize name_len = strlen(name);
    if (name_len > 255) name_len = 255;
    u32 needed = 8 + ((name_len + 3) & ~3);
    u8 *buf = kmalloc(fs->block_size);
    u32 blocks = (dir.i_size + fs->block_size - 1) / fs->block_size;
    if (blocks == 0) {
        u32 phys = ext1_alloc_block(fs);
        if (!phys) { kfree(buf); return -1; }
        ext1_set_block(fs, &dir, 0, phys);
        dir.i_blocks += fs->block_size / 512;
        dir.i_size = fs->block_size;
        ext1_write_inode(fs, dir_ino, &dir);
        memset(buf, 0, fs->block_size);
        struct ext1_dir_entry *e = (struct ext1_dir_entry *)buf;
        e->inode = inode_num; e->rec_len = fs->block_size; e->name_len = name_len;
        memcpy(e->name, name, name_len);
        ext1_write_block(fs, phys, buf);
        kfree(buf); return 0;
    }
    for (u32 b = 0; b < blocks; b++) {
        u32 phys = ext1_get_block(fs, &dir, b);
        if (!phys) continue;
        ext1_read_block(fs, phys, buf);
        usize off = 0;
        while (off < fs->block_size) {
            struct ext1_dir_entry *e = (struct ext1_dir_entry *)(buf + off);
            if (e->rec_len == 0) break;
            u32 actual = 8 + ((e->name_len + 3) & ~3);
            if (e->rec_len >= actual + needed) {
                u32 old_rec_len = e->rec_len;
                e->rec_len = actual;
                struct ext1_dir_entry *ne = (struct ext1_dir_entry *)(buf + off + actual);
                ne->inode = inode_num; ne->rec_len = old_rec_len - actual; ne->name_len = name_len;
                memcpy(ne->name, name, name_len);
                ext1_write_block(fs, phys, buf);
                kfree(buf); return 0;
            }
            off += e->rec_len;
        }
    }
    u32 phys = ext1_alloc_block(fs);
    if (phys) {
        ext1_set_block(fs, &dir, blocks, phys);
        dir.i_blocks += fs->block_size / 512; dir.i_size += fs->block_size;
        ext1_write_inode(fs, dir_ino, &dir);
        memset(buf, 0, fs->block_size);
        struct ext1_dir_entry *e = (struct ext1_dir_entry *)buf;
        e->inode = inode_num; e->rec_len = fs->block_size; e->name_len = name_len;
        memcpy(e->name, name, name_len);
        ext1_write_block(fs, phys, buf);
        kfree(buf); return 0;
    }
    kfree(buf); return -1;
}

static int ext1_vfs_create(struct vfs_node *dir, const char *name, const char *full_path, u32 mode);

static void ext1_setup_node(struct vfs_node *n, struct ext1_fs *fs, u32 ino, u32 mode) {
    struct ext1_inode_info *ni = kmalloc(sizeof(struct ext1_inode_info));
    ni->fs = fs; ni->inode_num = ino;
    n->inode->data = ni;
    n->inode->blk_dev = fs->bdev;
    if ((mode & EXT1_S_IFMT) == EXT1_S_IFDIR) {
        n->inode->create_cb = ext1_vfs_create;
    } else {
        n->inode->read_cb = ext1_vfs_read;
        if ((mode & EXT1_S_IFMT) != EXT1_S_IFLNK) n->inode->write_cb = ext1_vfs_write;
    }
}

static int ext1_vfs_create(struct vfs_node *dir, const char *name, const char *full_path, u32 mode) {
    (void)mode; (void)full_path;
    struct ext1_inode_info *dir_info = (struct ext1_inode_info *)dir->inode->data;
    struct ext1_fs *fs = dir_info->fs;
    u32 new_ino = ext1_alloc_inode(fs);
    if (!new_ino) return -1;
    struct ext1_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = EXT1_S_IFREG | 0644; inode.i_links_count = 1;
    ext1_write_inode(fs, new_ino, &inode);
    if (ext1_add_dir_entry(fs, dir_info->inode_num, new_ino, name) < 0) return -1;
    struct vfs_node *n = find_child(dir, name);
    if (n) {
        ext1_setup_node(n, fs, new_ino, inode.i_mode);
        vfs_node_put(n);
    }
    return 0;
}

static void ext1_populate_vfs(struct ext1_fs *fs, u32 ino, const char *base_path) {
    struct ext1_inode inode;
    if (ext1_read_inode(fs, ino, &inode) < 0) return;
    if ((inode.i_mode & EXT1_S_IFMT) != EXT1_S_IFDIR) return;
    u8 *buf = kmalloc(inode.i_size);
    for (u32 i = 0; i < (inode.i_size + fs->block_size - 1) / fs->block_size; i++) {
        u32 phys = ext1_get_block(fs, &inode, i);
        if (phys) ext1_read_block(fs, phys, buf + i * fs->block_size);
    }
    usize off = 0;
    while (off < inode.i_size) {
        struct ext1_dir_entry *e = (struct ext1_dir_entry *)(buf + off);
        if (e->rec_len == 0 || e->inode == 0) { off += e->rec_len ? e->rec_len : 4; continue; }
        char name[256]; memcpy(name, e->name, e->name_len); name[e->name_len] = '\0';
        if (strcmp(name, ".") && strcmp(name, "..")) {
            char full[256]; usize len = strlen(base_path);
            memcpy(full, base_path, len); if (full[len - 1] != '/') full[len++] = '/';
            memcpy(full + len, name, e->name_len + 1);
            struct ext1_inode ci;
            if (ext1_read_inode(fs, e->inode, &ci) == 0) {
                u32 fmt = ci.i_mode & EXT1_S_IFMT;
                struct vfs_node *n = vfs_add_node(full, (fmt == EXT1_S_IFDIR) ? VFS_DIRECTORY : VFS_FILE, 0, ci.i_size, 0);
                if (n) {
                    ext1_setup_node(n, fs, e->inode, ci.i_mode);
                    if (fmt == EXT1_S_IFDIR) ext1_populate_vfs(fs, e->inode, full);
                }
            }
        }
        off += e->rec_len;
    }
    kfree(buf);
}

static struct vfs_node *ext1_vfs_mount_cb(const char *source, u64 flags, void *data) {
    (void)flags; (void)data;
    struct block_device *dev = blk_get(source);
    if (!dev) return ERR_PTR(-ENODEV);
    u8 *sb_buf = kmalloc(1024);
    if (blk_read_cached(dev, 2, 2, sb_buf) < 0) { kfree(sb_buf); return ERR_PTR(-EIO); }
    struct ext1_superblock *sb = (struct ext1_superblock *)sb_buf;
    if (sb->s_magic != EXT1_SUPER_MAGIC || sb->s_rev_level != 0) { kfree(sb_buf); return ERR_PTR(-EINVAL); }
    struct ext1_fs *fs = kmalloc(sizeof(struct ext1_fs));
    fs->bdev = dev; memcpy(&fs->sb, sb, sizeof(struct ext1_superblock));
    fs->block_size = 1024 << fs->sb.s_log_block_size;
    fs->inodes_per_group = fs->sb.s_inodes_per_group;
    kfree(sb_buf);
    struct vfs_node *root = vfs_create_node(VFS_DIRECTORY);
    ext1_setup_node(root, fs, 2, EXT1_S_IFDIR);
    vfs_set_currently_mounting_root(root);
    if (data) ext1_populate_vfs(fs, 2, (const char *)data);
    return root;
}

static struct vfs_fs ext1_vfs = { .name = "ext1", .mount = ext1_vfs_mount_cb };

void ext1_init(void) {
    vfs_register_fs(&ext1_vfs);
    vfs_mount("virtio-blk0", "/ext1", "ext1", 0);
}
