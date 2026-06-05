#include <b1nix/loop.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <b1nix/errno.h>
#include <string.h>

struct loop_device {
    struct block_device bdev;
    struct vfs_node *backing_node;
    int readonly;
};

static int loop_read_blocks(struct block_device *dev, u64 lba, u32 count, void *buffer) {
    struct loop_device *loop = (struct loop_device *)dev->priv;
    if (!loop || !loop->backing_node || !loop->backing_node->inode) {
        return -1;
    }

    struct vfs_node *node = loop->backing_node;
    if (!node->inode->read_cb) {
        return -EINVAL;
    }

    u64 offset = lba * 512;
    usize size = (usize)count * 512;

    if (offset >= node->inode->size) {
        memset(buffer, 0, size);
        return 0;
    }

    if (offset + size > node->inode->size) {
        size = node->inode->size - (usize)offset;
    }

    isize read_bytes = node->inode->read_cb(node, offset, buffer, size, 0);
    if (read_bytes < 0) {
        return (int)read_bytes;
    }

    if ((usize)read_bytes < (usize)count * 512) {
        memset((char *)buffer + read_bytes, 0, ((usize)count * 512) - (usize)read_bytes);
    }

    return 0;
}

struct block_device *loop_register_file(const char *path, const char *name) {
    struct vfs_node *node = vfs_find_node(path);
    if (IS_ERR(node)) {
        return (struct block_device *)node;
    }

    struct loop_device *loop = kmalloc(sizeof(struct loop_device));
    if (!loop) {
        vfs_node_put(node);
        return ERR_PTR(-ENOMEM);
    }

    loop->backing_node = node;
    loop->readonly = 1;

    char *persistent_name = kmalloc(strlen(name) + 1);
    if (!persistent_name) {
        kfree(loop);
        vfs_node_put(node);
        return ERR_PTR(-ENOMEM);
    }
    strcpy(persistent_name, name);

    loop->bdev.name = persistent_name;
    loop->bdev.block_size = 512;
    loop->bdev.block_count = (node->inode->size + 511) / 512;
    loop->bdev.read_blocks = loop_read_blocks;
    loop->bdev.write_blocks = NULL;
    loop->bdev.priv = loop;

    blk_register(&loop->bdev);
    return &loop->bdev;
}
