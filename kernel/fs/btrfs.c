#include <b1nix/btrfs.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <b1nix/errno.h>
#include <string.h>

u64 btrfs_logical_to_physical(struct btrfs_fs_info *fs, u64 logical) {
    u8 *ptr = fs->sb.sys_chunk_array;
    u8 *end = ptr + fs->sb.sys_chunk_array_size;

    while (ptr < end) {
        struct btrfs_disk_key *key = (struct btrfs_disk_key *)ptr;
        ptr += sizeof(struct btrfs_disk_key);
        
        struct btrfs_chunk *chunk = (struct btrfs_chunk *)ptr;
        
        if (logical >= key->offset && logical < key->offset + chunk->length) {
            return chunk->stripes[0].physical + (logical - key->offset);
        }
        
        // Skip chunk and its stripes
        ptr += sizeof(struct btrfs_chunk) + chunk->num_stripes * sizeof(struct btrfs_stripe);
    }
    
    return 0;
}

static struct vfs_node *btrfs_vfs_mount_cb(const char *source, u64 flags, void *data) {
    (void)flags; (void)data;
    struct block_device *dev = blk_get(source);
    if (!dev) return ERR_PTR(-ENODEV);

    u8 *sb_buf = kmalloc(4096);
    if (blk_read_cached(dev, BTRFS_SUPER_INFO_OFFSET / 512, 8, sb_buf) < 0) {
        kfree(sb_buf);
        return ERR_PTR(-EIO);
    }

    struct btrfs_super_block *sb = (struct btrfs_super_block *)sb_buf;
    if (memcmp(sb->magic, BTRFS_MAGIC, 8) != 0) {
        kfree(sb_buf);
        return ERR_PTR(-EINVAL);
    }

    struct btrfs_fs_info *fs = kmalloc(sizeof(struct btrfs_fs_info));
    fs->bdev = dev;
    memcpy(&fs->sb, sb, sizeof(struct btrfs_super_block));
    kfree(sb_buf);

    console_write("btrfs: mounted, fsid=");
    const char *digits = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            console_putc('-');
        }
        console_putc(digits[(fs->sb.fsid[i] >> 4) & 0xf]);
        console_putc(digits[fs->sb.fsid[i] & 0xf]);
    }
    console_write(", label=\"");
    for (int i = 0; i < 256 && fs->sb.label[i] != '\0'; i++) {
        console_putc(fs->sb.label[i]);
    }
    console_write("\"\n");

    struct vfs_node *root = vfs_create_node(VFS_DIRECTORY);
    if (!root) {
        kfree(fs);
        return ERR_PTR(-ENOMEM);
    }
    root->inode->data = fs;
    root->inode->blk_dev = dev;

    return root;
}

static struct vfs_fs btrfs_vfs = {
    .name = "btrfs",
    .mount = btrfs_vfs_mount_cb,
};

int btrfs_mount_root(const char *device_name, const char *mount_point) {
    return vfs_mount(device_name, mount_point, "btrfs", 0);
}

void btrfs_init(void) {
    vfs_register_fs(&btrfs_vfs);
}
