#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/ext1.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <string.h>

/*
 * Ext1 (Original EXT Filesystem) — Full Read/Write Driver.
 *
 * Key differences from Ext2:
 *   - 128-byte fixed-size inodes (no i_extra_isize)
 *   - No dynamic superblock fields (rev_level == 0)
 *   - No file_type in directory entries
 *   - Only 15 block pointers
 *   - No i_generation, i_file_acl, i_dir_acl, i_faddr in inode
 *   - Superblock is smaller (ends after s_def_resgid)
 */

static struct block_device *ext1_dev = 0;
static struct ext1_superblock ext1_sb;
static u32 ext1_block_size;
static u32 ext1_inodes_per_group;

/* ── Block I/O ── */

static int ext1_read_block(u32 block, void *buffer)
{
    u64 lba = (u64)block * (ext1_block_size / 512);
    return blk_read_cached(ext1_dev, lba, ext1_block_size / 512, buffer);
}

static int ext1_write_block(u32 block, const void *buffer)
{
    u64 lba = (u64)block * (ext1_block_size / 512);
    return blk_write_cached(ext1_dev, lba, ext1_block_size / 512, buffer);
}

/* ── Block Group Descriptor helpers ── */

static void ext1_read_bgd(u32 group, struct ext1_bgd *bgd)
{
    u32 bg_desc_block = (ext1_block_size == 1024) ? 2 : 1;
    u32 bg_offset = group * sizeof(struct ext1_bgd);
    u32 bg_block = bg_desc_block + (bg_offset / ext1_block_size);

    u8 *buf = kmalloc(ext1_block_size);
    ext1_read_block(bg_block, buf);
    memcpy(bgd, buf + (bg_offset % ext1_block_size), sizeof(struct ext1_bgd));
    kfree(buf);
}

static void ext1_write_bgd(u32 group, struct ext1_bgd *bgd)
{
    u32 bg_desc_block = (ext1_block_size == 1024) ? 2 : 1;
    u32 bg_offset = group * sizeof(struct ext1_bgd);
    u32 bg_block = bg_desc_block + (bg_offset / ext1_block_size);

    u8 *buf = kmalloc(ext1_block_size);
    ext1_read_block(bg_block, buf);
    memcpy(buf + (bg_offset % ext1_block_size), bgd, sizeof(struct ext1_bgd));
    ext1_write_block(bg_block, buf);
    kfree(buf);
}

static void ext1_write_superblock(void)
{
    u8 *sb = kmalloc(1024);
    if (blk_read_cached(ext1_dev, 2, 2, sb) >= 0) {
        memcpy(sb, &ext1_sb, sizeof(struct ext1_superblock));
        blk_write_cached(ext1_dev, 2, 2, sb);
    }
    kfree(sb);
}

/* ── Inode operations ── */

static int ext1_read_inode(u32 inode_num, struct ext1_inode *inode)
{
    if (inode_num == 0) return -1;

    u32 group = (inode_num - 1) / ext1_inodes_per_group;
    u32 index = (inode_num - 1) % ext1_inodes_per_group;

    struct ext1_bgd bgd;
    ext1_read_bgd(group, &bgd);
    u32 inode_table_block = bgd.bg_inode_table;

    u32 inode_offset = index * 128;
    u32 block_idx = inode_table_block + (inode_offset / ext1_block_size);

    u8 *buf = kmalloc(ext1_block_size);
    if (ext1_read_block(block_idx, buf) < 0) { kfree(buf); return -1; }
    memcpy(inode, buf + (inode_offset % ext1_block_size), sizeof(struct ext1_inode));
    kfree(buf);
    return 0;
}

static int ext1_write_inode(u32 inode_num, const struct ext1_inode *inode)
{
    if (inode_num == 0) return -1;

    u32 group = (inode_num - 1) / ext1_inodes_per_group;
    u32 index = (inode_num - 1) % ext1_inodes_per_group;

    struct ext1_bgd bgd;
    ext1_read_bgd(group, &bgd);
    u32 inode_table_block = bgd.bg_inode_table;

    u32 inode_offset = index * 128;
    u32 block_idx = inode_table_block + (inode_offset / ext1_block_size);

    u8 *buf = kmalloc(ext1_block_size);
    if (ext1_read_block(block_idx, buf) < 0) { kfree(buf); return -1; }
    memcpy(buf + (inode_offset % ext1_block_size), inode, sizeof(struct ext1_inode));
    int ret = ext1_write_block(block_idx, buf);
    kfree(buf);
    return ret;
}

/* ── Block allocation ── */

static u32 ext1_alloc_block(void)
{
    u32 groups = (ext1_sb.s_blocks_count + ext1_sb.s_blocks_per_group - 1) / ext1_sb.s_blocks_per_group;
    for (u32 g = 0; g < groups; g++) {
        struct ext1_bgd bgd;
        ext1_read_bgd(g, &bgd);
        if (bgd.bg_free_blocks_count == 0) continue;

        u8 *bitmap = kmalloc(ext1_block_size);
        ext1_read_block(bgd.bg_block_bitmap, bitmap);

        for (u32 i = 0; i < ext1_sb.s_blocks_per_group; i++) {
            if (!(bitmap[i / 8] & (1 << (i % 8)))) {
                bitmap[i / 8] |= (1 << (i % 8));
                ext1_write_block(bgd.bg_block_bitmap, bitmap);
                kfree(bitmap);

                bgd.bg_free_blocks_count--;
                ext1_write_bgd(g, &bgd);
                ext1_sb.s_free_blocks_count--;
                ext1_write_superblock();

                u32 block_num = g * ext1_sb.s_blocks_per_group + i;
                if (ext1_sb.s_first_data_block == 1 && ext1_sb.s_log_block_size == 0)
                    block_num++;

                u8 *zero = kzalloc(ext1_block_size);
                ext1_write_block(block_num, zero);
                kfree(zero);
                return block_num;
            }
        }
        kfree(bitmap);
    }
    return 0;
}

/* ── Inode allocation ── */

static u32 ext1_alloc_inode(void)
{
    u32 groups = (ext1_sb.s_inodes_count + ext1_inodes_per_group - 1) / ext1_inodes_per_group;
    for (u32 g = 0; g < groups; g++) {
        struct ext1_bgd bgd;
        ext1_read_bgd(g, &bgd);
        if (bgd.bg_free_inodes_count == 0) continue;

        u8 *bitmap = kmalloc(ext1_block_size);
        ext1_read_block(bgd.bg_inode_bitmap, bitmap);

        for (u32 i = 0; i < ext1_inodes_per_group; i++) {
            if (!(bitmap[i / 8] & (1 << (i % 8)))) {
                bitmap[i / 8] |= (1 << (i % 8));
                ext1_write_block(bgd.bg_inode_bitmap, bitmap);
                kfree(bitmap);

                bgd.bg_free_inodes_count--;
                ext1_write_bgd(g, &bgd);
                ext1_sb.s_free_inodes_count--;
                ext1_write_superblock();

                u32 inode_num = g * ext1_inodes_per_group + i + 1;
                struct ext1_inode ni;
                memset(&ni, 0, sizeof(ni));
                ext1_write_inode(inode_num, &ni);
                return inode_num;
            }
        }
        kfree(bitmap);
    }
    return 0;
}

/* ── Block pointer resolution (indirect) ── */

static u32 ext1_get_block(struct ext1_inode *inode, u32 block_idx)
{
    if (block_idx < EXT1_NDIR_BLOCKS) return inode->i_block[block_idx];

    u32 ptrs = ext1_block_size / 4;
    block_idx -= EXT1_NDIR_BLOCKS;

    if (block_idx < ptrs) {
        if (!inode->i_block[EXT1_IND_BLOCK]) return 0;
        u32 *ind = kmalloc(ext1_block_size);
        ext1_read_block(inode->i_block[EXT1_IND_BLOCK], ind);
        u32 r = ind[block_idx];
        kfree(ind);
        return r;
    }
    block_idx -= ptrs;

    if (block_idx < ptrs * ptrs) {
        if (!inode->i_block[EXT1_DIND_BLOCK]) return 0;
        u32 *dind = kmalloc(ext1_block_size);
        ext1_read_block(inode->i_block[EXT1_DIND_BLOCK], dind);
        if (!dind[block_idx / ptrs]) { kfree(dind); return 0; }
        u32 *ind = kmalloc(ext1_block_size);
        ext1_read_block(dind[block_idx / ptrs], ind);
        u32 r = ind[block_idx % ptrs];
        kfree(ind); kfree(dind);
        return r;
    }
    return 0;
}

/* Allocate block pointer at given index (create indirect tree if needed) */
static u32 ext1_set_block(struct ext1_inode *inode, u32 block_idx, u32 phys)
{
    u32 ptrs = ext1_block_size / 4;

    if (block_idx < EXT1_NDIR_BLOCKS) {
        inode->i_block[block_idx] = phys;
        return 1;
    }

    block_idx -= EXT1_NDIR_BLOCKS;
    if (block_idx < ptrs) {
        if (!inode->i_block[EXT1_IND_BLOCK]) {
            u32 b = ext1_alloc_block();
            if (!b) return 0;
            inode->i_block[EXT1_IND_BLOCK] = b;
        }
        u32 *ind = kmalloc(ext1_block_size);
        ext1_read_block(inode->i_block[EXT1_IND_BLOCK], ind);
        ind[block_idx] = phys;
        ext1_write_block(inode->i_block[EXT1_IND_BLOCK], ind);
        kfree(ind);
        return 1;
    }

    block_idx -= ptrs;
    if (block_idx < ptrs * ptrs) {
        if (!inode->i_block[EXT1_DIND_BLOCK]) {
            u32 b = ext1_alloc_block();
            if (!b) return 0;
            inode->i_block[EXT1_DIND_BLOCK] = b;
        }
        u32 *dind = kmalloc(ext1_block_size);
        ext1_read_block(inode->i_block[EXT1_DIND_BLOCK], dind);
        u32 idx1 = block_idx / ptrs;
        if (!dind[idx1]) {
            u32 b = ext1_alloc_block();
            if (!b) { kfree(dind); return 0; }
            dind[idx1] = b;
            ext1_write_block(inode->i_block[EXT1_DIND_BLOCK], dind);
        }
        u32 *ind = kmalloc(ext1_block_size);
        ext1_read_block(dind[idx1], ind);
        ind[block_idx % ptrs] = phys;
        ext1_write_block(dind[idx1], ind);
        kfree(ind); kfree(dind);
        return 1;
    }

    return 0;
}

/* ── Read callback ── */

static isize ext1_vfs_read(struct vfs_node *node, u64 offset, char *buffer, usize size)
{
    u32 ino = (u32)(usize)node->data;
    struct ext1_inode inode;
    if (ext1_read_inode(ino, &inode) < 0) return -1;

    if (offset >= inode.i_size) return 0;

    /* Symlink inline */
    if ((inode.i_mode & EXT1_S_IFMT) == EXT1_S_IFLNK && inode.i_size < 60) {
        usize to_copy = size < (usize)inode.i_size ? size : (usize)inode.i_size;
        memcpy(buffer, (char *)inode.i_block, to_copy);
        return (isize)to_copy;
    }

    usize remaining = inode.i_size - offset;
    usize to_read = size < remaining ? size : remaining;
    usize done = 0;

    u8 *block_buf = kmalloc(ext1_block_size);
    while (done < to_read) {
        u32 b_idx = (offset + done) / ext1_block_size;
        u32 b_off = (offset + done) % ext1_block_size;
        u32 phys = ext1_get_block(&inode, b_idx);
        usize chunk = ext1_block_size - b_off;
        if (chunk > to_read - done) chunk = to_read - done;

        if (phys) {
            ext1_read_block(phys, block_buf);
            memcpy(buffer + done, block_buf + b_off, chunk);
        } else {
            memset(buffer + done, 0, chunk);
        }
        done += chunk;
    }
    kfree(block_buf);
    return done;
}

/* ── Write callback ── */

static isize ext1_vfs_write(struct vfs_node *node, u64 offset, const char *buffer, usize size)
{
    u32 ino = (u32)(usize)node->data;
    struct ext1_inode inode;
    if (ext1_read_inode(ino, &inode) < 0) return -1;

    u64 new_size = offset + size;
    if (new_size > inode.i_size) {
        u32 old_blks = (inode.i_size + ext1_block_size - 1) / ext1_block_size;
        u32 new_blks = (new_size + ext1_block_size - 1) / ext1_block_size;

        for (u32 b = old_blks; b < new_blks; b++) {
            u32 pblk = ext1_alloc_block();
            if (!pblk) return -1;
            if (!ext1_set_block(&inode, b, pblk)) return -1;
            inode.i_blocks += ext1_block_size / 512;
        }
        inode.i_size = (u32)new_size;
        ext1_write_inode(ino, &inode);
        node->size = inode.i_size;
    }

    usize done = 0;
    u8 *block_buf = kmalloc(ext1_block_size);
    while (done < size) {
        u32 b_idx = (offset + done) / ext1_block_size;
        u32 b_off = (offset + done) % ext1_block_size;
        u32 phys = ext1_get_block(&inode, b_idx);
        if (!phys) break;

        usize chunk = ext1_block_size - b_off;
        if (chunk > size - done) chunk = size - done;

        if (chunk < ext1_block_size) ext1_read_block(phys, block_buf);
        memcpy(block_buf + b_off, buffer + done, chunk);
        ext1_write_block(phys, block_buf);
        done += chunk;
    }
    kfree(block_buf);
    return done;
}

/* ── Directory entry operations ── */

static int ext1_add_dir_entry(u32 dir_ino, u32 inode_num, const char *name)
{
    struct ext1_inode dir;
    if (ext1_read_inode(dir_ino, &dir) < 0) return -1;

    usize name_len = strlen(name);
    if (name_len > 255) name_len = 255;
    u32 needed = 8 + ((name_len + 3) & ~3);

    u8 *buf = kmalloc(ext1_block_size);
    u32 blocks = (dir.i_size + ext1_block_size - 1) / ext1_block_size;

    if (blocks == 0) {
        u32 phys = ext1_alloc_block();
        if (!phys) { kfree(buf); return -1; }
        ext1_set_block(&dir, 0, phys);
        dir.i_blocks += ext1_block_size / 512;
        dir.i_size = ext1_block_size;
        ext1_write_inode(dir_ino, &dir);

        memset(buf, 0, ext1_block_size);
        struct ext1_dir_entry *e = (struct ext1_dir_entry *)buf;
        e->inode = inode_num;
        e->rec_len = ext1_block_size;
        e->name_len = name_len;
        memcpy(e->name, name, name_len);
        ext1_write_block(phys, buf);
        kfree(buf);
        return 0;
    }

    for (u32 b = 0; b < blocks; b++) {
        u32 phys = ext1_get_block(&dir, b);
        if (!phys) continue;
        ext1_read_block(phys, buf);
        usize off = 0;
        while (off < ext1_block_size) {
            struct ext1_dir_entry *e = (struct ext1_dir_entry *)(buf + off);
            if (e->rec_len == 0) break;
            u32 actual = 8 + ((e->name_len + 3) & ~3);
            if (e->rec_len >= actual + needed) {
                e->rec_len = actual;
                struct ext1_dir_entry *ne = (struct ext1_dir_entry *)(buf + off + actual);
                ne->inode = inode_num;
                ne->rec_len = e->rec_len - actual;
                ne->name_len = name_len;
                memcpy(ne->name, name, name_len);
                ext1_write_block(phys, buf);
                kfree(buf);
                return 0;
            }
            off += e->rec_len;
        }
    }

    u32 phys = ext1_alloc_block();
    if (phys) {
        ext1_set_block(&dir, blocks, phys);
        dir.i_blocks += ext1_block_size / 512;
        dir.i_size += ext1_block_size;
        ext1_write_inode(dir_ino, &dir);

        memset(buf, 0, ext1_block_size);
        struct ext1_dir_entry *e = (struct ext1_dir_entry *)buf;
        e->inode = inode_num;
        e->rec_len = ext1_block_size;
        e->name_len = name_len;
        memcpy(e->name, name, name_len);
        ext1_write_block(phys, buf);
        kfree(buf);
        return 0;
    }

    kfree(buf);
    return -1;
}

static int ext1_vfs_create(struct vfs_node *dir, const char *name, const char *full_path)
{
    (void)full_path;
    u32 dir_ino = (u32)(usize)dir->data;
    u32 new_ino = ext1_alloc_inode();
    if (!new_ino) return -1;

    struct ext1_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = EXT1_S_IFREG | 0644;
    inode.i_links_count = 1;
    ext1_write_inode(new_ino, &inode);

    if (ext1_add_dir_entry(dir_ino, new_ino, name) < 0) return -1;

    struct vfs_node *n = vfs_add_node(full_path, VFS_FILE, (void *)(usize)new_ino, 0, 0);
    if (n) { n->read_cb = ext1_vfs_read; n->write_cb = ext1_vfs_write; }
    return 0;
}

/* ── VFS population ── */

static void ext1_populate_vfs(u32 ino, const char *base_path)
{
    struct ext1_inode inode;
    if (ext1_read_inode(ino, &inode) < 0) return;
    if ((inode.i_mode & EXT1_S_IFMT) != EXT1_S_IFDIR) return;

    u8 *buf = kmalloc(inode.i_size);
    for (u32 i = 0; i < (inode.i_size + ext1_block_size - 1) / ext1_block_size; i++) {
        u32 phys = ext1_get_block(&inode, i);
        if (phys) ext1_read_block(phys, buf + i * ext1_block_size);
    }

    usize off = 0;
    while (off < inode.i_size) {
        struct ext1_dir_entry *e = (struct ext1_dir_entry *)(buf + off);
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

            struct ext1_inode ci;
            if (ext1_read_inode(e->inode, &ci) == 0) {
                u32 fmt = ci.i_mode & EXT1_S_IFMT;
                if (fmt == EXT1_S_IFDIR) {
                    struct vfs_node *dn = vfs_add_node(full, VFS_DIRECTORY, (void *)(usize)e->inode, 0, 0);
                    if (dn) dn->create_cb = ext1_vfs_create;
                    ext1_populate_vfs(e->inode, full);
                } else if (fmt == EXT1_S_IFLNK) {
                    struct vfs_node *sn = vfs_add_node(full, VFS_FILE, (void *)(usize)e->inode, ci.i_size, 0);
                    if (sn) sn->read_cb = ext1_vfs_read;
                } else {
                    struct vfs_node *n = vfs_add_node(full, VFS_FILE, (void *)(usize)e->inode, ci.i_size, 0);
                    if (n) { n->read_cb = ext1_vfs_read; n->write_cb = ext1_vfs_write; }
                }
            }
        }
        off += e->rec_len;
    }
    kfree(buf);
}

/* ── Init ── */

void ext1_init(void)
{
    ext1_dev = blk_get("virtio-blk0");
    if (!ext1_dev) ext1_dev = blk_get("sata0");
    if (!ext1_dev) return;

    u8 *sb = kmalloc(1024);
    if (blk_read_cached(ext1_dev, 2, 2, sb) < 0) { kfree(sb); return; }
    memcpy(&ext1_sb, sb, sizeof(struct ext1_superblock));
    kfree(sb);

    if (ext1_sb.s_magic != EXT1_SUPER_MAGIC) return;
    if (ext1_sb.s_rev_level != 0) return;

    ext1_block_size = 1024 << ext1_sb.s_log_block_size;
    ext1_inodes_per_group = ext1_sb.s_inodes_per_group;

    console_write("ext1: mounted, block_size=");
    console_write_dec(ext1_block_size);
    console_write("\n");

    struct vfs_node *root = vfs_add_node("/ext1", VFS_DIRECTORY, (void *)(usize)2, 0, 0);
    if (root) root->create_cb = ext1_vfs_create;
    ext1_populate_vfs(2, "/ext1");
}
