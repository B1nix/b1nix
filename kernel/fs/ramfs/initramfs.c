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
#ifdef __aarch64__
#include "initramfs_modules.inc"
#include "initramfs_openrc_init.inc"
#include "initramfs_hello.inc"
#include "initramfs_m8_aio_test.inc"
#include "initramfs_m12_smoke.inc"
#include "initramfs_m13_smoke.inc"
#include "initramfs_m13_job_control.inc"
#include "initramfs_m14_smoke.inc"
#include "initramfs_m15_smoke.inc"
#include "initramfs_m17_smoke.inc"
#include "initramfs_m24b_smoke.inc"
#include "initramfs_m25_smoke.inc"
#include "initramfs_m26_smoke.inc"
#include "initramfs_m27_smoke.inc"
#include "initramfs_m29_smoke.inc"
#include "initramfs_m30_pie.inc"
#include "initramfs_m31_smoke.inc"
#include "initramfs_m31_setuid.inc"
#include "initramfs_m32_smoke.inc"
#include "initramfs_m56_smoke.inc"
#include "initramfs_m109_switchroot.inc"
#ifdef B1NIX_MUSL
#include "initramfs_ld_musl_aarch64_so_1.inc"
#endif

static const struct initramfs_file files[] = {
    {"/sbin/openrc-init", (const char *)vfs_openrc_init_elf, sizeof(vfs_openrc_init_elf), INITRAMFS_EXECUTABLE},
    {"/bin/hello", (const char *)vfs_hello_elf, sizeof(vfs_hello_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m8_aio_test", (const char *)vfs_m8_aio_test_elf, sizeof(vfs_m8_aio_test_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m12_smoke", (const char *)vfs_m12_smoke_elf, sizeof(vfs_m12_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m13_smoke", (const char *)vfs_m13_smoke_elf, sizeof(vfs_m13_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m13_job_control", (const char *)vfs_m13_job_control_elf, sizeof(vfs_m13_job_control_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m14_smoke", (const char *)vfs_m14_smoke_elf, sizeof(vfs_m14_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m15_smoke", (const char *)vfs_m15_smoke_elf, sizeof(vfs_m15_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m17_smoke", (const char *)vfs_m17_smoke_elf, sizeof(vfs_m17_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m24b_smoke", (const char *)vfs_m24b_smoke_elf, sizeof(vfs_m24b_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m25_smoke", (const char *)vfs_m25_smoke_elf, sizeof(vfs_m25_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m26_smoke", (const char *)vfs_m26_smoke_elf, sizeof(vfs_m26_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m27_smoke", (const char *)vfs_m27_smoke_elf, sizeof(vfs_m27_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m29_smoke", (const char *)vfs_m29_smoke_elf, sizeof(vfs_m29_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m30_pie", (const char *)vfs_m30_pie_elf, sizeof(vfs_m30_pie_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m31_smoke", (const char *)vfs_m31_smoke_elf, sizeof(vfs_m31_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m31_setuid", (const char *)vfs_m31_setuid_elf, sizeof(vfs_m31_setuid_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m32_smoke", (const char *)vfs_m32_smoke_elf, sizeof(vfs_m32_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m56_smoke", (const char *)vfs_m56_smoke_elf, sizeof(vfs_m56_smoke_elf), INITRAMFS_EXECUTABLE},
#ifdef B1NIX_MUSL
    {"/lib/ld-musl-aarch64.so.1", (const char *)vfs_ld_musl_aarch64_so_1,
     sizeof(vfs_ld_musl_aarch64_so_1), INITRAMFS_EXECUTABLE},
    {"/lib/libc.so", "/lib/ld-musl-aarch64.so.1", 26, INITRAMFS_SYMLINK},
#endif
    /* M95: the .ko images + modules.dep/alias. They must be in the initramfs,
     * not the ext4 rootfs — request_module() runs during early boot, long
     * before a real root is mounted. */
    B1NIX_MODULE_INITRAMFS_FILES
    /* M109: root=initramfs boots this as PID 1 via the /init-shebang
     * interpreter line; it mounts the real root and hands over to
     * switch_root. Same pair the x86_64 file set carries below. */
    {"/init", (const char *)vfs_m109_switchroot_elf,
     sizeof(vfs_m109_switchroot_elf), INITRAMFS_EXECUTABLE},
    {"/init-shebang", "#!/init\n", 8, INITRAMFS_EXECUTABLE},
    {"/sbin/.keep", "", 0, 0},
    {"/etc/init.d/.keep", "", 0, 0},
    {"/etc/conf.d/.keep", "", 0, 0},
    {"/mnt/iso/.keep", "", 0, 0},
    {"/mnt/root/.keep", "", 0, 0},
};
#else
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
/* /init-shebang is how the switchroot instance is actually started, and it is
 * a test rather than a convenience: `init=` reached the ELF loader directly and
 * so could not name a script at all. A `#!` file failed on the magic number
 * ("bad magic 23 21", which is "#!") and the machine came up with no PID 1,
 * even though execve(2) had honoured `#!` for years. Booting the one instance
 * whose whole subject is which program the kernel starts as PID 1 through an
 * interpreter line means every M109 marker is also evidence that it resolves.
 *
 * The interpreter it names is /init itself, so what ends up running is exactly
 * the same program as before; only the route to it is under test. */
#define B1NIX_SWITCHROOT_INIT_FILE                                             \
  {"/init", (const char *)vfs_m109_switchroot_elf,                             \
   sizeof(vfs_m109_switchroot_elf), INITRAMFS_EXECUTABLE},                     \
  {"/init-shebang", "#!/init\n", 8, INITRAMFS_EXECUTABLE},
#else
#define B1NIX_MODULE_INITRAMFS_FILES
#define B1NIX_SWITCHROOT_INIT_FILE
#endif
#ifdef B1NIX_MUSL
#include "initramfs_ld_musl_x86_64_so_1.inc"
#endif

static const struct initramfs_file files[] = {
    {"/bin/native_smoke", (const char *)vfs_native_smoke_elf,
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
#endif

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
    if (node && !IS_ERR(node)) {
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
