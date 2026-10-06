/* SPDX-License-Identifier: GPL-2.0-only */
/* sysfs — synthetic /sys filesystem (M34).
 *
 * Exposes kernel configuration and hardware topology as read-on-demand
 * pseudo-files, mirroring the Linux /sys hierarchy closely enough for tools
 * like `sysctl` and `free`/`top` to probe. Same VFS_DEVICE + read_cb pattern
 * as procfs (see kernel/fs/proc/procfs.c for the rationale on node type).
 */

#include <b1nix/spinlock.h>
#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/lapic.h>
#include <b1nix/cpufreq.h>
#include <b1nix/hibernate.h>
#include <b1nix/thermal.h>
#include <b1nix/acpi_power.h>
#include <b1nix/cpuidle.h>
#include <b1nix/suspend.h>
#include <b1nix/mm.h>
#include <b1nix/thp.h>
#include <b1nix/numa.h>
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
#include <b1nix/bpf.h>
#include <b1nix/version.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef int (*sysfs_render)(char *buf, usize cap);
/* The same, for a family of files that differ only by an index: the index
 * rides in the node, so one function serves every CPU and state. */
typedef int (*sysfs_render_ctx)(int ctx, char *buf, usize cap);

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
  sysfs_render_ctx render_ctx;
  int ctx;
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
  if (!sn->render && !sn->render_ctx)
    return 0;
  char tmp[256];
  int len = sn->render ? sn->render(tmp, sizeof(tmp))
                       : sn->render_ctx(sn->ctx, tmp, sizeof(tmp));
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

/* A file rendered by `fn(ctx, ...)`. */
static struct vfs_node *sysfs_mkchild_ctx(struct vfs_node *parent,
                                          const char *name,
                                          sysfs_render_ctx fn, int ctx) {
  struct vfs_node *n = sysfs_mkchild(parent, name, VFS_DEVICE, 0);
  struct sysfs_node *sn;

  if (!n)
    return 0;
  sn = kzalloc(sizeof(*sn));
  if (sn) {
    sn->render_ctx = fn;
    sn->ctx = ctx;
    sn->live_blk = -1;
  }
  n->inode->data = sn;
  n->inode->read_cb = sysfs_read_cb;
  return n;
}

static int sysfs_node_ctx(struct vfs_node *node) {
  struct sysfs_node *sn =
      node && node->inode ? (struct sysfs_node *)node->inode->data : 0;

  return sn ? sn->ctx : -1;
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
 * /sys/class/block. The numbers are blk_devno()'s -- Linux's majors -- which
 * is exactly what /proc/partitions and /proc/self/mountinfo print, so the
 * three agree about which device is which.
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
  u32 devno; /* the major:minor it was published under */
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
                               const char *subsystem, u32 devno,
                               const char *name, const char *devtype) {
  sysfs_mkstr(dir, "uevent", "MAJOR=%d\nMINOR=%lu\nDEVNAME=%s\nDEVTYPE=%s\n",
              (int)(devno >> 8), (unsigned long)(devno & 0xFF), name, devtype);
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
  ue->major = (int)(devno >> 8);
  ue->minor = (int)(devno & 0xFF);
  sn->ue = ue;
  n->inode->mode = 0644;
  n->inode->write_cb = sysfs_uevent_write_cb;
}

/* The `subsystem` symlink every Linux device directory carries. udev reads its
 * basename to learn which subsystem a device enumerated from /sys belongs to;
 * without it a device found by a scan has no subsystem, and every
 * SUBSYSTEM=="…" rule — including the one that tags block devices for systemd
 * — silently fails to match. */
static void sysfs_mk_link(struct vfs_node *dir, const char *name,
                          const char *target) {
  if (!dir || sysfs_child(dir, name))
    return;
  struct vfs_node *n = sysfs_mkchild(dir, name, VFS_SYMLINK, 0);
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

static void sysfs_mk_subsystem_link(struct vfs_node *dir, const char *target) {
  sysfs_mk_link(dir, "subsystem", target);
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

  u32 devno = blk_devno(d);
  char majmin[24];
  snprintf(majmin, sizeof(majmin), "%u:%u", (unsigned)(devno >> 8),
           (unsigned)(devno & 0xFF));

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
      /* Both kinds carry these; a partition's start is in 512-byte sectors,
       * whatever the disk's block size. libblkid finds a partition's own
       * table entry (PART_ENTRY_TYPE, _NAME, _UUID) by matching start and
       * size against the parent's table: without start, `blkid -p` on a
       * partition answered nothing, and the installer could not tell which
       * partition was the BIOS boot one. */
      sysfs_mkstr(bd, "alignment_offset", "0\n");
      sysfs_mkstr(bd, "discard_alignment", "0\n");
      if (part) {
        u64 start = blk_partition_start(d);
        usize pbs = (parent && parent->block_size) ? parent->block_size : 512;
        sysfs_mkstr(bd, "partition", "%d\n", partno);
        sysfs_mkstr(bd, "start", "%llu\n",
                    (unsigned long long)(start * pbs / 512));
        sysfs_mkstr(bd, "ro", "0\n");
      } else {
        sysfs_mkstr(bd, "removable", "%d\n", blk_is_removable(d));
        sysfs_mkstr(bd, "ro", "0\n");
        sysfs_mk_queue_dir(bd, d);
        /* An optical drive says so the way a SCSI device does: device/type
         * 5 (TYPE_ROM). lsblk reads it for TYPE, and without it the CD drive
         * was a "disk" to every partitioner that asks lsblk which disks
         * there are. */
        if (strncmp(d->name, "sr", 2) == 0) {
          struct vfs_node *dv = sysfs_mkchild(bd, "device", VFS_DIRECTORY, 0);
          if (dv)
            sysfs_mkstr(dv, "type", "5\n");
        }
      }
      sysfs_mk_ident(bd, (int)index);
      sysfs_mk_uevent_at(bd, devpath, "block", devno, d->name, devtype);
      sysfs_mk_subsystem_link(bd, part ? "../../../class/block"
                                       : "../../class/block");
    }
  }

  /* /sys/dev/block/<major:minor> and /sys/class/block/<name>: links to the
   * device's own directory, as on Linux, not copies of it. lsblk names a
   * device by reading the /sys/dev/block link and taking its last component;
   * a directory there answered readlink with EINVAL, and `lsblk /dev/vda`
   * failed with "failed to get sysfs name" -- which is how the installer's
   * partitioning backend asks about each disk, and why it found none. */
  {
    char target[96];
    if (part && parent && parent->name)
      snprintf(target, sizeof(target), "../../block/%s/%s", parent->name,
               d->name);
    else
      snprintf(target, sizeof(target), "../../block/%s", d->name);
    if (!sysfs_child(g_sysfs_devblock, majmin))
      sysfs_mk_link(g_sysfs_devblock, majmin, target);
    if (!sysfs_child(g_sysfs_classblock, d->name))
      sysfs_mk_link(g_sysfs_classblock, d->name, target);
  }

  strncpy(g_sysfs_blkent[index].name, d->name,
          sizeof(g_sysfs_blkent[index].name) - 1);
  g_sysfs_blkent[index].name[sizeof(g_sysfs_blkent[index].name) - 1] = '\0';
  g_sysfs_blkent[index].devno = devno;
  g_sysfs_blkent[index].used = 1;
}

/* Take one registry entry back out of all three directories. */
static void sysfs_block_unpublish(usize index) {
  if (index >= SYSFS_MAX_BLK || !g_sysfs_blkent[index].used)
    return;
  const char *name = g_sysfs_blkent[index].name;

  char majmin[24];
  snprintf(majmin, sizeof(majmin), "%u:%u",
           (unsigned)(g_sysfs_blkent[index].devno >> 8),
           (unsigned)(g_sysfs_blkent[index].devno & 0xFF));
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

  spin_lock(&g_sysfs_blk_lock);
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
  spin_unlock(&g_sysfs_blk_lock);
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
  if (devnode->inode->blk_dev)
    return;
  /* DRM (226) publishes its own entries here, and they are better than what
   * this walk could build: each is a LINK to the card's minor directory, which
   * carries the `device` link up to the PCI node -- the chain libdrm follows
   * to name the bus a card sits on. Creating a plain directory of the same
   * name would shadow that link and leave /sys/dev/char/226:N/device missing,
   * which is worse than not publishing the entry at all. */
  if (major == 226)
    return;
  /* Input (13) publishes its own too: /sys/dev/char/13:N is a link to
   * /sys/devices/virtual/input/inputN/eventN, whose parent carries the
   * capabilities udev classifies the device by. A directory here shadowed
   * that link, and logind's TakeDevice for the mouse answered ENODEV. */
  if (major == 13)
    return;
  /* And so do the terminals (4: VTs and serial ports, 5: tty, console and
   * ptmx, 229: hvc): links into /sys/devices/.../tty/<name>, whose
   * `subsystem` link is what makes udev see a tty at all -- see
   * kernel/dev/tty_sysfs.c. This walk runs before those links are attached
   * to a fresh mount, so a directory made here would take their place. */
  if (major == 4 || major == 5 || major == 229)
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

/* ── /sys/class/rtc/rtc0 (M129) ──────────────────────────────────────────
 *
 * util-linux's rtcwake refuses to suspend unless the clock it is about to
 * arm is marked as a wakeup device: it opens
 * /sys/class/rtc/rtc0/device/power/wakeup and expects to read "enabled".
 * That file is the device-power-management surface Linux gives every device;
 * this kernel has no such model, and one file that tells the truth about the
 * one device that really can wake it is worth more than a model that does
 * not exist. `name` is there because rtcwake and hwclock print it. */

static int g_rtc_wakeup(char *b, usize c) {
  /* The RTC alarm is registered with the suspend path and really does end a
   * freeze — see kernel/dev/rtc_dev.c. */
  return snprintf(b, c, "enabled\n");
}

static int g_rtc_name(char *b, usize c) {
#if defined(__x86_64__)
  return snprintf(b, c, "rtc_cmos\n");
#else
  return snprintf(b, c, "pl031\n");
#endif
}

static void sysfs_build_rtc(struct vfs_node *classp) {
  struct vfs_node *rtc_class, *rtc0, *dev, *power;

  if (!classp)
    return;
  rtc_class = sysfs_mkchild(classp, "rtc", VFS_DIRECTORY, 0);
  if (!rtc_class)
    return;
  rtc0 = sysfs_mkchild(rtc_class, "rtc0", VFS_DIRECTORY, 0);
  if (!rtc0)
    return;
  sysfs_mkchild(rtc0, "name", VFS_DEVICE, g_rtc_name);
  /* Linux reaches the device through a symlink; a directory of the same name
   * is what a reader of `device/power/wakeup` actually needs, and this sysfs
   * has no symlinks. */
  dev = sysfs_mkchild(rtc0, "device", VFS_DIRECTORY, 0);
  if (!dev)
    return;
  power = sysfs_mkchild(dev, "power", VFS_DIRECTORY, 0);
  if (!power)
    return;
  sysfs_mkchild(power, "wakeup", VFS_DEVICE, g_rtc_wakeup);
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
  /* The one device in this machine that can end a suspend says so here. */
  sysfs_build_rtc(classp);
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
  /* What the processor says it is running at, where it will say (M129); the
   * measured clock otherwise. */
  u32 khz = cpufreq_cur_khz();

  return snprintf(b, c, "%lu\n",
                  (unsigned long)(khz ? khz : arch_cpu_khz()));
}

static int g_cpu_max_freq(char *b, usize c) {
  /* A scaling driver knows the ceiling it will actually honour; CPUID's
   * nominal maximum is the fallback for a machine with no driver at all. */
  u32 khz = cpufreq_max_khz();

  if (!khz)
    khz = arch_cpu_max_khz();
  return snprintf(b, c, "%lu\n",
                  (unsigned long)(khz ? khz : arch_cpu_khz()));
}

static int g_cpu_min_freq(char *b, usize c) {
  u32 khz = cpufreq_min_khz();

  return snprintf(b, c, "%lu\n",
                  (unsigned long)(khz ? khz : arch_cpu_khz()));
}

/* scaling_{max,min}_freq: the policy's window (M135) -- the processor's range
 * narrowed by what was written here and by the platform's _PPC. Without a
 * driver there is no window, only the clock. */
static int g_policy_max_freq(char *b, usize c) {
  u32 khz = cpufreq_policy_max_khz();

  return khz ? snprintf(b, c, "%lu\n", (unsigned long)khz)
             : g_cpu_max_freq(b, c);
}

static int g_policy_min_freq(char *b, usize c) {
  u32 khz = cpufreq_policy_min_khz();

  return khz ? snprintf(b, c, "%lu\n", (unsigned long)khz)
             : g_cpu_min_freq(b, c);
}

/* A decimal number written to a sysfs file, with an optional newline. */
static int sysfs_parse_ulong(const char *buffer, usize size,
                             unsigned long *out) {
  char text[24];
  usize n = size < sizeof(text) - 1 ? size : sizeof(text) - 1;
  unsigned long v = 0;

  if (!buffer || !size)
    return -EINVAL;
  memcpy(text, buffer, n);
  text[n] = 0;
  if (!text[0] || text[0] == '\n')
    return -EINVAL;
  for (usize i = 0; text[i] && text[i] != '\n'; i++) {
    if (text[i] < '0' || text[i] > '9')
      return -EINVAL;
    v = v * 10 + (unsigned long)(text[i] - '0');
  }
  *out = v;
  return 0;
}

static isize sysfs_policy_limit_write(struct vfs_node *node, int is_max,
                                      const char *buffer, usize size) {
  unsigned long v;

  (void)node;
  if (sysfs_parse_ulong(buffer, size, &v) != 0 || !v)
    return -EINVAL;
  if (cpufreq_set_policy_limit(is_max, (u32)v) != 0)
    return -EINVAL;
  return (isize)size;
}

static isize sysfs_policy_max_write(struct vfs_node *node, u64 offset,
                                    const char *buffer, usize size, int flags) {
  (void)offset;
  (void)flags;
  return sysfs_policy_limit_write(node, 1, buffer, size);
}

static isize sysfs_policy_min_write(struct vfs_node *node, u64 offset,
                                    const char *buffer, usize size, int flags) {
  (void)offset;
  (void)flags;
  return sysfs_policy_limit_write(node, 0, buffer, size);
}

/* bios_limit: what the platform allows now (_PPC), as acpi-cpufreq has it. */
static int g_cpu_bios_limit(char *b, usize c) {
  return snprintf(b, c, "%lu\n", (unsigned long)cpufreq_bios_limit_khz());
}

/* stats/time_in_state: "<kHz> <time in 10 ms units>" per state, and
 * stats/total_trans, the way Linux's cpufreq stats print them. */
static int g_cpu_time_in_state(char *b, usize c) {
  int len = 0;

  for (int i = 0; i < cpufreq_state_count(); i++)
    len += snprintf(b + len, c > (usize)len ? c - (usize)len : 0, "%lu %lu\n",
                    (unsigned long)cpufreq_state_khz(i),
                    (unsigned long)(cpufreq_state_time_ms(i) / 10));
  return len;
}

static int g_cpu_total_trans(char *b, usize c) {
  return snprintf(b, c, "%lu\n", (unsigned long)cpufreq_transitions());
}

/* The CPUs the one policy covers: all of them. */
static int g_policy_cpus(char *b, usize c) {
  int n = (g_max_cpus > 0) ? g_max_cpus : 1;
  int len = 0;

  for (int i = 0; i < n; i++)
    len += snprintf(b + len, c > (usize)len ? c - (usize)len : 0, "%s%d",
                    i ? " " : "", i);
  len += snprintf(b + len, c > (usize)len ? c - (usize)len : 0, "\n");
  return len;
}

/* /sys/devices/system/cpu/cpufreq/ondemand: the governor's tunables. */
static int g_od_sampling_rate(char *b, usize c) {
  return snprintf(b, c, "%lu\n", (unsigned long)cpufreq_od_sampling_rate_us());
}

static int g_od_up_threshold(char *b, usize c) {
  return snprintf(b, c, "%lu\n", (unsigned long)cpufreq_od_up_threshold());
}

static isize sysfs_od_rate_write(struct vfs_node *node, u64 offset,
                                 const char *buffer, usize size, int flags) {
  unsigned long v;

  (void)node;
  (void)offset;
  (void)flags;
  if (sysfs_parse_ulong(buffer, size, &v) != 0 ||
      cpufreq_od_set_sampling_rate_us((u32)v) != 0)
    return -EINVAL;
  return (isize)size;
}

static isize sysfs_od_up_write(struct vfs_node *node, u64 offset,
                               const char *buffer, usize size, int flags) {
  unsigned long v;

  (void)node;
  (void)offset;
  (void)flags;
  if (sysfs_parse_ulong(buffer, size, &v) != 0 ||
      cpufreq_od_set_up_threshold((u32)v) != 0)
    return -EINVAL;
  return (isize)size;
}

/* The frequencies the platform declared, in the order it declared them. Only
 * the ACPI driver has a list — HWP and the bus-ratio request describe a window,
 * and a made-up list is worse than an absent file, so the file is not created
 * for them (which is also what Linux does). */
static int g_cpu_avail_freqs(char *b, usize c) {
  int n = cpufreq_state_count();
  int len = 0;

  for (int i = 0; i < n; i++) {
    u32 khz = cpufreq_state_khz(i);

    if (!khz)
      continue;
    len += snprintf(b + len, c > (usize)len ? c - (usize)len : 0, "%s%lu",
                    len ? " " : "", (unsigned long)khz);
  }
  len += snprintf(b + len, c > (usize)len ? c - (usize)len : 0, "\n");
  return len;
}

/* scaling_setspeed: the userspace governor's frequency, and a write asks for
 * the slowest declared state at or above what was written. Under any other governor the
 * governor chooses, and Linux says so. */
static int g_cpu_setspeed(char *b, usize c) {
  u32 khz = cpufreq_setspeed_khz();

  if (!khz)
    return snprintf(b, c, "<unsupported>\n");
  return snprintf(b, c, "%lu\n", (unsigned long)khz);
}

static isize sysfs_setspeed_write(struct vfs_node *node, u64 offset,
                                  const char *buffer, usize size, int flags) {
  unsigned long want = 0;

  (void)node;
  (void)offset;
  (void)flags;
  if (cpufreq_state_count() == 0 ||
      sysfs_parse_ulong(buffer, size, &want) != 0 || !want)
    return -EINVAL;
  if (cpufreq_set_speed((u32)want) != 0)
    return -EINVAL;
  return (isize)size;
}

static int g_cpu_governor(char *b, usize c) {
  /* The governor in force. Without a scaling driver the clock is whatever the
   * hardware runs at, and "performance" is the truthful name for that. */
  const char *g = cpufreq_governor();

  return snprintf(b, c, "%s\n", (g && g[0] && strcmp(g, "none")) ? g
                                                                  : "performance");
}

static int g_cpu_driver(char *b, usize c) {
  return snprintf(b, c, "%s\n", cpufreq_driver_name());
}

static int g_cpu_governors(char *b, usize c) {
  /* Only what can really be asked for: a list naming governors nothing
   * implements is how a tuning daemon comes to believe it has set one. */
  return snprintf(b, c, "%s\n", cpufreq_governors());
}

/* Writing scaling_governor asks the processor for it. A driver that is not
 * there refuses, rather than accepting the write and changing nothing. */
static isize sysfs_governor_write(struct vfs_node *node, u64 offset,
                                  const char *buffer, usize size, int flags) {
  char name[32];
  usize n = size < sizeof(name) - 1 ? size : sizeof(name) - 1;

  (void)node;
  (void)offset;
  (void)flags;
  if (!buffer || !size)
    return -EINVAL;
  memcpy(name, buffer, n);
  name[n] = 0;
  while (n && (name[n - 1] == '\n' || name[n - 1] == ' '))
    name[--n] = 0;
  if (cpufreq_set_governor(name) != 0)
    return -EINVAL;
  return (isize)size;
}

static void sysfs_build_cpuidle(struct vfs_node *cn, int cpu);

/* /sys/devices/system/cpu/cpuN: its idle states (M129) and its link to the
 * one cpufreq policy (M135). */
static void sysfs_build_cpu(struct vfs_node *cpu, int i, int policy) {
  char name[16];
  struct vfs_node *cn;

  snprintf(name, sizeof(name), "cpu%d", i);
  if (sysfs_child(cpu, name))
    return;
  cn = sysfs_mkchild(cpu, name, VFS_DIRECTORY, 0);
  if (!cn)
    return;
  sysfs_build_cpuidle(cn, i);
  if (policy)
    sysfs_mk_link(cn, "cpufreq", "../cpufreq/policy0");
}

/* The boot mount of /sys comes before the secondary CPUs: they get their
 * directories when they are up. A later mount builds them all itself. */
static struct vfs_node *g_sysfs_cpu_dir;
static int g_sysfs_cpu_policy;
static int g_sysfs_cpus_built;

void sysfs_cpus_online(void) {
  int ncpu = (g_max_cpus > 0) ? g_max_cpus : 1;

  if (!g_sysfs_cpu_dir)
    return;
  for (int i = g_sysfs_cpus_built; i < ncpu; i++)
    sysfs_build_cpu(g_sysfs_cpu_dir, i, g_sysfs_cpu_policy);
  if (ncpu > g_sysfs_cpus_built)
    g_sysfs_cpus_built = ncpu;
}

static int g_cpu_range(char *b, usize c) {
  int n = (g_max_cpus > 0) ? g_max_cpus : 1;
  if (n == 1)
    return snprintf(b, c, "0\n");
  return snprintf(b, c, "0-%d\n", n - 1);
}

/* ── /sys/devices/system/node (M128) ─────────────────────────────────────
 *
 * What `numactl --hardware`, `lscpu` and libnuma read: which nodes exist,
 * which CPUs sit on each, how much memory each has and how far apart they
 * are. A machine with one node still publishes node0 — that is what Linux
 * does, and a tool that finds the directory missing concludes the kernel has
 * no NUMA support at all rather than that it has one node. */

static int node_range_str(char *b, usize c, u64 mask) {
  usize used = 0;
  int first = 1;

  for (int i = 0; i < 64;) {
    if (!(mask & (1ULL << i))) {
      i++;
      continue;
    }
    int start = i;

    while (i < 64 && (mask & (1ULL << i)))
      i++;
    if (used < c)
      used += (usize)snprintf(b + used, c - used, "%s%d", first ? "" : ",",
                              start);
    if (i - 1 > start && used < c)
      used += (usize)snprintf(b + used, c - used, "-%d", i - 1);
    first = 0;
  }
  if (used < c)
    used += (usize)snprintf(b + used, c - used, "\n");
  return (int)used;
}

static int node_meminfo(int node, char *b, usize c) {
  u64 total_kb = numa_node_bytes(node) / 1024;
  u64 free_kb = (u64)pmm_node_free_frames(node) * PAGE_SIZE / 1024;

  /* A node whose size the firmware never stated is the whole machine's. */
  if (!total_kb)
    total_kb = pmm_total_usable_memory() / 1024;
  return snprintf(b, c,
                  "Node %d MemTotal:       %lu kB\n"
                  "Node %d MemFree:        %lu kB\n"
                  "Node %d MemUsed:        %lu kB\n",
                  node, (unsigned long)total_kb, node, (unsigned long)free_kb,
                  node,
                  (unsigned long)(total_kb > free_kb ? total_kb - free_kb : 0));
}

static int node_distance_str(int node, char *b, usize c) {
  usize used = 0;

  for (int i = 0; i < numa_node_count(); i++)
    if (used < c)
      used += (usize)snprintf(b + used, c - used, "%s%d", i ? " " : "",
                              numa_distance(node, i));
  if (used < c)
    used += (usize)snprintf(b + used, c - used, "\n");
  return (int)used;
}

#define NODE_ATTRS(n)                                                        \
  static int g_node##n##_cpulist(char *b, usize c) {                         \
    return node_range_str(b, c, numa_cpumask(n));                            \
  }                                                                          \
  static int g_node##n##_cpumap(char *b, usize c) {                          \
    return snprintf(b, c, "%08lx\n", (unsigned long)numa_cpumask(n));        \
  }                                                                          \
  static int g_node##n##_meminfo(char *b, usize c) {                         \
    return node_meminfo(n, b, c);                                            \
  }                                                                          \
  static int g_node##n##_distance(char *b, usize c) {                        \
    return node_distance_str(n, b, c);                                       \
  }

NODE_ATTRS(0)
NODE_ATTRS(1)
NODE_ATTRS(2)
NODE_ATTRS(3)
NODE_ATTRS(4)
NODE_ATTRS(5)
NODE_ATTRS(6)
NODE_ATTRS(7)

struct node_attr_set {
  int (*cpulist)(char *, usize);
  int (*cpumap)(char *, usize);
  int (*meminfo)(char *, usize);
  int (*distance)(char *, usize);
};

#define NODE_SET(n)                                                          \
  { g_node##n##_cpulist, g_node##n##_cpumap, g_node##n##_meminfo,            \
    g_node##n##_distance }

static const struct node_attr_set g_node_attrs[NUMA_MAX_NODES] = {
    NODE_SET(0), NODE_SET(1), NODE_SET(2), NODE_SET(3),
    NODE_SET(4), NODE_SET(5), NODE_SET(6), NODE_SET(7),
};

static int g_node_online(char *b, usize c) {
  int n = numa_node_count();

  return node_range_str(b, c, n >= 64 ? ~0ULL : ((1ULL << n) - 1));
}

static int g_node_has_cpu(char *b, usize c) {
  u64 mask = 0;

  for (int i = 0; i < numa_node_count(); i++)
    if (numa_cpumask(i))
      mask |= 1ULL << i;
  return node_range_str(b, c, mask);
}

static void sysfs_build_nodes(struct vfs_node *sys) {
  struct vfs_node *nd = sysfs_mkchild(sys, "node", VFS_DIRECTORY, 0);
  int n = numa_node_count();

  if (!nd)
    return;
  sysfs_mkchild(nd, "online", VFS_DEVICE, g_node_online);
  sysfs_mkchild(nd, "possible", VFS_DEVICE, g_node_online);
  sysfs_mkchild(nd, "has_memory", VFS_DEVICE, g_node_online);
  sysfs_mkchild(nd, "has_normal_memory", VFS_DEVICE, g_node_online);
  sysfs_mkchild(nd, "has_cpu", VFS_DEVICE, g_node_has_cpu);
  for (int i = 0; i < n && i < NUMA_MAX_NODES; i++) {
    char name[16];
    struct vfs_node *one;

    snprintf(name, sizeof(name), "node%d", i);
    one = sysfs_mkchild(nd, name, VFS_DIRECTORY, 0);
    if (!one)
      continue;
    sysfs_mkchild(one, "cpulist", VFS_DEVICE, g_node_attrs[i].cpulist);
    sysfs_mkchild(one, "cpumap", VFS_DEVICE, g_node_attrs[i].cpumap);
    sysfs_mkchild(one, "meminfo", VFS_DEVICE, g_node_attrs[i].meminfo);
    sysfs_mkchild(one, "distance", VFS_DEVICE, g_node_attrs[i].distance);
  }
}

/* ── /sys/devices/system/cpu/cpuN/cpuidle (M129, M135) ──────────────────
 *
 * One directory per idle state, with the counters powertop and every
 * monitoring agent read, and `disable`, which takes a state away from the
 * governor on that CPU. The context of each file is cpu * 256 + state. */

enum { CI_NAME, CI_DESC, CI_USAGE, CI_TIME, CI_LAT, CI_RES, CI_POWER, CI_ABOVE,
       CI_BELOW, CI_DISABLE };

static int cpuidle_attr(int which, int ctx, char *b, usize c) {
  int cpu = ctx / 256, state = ctx % 256;

  switch (which) {
  case CI_NAME:
    return snprintf(b, c, "%s\n", cpuidle_state_name(state));
  case CI_DESC:
    return snprintf(b, c, "%s\n", cpuidle_state_desc(state));
  case CI_USAGE:
    return snprintf(b, c, "%lu\n",
                    (unsigned long)cpuidle_state_usage(cpu, state));
  case CI_TIME:
    return snprintf(b, c, "%lu\n",
                    (unsigned long)cpuidle_state_time_us(cpu, state));
  case CI_LAT:
    return snprintf(b, c, "%lu\n",
                    (unsigned long)cpuidle_state_latency_us(state));
  case CI_RES:
    return snprintf(b, c, "%lu\n",
                    (unsigned long)cpuidle_state_residency_us(state));
  case CI_POWER:
    return snprintf(b, c, "%lu\n",
                    (unsigned long)cpuidle_state_power_mw(state));
  case CI_ABOVE:
    return snprintf(b, c, "%lu\n",
                    (unsigned long)cpuidle_state_above(cpu, state));
  case CI_BELOW:
    return snprintf(b, c, "%lu\n",
                    (unsigned long)cpuidle_state_below(cpu, state));
  default:
    return snprintf(b, c, "%d\n", cpuidle_state_disabled(cpu, state));
  }
}

#define CI_FN(tag, which)                                                    \
  static int g_ci_##tag(int ctx, char *b, usize c) {                         \
    return cpuidle_attr(which, ctx, b, c);                                   \
  }
CI_FN(name, CI_NAME)
CI_FN(desc, CI_DESC)
CI_FN(usage, CI_USAGE)
CI_FN(time, CI_TIME)
CI_FN(lat, CI_LAT)
CI_FN(res, CI_RES)
CI_FN(power, CI_POWER)
CI_FN(above, CI_ABOVE)
CI_FN(below, CI_BELOW)
CI_FN(disable, CI_DISABLE)

static isize sysfs_cpuidle_disable_write(struct vfs_node *node, u64 offset,
                                         const char *buffer, usize size,
                                         int flags) {
  int ctx = sysfs_node_ctx(node);
  unsigned long v;

  (void)offset;
  (void)flags;
  if (ctx < 0 || sysfs_parse_ulong(buffer, size, &v) != 0 || v > 1)
    return -EINVAL;
  if (cpuidle_state_set_disabled(ctx / 256, ctx % 256, (int)v) != 0)
    return -EINVAL;
  return (isize)size;
}

static void sysfs_build_cpuidle(struct vfs_node *cpu_dir, int cpu) {
  static const struct {
    const char *name;
    sysfs_render_ctx fn;
  } files[] = {
      {"name", g_ci_name},       {"desc", g_ci_desc},   {"usage", g_ci_usage},
      {"time", g_ci_time},       {"latency", g_ci_lat}, {"residency", g_ci_res},
      {"power", g_ci_power},     {"above", g_ci_above}, {"below", g_ci_below},
  };
  struct vfs_node *ci;
  int n = cpuidle_state_count();

  if (cpu < 0 || cpu >= 256 || n <= 0)
    return;
  ci = sysfs_mkchild(cpu_dir, "cpuidle", VFS_DIRECTORY, 0);
  if (!ci)
    return;
  for (int st = 0; st < n && st < 256; st++) {
    char name[16];
    struct vfs_node *sd, *f;
    int ctx = cpu * 256 + st;

    snprintf(name, sizeof(name), "state%d", st);
    sd = sysfs_mkchild(ci, name, VFS_DIRECTORY, 0);
    if (!sd)
      continue;
    for (usize i = 0; i < sizeof(files) / sizeof(files[0]); i++)
      sysfs_mkchild_ctx(sd, files[i].name, files[i].fn, ctx);
    f = sysfs_mkchild_ctx(sd, "disable", g_ci_disable, ctx);
    if (f && f->inode) {
      f->inode->mode = 0644;
      f->inode->write_cb = sysfs_cpuidle_disable_write;
    }
  }
}

/* /sys/devices/system/cpu/cpuidle: which driver found the states and which
 * governor picks among them. */
static int g_cpuidle_driver(char *b, usize c) {
  return snprintf(b, c, "%s\n", cpuidle_driver_name());
}

static int g_cpuidle_governor(char *b, usize c) {
  return snprintf(b, c, "%s\n", cpuidle_governor_name());
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

/* /sys/kernel/mm/transparent_hugepage/enabled (M128).
 *
 * Linux's format exactly: the three modes with the live one in brackets, so
 * anything that greps for "[never]" or writes "madvise" behaves as it would
 * there. Writable, because a program that wants huge pages for a run should
 * not need a reboot to get them. */
static int sysfs_thp_enabled(char *buf, usize cap) {
  int m = thp_mode();

  return snprintf(buf, cap, "%s %s %s\n",
                  m == THP_MODE_ALWAYS ? "[always]" : "always",
                  m == THP_MODE_MADVISE ? "[madvise]" : "madvise",
                  m == THP_MODE_NEVER ? "[never]" : "never");
}

static isize sysfs_thp_enabled_write(struct vfs_node *node, u64 offset,
                                     const char *buffer, usize size,
                                     int flags) {
  (void)node;
  (void)offset;
  (void)flags;
  if (!buffer || size == 0)
    return -EINVAL;
  if (size >= 6 && memcmp(buffer, "always", 6) == 0)
    thp_set_mode(THP_MODE_ALWAYS);
  else if (size >= 7 && memcmp(buffer, "madvise", 7) == 0)
    thp_set_mode(THP_MODE_MADVISE);
  else if (size >= 5 && memcmp(buffer, "never", 5) == 0)
    thp_set_mode(THP_MODE_NEVER);
  else
    return -EINVAL;
  return (isize)size;
}

/* The four numbers that say whether the feature is doing anything: blocks
 * installed by a fault, faults that wanted one and could not have it, blocks
 * broken back into leaves, and ranges khugepaged collapsed into a block. */
static int sysfs_thp_stats(char *buf, usize cap) {
  return snprintf(buf, cap,
                  "thp_fault_alloc %lu\nthp_fault_fallback %lu\n"
                  "thp_split_page %lu\nthp_collapse_alloc %lu\n",
                  (unsigned long)thp_stat_alloc(),
                  (unsigned long)thp_stat_fallback(),
                  (unsigned long)thp_stat_split(),
                  (unsigned long)thp_stat_collapse());
}

/* /sys/kernel/mm/transparent_hugepage/khugepaged/, the two files of it that
 * mean something here: how many ranges the thread has collapsed, and how long
 * it sleeps between passes. Linux's directory has more knobs; inventing files
 * that change nothing would be worse than not having them. */
static int sysfs_khugepaged_collapsed(char *buf, usize cap) {
  return snprintf(buf, cap, "%lu\n", (unsigned long)thp_stat_collapse());
}

static int sysfs_khugepaged_sleep(char *buf, usize cap) {
  return snprintf(buf, cap, "%lu\n", (unsigned long)thp_scan_sleep_ms());
}

static isize sysfs_khugepaged_sleep_write(struct vfs_node *node, u64 offset,
                                          const char *buffer, usize size,
                                          int flags) {
  char tmp[24];
  usize n = size < sizeof(tmp) - 1 ? size : sizeof(tmp) - 1;
  u64 ms = 0;

  (void)node;
  (void)offset;
  (void)flags;
  if (!buffer || size == 0)
    return -EINVAL;
  memcpy(tmp, buffer, n);
  tmp[n] = '\0';
  for (usize i = 0; tmp[i] && tmp[i] != '\n'; i++) {
    if (tmp[i] < '0' || tmp[i] > '9')
      return -EINVAL;
    ms = ms * 10 + (u64)(tmp[i] - '0');
  }
  thp_set_scan_sleep_ms(ms);
  return (isize)size;
}

/* ── /sys/class/power_supply and /sys/class/thermal (M134) ───────────────
 *
 * Built only for what the firmware really declares. Every read re-evaluates
 * the firmware method behind it, because that is the only way a battery's
 * charge is ever current; a method that refuses returns the error rather
 * than a number, and the file reads as an error too.
 */
/* /sys/class/power_supply/BATn: one render for every attribute, the battery
 * and the attribute riding in the context as bat * 64 + which. */
static int g_bat_attr(int ctx, char *b, usize c) {
  return acpi_power_battery_attr(ctx / 64, ctx % 64, b, c);
}

static isize sysfs_bat_alarm_write(struct vfs_node *node, u64 offset,
                                   const char *buffer, usize size, int flags) {
  unsigned long v;

  (void)offset;
  (void)flags;
  if (sysfs_parse_ulong(buffer, size, &v) != 0 ||
      acpi_power_battery_set_alarm(sysfs_node_ctx(node) / 64, (u64)v) != 0)
    return -EINVAL;
  return (isize)size;
}

static isize sysfs_bat_charge_write(struct vfs_node *node, u64 offset,
                                    const char *buffer, usize size, int flags) {
  char mode[24];
  usize n = size;

  (void)offset;
  (void)flags;
  while (n && (buffer[n - 1] == '\n' || buffer[n - 1] == ' '))
    n--;
  if (!n || n >= sizeof(mode))
    return -EINVAL;
  memcpy(mode, buffer, n);
  mode[n] = 0;
  if (acpi_power_battery_set_charge_behaviour(sysfs_node_ctx(node) / 64, mode) != 0)
    return -EINVAL;
  return (isize)size;
}

static int g_ac0_online(char *b, usize c) { return acpi_power_ac_attr(0, b, c); }
static int g_ac0_type(char *b, usize c) { return snprintf(b, c, "Mains\n"); }

/* /sys/class/thermal (M134, M135): a zone's files take the zone index as
 * their context, a trip's take zone * 64 + trip, a binding's zone * 64 + k. */
static int g_tz_type(int z, char *b, usize c) {
  return acpi_power_thermal_attr(z, ACPI_TZ_TYPE, b, c);
}

static int g_tz_temp(int z, char *b, usize c) {
  int mc;

  if (thermal_trip_count(z) == 0 && thermal_zone_emul(z) == 0)
    return acpi_power_thermal_attr(z, ACPI_TZ_TEMP, b, c);
  if (thermal_zone_temp(z, &mc) != 0)
    return -1;
  return snprintf(b, c, "%d\n", mc);
}

static int g_tz_emul(int z, char *b, usize c) {
  return snprintf(b, c, "%d\n", thermal_zone_emul(z));
}

static isize sysfs_tz_emul_write(struct vfs_node *node, u64 offset,
                                 const char *buffer, usize size, int flags) {
  unsigned long v;

  (void)offset;
  (void)flags;
  if (sysfs_parse_ulong(buffer, size, &v) != 0 || v > 1000000ul ||
      thermal_zone_set_emul(sysfs_node_ctx(node), (int)v) != 0)
    return -EINVAL;
  return (isize)size;
}

static int g_tz_mode(int z, char *b, usize c) {
  (void)z;
  return snprintf(b, c, "enabled\n");
}

static int g_tz_policy(int z, char *b, usize c) {
  (void)z;
  return snprintf(b, c, "step_wise\n");
}

static int g_trip_type(int ctx, char *b, usize c) {
  const char *type;
  int mc;

  if (thermal_trip(ctx / 64, ctx % 64, &type, &mc) != 0)
    return -1;
  return snprintf(b, c, "%s\n", type);
}

static int g_trip_temp(int ctx, char *b, usize c) {
  const char *type;
  int mc;

  if (thermal_trip(ctx / 64, ctx % 64, &type, &mc) != 0)
    return -1;
  return snprintf(b, c, "%d\n", mc);
}

static int g_trip_hyst(int ctx, char *b, usize c) {
  (void)ctx;
  return snprintf(b, c, "0\n");
}

static int g_bind_trip(int ctx, char *b, usize c) {
  int cdev, trip;

  if (thermal_zone_binding(ctx / 64, ctx % 64, &cdev, &trip) != 0)
    return -1;
  return snprintf(b, c, "%d\n", trip);
}

static int g_cdev_type(int cd, char *b, usize c) {
  return snprintf(b, c, "%s\n", thermal_cdev_type(cd));
}

static int g_cdev_max(int cd, char *b, usize c) {
  return snprintf(b, c, "%d\n", thermal_cdev_max_state(cd));
}

static int g_cdev_cur(int cd, char *b, usize c) {
  return snprintf(b, c, "%d\n", thermal_cdev_cur_state(cd));
}

static isize sysfs_cdev_cur_write(struct vfs_node *node, u64 offset,
                                  const char *buffer, usize size, int flags) {
  unsigned long v;

  (void)offset;
  (void)flags;
  if (sysfs_parse_ulong(buffer, size, &v) != 0 ||
      thermal_cdev_set_state(sysfs_node_ctx(node), (int)v) != 0)
    return -EINVAL;
  return (isize)size;
}

static void sysfs_build_thermal(struct vfs_node *td, int ntz) {
  for (int i = 0; i < ntz && i < ACPI_PS_MAX_THERMAL; i++) {
    char name[32];
    struct vfs_node *d, *f;

    snprintf(name, sizeof(name), "thermal_zone%d", i);
    d = sysfs_mkchild(td, name, VFS_DIRECTORY, 0);
    if (!d)
      continue;
    sysfs_mkchild_ctx(d, "type", g_tz_type, i);
    sysfs_mkchild_ctx(d, "temp", g_tz_temp, i);
    if (thermal_trip_count(i) == 0)
      continue; /* a zone nothing acts on: the M134 files only */
    sysfs_mkchild_ctx(d, "mode", g_tz_mode, i);
    sysfs_mkchild_ctx(d, "policy", g_tz_policy, i);
    sysfs_mkchild_ctx(d, "available_policies", g_tz_policy, i);
    f = sysfs_mkchild_ctx(d, "emul_temp", g_tz_emul, i);
    if (f && f->inode) {
      f->inode->mode = 0644;
      f->inode->write_cb = sysfs_tz_emul_write;
    }
    for (int t = 0; t < thermal_trip_count(i) && t < 64; t++) {
      snprintf(name, sizeof(name), "trip_point_%d_type", t);
      sysfs_mkchild_ctx(d, name, g_trip_type, i * 64 + t);
      snprintf(name, sizeof(name), "trip_point_%d_temp", t);
      sysfs_mkchild_ctx(d, name, g_trip_temp, i * 64 + t);
      snprintf(name, sizeof(name), "trip_point_%d_hyst", t);
      sysfs_mkchild_ctx(d, name, g_trip_hyst, i * 64 + t);
    }
    for (int k = 0; k < 64; k++) {
      char target[40];
      int cdev, trip;

      if (thermal_zone_binding(i, k, &cdev, &trip) != 0)
        break;
      snprintf(name, sizeof(name), "cdev%d", k);
      snprintf(target, sizeof(target), "../cooling_device%d", cdev);
      sysfs_mk_link(d, name, target);
      snprintf(name, sizeof(name), "cdev%d_trip_point", k);
      sysfs_mkchild_ctx(d, name, g_bind_trip, i * 64 + k);
    }
  }
  for (int cd = 0; cd < thermal_cdev_count(); cd++) {
    char name[32];
    struct vfs_node *d, *f;

    snprintf(name, sizeof(name), "cooling_device%d", cd);
    d = sysfs_mkchild(td, name, VFS_DIRECTORY, 0);
    if (!d)
      continue;
    sysfs_mkchild_ctx(d, "type", g_cdev_type, cd);
    sysfs_mkchild_ctx(d, "max_state", g_cdev_max, cd);
    f = sysfs_mkchild_ctx(d, "cur_state", g_cdev_cur, cd);
    if (f && f->inode) {
      f->inode->mode = 0644;
      f->inode->write_cb = sysfs_cdev_cur_write;
    }
  }
}

/* /sys/class already exists by the time this runs; find it rather than
 * attaching a second directory of the same name. */
static struct vfs_node *sysfs_class_dir(struct vfs_node *root) {
  for (struct vfs_node *c = root->first_child; c; c = c->next_sibling)
    if (strcmp(c->name, "class") == 0)
      return c;
  return sysfs_mkchild(root, "class", VFS_DIRECTORY, 0);
}

static void sysfs_build_acpi_power(struct vfs_node *root) {
  int nbat = acpi_power_battery_count();
  int nac = acpi_power_ac_count();
  int ntz = acpi_power_thermal_count();
  struct vfs_node *classp;

  if (nbat <= 0 && nac <= 0 && ntz <= 0)
    return;          /* no battery, no adapter, no zone: publish nothing */
  classp = sysfs_class_dir(root);
  if (!classp)
    return;

  if (nbat > 0 || nac > 0) {
    struct vfs_node *psd = sysfs_mkchild(classp, "power_supply",
                                         VFS_DIRECTORY, 0);
    if (psd) {
      for (int i = 0; i < nbat && i < ACPI_PS_MAX_BATTERY; i++) {
        char name[8];
        struct vfs_node *d;
        snprintf(name, sizeof(name), "BAT%d", i);
        d = sysfs_mkchild(psd, name, VFS_DIRECTORY, 0);
        if (!d)
          continue;
        {
          /* Linux names the capacity files after the unit the firmware
           * chose, and a reader that divides one by the other has to be
           * told which it got. */
          int e = acpi_power_battery_in_energy_units(i);
          const struct {
            const char *name;
            int which;
          } files[] = {
              {"type", ACPI_BAT_TYPE},
              {"present", ACPI_BAT_PRESENT},
              {"status", ACPI_BAT_STATUS},
              {"capacity", ACPI_BAT_CAPACITY},
              {"capacity_level", ACPI_BAT_CAPACITY_LEVEL},
              {e ? "energy_now" : "charge_now", ACPI_BAT_NOW},
              {e ? "energy_full" : "charge_full", ACPI_BAT_FULL},
              {e ? "energy_full_design" : "charge_full_design",
               ACPI_BAT_FULL_DESIGN},
              {e ? "power_now" : "current_now", ACPI_BAT_RATE},
              {"voltage_now", ACPI_BAT_VOLTAGE},
              {"voltage_min_design", ACPI_BAT_VOLTAGE_MIN},
              {"technology", ACPI_BAT_TECHNOLOGY},
              {"model_name", ACPI_BAT_MODEL},
              {"serial_number", ACPI_BAT_SERIAL},
              {"manufacturer", ACPI_BAT_MANUFACTURER},
          };
          struct vfs_node *f;

          for (usize k = 0; k < sizeof(files) / sizeof(files[0]); k++)
            sysfs_mkchild_ctx(d, files[k].name, g_bat_attr,
                              i * 64 + files[k].which);
          if (acpi_power_battery_has(i, "_BIX"))
            sysfs_mkchild_ctx(d, "cycle_count", g_bat_attr,
                              i * 64 + ACPI_BAT_CYCLES);
          if (acpi_power_battery_has(i, "_BTP")) {
            f = sysfs_mkchild_ctx(d, "alarm", g_bat_attr,
                                  i * 64 + ACPI_BAT_ALARM);
            if (f && f->inode) {
              f->inode->mode = 0644;
              f->inode->write_cb = sysfs_bat_alarm_write;
            }
          }
          if (acpi_power_battery_has_charge_control(i)) {
            f = sysfs_mkchild_ctx(d, "charge_behaviour", g_bat_attr,
                                  i * 64 + ACPI_BAT_CHARGE_BEHAVIOUR);
            if (f && f->inode) {
              f->inode->mode = 0644;
              f->inode->write_cb = sysfs_bat_charge_write;
            }
          }
        }
      }
      if (nac > 0) {
        struct vfs_node *d = sysfs_mkchild(psd, "AC0", VFS_DIRECTORY, 0);
        if (d) {
          sysfs_mkchild(d, "type", VFS_DEVICE, g_ac0_type);
          sysfs_mkchild(d, "online", VFS_DEVICE, g_ac0_online);
        }
      }
    }
  }

  if (ntz > 0) {
    struct vfs_node *td = sysfs_mkchild(classp, "thermal", VFS_DIRECTORY, 0);

    if (td)
      sysfs_build_thermal(td, ntz);
  }
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

/* ── /sys/power/state (M129) ─────────────────────────────────────────────
 *
 * The one file every suspend tool writes to, from `systemctl suspend` down to
 * `echo freeze > /sys/power/state`. Reading it lists the states this machine
 * really has — which is why it is rendered from suspend_states() rather than
 * written out here: a kernel that advertises `mem` and cannot come back from
 * it has told the tool to hang the machine.
 *
 * A write blocks for as long as the machine is suspended and returns when it
 * has resumed, which is exactly the contract userspace already expects. */
static int g_power_state(char *b, usize c) {
  return snprintf(b, c, "%s\n", suspend_states());
}

static isize sysfs_power_state_write(struct vfs_node *node, u64 offset,
                                     const char *buffer, usize size,
                                     int flags) {
  char name[32];
  usize n = size < sizeof(name) - 1 ? size : sizeof(name) - 1;
  int rc;

  (void)node;
  (void)offset;
  (void)flags;
  if (!buffer || !size)
    return -EINVAL;
  memcpy(name, buffer, n);
  name[n] = 0;
  while (n && (name[n - 1] == '\n' || name[n - 1] == ' ' || name[n - 1] == '\t'))
    name[--n] = 0;
  rc = suspend_enter(name);
  if (rc < 0)
    return rc;
  return (isize)size;
}

/* /sys/power/mem_sleep and wakeup_count (M135). */
static int g_power_mem_sleep(char *b, usize c) {
  return snprintf(b, c, "%s\n", suspend_mem_sleep());
}

static isize sysfs_mem_sleep_write(struct vfs_node *node, u64 offset,
                                   const char *buffer, usize size, int flags) {
  char name[16];
  usize n = size < sizeof(name) - 1 ? size : sizeof(name) - 1;

  (void)node;
  (void)offset;
  (void)flags;
  if (!buffer || !size)
    return -EINVAL;
  memcpy(name, buffer, n);
  name[n] = 0;
  while (n && (name[n - 1] == '\n' || name[n - 1] == ' '))
    name[--n] = 0;
  return suspend_set_mem_sleep(name) == 0 ? (isize)size : -EINVAL;
}

/* /sys/power/disk and /sys/power/resume (M135): what follows the image, and
 * where it goes. */
static int g_power_disk(char *b, usize c) {
  return snprintf(b, c, "%s\n", hibernate_disk_modes());
}

static int g_power_resume(char *b, usize c) {
  return snprintf(b, c, "%s\n", hibernate_resume_device());
}

static isize sysfs_power_word_write(const char *buffer, usize size,
                                    int (*set)(const char *)) {
  char w[64];
  usize n = size < sizeof(w) - 1 ? size : sizeof(w) - 1;

  if (!buffer || !size)
    return -EINVAL;
  memcpy(w, buffer, n);
  w[n] = 0;
  while (n && (w[n - 1] == '\n' || w[n - 1] == ' '))
    w[--n] = 0;
  return set(w) == 0 ? (isize)size : -EINVAL;
}

static isize sysfs_disk_write(struct vfs_node *node, u64 offset,
                              const char *buffer, usize size, int flags) {
  (void)node;
  (void)offset;
  (void)flags;
  return sysfs_power_word_write(buffer, size, hibernate_set_disk_mode);
}

static isize sysfs_resume_write(struct vfs_node *node, u64 offset,
                                const char *buffer, usize size, int flags) {
  (void)node;
  (void)offset;
  (void)flags;
  return sysfs_power_word_write(buffer, size, hibernate_set_resume_device);
}

static int g_power_sync_on_suspend(char *b, usize c) {
  return snprintf(b, c, "%d\n", suspend_sync_on_suspend());
}

static isize sysfs_sync_on_suspend_write(struct vfs_node *node, u64 offset,
                                         const char *buffer, usize size,
                                         int flags) {
  unsigned long v;

  (void)node;
  (void)offset;
  (void)flags;
  if (sysfs_parse_ulong(buffer, size, &v) != 0 || v > 1)
    return -EINVAL;
  suspend_set_sync_on_suspend((int)v);
  return (isize)size;
}

static int g_power_wakeup_count(char *b, usize c) {
  return snprintf(b, c, "%lu\n", (unsigned long)suspend_event_count());
}

static isize sysfs_wakeup_count_write(struct vfs_node *node, u64 offset,
                                      const char *buffer, usize size,
                                      int flags) {
  unsigned long v;

  (void)node;
  (void)offset;
  (void)flags;
  if (sysfs_parse_ulong(buffer, size, &v) != 0)
    return -EINVAL;
  return suspend_save_event_count((u64)v) == 0 ? (isize)size : -EINVAL;
}

/* /sys/class/wakeup/wakeupN: one per wake source, with Linux's counters. An
 * event here is instantaneous (nothing holds the machine awake for a
 * while), so the active and prevent-suspend times are truly zero. */
static int g_wk_name(int i, char *b, usize c) {
  return snprintf(b, c, "%s\n", suspend_source_name(i));
}

static int g_wk_events(int i, char *b, usize c) {
  return snprintf(b, c, "%lu\n",
                  (unsigned long)suspend_source_stat(i, SUSPEND_STAT_EVENTS));
}

static int g_wk_wakeups(int i, char *b, usize c) {
  return snprintf(b, c, "%lu\n",
                  (unsigned long)suspend_source_stat(i, SUSPEND_STAT_WAKEUPS));
}

static int g_wk_last(int i, char *b, usize c) {
  return snprintf(b, c, "%lu\n",
                  (unsigned long)suspend_source_stat(i, SUSPEND_STAT_LAST_MS));
}

static int g_wk_zero(int i, char *b, usize c) {
  (void)i;
  return snprintf(b, c, "0\n");
}

static void sysfs_build_wakeup(struct vfs_node *root) {
  struct vfs_node *classp, *wk;
  int n = suspend_source_count();

  if (n <= 0)
    return;
  classp = sysfs_class_dir(root);
  wk = classp ? sysfs_mkchild(classp, "wakeup", VFS_DIRECTORY, 0) : 0;
  if (!wk)
    return;
  for (int i = 0; i < n; i++) {
    static const char *const zeros[] = {"expire_count", "active_time_ms",
                                        "total_time_ms", "max_time_ms",
                                        "prevent_suspend_time_ms"};
    char name[24];
    struct vfs_node *d;

    snprintf(name, sizeof(name), "wakeup%d", i);
    d = sysfs_mkchild(wk, name, VFS_DIRECTORY, 0);
    if (!d)
      continue;
    sysfs_mkchild_ctx(d, "name", g_wk_name, i);
    sysfs_mkchild_ctx(d, "event_count", g_wk_events, i);
    sysfs_mkchild_ctx(d, "active_count", g_wk_events, i);
    sysfs_mkchild_ctx(d, "wakeup_count", g_wk_wakeups, i);
    sysfs_mkchild_ctx(d, "last_change_ms", g_wk_last, i);
    for (usize k = 0; k < sizeof(zeros) / sizeof(zeros[0]); k++)
      sysfs_mkchild_ctx(d, zeros[k], g_wk_zero, i);
  }
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

static struct vfs_node *sysfs_build_tree(void) {
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
  /* /sys/kernel/mm/transparent_hugepage — the state of M128's huge pages. */
  if (km) {
    struct vfs_node *th =
        sysfs_mkchild(km, "transparent_hugepage", VFS_DIRECTORY, 0);

    if (th) {
      struct vfs_node *en =
          sysfs_mkchild(th, "enabled", VFS_DEVICE, sysfs_thp_enabled);

      if (en) {
        en->inode->mode = 0644;
        en->inode->write_cb = sysfs_thp_enabled_write;
      }
      sysfs_mkstr(th, "hpage_pmd_size", "%lu\n", (unsigned long)THP_SIZE);
      sysfs_mkchild(th, "stats", VFS_DEVICE, sysfs_thp_stats);
      {
        struct vfs_node *kh = sysfs_mkchild(th, "khugepaged", VFS_DIRECTORY, 0);

        if (kh) {
          struct vfs_node *sl;

          sysfs_mkchild(kh, "pages_collapsed", VFS_DEVICE,
                        sysfs_khugepaged_collapsed);
          sl = sysfs_mkchild(kh, "scan_sleep_millisecs", VFS_DEVICE,
                             sysfs_khugepaged_sleep);
          if (sl) {
            sl->inode->mode = 0644;
            sl->inode->write_cb = sysfs_khugepaged_sleep_write;
          }
        }
      }
    }
  }
  /* Where tracefs is mounted, as on Linux: an empty directory until someone
   * mounts it (systemd, perf, libtracefs do). */
  sysfs_mkchild(kern, "tracing", VFS_DIRECTORY, 0);
  /* The kernel's BTF, which CO-RE loaders relocate programs against. Present
   * only when the build had pahole: no file is better than a wrong one. */
  if (btf_vmlinux_size()) {
    struct vfs_node *btfd = sysfs_mkchild(kern, "btf", VFS_DIRECTORY, 0);
    struct vfs_node *vml =
        btfd ? sysfs_mkchild(btfd, "vmlinux", VFS_DEVICE, 0) : 0;

    if (vml) {
      vml->inode->read_cb = btf_vmlinux_read;
      vml->inode->size = btf_vmlinux_size();
    }
  }
  sysfs_mkchild(kern, "ostype", VFS_DEVICE, g_ostype);
  sysfs_mkchild(kern, "osrelease", VFS_DEVICE, g_osrelease);
  sysfs_mkchild(kern, "module_region", VFS_DEVICE, g_module_region);
  sysfs_mkchild(kern, "hostname", VFS_DEVICE, g_hostname);
  sysfs_mkchild(kern, "version", VFS_DEVICE, g_kversion);
  sysfs_mkchild(kern, "domainname", VFS_DEVICE, g_domainname);

  /* /sys/fs/cgroup: the mount point of the cgroup2 hierarchy, which Linux's
   * sysfs provides empty. OpenRC's cgroups service and podman look for the
   * hierarchy exactly there, and without the directory there is nowhere to
   * mount it. */
  struct vfs_node *fsd = sysfs_mkchild(root, "fs", VFS_DIRECTORY, 0);
  if (fsd)
    sysfs_mkchild(fsd, "cgroup", VFS_DIRECTORY, 0);

  /* /sys/power/state — the suspend surface (M129). */
  {
    struct vfs_node *pw = sysfs_mkchild(root, "power", VFS_DIRECTORY, 0);
    struct vfs_node *st =
        pw ? sysfs_mkchild(pw, "state", VFS_DEVICE, g_power_state) : 0;

    if (st && st->inode) {
      st->inode->mode = 0644;
      st->inode->write_cb = sysfs_power_state_write;
    }
    st = pw ? sysfs_mkchild(pw, "mem_sleep", VFS_DEVICE, g_power_mem_sleep) : 0;
    if (st && st->inode) {
      st->inode->mode = 0644;
      st->inode->write_cb = sysfs_mem_sleep_write;
    }
    st = pw ? sysfs_mkchild(pw, "disk", VFS_DEVICE, g_power_disk) : 0;
    if (st && st->inode) {
      st->inode->mode = 0644;
      st->inode->write_cb = sysfs_disk_write;
    }
    st = pw ? sysfs_mkchild(pw, "resume", VFS_DEVICE, g_power_resume) : 0;
    if (st && st->inode) {
      st->inode->mode = 0644;
      st->inode->write_cb = sysfs_resume_write;
    }
    st = pw ? sysfs_mkchild(pw, "sync_on_suspend", VFS_DEVICE,
                            g_power_sync_on_suspend)
            : 0;
    if (st && st->inode) {
      st->inode->mode = 0644;
      st->inode->write_cb = sysfs_sync_on_suspend_write;
    }
    st = pw ? sysfs_mkchild(pw, "wakeup_count", VFS_DEVICE,
                            g_power_wakeup_count)
            : 0;
    if (st && st->inode) {
      st->inode->mode = 0644;
      st->inode->write_cb = sysfs_wakeup_count_write;
    }
  }

  struct vfs_node *dev = sysfs_mkchild(root, "devices", VFS_DIRECTORY, 0);
  struct vfs_node *sys = sysfs_mkchild(dev, "system", VFS_DIRECTORY, 0);
  struct vfs_node *cpu = sysfs_mkchild(sys, "cpu", VFS_DIRECTORY, 0);
  sysfs_mkchild(cpu, "possible", VFS_DEVICE, g_cpu_range);
  sysfs_mkchild(cpu, "online", VFS_DEVICE, g_cpu_range);
  sysfs_mkchild(cpu, "present", VFS_DEVICE, g_cpu_range);
  if (cpuidle_state_count() > 0) {
    struct vfs_node *cid = sysfs_mkchild(cpu, "cpuidle", VFS_DIRECTORY, 0);

    if (cid) {
      sysfs_mkchild(cid, "current_driver", VFS_DEVICE, g_cpuidle_driver);
      sysfs_mkchild(cid, "current_governor", VFS_DEVICE, g_cpuidle_governor);
      sysfs_mkchild(cid, "current_governor_ro", VFS_DEVICE,
                    g_cpuidle_governor);
      sysfs_mkchild(cid, "available_governors", VFS_DEVICE,
                    g_cpuidle_governor);
    }
  }

  /* The clock under /sys/devices/system/cpu/cpufreq/policy0, and each CPU's
   * cpufreq a link to it, as Linux lays it out (M135): one policy, because
   * every driver here moves every CPU at once. Crash reporters and monitoring
   * tools read these; they exist only once the clock has actually been
   * measured. */
  {
    int ncpu = (g_max_cpus > 0) ? g_max_cpus : 1;
    struct vfs_node *pol = 0;

    if (arch_cpu_khz()) {
      struct vfs_node *cfd = sysfs_mkchild(cpu, "cpufreq", VFS_DIRECTORY, 0);

      pol = cfd ? sysfs_mkchild(cfd, "policy0", VFS_DIRECTORY, 0) : 0;
      if (cfd && strstr(cpufreq_governors(), "ondemand")) {
        struct vfs_node *od = sysfs_mkchild(cfd, "ondemand", VFS_DIRECTORY, 0);
        struct vfs_node *f;

        f = od ? sysfs_mkchild(od, "sampling_rate", VFS_DEVICE,
                               g_od_sampling_rate)
               : 0;
        if (f && f->inode) {
          f->inode->mode = 0644;
          f->inode->write_cb = sysfs_od_rate_write;
        }
        f = od ? sysfs_mkchild(od, "up_threshold", VFS_DEVICE,
                               g_od_up_threshold)
               : 0;
        if (f && f->inode) {
          f->inode->mode = 0644;
          f->inode->write_cb = sysfs_od_up_write;
        }
      }
    }
    if (pol) {
      struct vfs_node *f;

      sysfs_mkchild(pol, "scaling_cur_freq", VFS_DEVICE, g_cpu_cur_freq);
      f = sysfs_mkchild(pol, "scaling_max_freq", VFS_DEVICE, g_policy_max_freq);
      if (f && f->inode && strcmp(cpufreq_driver_name(), "none")) {
        f->inode->mode = 0644;
        f->inode->write_cb = sysfs_policy_max_write;
      }
      f = sysfs_mkchild(pol, "scaling_min_freq", VFS_DEVICE, g_policy_min_freq);
      if (f && f->inode && strcmp(cpufreq_driver_name(), "none")) {
        f->inode->mode = 0644;
        f->inode->write_cb = sysfs_policy_min_write;
      }
      sysfs_mkchild(pol, "cpuinfo_cur_freq", VFS_DEVICE, g_cpu_cur_freq);
      sysfs_mkchild(pol, "cpuinfo_max_freq", VFS_DEVICE, g_cpu_max_freq);
      sysfs_mkchild(pol, "cpuinfo_min_freq", VFS_DEVICE, g_cpu_min_freq);
      f = sysfs_mkchild(pol, "scaling_governor", VFS_DEVICE, g_cpu_governor);
      if (f && f->inode)
        f->inode->write_cb = sysfs_governor_write;
      sysfs_mkchild(pol, "scaling_driver", VFS_DEVICE, g_cpu_driver);
      sysfs_mkchild(pol, "scaling_available_governors", VFS_DEVICE,
                    g_cpu_governors);
      sysfs_mkchild(pol, "affected_cpus", VFS_DEVICE, g_policy_cpus);
      sysfs_mkchild(pol, "related_cpus", VFS_DEVICE, g_policy_cpus);
      /* Only where the platform really declared a list of them (M129). */
      if (cpufreq_state_count() > 0) {
        struct vfs_node *st;

        sysfs_mkchild(pol, "scaling_available_frequencies", VFS_DEVICE,
                      g_cpu_avail_freqs);
        f = sysfs_mkchild(pol, "scaling_setspeed", VFS_DEVICE, g_cpu_setspeed);
        if (f && f->inode) {
          f->inode->mode = 0644;
          f->inode->write_cb = sysfs_setspeed_write;
        }
        sysfs_mkchild(pol, "bios_limit", VFS_DEVICE, g_cpu_bios_limit);
        st = sysfs_mkchild(pol, "stats", VFS_DIRECTORY, 0);
        if (st) {
          sysfs_mkchild(st, "time_in_state", VFS_DEVICE, g_cpu_time_in_state);
          sysfs_mkchild(st, "total_trans", VFS_DEVICE, g_cpu_total_trans);
        }
      }
    }
    g_sysfs_cpu_dir = cpu;
    g_sysfs_cpu_policy = pol != 0;
    g_sysfs_cpus_built = 0;
    for (int i = 0; i < ncpu; i++)
      sysfs_build_cpu(cpu, i, pol != 0);
    g_sysfs_cpus_built = ncpu;
  }

  sysfs_build_nodes(sys);

  struct vfs_node *mem = sysfs_mkchild(root, "memory", VFS_DIRECTORY, 0);
  sysfs_mkchild(mem, "total_kb", VFS_DEVICE, g_memtotal);

  sysfs_build_block(root);
  sysfs_build_net(root);
  sysfs_build_acpi_power(root);
  sysfs_build_wakeup(root);
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

/* Every mount of sysfs is the same tree, as on Linux, where sysfs is one
 * kernfs whatever it is mounted on.
 *
 * Each mount used to build a tree of its own, and the registry of driver
 * directories -- /sys/class/tty, /sys/bus/pci, the DRM class -- can live in
 * only one, so it moved to the newest: a sandbox mounting sysfs for itself
 * (systemd does for every PrivateNetwork= service) took those directories
 * out of the host's /sys for good. A second tree also had nodes of its own,
 * so the mounts on the first one -- /sys/fs/cgroup -- were not at the same
 * place in it, and systemd, rebuilding a service's /sys, could not tell what
 * to bind back. The tree is built once and every mount shares it; the
 * reference kept here keeps it alive between mounts. */
static struct vfs_node *g_sysfs_tree;

static struct vfs_node *sysfs_mount_cb(const char *source, u64 flags,
                                       void *data) {
  (void)source;
  (void)flags;
  (void)data;
  if (!g_sysfs_tree) {
    struct vfs_node *root = sysfs_build_tree();
    if (IS_ERR(root) || !root)
      return root;
    g_sysfs_tree = root;
  }
  return vfs_node_get(g_sysfs_tree);
}

void sysfs_init(void) {
  sysfs_fs.name = "sysfs";
  sysfs_fs.mount = sysfs_mount_cb;
  sysfs_fs.flags = VFS_FS_NODEV | VFS_FS_USERNS_MOUNT;
  vfs_register_fs(&sysfs_fs);
}
