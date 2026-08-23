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
  /*
   * Real numbers, because a program that asks is entitled to act on them.
   *
   * Reporting zero blocks and zero free said "this filesystem cannot hold
   * anything", and systemd believes it: `systemctl daemon-reload` refuses
   * outright -- "not enough space available on /run/systemd. Currently, 0B are
   * free, but a safety buffer of 16.0M is expected" -- so no unit written at
   * runtime was ever loaded, and every one of them came back as "Unit not
   * found". df showed the same nothing.
   *
   * A tmpfs is backed by physical memory, so its size is the memory it may
   * use and its free space is the memory still free. Linux caps a default
   * tmpfs at half of RAM and reports that; the same convention is used here
   * rather than promising every page, since handing out the last frame to a
   * file is how a machine dies without a message.
   */
  u64 total_pages = pmm_total_usable_memory() / 4096ull / 2ull;
  u64 free_pages = (u64)pmm_free_frame_count();

  if (free_pages > total_pages)
    free_pages = total_pages;
  st->f_blocks = total_pages;
  st->f_bfree = free_pages;
  st->f_bavail = free_pages;
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
