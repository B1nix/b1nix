#include <b1nix/fat32.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
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

#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_LFN 0x0F

static struct block_device *fat_dev;
static struct fat32_bpb bpb;
static u32 data_start_sector;
static u32 fat_start_sector;

static u32 cluster_to_sector(u32 cluster)
{
	return data_start_sector + (cluster - 2) * bpb.sectors_per_cluster;
}

static u32 get_next_cluster(u32 cluster)
{
	u32 fat_sector = fat_start_sector + (cluster * 4) / bpb.bytes_per_sector;
	u32 fat_offset = (cluster * 4) % bpb.bytes_per_sector;
	u8 sector_buf[512]; // Assuming 512 byte sectors
	
	fat_dev->read_blocks(fat_dev, fat_sector, 1, sector_buf);
	u32 next = *(u32 *)(sector_buf + fat_offset);
	return next & 0x0FFFFFFF;
}

static void trim_spaces(char *str)
{
	isize i = strlen(str) - 1;
	while (i >= 0 && str[i] == ' ') {
		str[i] = '\0';
		i--;
	}
}

static void parse_dir(u32 cluster, const char *parent_path)
{
	u8 *cluster_buf = kmalloc(bpb.bytes_per_sector * bpb.sectors_per_cluster);
	if (!cluster_buf) return;

	while (cluster < 0x0FFFFFF8) {
		u32 sector = cluster_to_sector(cluster);
		fat_dev->read_blocks(fat_dev, sector, bpb.sectors_per_cluster, cluster_buf);

		struct fat32_dir_entry *entries = (struct fat32_dir_entry *)cluster_buf;
		u32 entries_per_cluster = (bpb.bytes_per_sector * bpb.sectors_per_cluster) / sizeof(struct fat32_dir_entry);

		for (u32 i = 0; i < entries_per_cluster; i++) {
			if (entries[i].name[0] == 0x00) break; // End of directory
			if (entries[i].name[0] == (char)0xE5) continue; // Deleted
			if (entries[i].attr == FAT_ATTR_LFN) continue; // Skip LFN for now
			
			char name[13];
			memcpy(name, entries[i].name, 8);
			name[8] = '\0';
			trim_spaces(name);

			if (entries[i].name[8] != ' ') {
				usize len = strlen(name);
				name[len] = '.';
				memcpy(name + len + 1, entries[i].name + 8, 3);
				name[len + 4] = '\0';
				trim_spaces(name);
			}

			if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

			char full_path[128];
			usize plen = strlen(parent_path);
			memcpy(full_path, parent_path, plen);
			if (full_path[plen - 1] != '/') full_path[plen++] = '/';
			usize nlen = strlen(name);
			memcpy(full_path + plen, name, nlen + 1);

			u32 entry_cluster = ((u32)entries[i].cluster_high << 16) | entries[i].cluster_low;

			if (entries[i].attr & FAT_ATTR_DIRECTORY) {
				vfs_mkdir(full_path);
				parse_dir(entry_cluster, full_path);
			} else {
				// We allocate and read the entire file into memory for simplicity
				u8 *file_data = kmalloc(entries[i].size + 1);
				if (file_data) {
					u32 fc = entry_cluster;
					usize offset = 0;
					while (fc < 0x0FFFFFF8 && offset < entries[i].size) {
						fat_dev->read_blocks(fat_dev, cluster_to_sector(fc), bpb.sectors_per_cluster, file_data + offset);
						offset += bpb.bytes_per_sector * bpb.sectors_per_cluster;
						fc = get_next_cluster(fc);
					}
					file_data[entries[i].size] = '\0';
					vfs_create(full_path, (char*)file_data);
				}
			}
		}
		cluster = get_next_cluster(cluster);
	}
}

int fat32_mount(struct block_device *dev, const char *mount_point)
{
	fat_dev = dev;

	u8 boot_sector[512];
	if (dev->read_blocks(dev, 0, 1, boot_sector) < 0) {
		console_write("fat32: failed to read boot sector\n");
		return -1;
	}

	memcpy(&bpb, boot_sector, sizeof(bpb));

	if (bpb.bytes_per_sector != 512) {
		console_write("fat32: unsupported sector size\n");
		return -1;
	}

	fat_start_sector = bpb.reserved_sectors;
	data_start_sector = fat_start_sector + (bpb.fat_count * bpb.sectors_per_fat_32);

	console_write("fat32: mounting to ");
	console_write(mount_point);
	console_write("\n");

	vfs_mkdir(mount_point);
	parse_dir(bpb.root_cluster, mount_point);

	console_write("fat32: mount complete\n");
	return 0;
}
