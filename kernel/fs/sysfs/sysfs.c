/* sysfs — synthetic /sys filesystem (M34).
 *
 * Exposes kernel configuration and hardware topology as read-on-demand
 * pseudo-files, mirroring the Linux /sys hierarchy closely enough for tools
 * like `sysctl` and `free`/`top` to probe. Same VFS_DEVICE + read_cb pattern
 * as procfs (see kernel/fs/proc/procfs.c for the rationale on node type).
 */

#include <b1nix/blk.h>
#include <b1nix/errno.h>
#include <b1nix/lapic.h>
#include <b1nix/mm.h>
#include <b1nix/module.h>
#include <b1nix/page_cache.h>
#include <b1nix/netdev.h>
#include <b1nix/posix.h>
#include <b1nix/procfs.h>
#include <b1nix/sched.h>
#include <b1nix/sysfs_attr.h>
#include <b1nix/uevent.h>
#include <b1nix/arch.h>
#include <b1nix/vfs.h>
#include <b1nix/version.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef int (*sysfs_render)(char *buf, usize cap);

/* The identity a writable `uevent` file re-announces. Held per node so a write
 * can rebuild the same message the device's own registration sent, rather than
 * a second, differently-shaped description of one device. */
struct sysfs_uevent {
  char devpath[96];
  char subsystem[24];
  /* What the device calls itself inside its subsystem. Recorded here because a
   * `udevadm trigger` re-announcement must be indistinguishable from the
   * driver's own — and a message without DEVTYPE is one udevd throws away. */
  char devtype[24];
  char devname[40];
  int major;
  int minor;
};

struct sysfs_node {
  sysfs_render render;
  const char *content; /* if set: emitted verbatim (kmalloc'd, never freed) */
  int live_blk;        /* >=0: emit the live 512-sector count of blk_at(live_blk)
                        * (so e.g. losetup size changes are reflected); else -1 */
  /* Volume identity, read from the device on each read for the same reason
   * `size` is: a loop device's superblock only exists once something is
   * attached, and the sysfs tree is built long before that. */
  int ident_blk;       /* >=0: index into the block registry, else -1 */
  int ident_kind;      /* SYSFS_IDENT_* */
  /* Non-NULL on a `uevent` file: writing an action to it re-announces the
   * device on the hotplug netlink group. */
  struct sysfs_uevent *ue;
};

#define SYSFS_IDENT_UUID   1
#define SYSFS_IDENT_LABEL  2
#define SYSFS_IDENT_FSTYPE 3

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
  if (sn->ident_kind) {
    struct block_device *d = blk_at((usize)sn->ident_blk);
    char val[64];
    val[0] = '\0';
    if (d) {
      if (sn->ident_kind == SYSFS_IDENT_UUID)
        (void)blk_probe_uuid(d, val, sizeof(val));
      else if (sn->ident_kind == SYSFS_IDENT_LABEL)
        (void)blk_probe_label(d, val, sizeof(val));
      else {
        const char *t = blk_probe_fstype(d);
        strncpy(val, t ? t : "", sizeof(val) - 1);
        val[sizeof(val) - 1] = '\0';
      }
    }
    char tmp[72];
    int len = snprintf(tmp, sizeof(tmp), "%s\n", val);
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
  /* A sysfs attribute is a regular file, not a character device. */
  if (type == VFS_DEVICE || type == VFS_FILE)
    n->inode->flags |= VFS_NODE_PSEUDO_REG;
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
  /* Big enough for the whole of a `uevent` file: four properties, one of them
   * a device name. A truncated uevent is not a smaller uevent — mdev reads
   * DEVNAME out of it and names the node it creates after what it finds. */
  char tmp[192];
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

/* uuid/label/fstype for one device, each read live from its superblock. This
 * is where `lsblk -o UUID` and udev-style by-uuid rules look, and it is the
 * same answer `findfs UUID=…` gets from reading the device itself. */
static void sysfs_mk_ident(struct vfs_node *parent, int blk_index) {
  static const struct {
    const char *name;
    int kind;
  } attrs[] = {{"uuid", SYSFS_IDENT_UUID},
               {"label", SYSFS_IDENT_LABEL},
               {"fstype", SYSFS_IDENT_FSTYPE}};
  for (usize i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++) {
    struct vfs_node *n = sysfs_mkchild(parent, attrs[i].name, VFS_DEVICE, 0);
    if (!n)
      continue;
    struct sysfs_node *sn = kzalloc(sizeof(*sn));
    if (!sn)
      continue;
    sn->live_blk = -1;
    sn->ident_blk = blk_index;
    sn->ident_kind = attrs[i].kind;
    n->inode->data = sn;
    n->inode->read_cb = sysfs_read_cb;
  }
}

/* ── Block topology (/sys/block, /sys/dev/block, /sys/class/block) ──────────
 *
 * The Linux-style block hierarchy, built from the block registry: a
 * /sys/block/<disk>/ directory with partition subdirectories, a
 * /sys/dev/block/<major:minor>/ mirror keyed by device number, and a flat
 * /sys/class/block. The major is BLK_SYSFS_MAJOR and the minor is the registry
 * index, which is exactly what /proc/partitions and /proc/self/mountinfo
 * print, so the three agree about which device is which.
 *
 * Every device directory carries the two files a hot-plug helper reads:
 * `dev` ("major:minor") and `uevent` (MAJOR/MINOR/DEVNAME/DEVTYPE). `mdev -s`
 * walks /sys/dev, and without DEVNAME it would name the node it creates after
 * the containing directory — "8:0", which is no use to anyone.
 *
 * The tree is no longer frozen at mount. A device registered after boot (a
 * loop device created through LOOP_CTL_ADD) has to appear, and one that is
 * removed has to disappear, so the three directories refresh themselves from
 * the registry on readdir and on lookup. The refresh is a single atomic load
 * unless something has actually been plugged or unplugged.
 *
 * `size` is a live read of the device's current block_count for the same
 * reason it always was: a losetup attach changes it without changing the
 * registry at all.
 */

#define SYSFS_MAX_BLK 64

static struct vfs_node *g_sysfs_block;
static struct vfs_node *g_sysfs_devblock;
static struct vfs_node *g_sysfs_devchar;
static struct vfs_node *g_sysfs_classblock;
/* The registry generation the tree below was built from. */
static u32 g_sysfs_blk_gen;
static volatile int g_sysfs_blk_lock;

/* What is currently published for registry index i. The name is kept here so a
 * device that has gone away can be unpublished under the name it HAD, rather
 * than under whatever name now occupies that slot. */
static struct {
  char name[32];
  u8 used;
} g_sysfs_blkent[SYSFS_MAX_BLK];

/* Child lookup by name. find_child takes the VFS tree's read lock, which is
 * what makes this safe against a readdir running on another CPU; the reference
 * it returns is dropped immediately because the parent's own link keeps the
 * node alive, and every removal here happens under g_sysfs_blk_lock with this
 * caller holding it. */
static struct vfs_node *sysfs_child(struct vfs_node *parent, const char *name) {
  if (!parent)
    return 0;
  struct vfs_node *c = find_child(parent, name);
  if (!c)
    return 0;
  vfs_node_put(c);
  return c;
}

/* Release the per-file state behind one node. The nodes themselves go with the
 * subtree's refcount; this is the kmalloc'd content and descriptor that
 * sysfs_mkstr and friends attached, which nothing else would ever free. */
static void sysfs_free_payload(struct vfs_node *n) {
  if (!n || !n->inode || n->inode->read_cb != sysfs_read_cb)
    return;
  struct sysfs_node *sn = (struct sysfs_node *)n->inode->data;
  n->inode->data = 0;
  n->inode->read_cb = 0;
  if (!sn)
    return;
  if (sn->content)
    kfree((void *)sn->content);
  kfree(sn);
}

static void sysfs_free_subtree(struct vfs_node *n) {
  if (!n)
    return;
  for (struct vfs_node *c = n->first_child; c; c = c->next_sibling)
    sysfs_free_subtree(c);
  sysfs_free_payload(n);
}

/* Unlink one named child and free everything under it. 1 if it was there. */
static int sysfs_drop(struct vfs_node *parent, const char *name) {
  struct vfs_node *n = sysfs_child(parent, name);
  if (!n)
    return 0;
  vfs_detach_child(parent, n);
  sysfs_free_subtree(n);
  vfs_node_put(n);
  return 1;
}

/* Registry index of a device, or -1. */
static int sysfs_blk_index(struct block_device *dev) {
  usize n = blk_count();
  for (usize i = 0; i < n; i++) {
    if (blk_at(i) == dev)
      return (int)i;
  }
  return -1;
}

/* Writing to a `uevent` file re-announces the device.
 *
 * This is not a convenience: it is the whole of device coldplug. A kernel
 * announces each device once, when it appears, which for everything present at
 * boot is long before any listener exists. `udevadm trigger` — and mdev -s, and
 * every other hotplug manager — recovers those missed announcements by writing
 * "add" to each device's uevent file, and a kernel that ignores the write
 * leaves udev with an empty database and systemd with no `.device` unit at all.
 *
 * The accepted actions are Linux's; the event carries the same DEVPATH,
 * SUBSYSTEM, DEVNAME and MAJOR/MINOR the device's own registration sent, so a
 * triggered event and a real hotplug event are indistinguishable, which is
 * exactly what makes the trigger worth having.
 */
static isize sysfs_uevent_write_cb(struct vfs_node *node, u64 offset,
                                   const char *buffer, usize size, int flags) {
  (void)offset;
  (void)flags;
  struct sysfs_node *sn = node ? (struct sysfs_node *)node->inode->data : 0;
  if (!sn || !sn->ue || !buffer || size == 0)
    return -EINVAL;

  /* The first word is the action; udev appends a synthetic-event UUID after
   * it, which the kernel records but nothing here needs. */
  char action[16];
  usize n = 0;
  while (n < size && n < sizeof(action) - 1 && buffer[n] != ' ' &&
         buffer[n] != '\n' && buffer[n] != '\0')
    n++;
  memcpy(action, buffer, n);
  action[n] = '\0';

  static const char *const known[] = {"add",  "remove", "change", "move",
                                      "online", "offline", "bind", "unbind"};
  int ok = 0;
  for (usize i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
    if (strcmp(action, known[i]) == 0) {
      ok = 1;
      break;
    }
  }
  if (!ok)
    return -EINVAL;

  uevent_post(action, sn->ue->devpath, sn->ue->subsystem,
              sn->ue->devtype[0] ? sn->ue->devtype : 0,
              sn->ue->devname[0] ? sn->ue->devname : 0, sn->ue->major,
              sn->ue->minor);
  return (isize)size;
}

/* The `uevent` file, in the shape Linux writes it: the properties a helper
 * reads for a device it did not learn about from a netlink message, and a
 * write that re-announces the device. */
static void sysfs_mk_uevent_at(struct vfs_node *dir, const char *devpath,
                               const char *subsystem, usize index,
                               const char *name, const char *devtype) {
  sysfs_mkstr(dir, "uevent", "MAJOR=%d\nMINOR=%lu\nDEVNAME=%s\nDEVTYPE=%s\n",
              BLK_SYSFS_MAJOR, (unsigned long)index, name, devtype);
  struct vfs_node *n = sysfs_child(dir, "uevent");
  if (!n)
    return;
  struct sysfs_node *sn = (struct sysfs_node *)n->inode->data;
  if (!sn)
    return;
  struct sysfs_uevent *ue = kzalloc(sizeof(*ue));
  if (!ue)
    return;
  strncpy(ue->devpath, devpath, sizeof(ue->devpath) - 1);
  strncpy(ue->subsystem, subsystem, sizeof(ue->subsystem) - 1);
  if (devtype)
    strncpy(ue->devtype, devtype, sizeof(ue->devtype) - 1);
  strncpy(ue->devname, name, sizeof(ue->devname) - 1);
  ue->major = BLK_SYSFS_MAJOR;
  ue->minor = (int)index;
  sn->ue = ue;
  n->inode->mode = 0644;
  n->inode->write_cb = sysfs_uevent_write_cb;
}

/* The `subsystem` symlink every Linux device directory carries. udev reads its
 * basename to learn which subsystem a device enumerated from /sys belongs to;
 * without it a device found by a scan has no subsystem, and every
 * SUBSYSTEM=="…" rule — including the one that tags block devices for systemd
 * — silently fails to match. */
static void sysfs_mk_subsystem_link(struct vfs_node *dir, const char *target) {
  if (!dir || sysfs_child(dir, "subsystem"))
    return;
  struct vfs_node *n = sysfs_mkchild(dir, "subsystem", VFS_SYMLINK, 0);
  if (!n)
    return;
  usize len = strlen(target);
  char *copy = kmalloc(len + 1);
  if (!copy)
    return;
  memcpy(copy, target, len + 1);
  n->inode->mode = 0777;
  n->inode->data = copy;
  n->inode->size = len;
}

/* The `queue` directory a whole disk carries, and a partition does not.
 *
 * That distinction is not decoration: it is how systemd tells the two apart.
 * block_get_whole_disk() asks for <dev>/queue first and calls the device a
 * whole disk if it is there; failing that it looks for <dev>/partition and
 * walks to the parent. b1nix published neither for a disk, so systemd-udevd
 * answered `vda: Failed to get whole disk device: No such file or directory`,
 * abandoned the event before running a single rule -- `Failed to process
 * device, ignoring` -- and the device was never tagged. An untagged device
 * gets no `.device` unit, so nothing that is `BoundTo=` a device could ever
 * start on this machine.
 *
 * Every value below is one the block layer actually knows. Attributes whose
 * answer this kernel does not have (optimal_io_size, write_cache) are left out
 * rather than guessed: a wrong number here is a number a filesystem will lay
 * itself out around. The scheduler really is `none` -- there is no I/O
 * scheduler to name. */
static void sysfs_mk_queue_dir(struct vfs_node *dir, struct block_device *d) {
  if (!dir || !d || sysfs_child(dir, "queue"))
    return;
  struct vfs_node *q = sysfs_mkchild(dir, "queue", VFS_DIRECTORY, 0);
  if (!q)
    return;

  unsigned long bs = (unsigned long)(d->block_size ? d->block_size : 512);
  u32 max_sectors = d->limits.max_sectors ? d->limits.max_sectors
                                          : BLK_DEF_MAX_SECTORS;
  u32 max_segments = d->limits.max_segments ? d->limits.max_segments
                                            : BLK_DEF_MAX_SEGMENTS;
  u32 depth = d->limits.queue_depth ? d->limits.queue_depth
                                    : BLK_DEF_QUEUE_DEPTH;

  sysfs_mkstr(q, "logical_block_size", "%lu\n", bs);
  sysfs_mkstr(q, "physical_block_size", "%lu\n", bs);
  sysfs_mkstr(q, "hw_sector_size", "%lu\n", bs);
  sysfs_mkstr(q, "minimum_io_size", "%lu\n", bs);
  /* Linux reports these in KiB, from a limit counted in 512-byte sectors. */
  sysfs_mkstr(q, "max_sectors_kb", "%lu\n", (unsigned long)(max_sectors / 2));
  sysfs_mkstr(q, "max_hw_sectors_kb", "%lu\n",
              (unsigned long)(max_sectors / 2));
  sysfs_mkstr(q, "max_segments", "%lu\n", (unsigned long)max_segments);
  sysfs_mkstr(q, "nr_requests", "%lu\n", (unsigned long)depth);
  sysfs_mkstr(q, "rotational", "%d\n", d->rotational ? 1 : 0);
  /* No I/O scheduler exists here, and Linux spells that "none". */
  sysfs_mkstr(q, "scheduler", "none\n");
  /* Zero means "does not support discard", which is exactly true of a device
   * whose driver never offered the command. */
  sysfs_mkstr(q, "discard_granularity", "%lu\n", d->discard ? bs : 0UL);
}

/* Publish one registry entry in all three directories. */
static void sysfs_block_publish(usize index, struct block_device *d) {
  if (!d || !d->name || index >= SYSFS_MAX_BLK)
    return;

  int part = blk_is_partition(d);
  struct block_device *parent = part ? blk_partition_parent(d) : 0;
  const char *devtype = part ? "partition" : "disk";
  int partno = 0;
  if (part) {
    partno = blk_partition_number(d);
    if (partno < 0)
      partno = (int)(index + 1);
  }

  char majmin[24];
  snprintf(majmin, sizeof(majmin), "%d:%lu", BLK_SYSFS_MAJOR,
           (unsigned long)index);

  /* One canonical DEVPATH per device, the same one blk_announce() puts in the
   * hotplug message, so a re-announcement triggered through any of the three
   * directories below names the device udev already knows. */
  char devpath[96];
  if (part && parent && parent->name)
    snprintf(devpath, sizeof(devpath), "/block/%s/%s", parent->name, d->name);
  else
    snprintf(devpath, sizeof(devpath), "/block/%s", d->name);

  /* /sys/block/<disk>/ — a partition is a subdirectory of its disk, which the
   * ascending walk over the registry has already published (a partition is
   * only ever registered by the scan that follows its disk). */
  struct vfs_node *bparent =
      part ? ((parent && parent->name)
                  ? sysfs_child(g_sysfs_block, parent->name)
                  : 0)
           : g_sysfs_block;
  if (bparent && !sysfs_child(bparent, d->name)) {
    struct vfs_node *bd = sysfs_mkchild(bparent, d->name, VFS_DIRECTORY, 0);
    if (bd) {
      sysfs_mkstr(bd, "dev", "%s\n", majmin);
      sysfs_mk_live_size(bd, (int)index);
      if (part) {
        sysfs_mkstr(bd, "partition", "%d\n", partno);
      } else {
        sysfs_mkstr(bd, "removable", "%d\n", blk_is_removable(d));
        sysfs_mkstr(bd, "ro", "0\n");
        sysfs_mk_queue_dir(bd, d);
      }
      sysfs_mk_ident(bd, (int)index);
      sysfs_mk_uevent_at(bd, devpath, "block", index, d->name, devtype);
      sysfs_mk_subsystem_link(bd, part ? "../../../class/block"
                                       : "../../class/block");
    }
  }

  /* /sys/dev/block/<major:minor>/ */
  if (!sysfs_child(g_sysfs_devblock, majmin)) {
    struct vfs_node *dbd =
        sysfs_mkchild(g_sysfs_devblock, majmin, VFS_DIRECTORY, 0);
    if (dbd) {
      sysfs_mkstr(dbd, "dev", "%s\n", majmin);
      sysfs_mk_live_size(dbd, (int)index);
      if (part)
        sysfs_mkstr(dbd, "partition", "%d\n", partno);
      else
        sysfs_mk_queue_dir(dbd, d);
      sysfs_mk_uevent_at(dbd, devpath, "block", index, d->name, devtype);
      sysfs_mk_subsystem_link(dbd, "../../../class/block");
    }
  }

  /* An entry for the partition under its disk's /sys/dev/block directory, so
   * that directory's getdents enumerates it (lsblk reads only the name). */
  if (part && parent) {
    int pidx = sysfs_blk_index(parent);
    if (pidx >= 0) {
      char pmajmin[24];
      snprintf(pmajmin, sizeof(pmajmin), "%d:%d", BLK_SYSFS_MAJOR, pidx);
      struct vfs_node *pdir = sysfs_child(g_sysfs_devblock, pmajmin);
      if (pdir && !sysfs_child(pdir, d->name))
        sysfs_mkchild(pdir, d->name, VFS_DIRECTORY, 0);
    }
  }

  /* /sys/class/block/<name>/ — flat, disks and partitions alike. */
  if (!sysfs_child(g_sysfs_classblock, d->name)) {
    struct vfs_node *cb =
        sysfs_mkchild(g_sysfs_classblock, d->name, VFS_DIRECTORY, 0);
    if (cb) {
      sysfs_mkstr(cb, "dev", "%s\n", majmin);
      if (!part)
        sysfs_mk_queue_dir(cb, d);
      sysfs_mk_uevent_at(cb, devpath, "block", index, d->name, devtype);
      sysfs_mk_subsystem_link(cb, "../../../class/block");
    }
  }

  strncpy(g_sysfs_blkent[index].name, d->name,
          sizeof(g_sysfs_blkent[index].name) - 1);
  g_sysfs_blkent[index].name[sizeof(g_sysfs_blkent[index].name) - 1] = '\0';
  g_sysfs_blkent[index].used = 1;
}

/* Take one registry entry back out of all three directories. */
static void sysfs_block_unpublish(usize index) {
  if (index >= SYSFS_MAX_BLK || !g_sysfs_blkent[index].used)
    return;
  const char *name = g_sysfs_blkent[index].name;

  char majmin[24];
  snprintf(majmin, sizeof(majmin), "%d:%lu", BLK_SYSFS_MAJOR,
           (unsigned long)index);
  sysfs_drop(g_sysfs_devblock, majmin);
  sysfs_drop(g_sysfs_classblock, name);

  /* Under /sys/block the device is either a disk at the top or a partition
   * inside one; try the top first, then each disk. */
  if (!sysfs_drop(g_sysfs_block, name)) {
    for (struct vfs_node *c = g_sysfs_block->first_child; c;
         c = c->next_sibling) {
      if (sysfs_drop(c, name))
        break;
    }
  }
  /* And the enumeration stub inside its disk's /sys/dev/block directory. */
  for (struct vfs_node *c = g_sysfs_devblock->first_child; c;
       c = c->next_sibling) {
    if (sysfs_drop(c, name))
      break;
  }

  g_sysfs_blkent[index].used = 0;
  g_sysfs_blkent[index].name[0] = '\0';
}

/* Bring the three directories in line with the registry. Cheap by design: the
 * generation only moves when a device is registered or unregistered, so the
 * readdir of /sys that every `ls` performs costs one atomic load. */
static void sysfs_block_refresh(void) {
  if (!g_sysfs_block || !g_sysfs_devblock || !g_sysfs_classblock)
    return;
  u32 gen = blk_generation();
  if (gen == __atomic_load_n(&g_sysfs_blk_gen, __ATOMIC_ACQUIRE))
    return;

  while (__sync_lock_test_and_set(&g_sysfs_blk_lock, 1))
    ;
  if (gen != g_sysfs_blk_gen) {
    __atomic_store_n(&g_sysfs_blk_gen, gen, __ATOMIC_RELEASE);
    usize n = blk_count();
    for (usize i = 0; i < SYSFS_MAX_BLK; i++) {
      struct block_device *d = (i < n) ? blk_at(i) : 0;
      const char *nm = (d && d->name) ? d->name : 0;
      /* Gone, or the slot now holds a different device. */
      if (g_sysfs_blkent[i].used &&
          (!nm || strcmp(nm, g_sysfs_blkent[i].name) != 0))
        sysfs_block_unpublish(i);
      if (nm && !g_sysfs_blkent[i].used)
        sysfs_block_publish(i, d);
    }
  }
  __sync_lock_release(&g_sysfs_blk_lock);
}

/* The block layer telling sysfs that the registry moved.
 *
 * The readdir and lookup hooks below are not enough on their own, and a device
 * that went away is where that shows: the path resolver only calls lookup_cb
 * when find_child MISSES, so as long as the stale directory is still in the
 * child list every lookup of it succeeds and the refresh is never reached —
 * /sys/block/<gone device>/dev went on being readable forever. Registration
 * and unregistration therefore push, and the pull below stays as the cheap
 * safety net for a listing.
 *
 * Called before the uevent is broadcast, so a listener that reads /sys the
 * instant it sees the announcement finds the device already published (or
 * already gone). No-op until /sys is mounted. */
void sysfs_block_changed(void) { sysfs_block_refresh(); }

static isize sysfs_block_readdir(struct vfs_node *dir, usize offset,
                                 struct dirent *buf, usize max_entries) {
  sysfs_block_refresh();
  return vfs_readdir_children(dir, offset, buf, max_entries);
}

/* A direct open of /sys/block/<new device>/dev must work without anything
 * having listed the directory first — that is exactly what mdev does when it
 * turns a uevent's DEVPATH into a path. */
static int sysfs_block_lookup(struct vfs_node *dir, const char *name) {
  sysfs_block_refresh();
  return sysfs_child(dir, name) ? 0 : -1;
}

static void sysfs_block_hook(struct vfs_node *dir) {
  if (!dir || !dir->inode)
    return;
  dir->inode->readdir_cb = sysfs_block_readdir;
  dir->inode->lookup_cb = sysfs_block_lookup;
  dir->inode->readdir_lists_children = 1;
}

/* ── /sys/dev/char ──────────────────────────────────────────────────────────
 *
 * The counterpart of /sys/dev/block, and it did not exist at all: the `dev`
 * directory carried only `block`. That is the view libudev uses to find a
 * device by its number -- udev_device_new_from_devnum() for a character device
 * looks up /sys/dev/char/<major>:<minor> and gets nothing -- so every
 * character device in the machine was invisible to anything that identifies
 * devices the way udev does rather than by path.
 *
 * Populated from the character devices actually present under /dev: a node is
 * listed here because it exists, with the numbers it really reports as
 * st_rdev. Nothing is enumerated that the kernel has not created.
 *
 * What is deliberately NOT here is the `device` symlink. On Linux it points
 * into /sys/devices/... at the bus address the node hangs off, and libdrm's
 * drmGetDevice2() follows it to read a card's vendor and device id. b1nix's
 * sysfs has no bus tree to point at -- /sys/devices holds `system` and nothing
 * else -- so there is no target that would be true. A symlink to a directory
 * we invented would be worse than its absence: absence is an honest "this
 * kernel does not publish bus topology", which a caller can handle, while a
 * wrong link is an answer it cannot check. Publishing the bus tree is the
 * separate piece of work this needs.
 */
static const char *sysfs_char_subsystem(u32 major) {
  /* Only where the major really is one of ours. An unknown major gets no
   * subsystem link rather than a guessed one. */
  switch (major) {
  case 226:
    return "drm";
  case 4:
  case 5:
  case 136:
    return "tty";
  case 1:
    return "mem";
  case 10:
    return "misc";
  default:
    return 0;
  }
}

static void sysfs_char_publish(struct vfs_node *devnode, const char *name) {
  if (!g_sysfs_devchar || !devnode || !devnode->inode)
    return;
  u64 rdev = devnode->inode->rdev;
  if (!rdev)
    return; /* no device number: nothing to file it under */
  u32 major = (u32)(rdev >> 8);
  u32 minor = (u32)(rdev & 0xff);
  /* Block devices have their own view; this one is for character devices. */
  if (major == BLK_SYSFS_MAJOR)
    return;
  /* DRM (226) publishes its own entries here, and they are better than what
   * this walk could build: each is a LINK to the card's minor directory, which
   * carries the `device` link up to the PCI node -- the chain libdrm follows
   * to name the bus a card sits on. Creating a plain directory of the same
   * name would shadow that link and leave /sys/dev/char/226:N/device missing,
   * which is worse than not publishing the entry at all. */
  if (major == 226)
    return;

  char majmin[24];
  snprintf(majmin, sizeof(majmin), "%u:%u", (unsigned)major, (unsigned)minor);
  if (sysfs_child(g_sysfs_devchar, majmin))
    return;
  struct vfs_node *cd =
      sysfs_mkchild(g_sysfs_devchar, majmin, VFS_DIRECTORY, 0);
  if (!cd)
    return;
  sysfs_mkstr(cd, "dev", "%s\n", majmin);
  /* The same four properties Linux puts in a character device's uevent. */
  sysfs_mkstr(cd, "uevent", "MAJOR=%u\nMINOR=%u\nDEVNAME=%s\n",
              (unsigned)major, (unsigned)minor, name);
  const char *sub = sysfs_char_subsystem(major);
  if (sub) {
    char target[64];
    snprintf(target, sizeof(target), "../../../class/%s", sub);
    sysfs_mk_subsystem_link(cd, target);
  }
}

/* Walk /dev and file every character device under /sys/dev/char. Called on
 * lookup and readdir, so a node created after the mount still appears -- the
 * same laziness /sys/dev/block already uses.
 *
 * Recursive, because the devices that most need this are not at the top level:
 * a DRM card is /dev/dri/card0 and a pty slave is /dev/pts/N, so a walk of
 * /dev's immediate children would have found neither -- and finding the DRM
 * node by its number is the whole reason libdrm reads this directory. DEVNAME
 * is the path relative to /dev, which is what Linux puts there ("dri/card0",
 * not "card0"). Depth is bounded: /dev is a device tree, not a filesystem to
 * recurse into without limit. */
static void sysfs_char_walk(struct vfs_node *dir, const char *prefix,
                            int depth) {
  if (!dir || depth > 3)
    return;
  for (struct vfs_node *c = dir->first_child; c; c = c->next_sibling) {
    if (!c->inode || c->deleted)
      continue;
    char name[96];
    if (prefix[0])
      snprintf(name, sizeof(name), "%s/%s", prefix, c->name);
    else
      snprintf(name, sizeof(name), "%s", c->name);
    if (c->inode->type == VFS_DIRECTORY) {
      sysfs_char_walk(c, name, depth + 1);
      continue;
    }
    if (c->inode->type != VFS_DEVICE)
      continue;
    sysfs_char_publish(c, name);
  }
}

static void sysfs_char_refresh(void) {
  if (!g_sysfs_devchar)
    return;
  struct vfs_node *dev = vfs_find_node("/dev");
  if (!dev || IS_ERR(dev))
    return;
  sysfs_char_walk(dev, "", 0);
  vfs_node_put(dev);
}

static isize sysfs_char_readdir(struct vfs_node *dir, usize offset,
                                struct dirent *buf, usize max_entries) {
  sysfs_char_refresh();
  return vfs_readdir_children(dir, offset, buf, max_entries);
}

static int sysfs_char_lookup(struct vfs_node *dir, const char *name) {
  sysfs_char_refresh();
  return sysfs_child(dir, name) ? 0 : -1;
}

static void sysfs_build_block(struct vfs_node *root) {
  struct vfs_node *block = sysfs_mkchild(root, "block", VFS_DIRECTORY, 0);
  struct vfs_node *devp = sysfs_mkchild(root, "dev", VFS_DIRECTORY, 0);
  struct vfs_node *devblock =
      devp ? sysfs_mkchild(devp, "block", VFS_DIRECTORY, 0) : 0;
  struct vfs_node *devchar =
      devp ? sysfs_mkchild(devp, "char", VFS_DIRECTORY, 0) : 0;
  struct vfs_node *classp = sysfs_mkchild(root, "class", VFS_DIRECTORY, 0);
  struct vfs_node *classblock =
      classp ? sysfs_mkchild(classp, "block", VFS_DIRECTORY, 0) : 0;
  if (!block || !devblock || !classblock)
    return;

  g_sysfs_block = block;
  g_sysfs_devblock = devblock;
  g_sysfs_classblock = classblock;
  /* A remount starts from an empty tree, so nothing may be remembered from the
   * previous one. */
  memset(g_sysfs_blkent, 0, sizeof(g_sysfs_blkent));
  g_sysfs_blk_gen = 0; /* blk_generation() is never 0 — forces the first build */

  g_sysfs_devchar = devchar;
  if (devchar && devchar->inode) {
    devchar->inode->readdir_cb = sysfs_char_readdir;
    devchar->inode->lookup_cb = sysfs_char_lookup;
    devchar->inode->readdir_lists_children = 1;
  }

  sysfs_block_hook(block);
  sysfs_block_hook(devblock);
  sysfs_block_hook(classblock);
  sysfs_block_refresh();
  sysfs_char_refresh();
}

/* ── content generators ── */
static int g_ostype(char *b, usize c) { return snprintf(b, c, "B1NIX\n"); }
static int g_osrelease(char *b, usize c) { return snprintf(b, c, "%s\n", B1NIX_RELEASE_STR); }
/* Where loadable modules are mapped, as "<base> <size>".
 *
 * On x86_64 the region is a compile-time constant, but on aarch64 it is the
 * first 2 MiB boundary past the kernel image, so it moves whenever the kernel
 * grows. Anything checking that a module really landed in the region had to
 * mirror that constant and go stale the next time the kernel changed size --
 * which is exactly what happened to the M95 check. Publish the fact instead. */
static int g_module_region(char *b, usize c) {
  return snprintf(b, c, "0x%llx %llu\n",
                  (unsigned long long)MODULE_REGION_BASE,
                  (unsigned long long)MODULE_REGION_SIZE);
}
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

/* cpufreq: the measured processor clock, in kHz, the unit every reader of these
 * files expects. b1nix does not scale frequency, so the current, minimum and
 * maximum are the same measured value — except cpuinfo_max_freq, which prefers
 * the CPU's own nominal maximum when CPUID publishes one. A CPU whose clock was
 * never measured gets no cpufreq directory at all rather than a made-up number. */
static int g_cpu_cur_freq(char *b, usize c) {
  return snprintf(b, c, "%lu\n", (unsigned long)arch_cpu_khz());
}

static int g_cpu_max_freq(char *b, usize c) {
  u32 khz = arch_cpu_max_khz();
  return snprintf(b, c, "%lu\n",
                  (unsigned long)(khz ? khz : arch_cpu_khz()));
}

static int g_cpu_min_freq(char *b, usize c) {
  return snprintf(b, c, "%lu\n", (unsigned long)arch_cpu_khz());
}

static int g_cpu_governor(char *b, usize c) {
  /* No frequency scaling: the clock is whatever the hardware runs at. */
  return snprintf(b, c, "performance\n");
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

/* SYSFS_MAGIC. statfs on a synthetic filesystem used to report ENOSYS, which
 * userspace reads as "this kernel has no statfs" rather than "this filesystem
 * has none" — systemd identifies /sys, /proc and /sys/fs/cgroup by their magic
 * numbers and takes a different path when it cannot. */
static int sysfs_statfs(struct vfs_node *node, struct b1nix_statfs *st) {
  (void)node;
  if (!st)
    return -EINVAL;
  memset(st, 0, sizeof(*st));
  st->f_type = 0x62656572;
  st->f_bsize = 4096;
  st->f_namelen = 255;
  return 0;
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
  root->inode->statfs_cb = sysfs_statfs;

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
  sysfs_mkchild(kern, "module_region", VFS_DEVICE, g_module_region);
  sysfs_mkchild(kern, "hostname", VFS_DEVICE, g_hostname);
  sysfs_mkchild(kern, "version", VFS_DEVICE, g_kversion);
  sysfs_mkchild(kern, "domainname", VFS_DEVICE, g_domainname);

  struct vfs_node *dev = sysfs_mkchild(root, "devices", VFS_DIRECTORY, 0);
  struct vfs_node *sys = sysfs_mkchild(dev, "system", VFS_DIRECTORY, 0);
  struct vfs_node *cpu = sysfs_mkchild(sys, "cpu", VFS_DIRECTORY, 0);
  sysfs_mkchild(cpu, "possible", VFS_DEVICE, g_cpu_range);
  sysfs_mkchild(cpu, "online", VFS_DEVICE, g_cpu_range);
  sysfs_mkchild(cpu, "present", VFS_DEVICE, g_cpu_range);

  /* Per-CPU clock under /sys/devices/system/cpu/cpuN/cpufreq. Crash
   * reporters and monitoring tools read these; they exist only once the clock
   * has actually been measured. */
  if (arch_cpu_khz()) {
    int ncpu = (g_max_cpus > 0) ? g_max_cpus : 1;
    for (int i = 0; i < ncpu; i++) {
      char name[16];
      snprintf(name, sizeof(name), "cpu%d", i);
      struct vfs_node *cn = sysfs_mkchild(cpu, name, VFS_DIRECTORY, 0);
      if (!cn)
        continue;
      struct vfs_node *cf = sysfs_mkchild(cn, "cpufreq", VFS_DIRECTORY, 0);
      if (!cf)
        continue;
      sysfs_mkchild(cf, "scaling_cur_freq", VFS_DEVICE, g_cpu_cur_freq);
      sysfs_mkchild(cf, "scaling_max_freq", VFS_DEVICE, g_cpu_max_freq);
      sysfs_mkchild(cf, "scaling_min_freq", VFS_DEVICE, g_cpu_min_freq);
      sysfs_mkchild(cf, "cpuinfo_cur_freq", VFS_DEVICE, g_cpu_cur_freq);
      sysfs_mkchild(cf, "cpuinfo_max_freq", VFS_DEVICE, g_cpu_max_freq);
      sysfs_mkchild(cf, "cpuinfo_min_freq", VFS_DEVICE, g_cpu_min_freq);
      sysfs_mkchild(cf, "scaling_governor", VFS_DEVICE, g_cpu_governor);
    }
  }

  struct vfs_node *mem = sysfs_mkchild(root, "memory", VFS_DIRECTORY, 0);
  sysfs_mkchild(mem, "total_kb", VFS_DEVICE, g_memtotal);

  sysfs_build_block(root);
  sysfs_build_net(root);
  /* M96: /sys/module/<name>/ — refcnt, initstate and a parameters directory —
   * for whatever is loaded now; later loads and unloads maintain the tree
   * themselves. */
  module_sysfs_attach_root(root);
  /* M101: whatever a driver registered before /sys was mounted — a DRM class,
   * a device's attribute group — appears now. Registrations after this point
   * materialise as they happen. */
  sysfs_reg_attach_root(root);
  return root;
}

void sysfs_init(void) {
  sysfs_fs.name = "sysfs";
  sysfs_fs.mount = sysfs_mount_cb;
  sysfs_fs.flags = VFS_FS_NODEV;
  vfs_register_fs(&sysfs_fs);
}
