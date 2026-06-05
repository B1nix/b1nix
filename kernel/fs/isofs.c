#include <b1nix/isofs.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <b1nix/blk.h>
#include <b1nix/errno.h>
#include <string.h>


struct isofs_inode_info {
    struct block_device *bdev;
    u32 extent_lba;
    u32 data_len;
    u8 is_dir;
};

static int normalize_iso_name(const char *in, int len, char *out, int max_len) {
    if (max_len <= 0) return -EINVAL;
    if (len == 1 && in[0] == '\0') {
        strncpy(out, ".", max_len);
        out[max_len - 1] = '\0';
        return 0;
    }
    if (len == 1 && in[0] == '\1') {
        strncpy(out, "..", max_len);
        out[max_len - 1] = '\0';
        return 0;
    }

    int out_idx = 0;
    for (int i = 0; i < len; i++) {
        char c = in[i];
        if (c == ';') {
            break;
        }
        if (out_idx >= max_len - 1) {
            return -ENAMETOOLONG;
        }
        if (c >= 'A' && c <= 'Z') {
            c = c - 'A' + 'a';
        }
        out[out_idx++] = c;
    }
    out[out_idx] = '\0';

    if (out_idx > 0 && out[out_idx - 1] == '.') {
        out[out_idx - 1] = '\0';
    }

    return 0;
}

static int isofs_read_sector(struct block_device *dev, u32 sector, void *buf) {
    u32 blocks_per_sector = 2048 / dev->block_size;
    return blk_read_cached(dev, (u64)sector * blocks_per_sector, blocks_per_sector, buf);
}

static isize isofs_vfs_read(struct vfs_node *node, u64 offset, char *buffer, usize size, int flags) {
    (void)flags;
    struct isofs_inode_info *info = (struct isofs_inode_info *)node->inode->data;
    struct block_device *dev = info->bdev;

    if (offset >= info->data_len) return 0;
    if (offset + size > info->data_len) size = info->data_len - (usize)offset;

    u8 *sector_buf = kmalloc(2048);
    if (!sector_buf) return -ENOMEM;

    usize bytes_read = 0;
    while (bytes_read < size) {
        u64 current_offset = offset + bytes_read;
        u32 sector_index = (u32)(current_offset / 2048);
        u32 offset_in_sector = (u32)(current_offset % 2048);

        if (isofs_read_sector(dev, info->extent_lba + sector_index, sector_buf) < 0) {
            break;
        }

        u32 to_copy = 2048 - offset_in_sector;
        if (to_copy > size - bytes_read) {
            to_copy = (u32)(size - bytes_read);
        }

        memcpy(buffer + bytes_read, sector_buf + offset_in_sector, to_copy);
        bytes_read += to_copy;
    }

    kfree(sector_buf);
    return (isize)bytes_read;
}

static isize isofs_vfs_readdir(struct vfs_node *dir, usize offset, struct dirent *buf, usize max_entries) {
    struct isofs_inode_info *info = (struct isofs_inode_info *)dir->inode->data;
    struct block_device *dev = info->bdev;

    u8 *sector_buf = kmalloc(2048);
    if (!sector_buf) return -ENOMEM;

    usize count = 0;
    usize entry_idx = 0;

    u32 num_sectors = (info->data_len + 2047) / 2048;
    for (u32 s = 0; s < num_sectors && count < max_entries; s++) {
        if (isofs_read_sector(dev, info->extent_lba + s, sector_buf) < 0) {
            break;
        }

        u32 p = 0;
        while (p < 2048 && count < max_entries) {
            u8 len = sector_buf[p];
            if (len == 0) {
                break;
            }
            if (p + len > 2048 || len < 33) {
                break;
            }

            u8 name_len = sector_buf[p + 32];
            char *name_ptr = (char *)(sector_buf + p + 33);
            u8 flags = sector_buf[p + 25];
            u32 size = *(u32 *)(sector_buf + p + 10);
            if (name_len > len - 33) {
                break;
            }

            if (entry_idx >= offset) {
                char normal_name[64];
                if (normalize_iso_name(name_ptr, name_len, normal_name, sizeof(normal_name)) < 0) {
                    entry_idx++;
                    p += len;
                    continue;
                }

                strncpy(buf[count].name, normal_name, sizeof(buf[count].name));
                buf[count].name[sizeof(buf[count].name) - 1] = '\0';
                buf[count].type = (flags & 0x02) ? VFS_DIRECTORY : VFS_FILE;
                buf[count].is_dir = ((flags & 0x02) != 0);
                buf[count].size = size;
                count++;
            }
            entry_idx++;
            p += len;
        }
    }

    kfree(sector_buf);
    return (isize)count;
}

static int isofs_vfs_statfs(struct vfs_node *node, struct b1nix_statfs *st) {
    (void)node;
    memset(st, 0, sizeof(*st));
    st->f_type = 0x9660;
    st->f_bsize = 2048;
    st->f_blocks = 0;
    st->f_bfree = 0;
    st->f_bavail = 0;
    st->f_files = 0;
    st->f_ffree = 0;
    st->f_namelen = 30;
    return 0;
}

static void isofs_populate_vfs(struct block_device *dev, u32 dir_lba, u32 dir_len, const char *parent_path) {
    u8 *sector_buf = kmalloc(2048);
    if (!sector_buf) return;

    u32 num_sectors = (dir_len + 2047) / 2048;
    for (u32 s = 0; s < num_sectors; s++) {
        if (isofs_read_sector(dev, dir_lba + s, sector_buf) < 0) {
            break;
        }

        u32 p = 0;
        while (p < 2048) {
            u8 len = sector_buf[p];
            if (len == 0) {
                break;
            }
            if (p + len > 2048 || len < 33) {
                break;
            }

            u8 name_len = sector_buf[p + 32];
            char *name_ptr = (char *)(sector_buf + p + 33);
            u8 flags = sector_buf[p + 25];
            u32 extent_lba = *(u32 *)(sector_buf + p + 2);
            u32 data_len = *(u32 *)(sector_buf + p + 10);
            if (name_len > len - 33) {
                break;
            }

            if ((name_len == 1 && name_ptr[0] == '\0') || (name_len == 1 && name_ptr[0] == '\1')) {
                p += len;
                continue;
            }

            char normal_name[64];
            if (normalize_iso_name(name_ptr, name_len, normal_name, sizeof(normal_name)) < 0) {
                p += len;
                continue;
            }

            usize plen = strlen(parent_path);
            if (plen + strlen(normal_name) + 2 > 256) {
                /* Exceeds maximum VFS component path size in populate limit, skip it */
                p += len;
                continue;
            }

            char full_path[256];
            memcpy(full_path, parent_path, plen);
            if (plen > 0 && full_path[plen - 1] != '/') {
                full_path[plen++] = '/';
            }
            strcpy(full_path + plen, normal_name);

            if (flags & 0x02) {
                struct vfs_node *node = vfs_add_node(full_path, VFS_DIRECTORY, 0, 0, 0);
                if (node && !IS_ERR(node)) {
                    struct isofs_inode_info *info = kmalloc(sizeof(struct isofs_inode_info));
                    if (!info) {
                        vfs_node_put(node);
                        p += len;
                        continue;
                    }
                    info->bdev = dev;
                    info->extent_lba = extent_lba;
                    info->data_len = data_len;
                    info->is_dir = 1;
                    node->inode->data = info;
                    node->inode->flags |= VFS_NODE_OWNS_DATA;
                    node->inode->readdir_cb = isofs_vfs_readdir;
                    node->inode->statfs_cb = isofs_vfs_statfs;
                    vfs_node_put(node);
                }
                isofs_populate_vfs(dev, extent_lba, data_len, full_path);
            } else {
                struct vfs_node *node = vfs_add_node(full_path, VFS_FILE, 0, data_len, 0);
                if (node && !IS_ERR(node)) {
                    struct isofs_inode_info *info = kmalloc(sizeof(struct isofs_inode_info));
                    if (!info) {
                        vfs_node_put(node);
                        p += len;
                        continue;
                    }
                    info->bdev = dev;
                    info->extent_lba = extent_lba;
                    info->data_len = data_len;
                    info->is_dir = 0;
                    node->inode->data = info;
                    node->inode->flags |= VFS_NODE_OWNS_DATA;
                    node->inode->read_cb = isofs_vfs_read;
                    node->inode->statfs_cb = isofs_vfs_statfs;
                    vfs_node_put(node);
                }
            }

            p += len;
        }
    }

    kfree(sector_buf);
}

static struct vfs_node *isofs_vfs_mount_cb(const char *source, u64 flags, void *data) {
    (void)flags;
    struct block_device *dev = blk_get(source);
    if (!dev) return ERR_PTR(-ENODEV);

    u8 *pvd_buf = kmalloc(2048);
    if (!pvd_buf) return ERR_PTR(-ENOMEM);

    if (isofs_read_sector(dev, 16, pvd_buf) < 0) {
        kfree(pvd_buf);
        return ERR_PTR(-EIO);
    }

    if (pvd_buf[0] != 1 || memcmp(pvd_buf + 1, "CD001", 5) != 0) {
        kfree(pvd_buf);
        return ERR_PTR(-EINVAL);
    }

    u32 root_lba = *(u32 *)(pvd_buf + 156 + 2);
    u32 root_len = *(u32 *)(pvd_buf + 156 + 10);
    kfree(pvd_buf);

    struct vfs_node *root = vfs_create_node(VFS_DIRECTORY);
    if (!root) return ERR_PTR(-ENOMEM);

    struct isofs_inode_info *info = kmalloc(sizeof(struct isofs_inode_info));
    if (!info) {
        vfs_node_put(root);
        return ERR_PTR(-ENOMEM);
    }
    info->bdev = dev;
    info->extent_lba = root_lba;
    info->data_len = root_len;
    info->is_dir = 1;

    root->inode->data = info;
    root->inode->flags |= VFS_NODE_OWNS_DATA;
    root->inode->readdir_cb = isofs_vfs_readdir;
    root->inode->statfs_cb = isofs_vfs_statfs;

    vfs_set_currently_mounting_root(root);
    if (data) {
        isofs_populate_vfs(dev, root_lba, root_len, (const char *)data);
    }

    return root;
}

static struct vfs_fs isofs_vfs = {
    .name = "iso9660",
    .mount = isofs_vfs_mount_cb,
};

static struct vfs_fs isofs_vfs_alias = {
    .name = "isofs",
    .mount = isofs_vfs_mount_cb,
};

void isofs_init(void) {
    vfs_register_fs(&isofs_vfs);
    vfs_register_fs(&isofs_vfs_alias);
}
