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

/* ── VFAT long file names ──────────────────────────────────────────────────
 *
 * A long name is stored in the entries that PRECEDE the 8.3 entry it belongs
 * to, in reverse order: the last fragment comes first and carries 0x40 in its
 * order byte, the fragment numbered 1 sits immediately before the short entry.
 * Each fragment holds 13 UTF-16 code units in three runs (5 + 6 + 2) around
 * the fields the 8.3 layout needs for its attribute and checksum bytes.
 *
 * These entries were previously skipped outright, so every name on a FAT
 * volume came back in its 8.3 form — `PROGRA~1` for `Program Files` — and
 * statfs advertised a 12-character limit to match. The chain is now assembled
 * and validated against the short entry's checksum, which is what tells a real
 * long name apart from a stale fragment left by a deleted file. */

struct fat_lfn_acc {
        /* UTF-16 code units, fragment 1 first. 20 fragments x 13 = 260 units,
         * which is the most a conformant chain can carry (255 + terminator). */
        u16 units[20 * 13];
        u32 count;      /* code units gathered so far */
        u8 checksum;    /* of the short entry this chain belongs to */
        int valid;      /* chain is contiguous and self-consistent so far */
};

static void fat_lfn_reset(struct fat_lfn_acc *acc) {
        acc->count = 0;
        acc->checksum = 0;
        acc->valid = 0;
}

/* The 8.3 name's checksum, as every LFN fragment records it. */
static u8 fat_short_checksum(const char *name11) {
        u8 sum = 0;

        for (int i = 0; i < 11; i++)
                sum = (u8)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + (u8)name11[i]);
        return sum;
}

/* Absorb one 0x0F entry. Fragments arrive highest-order first, so each one is
 * written at its own offset rather than appended. */
static void fat_lfn_absorb(struct fat_lfn_acc *acc,
                           const struct fat32_dir_entry *e) {
        const u8 *raw = (const u8 *)e;
        u8 order = raw[0];
        u8 checksum = raw[13];
        int last = (order & 0x40) != 0;
        u32 seq = (u32)(order & 0x3F);

        if (order == 0xE5 || seq == 0 || seq > 20) { /* deleted or malformed */
                fat_lfn_reset(acc);
                return;
        }
        if (last) {
                fat_lfn_reset(acc);
                acc->checksum = checksum;
                acc->valid = 1;
                acc->count = seq * 13;
        } else if (!acc->valid || checksum != acc->checksum) {
                /* A fragment with no chain head, or one belonging to a
                 * different short entry: the chain is not trustworthy. */
                fat_lfn_reset(acc);
                return;
        }

        static const u8 offsets[13] = {1,  3,  5,  7,  9,  14, 16,
                                       18, 20, 22, 24, 28, 30};
        u32 base = (seq - 1) * 13;

        for (u32 k = 0; k < 13; k++) {
                u32 idx = base + k;

                if (idx >= sizeof(acc->units) / sizeof(acc->units[0]))
                        break;
                acc->units[idx] = (u16)(raw[offsets[k]] |
                                        ((u16)raw[offsets[k] + 1] << 8));
        }
}

/* Write the assembled name as UTF-8 into `out` (capacity `cap`, NUL included).
 * Returns 0 when there is no valid chain for this short entry, or when the
 * name does not fit — the caller then uses the 8.3 name, which always fits and
 * is a genuine, openable name for the same file rather than a truncation. */
static int fat_lfn_take(const struct fat_lfn_acc *acc,
                        const struct fat32_dir_entry *short_e, char *out,
                        usize cap) {
        if (!acc->valid || acc->count == 0)
                return 0;
        if (acc->checksum != fat_short_checksum(short_e->name))
                return 0;

        usize o = 0;

        for (u32 i = 0; i < acc->count; i++) {
                u16 u = acc->units[i];

                if (u == 0x0000 || u == 0xFFFF)
                        break;
                /* UTF-16 -> UTF-8, BMP only: a surrogate half has no scalar
                 * value of its own, and FAT names outside the BMP are rare
                 * enough that the short name is a better answer than mojibake. */
                if (u >= 0xD800 && u <= 0xDFFF)
                        return 0;
                if (u < 0x80) {
                        if (o + 1 >= cap) return 0;
                        out[o++] = (char)u;
                } else if (u < 0x800) {
                        if (o + 2 >= cap) return 0;
                        out[o++] = (char)(0xC0 | (u >> 6));
                        out[o++] = (char)(0x80 | (u & 0x3F));
                } else {
                        if (o + 3 >= cap) return 0;
                        out[o++] = (char)(0xE0 | (u >> 12));
                        out[o++] = (char)(0x80 | ((u >> 6) & 0x3F));
                        out[o++] = (char)(0x80 | (u & 0x3F));
                }
        }
        if (o == 0)
                return 0;
        out[o] = '\0';
        return 1;
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
    struct fat_lfn_acc lfn;

    fat_lfn_reset(&lfn);

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
            if (entries[i].name[0] == (char)0xE5) { /* Deleted */
                fat_lfn_reset(&lfn);
                continue;
            }
            if (entries[i].attr == FAT_ATTR_LFN) {
                fat_lfn_absorb(&lfn, &entries[i]);
                continue;
            }
            if (entries[i].attr & FAT_ATTR_VOLUME_ID) { /* Volume label */
                fat_lfn_reset(&lfn);
                continue;
            }

            /* Take the long name before anything can skip past this entry:
             * the chain belongs to THIS short entry and to no other. */
            char name[sizeof(buf[0].name)];
            if (!fat_lfn_take(&lfn, &entries[i], name, sizeof(name)))
                fat_name_to_normal(entries[i].name, name);
            fat_lfn_reset(&lfn);

            if (entry_idx >= offset) {
                
                memcpy(buf[count].name, name, strlen(name) + 1);
                buf[count].type = (entries[i].attr & FAT_ATTR_DIRECTORY) ? VFS_DIRECTORY : VFS_FILE;
                buf[count].is_dir = (entries[i].attr & FAT_ATTR_DIRECTORY);
                buf[count].size = entries[i].size;
                /* FAT has no inodes: the first cluster is the closest thing to
                 * a stable per-file identity, which is what d_ino needs to be
                 * (an empty file has cluster 0, so fall back to the directory
                 * index the syscall layer supplies). */
                buf[count].ino = ((u64)entries[i].cluster_high << 16) |
                                 entries[i].cluster_low;
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
	/* Long names are read (see fat_lfn_take), so the limit is no longer the
	 * 8.3 form. What binds now is the VFS dirent this driver reports through —
	 * a name that does not fit falls back to its 8.3 short name rather than
	 * being truncated, so this is the longest name a caller can actually
	 * receive from here. */
	st->f_namelen = (i64)(sizeof(((struct dirent *)0)->name) - 1);
    
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

	struct fat_lfn_acc lfn;

	fat_lfn_reset(&lfn);

	while (cluster < 0x0FFFFFF8) {
		u32 base_sector = cluster_to_sector(fs, cluster);
		blk_read_cached(fs->bdev, base_sector, fs->sectors_per_cluster, cluster_buf);

		struct fat32_dir_entry *entries = (struct fat32_dir_entry *)cluster_buf;
		u32 entries_per_cluster = cluster_size / sizeof(struct fat32_dir_entry);

		for (u32 i = 0; i < entries_per_cluster; i++) {
			if (entries[i].name[0] == 0x00) break; 
			if (entries[i].name[0] == (char)0xE5) { /* deleted */
				fat_lfn_reset(&lfn);
				continue;
			}
			if (entries[i].attr == FAT_ATTR_LFN) {
				fat_lfn_absorb(&lfn, &entries[i]);
				continue;
			}
			if (entries[i].attr & FAT_ATTR_VOLUME_ID) {
				fat_lfn_reset(&lfn);
				continue;
			}

			/* Long name when the volume carries one, 8.3 otherwise. Bounded by
			 * what the VFS dirent can report, so a name that reaches the tree
			 * is a name readdir can hand back. */
			char name[64];
			if (!fat_lfn_take(&lfn, &entries[i], name, sizeof(name)))
				fat_name_to_normal(entries[i].name, name);
			fat_lfn_reset(&lfn);

			if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

			/* Bounds: parent + '/' + name + NUL. Long names made the old
			 * unchecked memcpy into a stack overflow waiting for a deep tree —
			 * 8.3 names simply never got near 128 bytes. */
			char full_path[VFS_MAX_PATH];
			usize plen = strlen(parent_path);
			usize nlen = strlen(name);
			if (plen == 0 || plen + 1 + nlen + 1 > sizeof(full_path))
				continue;
			memcpy(full_path, parent_path, plen);
			if (full_path[plen - 1] != '/') full_path[plen++] = '/';
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
