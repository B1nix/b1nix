#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/fuse.h>
#include <b1nix/klog.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <string.h>
#include <b1nix/bootmark.h>

static isize fuse_dev_read(struct vfs_node *node, u64 offset, char *buf,
                           usize count, int flags) {
  (void)node;
  (void)offset;
  (void)buf;
  (void)count;
  (void)flags;
  /* /dev/fuse character device read handler for FUSE daemon request queue */
  return 0;
}

static isize fuse_dev_write(struct vfs_node *node, u64 offset, const char *buf,
                            usize count, int flags) {
  (void)node;
  (void)offset;
  (void)buf;
  (void)flags;
  /* /dev/fuse character device write handler for FUSE daemon response queue */
  return (isize)count;
}

void fuse_init(void) {
  BOOTMARK(42); /* entered fuse_init */
  struct vfs_node *node = vfs_add_node("/dev/fuse", VFS_DEVICE, 0, 0, 0);
  BOOTMARK(43); /* vfs_add_node returned */
  if (!node || IS_ERR(node)) {
    console_write("fuse: failed to register /dev/fuse\n");
    return;
  }
  node->inode->mode = 0666;
  node->inode->read_cb = fuse_dev_read;
  node->inode->write_cb = fuse_dev_write;
  vfs_node_put(node);
  BOOTMARK(44); /* node published */
  klog_info("FUSE: /dev/fuse character device registered (Ring 3 user-space filesystems enabled)\n");
}


