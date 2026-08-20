/* MINIMAL_INITRAMFS selects a tiny embedded file set (init + native/b1cc smoke
 * binaries only) for the RAM-constrained in-guest self-host build. It is opt-in
 * via the build system (`make MINIMAL_INITRAMFS=1` adds -DMINIMAL_INITRAMFS; the
 * self-host module staging injects the same define into its standalone copy of
 * this file). The default build embeds the FULL initramfs — do NOT hardcode the
 * define here, or every normal/smoke build silently drops all but a handful of
 * binaries (the entire suite then fails to spawn them). */

#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/initramfs.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <string.h>
#include "initramfs_native_smoke.inc"
/* M95: the .ko images plus the generated modules.dep / modules.alias. Modules
 * must live in the initramfs, not the ext4 rootfs: the kernel loads the
 * filesystem, sound and IPv6 modules during early boot, long before a real
 * root is mounted. The RAM-constrained self-host build stages neither the .inc
 * nor the modules and simply runs without them. */
#ifndef MINIMAL_INITRAMFS
#include "initramfs_modules.inc"
/* M109: /init for a boot that keeps the initramfs as / (root=initramfs) — it
 * mounts the real root below / and hands over to BusyBox's switch_root. */
#include "initramfs_m109_switchroot.inc"
#define B1NIX_SWITCHROOT_INIT_FILE                                             \
  {"/init", (const char *)vfs_m109_switchroot_elf,                             \
   sizeof(vfs_m109_switchroot_elf), INITRAMFS_EXECUTABLE},
#else
#define B1NIX_MODULE_INITRAMFS_FILES
#define B1NIX_SWITCHROOT_INIT_FILE
#endif
#ifdef B1NIX_MUSL
#include "initramfs_ld_musl_x86_64_so_1.inc"
#endif

static const struct initramfs_file files[] = {
    {"/bin/native-smoke", (const char *)vfs_native_smoke_elf,
     sizeof(vfs_native_smoke_elf), INITRAMFS_EXECUTABLE},
#ifdef B1NIX_MUSL
    {"/lib/ld-musl-x86_64.so.1", (const char *)vfs_ld_musl_x86_64_so_1,
     sizeof(vfs_ld_musl_x86_64_so_1), INITRAMFS_EXECUTABLE},
    {"/lib/libc.so", "/lib/ld-musl-x86_64.so.1", 25, INITRAMFS_SYMLINK},
#endif
    B1NIX_MODULE_INITRAMFS_FILES
    B1NIX_SWITCHROOT_INIT_FILE
    {"/sbin/.keep", "", 0, 0},
    {"/etc/init.d/.keep", "", 0, 0},
    {"/etc/conf.d/.keep", "", 0, 0},
    {"/mnt/iso/.keep", "", 0, 0},
    {"/mnt/root/.keep", "", 0, 0},
};

static int initramfs_vfs_statfs(struct vfs_node *node,
                                struct b1nix_statfs *st) {
  (void)node;
  memset(st, 0, sizeof(*st));
  st->f_type = 0x858458f6;
  st->f_bsize = 4096;

  usize total_size = 0;
  for (usize i = 0; i < (sizeof(files) / sizeof(files[0])); i++) {
    total_size += files[i].size;
  }

  st->f_blocks = (total_size + 4095) / 4096;
  if (st->f_blocks == 0)
    st->f_blocks = 1;
  st->f_bfree = 0;
  st->f_bavail = 0;
  st->f_files = (sizeof(files) / sizeof(files[0]));
  st->f_ffree = 0;
  st->f_namelen = 64;
  return 0;
}

static struct vfs_node *initramfs_mount_cb(const char *source, u64 flags,
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
  root->inode->statfs_cb = initramfs_vfs_statfs;
  vfs_set_currently_mounting_root(root);

  for (usize i = 0; i < (sizeof(files) / sizeof(files[0])); i++) {
    enum vfs_node_type type = (files[i].flags & INITRAMFS_SYMLINK) ? VFS_SYMLINK : VFS_FILE;
    struct vfs_node *node =
        vfs_add_node(files[i].path, type, (void *)files[i].data,
                     files[i].size, files[i].flags);
    if (node) {
      node->inode->statfs_cb = initramfs_vfs_statfs;
    }
  }

  return root;
}

static struct vfs_fs initramfs_fs = {
    .name = "initramfs",
    .mount = initramfs_mount_cb,
    .flags = VFS_FS_NODEV,
};

void initramfs_init(void) {
  vfs_register_fs(&initramfs_fs);
  console_write("initramfs: files 0x");
  console_write_hex64(initramfs_count());
  console_write("\n");
}

const struct initramfs_file *initramfs_find(const char *path) {
  for (usize i = 0; i < initramfs_count(); i++) {
    if (strcmp(files[i].path, path) == 0) {
      return &files[i];
    }
  }
  return 0;
}

const struct initramfs_file *initramfs_get(usize index) {
  if (index >= initramfs_count())
    return 0;
  return &files[index];
}

usize initramfs_count(void) { return sizeof(files) / sizeof(files[0]); }
