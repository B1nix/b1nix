#include <b1nix/loop.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <b1nix/errno.h>
#include <b1nix/sched.h>
#include <stdio.h>
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

/* ── Loop-device control surface for BusyBox losetup ──
 * Eight pre-registered loop block devices (/dev/loop0../dev/loop7, created by
 * blk_create_dev_nodes) plus /dev/loop-control. LOOP_SET_FD associates an open
 * file's backing node; the device's byte reads then route through
 * loop_read_blocks. */
#define NUM_LOOPS 8
static struct loop_device g_loops[NUM_LOOPS];
static int g_loops_inited;

void loop_init(void) {
  if (g_loops_inited)
    return;
  g_loops_inited = 1;
  for (int i = 0; i < NUM_LOOPS; i++) {
    char *nm = kmalloc(8);
    if (!nm)
      return;
    snprintf(nm, 8, "loop%d", i);
    g_loops[i].backing_node = 0;
    g_loops[i].readonly = 1;
    g_loops[i].bdev.name = nm;
    g_loops[i].bdev.block_size = 512;
    g_loops[i].bdev.block_count = 0;
    g_loops[i].bdev.read_blocks = loop_read_blocks;
    g_loops[i].bdev.write_blocks = 0;
    g_loops[i].bdev.priv = &g_loops[i];
    blk_register(&g_loops[i].bdev);
  }
  struct vfs_node *ctl = vfs_add_node("/dev/loop-control", VFS_DEVICE, 0, 0, 0);
  if (ctl)
    ctl->inode->mode = 0660;
}

int loop_ioctl(struct vfs_node *node, u64 request, void *arg) {
  if (strcmp(node->name, "loop-control") == 0) {
    if (request == 0x4C82) { /* LOOP_CTL_GET_FREE */
      for (int i = 0; i < NUM_LOOPS; i++)
        if (!g_loops[i].backing_node)
          return i;
      return -ENOSPC;
    }
    return -ENOTTY;
  }
  if (strncmp(node->name, "loop", 4) != 0)
    return -ENOTTY;
  int idx = 0;
  for (const char *c = node->name + 4; *c >= '0' && *c <= '9'; c++)
    idx = idx * 10 + (*c - '0');
  if (idx < 0 || idx >= NUM_LOOPS)
    return -ENXIO;
  struct loop_device *lo = &g_loops[idx];
  switch (request) {
  case 0x4C00: { /* LOOP_SET_FD: arg is the backing file descriptor */
    int backing_fd = (int)(usize)arg;
    struct vfs_handle *h = scheduler_fd_get(backing_fd);
    if (!h || !h->node)
      return -EBADF;
    lo->backing_node = h->node;
    lo->bdev.block_count = (h->node->inode->size + 511) / 512;
    return 0;
  }
  case 0x4C01: /* LOOP_CLR_FD */
    lo->backing_node = 0;
    lo->bdev.block_count = 0;
    return 0;
  case 0x4C03: /* LOOP_GET_STATUS */
  case 0x4C05: /* LOOP_GET_STATUS64 */
    return lo->backing_node ? 0 : -ENXIO;
  case 0x4C02: /* LOOP_SET_STATUS */
  case 0x4C04: /* LOOP_SET_STATUS64 */
    return 0; /* accept; the filename metadata is not persisted */
  default:
    return -ENOTTY;
  }
}
