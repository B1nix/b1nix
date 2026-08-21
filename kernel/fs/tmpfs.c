/*
 * tmpfs — the volatile RAM filesystem an init system expects on /run, /tmp and
 * /dev/shm.
 *
 * b1nix's VFS is already an in-memory tree: a directory node without a
 * filesystem's create_cb keeps its children, file contents and FIFOs in the
 * heap. A tmpfs mount is therefore just a fresh empty directory published as a
 * mount root — everything below it is served by the generic VFS paths, and it
 * all disappears when the mount goes away, which is exactly tmpfs semantics.
 *
 * "ramfs" is registered as an alias: it differs from tmpfs on Linux only in
 * swap/size accounting, which does not change what a mount of it looks like
 * here.
 *
 * "devtmpfs" is NOT an alias. On Linux it arrives already populated with every
 * device node the kernel knows about, and an init system mounts it over /dev
 * expecting exactly that. As a plain tmpfs it replaced the whole of /dev with
 * an empty directory: systemd lost /dev/console the moment it mounted it and
 * printed nothing for the rest of the boot, and no getty could open
 * /dev/ttyS0. Its mount publishes the root first (so path lookups reach it)
 * and then asks the VFS to lay the device nodes down inside it.
 */

#include <b1nix/vfs.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <string.h>

static int tmpfs_statfs(struct vfs_node *node, struct b1nix_statfs *st) {
  (void)node;
  if (!st)
    return -EINVAL;
  memset(st, 0, sizeof(*st));
  st->f_type = 0x01021994; /* TMPFS_MAGIC, as Linux reports */
  st->f_bsize = 4096;
  /* Backed by the kernel heap, so the free/total counts are the heap's. */
  st->f_blocks = 0;
  st->f_bfree = 0;
  st->f_bavail = 0;
  st->f_namelen = 255;
  return 0;
}

static struct vfs_node *tmpfs_mount_cb(const char *source, u64 flags,
                                       void *data) {
  (void)source;
  (void)flags;
  (void)data;
  struct vfs_node *root = vfs_create_node(VFS_DIRECTORY);
  if (!root)
    return ERR_PTR(-ENOMEM);
  root->inode->mode = 0755;
  root->inode->uid = 0;
  root->inode->gid = 0;
  root->inode->statfs_cb = tmpfs_statfs;
  return root;
}

static struct vfs_fs tmpfs_fs = {.name = "tmpfs", .mount = tmpfs_mount_cb,
                                 .flags = VFS_FS_NODEV};
static struct vfs_fs ramfs_fs = {.name = "ramfs", .mount = tmpfs_mount_cb,
                                 .flags = VFS_FS_NODEV};
static struct vfs_node *devtmpfs_mount_cb(const char *source, u64 flags,
                                          void *data) {
  struct vfs_node *root = tmpfs_mount_cb(source, flags, data);
  if (IS_ERR(root))
    return root;
  /* Publish the mount root before populating: vfs_populate_dev creates its
   * nodes by absolute path, and "/dev" only resolves into this filesystem once
   * the mount entry knows its root. */
  vfs_set_currently_mounting_root(root);
  vfs_populate_dev();
  return root;
}

static struct vfs_fs devtmpfs_fs = {.name = "devtmpfs",
                                    .mount = devtmpfs_mount_cb,
                                    .flags = VFS_FS_NODEV};

void tmpfs_init(void) {
  vfs_register_fs(&tmpfs_fs);
  vfs_register_fs(&ramfs_fs);
  vfs_register_fs(&devtmpfs_fs);
}
