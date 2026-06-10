/* sysfs — synthetic /sys filesystem (M34).
 *
 * Exposes kernel configuration and hardware topology as read-on-demand
 * pseudo-files, mirroring the Linux /sys hierarchy closely enough for tools
 * like `sysctl` and `free`/`top` to probe. Same VFS_DEVICE + read_cb pattern
 * as procfs (see kernel/fs/procfs.c for the rationale on node type).
 */

#include <b1nix/blk.h>
#include <b1nix/errno.h>
#include <b1nix/lapic.h>
#include <b1nix/mm.h>
#include <b1nix/procfs.h>
#include <b1nix/sched.h>
#include <b1nix/vfs.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef int (*sysfs_render)(char *buf, usize cap);

struct sysfs_node {
  sysfs_render render;
  const char *content; /* if set: emitted verbatim (kmalloc'd, never freed) */
};

static isize sysfs_emit(const char *buf, usize len, u64 offset, char *out,
                        usize size) {
  if (offset >= (u64)len)
    return 0;
  usize avail = len - (usize)offset;
  usize n = avail < size ? avail : size;
  memcpy(out, buf + (usize)offset, n);
  return (isize)n;
}

static isize sysfs_read_cb(struct vfs_node *node, u64 offset, char *buffer,
                           usize size, int flags) {
  (void)flags;
  struct sysfs_node *sn = (struct sysfs_node *)node->inode->data;
  if (!sn)
    return 0;
  if (sn->content)
    return sysfs_emit(sn->content, strlen(sn->content), offset, buffer, size);
  if (!sn->render)
    return 0;
  char tmp[256];
  int len = sn->render(tmp, sizeof(tmp));
  if (len < 0)
    return len;
  return sysfs_emit(tmp, (usize)len, offset, buffer, size);
}

static struct vfs_node *sysfs_mkchild(struct vfs_node *parent,
                                      const char *name,
                                      enum vfs_node_type type,
                                      sysfs_render render) {
  struct vfs_node *n = vfs_create_node(type);
  if (!n)
    return 0;
  usize nl = strlen(name);
  if (nl > 63)
    nl = 63;
  memcpy(n->name, name, nl);
  n->name[nl] = '\0';
  n->inode->mode = (type == VFS_DIRECTORY) ? 0555 : 0444;
  n->inode->nlink = (type == VFS_DIRECTORY) ? 2 : 1;
  if (render) {
    struct sysfs_node *sn = kzalloc(sizeof(*sn));
    if (sn)
      sn->render = render;
    n->inode->data = sn;
    n->inode->read_cb = sysfs_read_cb;
  }
  n->parent = parent;
  n->refcount++;
  vfs_attach_child(parent, n);
  return n;
}

/* Create a VFS_DEVICE node whose contents are a fixed string (printf-style).
 * The string is kmalloc'd and lives for the lifetime of the mount. */
static void sysfs_mkstr(struct vfs_node *parent, const char *name,
                        const char *fmt, ...) {
  char tmp[64];
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (len < 0)
    return;
  struct vfs_node *n = sysfs_mkchild(parent, name, VFS_DEVICE, 0);
  if (!n)
    return;
  char *copy = kmalloc((usize)len + 1);
  if (!copy)
    return;
  memcpy(copy, tmp, (usize)len + 1);
  struct sysfs_node *sn = kzalloc(sizeof(*sn));
  if (!sn) {
    kfree(copy);
    return;
  }
  sn->content = copy;
  n->inode->data = sn;
  n->inode->read_cb = sysfs_read_cb;
}

/* Whole-disk sectors (512-byte units) regardless of the device block size. */
static u64 blk_sectors(struct block_device *d) {
  u64 spb = d->block_size >= 512 ? d->block_size / 512 : 1;
  return d->block_count * spb;
}

/* Build the Linux-style block topology: /sys/block/<disk>/{dev,size,...} with
 * partition subdirs, /sys/dev/block/<maj:min>/ mirrors, and /sys/class/block.
 * Major is a synthetic 8 (sd-like); minor is the blk registry index, matching
 * /proc/partitions and /proc/self/mountinfo. Built once at mount — all storage
 * drivers have probed by the time /sys is mounted (kernel/main.c). */
static void sysfs_build_block(struct vfs_node *root) {
  struct vfs_node *block = sysfs_mkchild(root, "block", VFS_DIRECTORY, 0);
  struct vfs_node *devp = sysfs_mkchild(root, "dev", VFS_DIRECTORY, 0);
  struct vfs_node *devblock = sysfs_mkchild(devp, "block", VFS_DIRECTORY, 0);
  struct vfs_node *classp = sysfs_mkchild(root, "class", VFS_DIRECTORY, 0);
  struct vfs_node *classblock = sysfs_mkchild(classp, "block", VFS_DIRECTORY, 0);
  if (!block || !devblock || !classblock)
    return;

  usize n = blk_count();
  for (usize i = 0; i < n; i++) {
    struct block_device *d = blk_at(i);
    if (!d || !d->name || blk_is_partition(d))
      continue;

    char majmin[24];
    snprintf(majmin, sizeof(majmin), "8:%lu", (unsigned long)i);
    struct vfs_node *bd = sysfs_mkchild(block, d->name, VFS_DIRECTORY, 0);
    struct vfs_node *dbd = sysfs_mkchild(devblock, majmin, VFS_DIRECTORY, 0);
    if (bd) {
      sysfs_mkstr(bd, "dev", "8:%lu\n", (unsigned long)i);
      sysfs_mkstr(bd, "size", "%lu\n", (unsigned long)blk_sectors(d));
      sysfs_mkstr(bd, "removable", "0\n");
      sysfs_mkstr(bd, "ro", "0\n");
    }
    if (dbd)
      sysfs_mkstr(dbd, "size", "%lu\n", (unsigned long)blk_sectors(d));
    struct vfs_node *cb = sysfs_mkchild(classblock, d->name, VFS_DIRECTORY, 0);
    if (cb)
      sysfs_mkstr(cb, "dev", "8:%lu\n", (unsigned long)i);

    /* Partitions whose parent is this disk. */
    for (usize j = 0; j < n; j++) {
      struct block_device *p = blk_at(j);
      if (!p || !p->name || blk_partition_parent(p) != d)
        continue;
      int num = blk_partition_number(p);
      if (num < 0)
        num = (int)(j + 1);

      if (bd) {
        struct vfs_node *pd = sysfs_mkchild(bd, p->name, VFS_DIRECTORY, 0);
        if (pd)
          sysfs_mkstr(pd, "dev", "8:%lu\n", (unsigned long)j);
      }
      /* Entry under the disk's /sys/dev/block dir so its getdents enumerates
       * the partition (lsblk only reads the entry name here). */
      if (dbd)
        sysfs_mkchild(dbd, p->name, VFS_DIRECTORY, 0);

      char pmajmin[24];
      snprintf(pmajmin, sizeof(pmajmin), "8:%lu", (unsigned long)j);
      struct vfs_node *dpd = sysfs_mkchild(devblock, pmajmin, VFS_DIRECTORY, 0);
      if (dpd) {
        sysfs_mkstr(dpd, "size", "%lu\n", (unsigned long)blk_sectors(p));
        sysfs_mkstr(dpd, "partition", "%d\n", num);
      }
      struct vfs_node *cbp =
          sysfs_mkchild(classblock, p->name, VFS_DIRECTORY, 0);
      if (cbp)
        sysfs_mkstr(cbp, "dev", "8:%lu\n", (unsigned long)j);
    }
  }
}

/* ── content generators ── */
static int g_ostype(char *b, usize c) { return snprintf(b, c, "B1NIX\n"); }
static int g_osrelease(char *b, usize c) { return snprintf(b, c, "0.22.0\n"); }
static int g_hostname(char *b, usize c) { return snprintf(b, c, "b1nix\n"); }
static int g_kversion(char *b, usize c) {
  return snprintf(b, c, "#1 SMP b1nix\n");
}

static int g_cpu_range(char *b, usize c) {
  int n = (g_max_cpus > 0) ? g_max_cpus : 1;
  if (n == 1)
    return snprintf(b, c, "0\n");
  return snprintf(b, c, "0-%d\n", n - 1);
}

static int g_memtotal(char *b, usize c) {
  u64 kb = pmm_total_usable_memory() / 1024;
  return snprintf(b, c, "%lu\n", (unsigned long)kb);
}

static struct vfs_fs sysfs_fs;

static struct vfs_node *sysfs_mount_cb(const char *source, u64 flags,
                                       void *data) {
  (void)source;
  (void)flags;
  (void)data;
  struct vfs_node *root = vfs_create_node(VFS_DIRECTORY);
  if (!root)
    return ERR_PTR(-ENOMEM);
  root->inode->mode = 0555;

  struct vfs_node *kern = sysfs_mkchild(root, "kernel", VFS_DIRECTORY, 0);
  sysfs_mkchild(kern, "ostype", VFS_DEVICE, g_ostype);
  sysfs_mkchild(kern, "osrelease", VFS_DEVICE, g_osrelease);
  sysfs_mkchild(kern, "hostname", VFS_DEVICE, g_hostname);
  sysfs_mkchild(kern, "version", VFS_DEVICE, g_kversion);

  struct vfs_node *dev = sysfs_mkchild(root, "devices", VFS_DIRECTORY, 0);
  struct vfs_node *sys = sysfs_mkchild(dev, "system", VFS_DIRECTORY, 0);
  struct vfs_node *cpu = sysfs_mkchild(sys, "cpu", VFS_DIRECTORY, 0);
  sysfs_mkchild(cpu, "possible", VFS_DEVICE, g_cpu_range);
  sysfs_mkchild(cpu, "online", VFS_DEVICE, g_cpu_range);
  sysfs_mkchild(cpu, "present", VFS_DEVICE, g_cpu_range);

  struct vfs_node *mem = sysfs_mkchild(root, "memory", VFS_DIRECTORY, 0);
  sysfs_mkchild(mem, "total_kb", VFS_DEVICE, g_memtotal);

  sysfs_build_block(root);
  return root;
}

void sysfs_init(void) {
  sysfs_fs.name = "sysfs";
  sysfs_fs.mount = sysfs_mount_cb;
  vfs_register_fs(&sysfs_fs);
}
