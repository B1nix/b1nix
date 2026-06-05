#include <b1nix/exfat.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <b1nix/blk.h>
#include <b1nix/errno.h>
#include <string.h>

static struct exfat_fs *exfat_instances = NULL;

static u32 exfat_get_next_cluster(struct exfat_fs *fs, u32 cluster) {
    u32 sector_size = 1 << fs->bytes_per_sector_shift;
    u64 fat_sector = fs->fat_offset + ((u64)cluster * 4) / sector_size;
    u32 fat_offset_in_sector = ((u64)cluster * 4) % sector_size;

    u8 *sector_buf = kmalloc(sector_size);
    if (!sector_buf) return 0xFFFFFFFF;

    u32 blocks_per_sector = sector_size / fs->bdev->block_size;
    if (blk_read_cached(fs->bdev, fat_sector * blocks_per_sector, blocks_per_sector, sector_buf) < 0) {
        kfree(sector_buf);
        return 0xFFFFFFFF;
    }

    u32 next;
    memcpy(&next, sector_buf + fat_offset_in_sector, 4);
    kfree(sector_buf);
    return next;
}

static isize exfat_vfs_read(struct vfs_node *node, u64 offset, char *buffer, usize size, int flags) {
    (void)flags;
    struct exfat_inode_info *info = (struct exfat_inode_info *)node->inode->data;
    struct exfat_fs *fs = info->fs;

    if (offset >= info->size) return 0;
    if (offset + size > info->size) size = info->size - (usize)offset;

    u32 sector_size = 1 << fs->bytes_per_sector_shift;
    u32 cluster_size = sector_size << fs->sectors_per_cluster_shift;

    u32 cluster = info->first_cluster;

    u64 current_offset = 0;
    while (current_offset + cluster_size <= offset) {
        if (cluster < 2 || cluster > fs->cluster_count + 1) return 0;
        if (info->no_fat_chain) {
            cluster = cluster + 1;
        } else {
            cluster = exfat_get_next_cluster(fs, cluster);
        }
        current_offset += cluster_size;
    }

    usize bytes_read = 0;
    u8 *cluster_buf = kmalloc(cluster_size);
    if (!cluster_buf) return -ENOMEM;

    while (bytes_read < size) {
        if (cluster < 2 || cluster > fs->cluster_count + 1) break;

        u64 sector = fs->cluster_heap_offset + (u64)(cluster - 2) * (1 << fs->sectors_per_cluster_shift);
        u32 blocks_per_cluster = cluster_size / fs->bdev->block_size;

        if (blk_read_cached(fs->bdev, sector * (sector_size / fs->bdev->block_size), blocks_per_cluster, cluster_buf) < 0) {
            break;
        }

        u32 offset_in_cluster = (u32)(offset + bytes_read - current_offset);
        u32 to_copy = cluster_size - offset_in_cluster;
        if (to_copy > size - bytes_read) to_copy = (u32)(size - bytes_read);

        memcpy(buffer + bytes_read, cluster_buf + offset_in_cluster, to_copy);
        bytes_read += to_copy;

        if (bytes_read < size) {
            if (info->no_fat_chain) {
                cluster = cluster + 1;
            } else {
                cluster = exfat_get_next_cluster(fs, cluster);
            }
            current_offset += cluster_size;
        }
    }

    kfree(cluster_buf);
    return (isize)bytes_read;
}

static isize exfat_vfs_readdir(struct vfs_node *dir, usize offset, struct dirent *buf, usize max_entries) {
    struct exfat_inode_info *info = (struct exfat_inode_info *)dir->inode->data;
    struct exfat_fs *fs = info->fs;

    u32 sector_size = 1 << fs->bytes_per_sector_shift;
    u32 cluster_size = sector_size << fs->sectors_per_cluster_shift;

    u8 *cluster_buf = kmalloc(cluster_size);
    if (!cluster_buf) return -ENOMEM;

    usize count = 0;
    usize entry_idx = 0;

    u32 cluster = info->first_cluster;
    u8 dir_no_fat_chain = info->no_fat_chain;

    while (cluster >= 2 && cluster <= fs->cluster_count + 1 && count < max_entries) {
        u64 sector = fs->cluster_heap_offset + (u64)(cluster - 2) * (1 << fs->sectors_per_cluster_shift);
        u32 blocks_per_cluster = cluster_size / fs->bdev->block_size;

        if (blk_read_cached(fs->bdev, sector * (sector_size / fs->bdev->block_size), blocks_per_cluster, cluster_buf) < 0) {
            break;
        }

        u32 entry_index = 0;
        u32 entries_per_cluster = cluster_size / 32;

        while (entry_index < entries_per_cluster && count < max_entries) {
            struct exfat_dir_entry *entry = (struct exfat_dir_entry *)(cluster_buf + entry_index * 32);
            if (entry->type == 0x00) {
                cluster = 0xFFFFFFFF;
                break;
            }
            if (!(entry->type & 0x80)) {
                entry_index++;
                continue;
            }

            if (entry->type == 0x85) {
                struct exfat_entry_file *file_entry = (struct exfat_entry_file *)entry;
                u8 sec_count = file_entry->secondary_count;

                if (entry_index + sec_count >= entries_per_cluster) {
                    break;
                }

                struct exfat_entry_stream *stream_entry = (struct exfat_entry_stream *)(cluster_buf + (entry_index + 1) * 32);
                if (stream_entry->type == 0xC0) {
                    char filename[256];
                    memset(filename, 0, sizeof(filename));
                    int fn_idx = 0;

                    for (int s = 2; s <= sec_count; s++) {
                        struct exfat_entry_name *name_entry = (struct exfat_entry_name *)(cluster_buf + (entry_index + s) * 32);
                        if (name_entry->type == 0xC1) {
                            for (int c = 0; c < 15; c++) {
                                u16 utf16_char = name_entry->file_name[c];
                                char ascii = (char)(utf16_char & 0xFF);
                                if (ascii == '\0') break;
                                if (ascii >= 'A' && ascii <= 'Z') {
                                    ascii = ascii - 'A' + 'a';
                                }
                                if (fn_idx < 255) {
                                    filename[fn_idx++] = ascii;
                                }
                            }
                        }
                    }
                    filename[fn_idx] = '\0';

                    if (fn_idx > 0) {
                        if (entry_idx >= offset) {
                            strncpy(buf[count].name, filename, sizeof(buf[count].name));
                            buf[count].name[sizeof(buf[count].name) - 1] = '\0';
                            buf[count].type = (file_entry->file_attributes & 0x10) ? VFS_DIRECTORY : VFS_FILE;
                            buf[count].is_dir = ((file_entry->file_attributes & 0x10) != 0);
                            buf[count].size = stream_entry->data_length;
                            count++;
                        }
                        entry_idx++;
                    }
                }

                entry_index += 1 + sec_count;
            } else {
                entry_index++;
            }
        }

        if (cluster != 0xFFFFFFFF) {
            if (dir_no_fat_chain) {
                cluster = cluster + 1;
            } else {
                cluster = exfat_get_next_cluster(fs, cluster);
            }
        }
    }

    kfree(cluster_buf);
    return (isize)count;
}

static int exfat_vfs_statfs(struct vfs_node *node, struct b1nix_statfs *st) {
    struct exfat_inode_info *info = (struct exfat_inode_info *)node->inode->data;
    struct exfat_fs *fs = info->fs;

    memset(st, 0, sizeof(*st));
    st->f_type = 0x2011BAB0;
    st->f_bsize = (1 << fs->bytes_per_sector_shift) << fs->sectors_per_cluster_shift;
    st->f_blocks = fs->cluster_count;
    st->f_bfree = 0;
    st->f_bavail = 0;
    st->f_files = 0;
    st->f_ffree = 0;
    st->f_namelen = 255;
    return 0;
}

static void exfat_populate_vfs(struct exfat_fs *fs, u32 dir_cluster, u8 dir_no_fat_chain, const char *parent_path) {
    u32 sector_size = 1 << fs->bytes_per_sector_shift;
    u32 cluster_size = sector_size << fs->sectors_per_cluster_shift;

    u8 *cluster_buf = kmalloc(cluster_size);
    if (!cluster_buf) return;

    u32 cluster = dir_cluster;

    while (cluster >= 2 && cluster <= fs->cluster_count + 1) {
        u64 sector = fs->cluster_heap_offset + (u64)(cluster - 2) * (1 << fs->sectors_per_cluster_shift);
        u32 blocks_per_cluster = cluster_size / fs->bdev->block_size;

        if (blk_read_cached(fs->bdev, sector * (sector_size / fs->bdev->block_size), blocks_per_cluster, cluster_buf) < 0) {
            break;
        }

        u32 entry_index = 0;
        u32 entries_per_cluster = cluster_size / 32;

        while (entry_index < entries_per_cluster) {
            struct exfat_dir_entry *entry = (struct exfat_dir_entry *)(cluster_buf + entry_index * 32);
            if (entry->type == 0x00) {
                kfree(cluster_buf);
                return;
            }
            if (!(entry->type & 0x80)) {
                entry_index++;
                continue;
            }
            if (entry->type == 0x85) {
                struct exfat_entry_file *file_entry = (struct exfat_entry_file *)entry;
                u8 sec_count = file_entry->secondary_count;

                if (entry_index + sec_count >= entries_per_cluster) {
                    break;
                }

                struct exfat_entry_stream *stream_entry = (struct exfat_entry_stream *)(cluster_buf + (entry_index + 1) * 32);
                if (stream_entry->type == 0xC0) {
                    char filename[256];
                    memset(filename, 0, sizeof(filename));
                    int fn_idx = 0;

                    for (int s = 2; s <= sec_count; s++) {
                        struct exfat_entry_name *name_entry = (struct exfat_entry_name *)(cluster_buf + (entry_index + s) * 32);
                        if (name_entry->type == 0xC1) {
                            for (int c = 0; c < 15; c++) {
                                u16 utf16_char = name_entry->file_name[c];
                                char ascii = (char)(utf16_char & 0xFF);
                                if (ascii == '\0') break;
                                if (ascii >= 'A' && ascii <= 'Z') {
                                    ascii = ascii - 'A' + 'a';
                                }
                                if (fn_idx < 255) {
                                    filename[fn_idx++] = ascii;
                                }
                            }
                        }
                    }
                    filename[fn_idx] = '\0';

                    if (fn_idx > 0) {
                        char full_path[256];
                        usize plen = strlen(parent_path);
                        if (plen + strlen(filename) + 2 <= sizeof(full_path)) {
                            memcpy(full_path, parent_path, plen);
                            if (plen > 0 && full_path[plen - 1] != '/') {
                                full_path[plen++] = '/';
                            }
                            strcpy(full_path + plen, filename);

                            u16 attrs = file_entry->file_attributes;
                            u32 entry_cluster = stream_entry->first_cluster;
                            u64 size = stream_entry->data_length;
                            u8 no_fat_chain = (stream_entry->general_secondary_flags & 0x02) != 0;

                            if (attrs & 0x10) {
                                struct vfs_node *node = vfs_add_node(full_path, VFS_DIRECTORY, 0, 0, 0);
                                if (node && !IS_ERR(node)) {
                                    struct exfat_inode_info *info = kmalloc(sizeof(struct exfat_inode_info));
                                    info->fs = fs;
                                    info->first_cluster = entry_cluster;
                                    info->size = 0;
                                    info->no_fat_chain = no_fat_chain;
                                    info->is_dir = 1;

                                    node->inode->data = info;
                                    node->inode->flags |= VFS_NODE_OWNS_DATA;
                                    node->inode->readdir_cb = exfat_vfs_readdir;
                                    node->inode->statfs_cb = exfat_vfs_statfs;
                                    vfs_node_put(node);
                                }
                                exfat_populate_vfs(fs, entry_cluster, no_fat_chain, full_path);
                            } else {
                                struct vfs_node *node = vfs_add_node(full_path, VFS_FILE, 0, size, 0);
                                if (node && !IS_ERR(node)) {
                                    struct exfat_inode_info *info = kmalloc(sizeof(struct exfat_inode_info));
                                    info->fs = fs;
                                    info->first_cluster = entry_cluster;
                                    info->size = size;
                                    info->no_fat_chain = no_fat_chain;
                                    info->is_dir = 0;

                                    node->inode->data = info;
                                    node->inode->flags |= VFS_NODE_OWNS_DATA;
                                    node->inode->read_cb = exfat_vfs_read;
                                    node->inode->statfs_cb = exfat_vfs_statfs;
                                    vfs_node_put(node);
                                }
                            }
                        }
                    }
                }

                entry_index += 1 + sec_count;
            } else {
                entry_index++;
            }
        }

        if (dir_no_fat_chain) {
            cluster = cluster + 1;
        } else {
            cluster = exfat_get_next_cluster(fs, cluster);
        }
    }

    kfree(cluster_buf);
}

static struct vfs_node *exfat_vfs_mount_cb(const char *source, u64 flags, void *data) {
    (void)flags;
    struct block_device *dev = blk_get(source);
    if (!dev) return ERR_PTR(-ENODEV);

    u8 *boot_sector_buf = kmalloc(512);
    if (!boot_sector_buf) return ERR_PTR(-ENOMEM);

    if (blk_read_cached(dev, 0, 1, boot_sector_buf) < 0) {
        kfree(boot_sector_buf);
        return ERR_PTR(-EIO);
    }

    struct exfat_boot_sector *bs = (struct exfat_boot_sector *)boot_sector_buf;
    if (memcmp(bs->fs_name, "EXFAT   ", 8) != 0) {
        kfree(boot_sector_buf);
        return ERR_PTR(-EINVAL);
    }
    if (bs->signature != 0xAA55) {
        kfree(boot_sector_buf);
        return ERR_PTR(-EINVAL);
    }

    struct exfat_fs *fs = kmalloc(sizeof(struct exfat_fs));
    if (!fs) {
        kfree(boot_sector_buf);
        return ERR_PTR(-ENOMEM);
    }
    memset(fs, 0, sizeof(struct exfat_fs));

    fs->bdev = dev;

    memcpy(&fs->partition_offset, &bs->partition_offset, 8);
    memcpy(&fs->volume_length, &bs->volume_length, 8);
    memcpy(&fs->fat_offset, &bs->fat_offset, 4);
    memcpy(&fs->fat_length, &bs->fat_length, 4);
    memcpy(&fs->cluster_heap_offset, &bs->cluster_heap_offset, 4);
    memcpy(&fs->cluster_count, &bs->cluster_count, 4);
    memcpy(&fs->root_dir_cluster, &bs->root_dir_cluster, 4);

    fs->bytes_per_sector_shift = bs->bytes_per_sector_shift;
    fs->sectors_per_cluster_shift = bs->sectors_per_cluster_shift;
    fs->number_of_fats = bs->number_of_fats;

    kfree(boot_sector_buf);

    struct vfs_node *root = vfs_create_node(VFS_DIRECTORY);
    if (!root) {
        kfree(fs);
        return ERR_PTR(-ENOMEM);
    }

    struct exfat_inode_info *info = kmalloc(sizeof(struct exfat_inode_info));
    if (!info) {
        vfs_node_put(root);
        kfree(fs);
        return ERR_PTR(-ENOMEM);
    }
    info->fs = fs;
    info->first_cluster = fs->root_dir_cluster;
    info->size = 0;
    info->no_fat_chain = 0;
    info->is_dir = 1;

    root->inode->data = info;
    root->inode->flags |= VFS_NODE_OWNS_DATA;
    root->inode->readdir_cb = exfat_vfs_readdir;
    root->inode->statfs_cb = exfat_vfs_statfs;

    fs->next = exfat_instances;
    exfat_instances = fs;

    vfs_set_currently_mounting_root(root);
    if (data) {
        exfat_populate_vfs(fs, fs->root_dir_cluster, 0, (const char *)data);
    }

    return root;
}

static struct vfs_fs exfat_vfs = {
    .name = "exfat",
    .mount = exfat_vfs_mount_cb,
};

static struct vfs_fs exfat_vfs_alias = {
    .name = "exFAT",
    .mount = exfat_vfs_mount_cb,
};

void exfat_init(void) {
    vfs_register_fs(&exfat_vfs);
    vfs_register_fs(&exfat_vfs_alias);
}
