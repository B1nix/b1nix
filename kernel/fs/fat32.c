#include <b1nix/fat32.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <b1nix/blk.h>
#include <b1nix/errno.h>
#include <string.h>

struct fat32_bpb {
	u8 jmp[3];
	char oem[8];
	u16 bytes_per_sector;
	u8 sectors_per_cluster;
	u16 reserved_sectors;
	u8 fat_count;
	u16 root_dir_entries;
	u16 total_sectors_16;
	u8 media_descriptor;
	u16 sectors_per_fat_16;
	u16 sectors_per_track;
	u16 heads;
	u32 hidden_sectors;
	u32 total_sectors_32;

	// FAT32 Extended fields
	u32 sectors_per_fat_32;
	u16 ext_flags;
	u16 fs_version;
	u32 root_cluster;
	u16 fs_info;
	u16 backup_boot_sector;
	u8 reserved[12];
	u8 drive_number;
	u8 reserved1;
	u8 boot_signature;
	u32 volume_id;
	char volume_label[11];
	char fs_type[8];
} __attribute__((packed));

struct fat32_dir_entry {
	char name[11];
	u8 attr;
	u8 reserved;
	u8 create_time_tenths;
	u16 create_time;
	u16 create_date;
	u16 access_date;
	u16 cluster_high;
	u16 mod_time;
	u16 mod_date;
	u16 cluster_low;
	u32 size;
} __attribute__((packed));

#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LFN       0x0F

static struct fat32_fs *fat32_instances = NULL;

static u32 cluster_to_sector(struct fat32_fs *fs, u32 cluster) {
	return fs->data_start_sector + (cluster - 2) * fs->sectors_per_cluster;
}

static u32 get_next_cluster(struct fat32_fs *fs, u32 cluster) {
	/* Reject an out-of-range cluster (corrupt/crafted FAT) — walking it would
	 * read a wild FAT sector. Returning an end-of-chain marker terminates the
	 * caller's chain walk safely (R3-9). */
	if (cluster < 2 || (fs->total_clusters && cluster >= fs->total_clusters + 2))
		return 0x0FFFFFFF;

	u32 fat_sector = fs->fat_start_sector + (cluster * 4) / fs->bytes_per_sector;
	u32 fat_offset = (cluster * 4) % fs->bytes_per_sector;
	u8 sector_buf[512];

	if (blk_read_cached(fs->bdev, fat_sector, 1, sector_buf) < 0)
        return 0x0FFFFFF7; // Bad cluster

	u32 next = *(u32 *)(sector_buf + fat_offset);
	next &= 0x0FFFFFFF;
	/* A self-referential entry (next == cluster) would loop the chain walk
	 * forever — treat it as end-of-chain. */
	if (next == cluster)
		return 0x0FFFFFFF;
	return next;
}

static void trim_spaces(char *str) {
	isize i = (isize)strlen(str) - 1;
	while (i >= 0 && str[i] == ' ') {
		str[i] = '\0';
		i--;
	}
}

static void fat_name_to_normal(const char *fat_name, char *normal) {
    memcpy(normal, fat_name, 8);
    normal[8] = '\0';
    trim_spaces(normal);
    if (fat_name[8] != ' ') {
        usize len = strlen(normal);
        normal[len] = '.';
        memcpy(normal + len + 1, fat_name + 8, 3);
        normal[len + 4] = '\0';
        trim_spaces(normal);
    }
}

static isize fat32_vfs_read(struct vfs_node *node, u64 offset, char *buffer, usize size, int flags) {
    (void)flags;
    struct fat32_inode_info *info = (struct fat32_inode_info *)node->inode->data;
    struct fat32_fs *fs = info->fs;

    if (offset >= info->size) return 0;
    if (offset + size > info->size) size = info->size - (usize)offset;

    u32 cluster = info->first_cluster;
    u32 cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    
    // Skip clusters to reach offset
    u64 current_offset = 0;
    while (current_offset + cluster_size <= offset) {
        cluster = get_next_cluster(fs, cluster);
        if (cluster >= 0x0FFFFFF8) return 0;
        current_offset += cluster_size;
    }

    usize bytes_read = 0;
    u8 *cluster_buf = kmalloc(cluster_size);

    while (bytes_read < size) {
        u32 sector = cluster_to_sector(fs, cluster);
        blk_read_cached(fs->bdev, sector, fs->sectors_per_cluster, cluster_buf);

        u32 offset_in_cluster = (u32)(offset + bytes_read - current_offset);
        u32 to_copy = cluster_size - offset_in_cluster;
        if (to_copy > size - bytes_read) to_copy = (u32)(size - bytes_read);

        memcpy(buffer + bytes_read, cluster_buf + offset_in_cluster, to_copy);
        bytes_read += to_copy;

        if (bytes_read < size) {
            cluster = get_next_cluster(fs, cluster);
            if (cluster >= 0x0FFFFFF8) break;
            current_offset += cluster_size;
        }
    }

    kfree(cluster_buf);
    return (isize)bytes_read;
}

static isize fat32_vfs_readdir(struct vfs_node *dir, usize offset, struct dirent *buf, usize max_entries) {
    struct fat32_inode_info *info = (struct fat32_inode_info *)dir->inode->data;
    struct fat32_fs *fs = info->fs;

    u32 cluster = info->first_cluster;
    u32 cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    u8 *cluster_buf = kmalloc(cluster_size);
    
    usize count = 0;
    usize entry_idx = 0;

    while (cluster < 0x0FFFFFF8 && count < max_entries) {
        u32 sector = cluster_to_sector(fs, cluster);
        blk_read_cached(fs->bdev, sector, fs->sectors_per_cluster, cluster_buf);

        struct fat32_dir_entry *entries = (struct fat32_dir_entry *)cluster_buf;
        u32 entries_per_cluster = cluster_size / sizeof(struct fat32_dir_entry);

        for (u32 i = 0; i < entries_per_cluster && count < max_entries; i++) {
            if (entries[i].name[0] == 0x00) {
                cluster = 0x0FFFFFF8; // End of directory
                break;
            }
            if (entries[i].name[0] == (char)0xE5) continue; // Deleted
            if (entries[i].attr == FAT_ATTR_LFN) continue; // Skip LFN artifacts
            if (entries[i].attr & FAT_ATTR_VOLUME_ID) continue; // Skip volume label

            if (entry_idx >= offset) {
                char name[13];
                fat_name_to_normal(entries[i].name, name);
                
                memcpy(buf[count].name, name, strlen(name) + 1);
                buf[count].type = (entries[i].attr & FAT_ATTR_DIRECTORY) ? VFS_DIRECTORY : VFS_FILE;
                buf[count].is_dir = (entries[i].attr & FAT_ATTR_DIRECTORY);
                buf[count].size = entries[i].size;
                count++;
            }
            entry_idx++;
        }

        if (cluster < 0x0FFFFFF8)
            cluster = get_next_cluster(fs, cluster);
    }

    kfree(cluster_buf);
    return (isize)count;
}

static int fat32_vfs_statfs(struct vfs_node *node, struct b1nix_statfs *st) {
    struct fat32_inode_info *info = (struct fat32_inode_info *)node->inode->data;
    struct fat32_fs *fs = info->fs;

    memset(st, 0, sizeof(*st));
    st->f_type = 0x4D44; // FAT
    st->f_bsize = fs->bytes_per_sector * fs->sectors_per_cluster;
    st->f_blocks = fs->total_clusters;
    
    // Calculate free space if not cached
    if (fs->free_clusters == 0xFFFFFFFF) {
        u32 free = 0;
        u8 *fat_buf = kmalloc(fs->bytes_per_sector);
        for (u32 s = 0; s < fs->sectors_per_fat; s++) {
            blk_read_cached(fs->bdev, fs->fat_start_sector + s, 1, fat_buf);
            u32 *fat = (u32 *)fat_buf;
            for (u32 i = 0; i < fs->bytes_per_sector / 4; i++) {
                if ((fat[i] & 0x0FFFFFFF) == 0) free++;
            }
        }
        kfree(fat_buf);
        fs->free_clusters = free;
    }
    
    st->f_bfree = fs->free_clusters;
    st->f_bavail = fs->free_clusters;
    st->f_files = 0; // Not strictly applicable to FAT
    st->f_ffree = 0;
    st->f_namelen = 12; // 8.3 format for now
    
    return 0;
}

static int fat32_vfs_fsync(struct vfs_node *node) {
    struct fat32_inode_info *info = (struct fat32_inode_info *)node->inode->data;
    struct fat32_fs *fs = info->fs;

    // If it's the root directory or has no valid entry location, just flush device
    if (info->entry_sector == 0) {
        blk_cache_flush(fs->bdev);
        return 0;
    }

    u8 sector_buf[512];
    if (blk_read_cached(fs->bdev, info->entry_sector, 1, sector_buf) < 0) return -EIO;

    struct fat32_dir_entry *entry = (struct fat32_dir_entry *)(sector_buf + info->entry_offset);
    
    // Update size and cluster if they changed in memory
    // (In a full implementation, we'd have a dirty flag on the inode info)
    entry->size = (u32)info->size;
    entry->cluster_low = (u16)(info->first_cluster & 0xFFFF);
    entry->cluster_high = (u16)((info->first_cluster >> 16) & 0xFFFF);
    
    if (blk_write_cached(fs->bdev, info->entry_sector, 1, sector_buf) < 0) return -EIO;

    blk_cache_flush(fs->bdev);
    return 0;
}

static void fat32_populate_vfs(struct fat32_fs *fs, u32 cluster, const char *parent_path) {
    u32 cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
	u8 *cluster_buf = kmalloc(cluster_size);
	if (!cluster_buf) return;

	while (cluster < 0x0FFFFFF8) {
		u32 base_sector = cluster_to_sector(fs, cluster);
		blk_read_cached(fs->bdev, base_sector, fs->sectors_per_cluster, cluster_buf);

		struct fat32_dir_entry *entries = (struct fat32_dir_entry *)cluster_buf;
		u32 entries_per_cluster = cluster_size / sizeof(struct fat32_dir_entry);

		for (u32 i = 0; i < entries_per_cluster; i++) {
			if (entries[i].name[0] == 0x00) break; 
			if (entries[i].name[0] == (char)0xE5) continue; 
			if (entries[i].attr == FAT_ATTR_LFN) continue; 
            if (entries[i].attr & FAT_ATTR_VOLUME_ID) continue;
			
			char name[13];
            fat_name_to_normal(entries[i].name, name);

			if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

			char full_path[128];
			usize plen = strlen(parent_path);
			memcpy(full_path, parent_path, plen);
			if (full_path[plen - 1] != '/') full_path[plen++] = '/';
			usize nlen = strlen(name);
			memcpy(full_path + plen, name, nlen + 1);

			u32 entry_cluster = ((u32)entries[i].cluster_high << 16) | entries[i].cluster_low;
            
            u32 entry_offset_in_cluster = i * sizeof(struct fat32_dir_entry);
            u32 entry_phys_sector = base_sector + (entry_offset_in_cluster / fs->bytes_per_sector);
            u32 entry_phys_offset = entry_offset_in_cluster % fs->bytes_per_sector;

			if (entries[i].attr & FAT_ATTR_DIRECTORY) {
				struct vfs_node *node = vfs_add_node(full_path, VFS_DIRECTORY, 0, 0, 0);
                if (node) {
                    struct fat32_inode_info *info = kmalloc(sizeof(struct fat32_inode_info));
                    info->fs = fs;
                    info->first_cluster = entry_cluster;
                    info->size = 0;
                    info->entry_sector = entry_phys_sector;
                    info->entry_offset = entry_phys_offset;
                    node->inode->data = info;
                    node->inode->readdir_cb = fat32_vfs_readdir;
                    node->inode->statfs_cb = fat32_vfs_statfs;
                    node->inode->fsync_cb = fat32_vfs_fsync;
                }
				fat32_populate_vfs(fs, entry_cluster, full_path);
			} else {
                struct vfs_node *node = vfs_add_node(full_path, VFS_FILE, 0, entries[i].size, 0);
                if (node) {
                    struct fat32_inode_info *info = kmalloc(sizeof(struct fat32_inode_info));
                    info->fs = fs;
                    info->first_cluster = entry_cluster;
                    info->size = entries[i].size;
                    info->entry_sector = entry_phys_sector;
                    info->entry_offset = entry_phys_offset;
                    node->inode->data = info;
                    node->inode->read_cb = fat32_vfs_read;
                    node->inode->statfs_cb = fat32_vfs_statfs;
                    node->inode->fsync_cb = fat32_vfs_fsync;
                }
			}
		}
		cluster = get_next_cluster(fs, cluster);
	}
    kfree(cluster_buf);
}

static struct vfs_node *fat32_vfs_mount_cb(const char *source, u64 flags, void *data) {
    (void)flags;
    struct block_device *dev = blk_get(source);
    if (!dev) return ERR_PTR(-ENODEV);

    u8 boot_sector[512];
    if (blk_read_cached(dev, 0, 1, boot_sector) < 0) return ERR_PTR(-EIO);

    struct fat32_bpb *bpb = (struct fat32_bpb *)boot_sector;
    if (bpb->bytes_per_sector != 512) return ERR_PTR(-EINVAL);
    /* Reject a crafted/corrupt BPB before it divides by these (R3-9):
     * sectors_per_cluster == 0 makes total_clusters a divide-by-zero panic, and
     * fat_count == 0 produces a degenerate data_start_sector. */
    if (bpb->sectors_per_cluster == 0 || bpb->fat_count == 0)
        return ERR_PTR(-EINVAL);

    struct fat32_fs *fs = kmalloc(sizeof(struct fat32_fs));
    memset(fs, 0, sizeof(struct fat32_fs));
    fs->bdev = dev;
    fs->bytes_per_sector = bpb->bytes_per_sector;
    fs->sectors_per_cluster = bpb->sectors_per_cluster;
    fs->fat_start_sector = bpb->reserved_sectors;
    fs->sectors_per_fat = bpb->sectors_per_fat_32;
    fs->data_start_sector = fs->fat_start_sector + (bpb->fat_count * bpb->sectors_per_fat_32);
    fs->root_cluster = bpb->root_cluster;
    fs->total_clusters = bpb->total_sectors_32 / bpb->sectors_per_cluster;
    fs->free_clusters = 0xFFFFFFFF; // Mark as unknown
    fs->fsinfo_sector = bpb->fs_info;
    
    // Add to instances linked list
    fs->next = fat32_instances;
    fat32_instances = fs;

    struct vfs_node *root = vfs_create_node(VFS_DIRECTORY);
    struct fat32_inode_info *info = kmalloc(sizeof(struct fat32_inode_info));
    info->fs = fs;
    info->first_cluster = fs->root_cluster;
    info->size = 0;
    info->entry_sector = 0; // Root has no parent entry
    info->entry_offset = 0;
    root->inode->data = info;
    root->inode->readdir_cb = fat32_vfs_readdir;
    root->inode->statfs_cb = fat32_vfs_statfs;
    root->inode->fsync_cb = fat32_vfs_fsync;

    if (data) {
        fat32_populate_vfs(fs, fs->root_cluster, (const char *)data);
    }

    return root;
}

void fat32_sync_all_fs(void) {
    struct fat32_fs *fs = fat32_instances;
    while (fs) {
        if (fs->fsinfo_dirty && fs->fsinfo_sector != 0) {
            u8 buf[512];
            if (blk_read_cached(fs->bdev, fs->fsinfo_sector, 1, buf) >= 0) {
                // Update free clusters in FSInfo (offset 488)
                *(u32 *)(buf + 488) = fs->free_clusters;
                blk_write_cached(fs->bdev, fs->fsinfo_sector, 1, buf);
            }
            fs->fsinfo_dirty = 0;
        }
        // fat_dirty is handled by blk_cache_flush because FAT sectors are written via blk_write_cached
        blk_cache_flush(fs->bdev);
        fs->fat_dirty = 0;
        fs = fs->next;
    }
}

static struct vfs_fs fat32_vfs = {
    .name = "fat32",
    .mount = fat32_vfs_mount_cb,
};

int fat32_mount(struct block_device *dev, const char *mount_point) {
    if (!dev) return -ENODEV;
    return vfs_mount(dev->name, mount_point, "fat32", 0);
}

void fat32_init(void) {
    vfs_register_fs(&fat32_vfs);
}
