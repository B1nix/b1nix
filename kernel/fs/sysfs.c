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
#include <b1nix/page_cache.h>
#include <b1nix/netdev.h>
#include <b1nix/posix.h>
#include <b1nix/procfs.h>
#include <b1nix/sched.h>
#include <b1nix/vfs.h>
#include <b1nix/version.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef int (*sysfs_render)(char *buf, usize cap);

struct sysfs_node {
  sysfs_render render;
  const char *content; /* if set: emitted verbatim (kmalloc'd, never freed) */
  int live_blk;        /* >=0: emit the live 512-sector count of blk_at(live_blk)
                        * (so e.g. losetup size changes are reflected); else -1 */
};

/* Whole-device size in 512-byte sectors, regardless of the device block size. */
static u64 blk_sectors(struct block_device *d) {
  u64 spb = d->block_size >= 512 ? d->block_size / 512 : 1;
  return d->block_count * spb;
}

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
  if (sn->live_blk >= 0) {
    struct block_device *d = blk_at((usize)sn->live_blk);
    char tmp[32];
    int len = snprintf(tmp, sizeof(tmp), "%lu\n",
                       d ? (unsigned long)blk_sectors(d) : 0UL);
    return sysfs_emit(tmp, (usize)len, offset, buffer, size);
  }
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
    if (sn) {
      sn->render = render;
      sn->live_blk = -1;
    }
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
  sn->live_blk = -1;
  n->inode->data = sn;
  n->inode->read_cb = sysfs_read_cb;
}

/* Create a VFS_DEVICE node whose `size` is recomputed live from blk_at(index)
 * on each read, so e.g. a losetup attach is reflected without rebuilding. */
static void sysfs_mk_live_size(struct vfs_node *parent, int blk_index) {
  struct vfs_node *n = sysfs_mkchild(parent, "size", VFS_DEVICE, 0);
  if (!n)
    return;
  struct sysfs_node *sn = kzalloc(sizeof(*sn));
  if (!sn)
    return;
  sn->live_blk = blk_index;
  n->inode->data = sn;
  n->inode->read_cb = sysfs_read_cb;
}

/* Build the Linux-style block topology from the block registry: a
 * /sys/block/<disk>/{dev,size,removable,ro} dir with partition subdirs, a
 * /sys/dev/block/<maj:min>/{size,partition} mirror, and a flat /sys/class/block.
 * Major is a synthetic 8 (sd-like); minor is the registry index, matching
 * /proc/partitions and /proc/self/mountinfo. The *structure* is built once at
 * mount — every storage driver (incl. the eight loopN) has registered by the
 * time /sys mounts (kernel/main.c), so no readdir-time refresh (and its
 * cross-CPU locking) is needed. `size` is still a **live** read of the device's
 * current block_count, so a `losetup` attach is reflected without a rebuild. */
static void sysfs_build_block(struct vfs_node *root) {
  struct vfs_node *block = sysfs_mkchild(root, "block", VFS_DIRECTORY, 0);
  struct vfs_node *devp = sysfs_mkchild(root, "dev", VFS_DIRECTORY, 0);
  struct vfs_node *devblock = devp ? sysfs_mkchild(devp, "block", VFS_DIRECTORY, 0) : 0;
  struct vfs_node *classp = sysfs_mkchild(root, "class", VFS_DIRECTORY, 0);
  struct vfs_node *classblock =
      classp ? sysfs_mkchild(classp, "block", VFS_DIRECTORY, 0) : 0;
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
      sysfs_mk_live_size(bd, (int)i);
      sysfs_mkstr(bd, "removable", "0\n");
      sysfs_mkstr(bd, "ro", "0\n");
    }
    if (dbd)
      sysfs_mk_live_size(dbd, (int)i);
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

      struct vfs_node *pd = bd ? sysfs_mkchild(bd, p->name, VFS_DIRECTORY, 0) : 0;
      if (pd)
        sysfs_mkstr(pd, "dev", "8:%lu\n", (unsigned long)j);
      /* Entry under the disk's /sys/dev/block dir so its getdents enumerates
       * the partition (lsblk only reads the entry name here). */
      if (dbd)
        sysfs_mkchild(dbd, p->name, VFS_DIRECTORY, 0);

      char pmajmin[24];
      snprintf(pmajmin, sizeof(pmajmin), "8:%lu", (unsigned long)j);
      struct vfs_node *dpd = sysfs_mkchild(devblock, pmajmin, VFS_DIRECTORY, 0);
      if (dpd) {
        sysfs_mk_live_size(dpd, (int)j);
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
static int g_osrelease(char *b, usize c) { return snprintf(b, c, "%s\n", B1NIX_VERSION_STR); }
static int g_hostname(char *b, usize c) {
  char h[65];
  kernel_hostname_get(h, sizeof(h));
  return snprintf(b, c, "%s\n", h);
}
static int g_domainname(char *b, usize c) {
  char d[65];
  kernel_domainname_get(d, sizeof(d));
  return snprintf(b, c, "%s\n", d);
}
/* /sys/class/net/<if> attributes. The MAC comes from the registered driver;
 * operstate/carrier come from its link_up callback, so both reflect the real
 * device rather than a constant. */
static int g_net_mac(char *b, usize c) {
  struct netdev *nd = netdev_active();
  if (!nd)
    return snprintf(b, c, "00:00:00:00:00:00\n");
  return snprintf(b, c, "%02x:%02x:%02x:%02x:%02x:%02x\n", nd->mac.bytes[0],
                  nd->mac.bytes[1], nd->mac.bytes[2], nd->mac.bytes[3],
                  nd->mac.bytes[4], nd->mac.bytes[5]);
}
static int g_net_operstate(char *b, usize c) {
  struct netdev *nd = netdev_active();
  int up = (nd && nd->link_up) ? nd->link_up(nd) : (nd != 0);
  return snprintf(b, c, "%s\n", up ? "up" : "down");
}
static int g_net_carrier(char *b, usize c) {
  struct netdev *nd = netdev_active();
  int up = (nd && nd->link_up) ? nd->link_up(nd) : (nd != 0);
  return snprintf(b, c, "%d\n", up ? 1 : 0);
}
static int g_lo_mac(char *b, usize c) {
  return snprintf(b, c, "00:00:00:00:00:00\n");
}
static int g_lo_operstate(char *b, usize c) { return snprintf(b, c, "unknown\n"); }
static int g_lo_carrier(char *b, usize c) { return snprintf(b, c, "1\n"); }
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

/* /sys/class/net — one directory per interface, with the attributes Linux
 * network tools read (ip, ifconfig, busybox ifup). b1nix has the loopback
 * device plus at most one registered NIC (kernel/net), named eth0 to match
 * /proc/net/dev. */
static void sysfs_build_net(struct vfs_node *root) {
  /* sysfs_build_block already created /sys/class; reuse it rather than
   * attaching a second directory with the same name. */
  struct vfs_node *classp = 0;
  for (struct vfs_node *c = root->first_child; c; c = c->next_sibling) {
    if (strcmp(c->name, "class") == 0) {
      classp = c;
      break;
    }
  }
  if (!classp)
    classp = sysfs_mkchild(root, "class", VFS_DIRECTORY, 0);
  if (!classp)
    return;
  struct vfs_node *netd = sysfs_mkchild(classp, "net", VFS_DIRECTORY, 0);
  if (!netd)
    return;

  struct vfs_node *lo = sysfs_mkchild(netd, "lo", VFS_DIRECTORY, 0);
  if (lo) {
    sysfs_mkchild(lo, "address", VFS_DEVICE, g_lo_mac);
    sysfs_mkchild(lo, "operstate", VFS_DEVICE, g_lo_operstate);
    sysfs_mkchild(lo, "carrier", VFS_DEVICE, g_lo_carrier);
    sysfs_mkstr(lo, "mtu", "65536\n");
    sysfs_mkstr(lo, "ifindex", "1\n");
    sysfs_mkstr(lo, "type", "772\n"); /* ARPHRD_LOOPBACK */
    sysfs_mkstr(lo, "flags", "0x9\n"); /* IFF_UP|IFF_LOOPBACK */
  }

  if (!netdev_active())
    return;
  struct vfs_node *eth = sysfs_mkchild(netd, "eth0", VFS_DIRECTORY, 0);
  if (!eth)
    return;
  sysfs_mkchild(eth, "address", VFS_DEVICE, g_net_mac);
  sysfs_mkchild(eth, "operstate", VFS_DEVICE, g_net_operstate);
  sysfs_mkchild(eth, "carrier", VFS_DEVICE, g_net_carrier);
  sysfs_mkstr(eth, "mtu", "1500\n");
  sysfs_mkstr(eth, "ifindex", "2\n");
  sysfs_mkstr(eth, "type", "1\n");    /* ARPHRD_ETHER */
  sysfs_mkstr(eth, "flags", "0x1003\n"); /* IFF_UP|IFF_BROADCAST|IFF_MULTICAST */
}

static struct vfs_fs sysfs_fs;

/* Writable /sys/kernel/mm/drop_caches — mirrors Linux /proc/sys/vm/drop_caches.
 * Any write forces a full page-cache eviction pass (dirty pages are written back
 * via inode->write_cb, clean pages dropped). A real low-memory reclaim knob, and
 * a deterministic way to force reclaim — e.g. to validate that a writable
 * MAP_SHARED mmap store survives reclaim (it is now marked dirty on map-in). */
static isize sysfs_drop_caches_write(struct vfs_node *node, u64 offset,
                                     const char *buffer, usize size, int flags) {
  (void)node;
  (void)offset;
  (void)buffer;
  (void)flags;
  page_cache_evict((usize)-1); /* evict everything reclaimable */
  return (isize)size;          /* consume the whole write */
}

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
  /* /sys/kernel/mm/drop_caches — writable reclaim knob (see write_cb above). */
  struct vfs_node *km = sysfs_mkchild(kern, "mm", VFS_DIRECTORY, 0);
  struct vfs_node *dc = km ? sysfs_mkchild(km, "drop_caches", VFS_DEVICE, 0) : 0;
  if (dc) {
    dc->inode->mode = 0644;
    dc->inode->write_cb = sysfs_drop_caches_write;
  }
  sysfs_mkchild(kern, "ostype", VFS_DEVICE, g_ostype);
  sysfs_mkchild(kern, "osrelease", VFS_DEVICE, g_osrelease);
  sysfs_mkchild(kern, "hostname", VFS_DEVICE, g_hostname);
  sysfs_mkchild(kern, "version", VFS_DEVICE, g_kversion);
  sysfs_mkchild(kern, "domainname", VFS_DEVICE, g_domainname);

  struct vfs_node *dev = sysfs_mkchild(root, "devices", VFS_DIRECTORY, 0);
  struct vfs_node *sys = sysfs_mkchild(dev, "system", VFS_DIRECTORY, 0);
  struct vfs_node *cpu = sysfs_mkchild(sys, "cpu", VFS_DIRECTORY, 0);
  sysfs_mkchild(cpu, "possible", VFS_DEVICE, g_cpu_range);
  sysfs_mkchild(cpu, "online", VFS_DEVICE, g_cpu_range);
  sysfs_mkchild(cpu, "present", VFS_DEVICE, g_cpu_range);

  struct vfs_node *mem = sysfs_mkchild(root, "memory", VFS_DIRECTORY, 0);
  sysfs_mkchild(mem, "total_kb", VFS_DEVICE, g_memtotal);

  sysfs_build_block(root);
  sysfs_build_net(root);
  return root;
}

void sysfs_init(void) {
  sysfs_fs.name = "sysfs";
  sysfs_fs.mount = sysfs_mount_cb;
  vfs_register_fs(&sysfs_fs);
}
