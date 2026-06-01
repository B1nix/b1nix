/* sysfs — synthetic /sys filesystem (M34).
 *
 * Exposes kernel configuration and hardware topology as read-on-demand
 * pseudo-files, mirroring the Linux /sys hierarchy closely enough for tools
 * like `sysctl` and `free`/`top` to probe. Same VFS_DEVICE + read_cb pattern
 * as procfs (see kernel/fs/procfs.c for the rationale on node type).
 */

#include <b1nix/errno.h>
#include <b1nix/lapic.h>
#include <b1nix/mm.h>
#include <b1nix/procfs.h>
#include <b1nix/sched.h>
#include <b1nix/vfs.h>
#include <stdio.h>
#include <string.h>

typedef int (*sysfs_render)(char *buf, usize cap);

struct sysfs_node {
  sysfs_render render;
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
  if (!sn || !sn->render)
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
  return root;
}

void sysfs_init(void) {
  sysfs_fs.name = "sysfs";
  sysfs_fs.mount = sysfs_mount_cb;
  vfs_register_fs(&sysfs_fs);
}
