#include <b1nix/loop.h>
#include <b1nix/klog.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <b1nix/errno.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <b1nix/posix.h>
#include <stdio.h>
#include <string.h>

struct loop_device {
    struct block_device bdev;
    struct vfs_node *backing_node;
    int readonly;
    /* M107: the association metadata losetup shows and mount -o loop sets. */
    u64 offset;      /* byte offset of the mapped region in the backing file */
    u64 sizelimit;   /* 0 = to the end of the file */
    u32 flags;
    char file_name[64];
    /* Re-entrancy guard: a write to the backing file goes through ext4, which
     * may evict a dirty block cache entry, and the entry it picks can belong to
     * this very loop device. Coming back in here would try to take the backing
     * inode's (non-recursive) lock a second time on the same thread. */
    int in_io;
};

/* Byte extent of the mapping inside the backing file. */
static u64 loop_limit(const struct loop_device *loop) {
    u64 file_size = loop->backing_node->inode->size;
    u64 end = file_size;
    if (loop->sizelimit && loop->offset + loop->sizelimit < end)
        end = loop->offset + loop->sizelimit;
    return end;
}

static int loop_read_blocks(struct block_device *dev, u64 lba, u32 count, void *buffer) {
    struct loop_device *loop = (struct loop_device *)dev->priv;
    if (!loop || !loop->backing_node || !loop->backing_node->inode) {
        return -1;
    }

    struct vfs_node *node = loop->backing_node;
    if (loop->in_io)
        return -EAGAIN;

    u64 offset = loop->offset + lba * 512;
    usize size = (usize)count * 512;
    u64 limit = loop_limit(loop);

    if (offset >= limit) {
        memset(buffer, 0, size);
        return 0;
    }

    if (offset + size > limit) {
        size = (usize)(limit - offset);
    }

    /* Read the backing file the way read(2) does — through its page cache.
     * Going straight to inode->read_cb would return whatever is on disk while
     * the file's own cached pages held newer data. */
    loop->in_io = 1;
    isize read_bytes = vfs_node_pread(node, buffer, size, offset);
    loop->in_io = 0;
    if (read_bytes < 0) {
        return (int)read_bytes;
    }

    if ((usize)read_bytes < (usize)count * 512) {
        memset((char *)buffer + read_bytes, 0, ((usize)count * 512) - (usize)read_bytes);
    }

    return 0;
}

/* M107: loop devices are writable. Without this a loop-mounted image was
 * read-only in a way nothing reported — writes reached the block cache and
 * were silently dropped at flush time because the device had no write path. */
static int loop_write_blocks(struct block_device *dev, u64 lba, u32 count,
                             const void *buffer) {
    struct loop_device *loop = (struct loop_device *)dev->priv;
    if (!loop || !loop->backing_node || !loop->backing_node->inode)
        return -EIO;
    if (loop->readonly)
        return -EROFS;
    struct vfs_node *node = loop->backing_node;
    if (loop->in_io)
        return -EAGAIN;

    u64 offset = loop->offset + lba * 512;
    usize size = (usize)count * 512;
    u64 limit = loop_limit(loop);
    if (offset >= limit)
        return -ENOSPC;
    if (offset + size > limit)
        size = (usize)(limit - offset);

    /* Same page cache as write(2): a write that went straight to inode->write_cb
     * landed on disk but left the file's cached pages stale, so reading the
     * backing file back returned the pre-write bytes (and a later writeback of
     * those clean-looking pages could undo the loop write entirely). */
    loop->in_io = 1;
    isize written = vfs_node_pwrite(node, (const char *)buffer, size, offset);
    loop->in_io = 0;
    if (written < 0)
        return (int)written;
    return (usize)written == size ? 0 : -EIO;
}

/* fsync(/dev/loopN) has two stages: blk_cache_flush drains the block cache into
 * the backing file's page cache, and this pushes that file out to storage. */
static int loop_flush(struct block_device *dev) {
    struct loop_device *loop = (struct loop_device *)dev->priv;
    if (!loop || !loop->backing_node)
        return 0;
    if (loop->readonly)
        return 0;
    return vfs_node_fsync(loop->backing_node);
}

struct block_device *loop_register_file(const char *path, const char *name) {
    struct vfs_node *node = vfs_find_node(path);
    if (IS_ERR(node)) {
        return (struct block_device *)node;
    }

    struct loop_device *loop = kmalloc(sizeof(struct loop_device));
    if (!loop) {
        vfs_node_put(node);
        return ERR_PTR(-ENOMEM);
    }

    loop->backing_node = node;
    loop->readonly = 1;
    loop->offset = 0;
    loop->sizelimit = 0;
    loop->flags = 0;
    loop->file_name[0] = '\0';
    loop->in_io = 0;

    char *persistent_name = kmalloc(strlen(name) + 1);
    if (!persistent_name) {
        kfree(loop);
        vfs_node_put(node);
        return ERR_PTR(-ENOMEM);
    }
    strcpy(persistent_name, name);

    loop->bdev.name = persistent_name;
    loop->bdev.bus = BLK_BUS_LOOP;
    loop->bdev.block_size = 512;
    loop->bdev.block_count = (node->inode->size + 511) / 512;
    loop->bdev.read_blocks = loop_read_blocks;
    loop->bdev.write_blocks = NULL;
    loop->bdev.flush = NULL; /* read-only association: nothing to push out */
    loop->bdev.priv = loop;

    blk_register(&loop->bdev);
    return &loop->bdev;
}


/* Linux's two loop status structures, byte-for-byte. losetup reads the 64-bit
 * one to print the backing file and offset; without it every association
 * printed as an unnamed device. */
#define LO_NAME_SIZE 64
#define LO_KEY_SIZE 32
#define LO_FLAGS_READ_ONLY 1

struct loop_info64 {
  u64 lo_device;
  u64 lo_inode;
  u64 lo_rdevice;
  u64 lo_offset;
  u64 lo_sizelimit;
  u32 lo_number;
  u32 lo_encrypt_type;
  u32 lo_encrypt_key_size;
  u32 lo_flags;
  u8 lo_file_name[LO_NAME_SIZE];
  u8 lo_crypt_name[LO_NAME_SIZE];
  u8 lo_encrypt_key[LO_KEY_SIZE];
  u64 lo_init[2];
};

struct loop_info32 {
  int lo_number;
  u32 lo_device;
  unsigned long lo_inode;
  u32 lo_rdevice;
  int lo_offset;
  int lo_encrypt_type;
  int lo_encrypt_key_size;
  int lo_flags;
  char lo_name[LO_NAME_SIZE];
  unsigned char lo_encrypt_key[LO_KEY_SIZE];
  unsigned long lo_init[2];
  char reserved[4];
};

/* Record the backing file's path for LOOP_GET_STATUS. */
static void loop_set_name_from_fd(struct loop_device *lo, int fd) {
  char path[VFS_MAX_PATH];
  lo->file_name[0] = '\0';
  /* vfs_fd_abspath returns the path LENGTH, not 0, on success — testing for 0
   * here sent every association down the "unresolved" branch below, which is
   * why losetup showed an unnamed device for a path it had just resolved. */
  int rc = vfs_fd_abspath(fd, path, sizeof(path));
  if (rc > 0) {
    strncpy(lo->file_name, path, sizeof(lo->file_name) - 1);
    lo->file_name[sizeof(lo->file_name) - 1] = '\0';
    return;
  }
  /* No fabricated fallback: a name that is not the real path would make
   * `losetup -a` point at a file that does not exist, which is worse than an
   * empty field. vfs_fd_abspath currently cannot reconstruct a path whose
   * parent chain crosses the ext4 root's lazily materialised directories —
   * tracked as an open M107 item. */
  klog_debug_category("loop", "backing path unresolved");
}

/* blk_create_dev_nodes() stamps /dev/loopN's size while the device is still
 * unassociated, so it stays 0 forever unless the association updates it.
 * fdisk, blockdev and mkfs all read that size through stat(). */
static void loop_refresh_node_size(int idx, struct loop_device *lo) {
  char path[24];
  snprintf(path, sizeof(path), "/dev/loop%d", idx);
  struct vfs_node *n = vfs_find_node(path);
  if (!n || IS_ERR(n))
    return;
  n->inode->size = (usize)(lo->bdev.block_size * lo->bdev.block_count);
  vfs_node_put(n);
}

/* ── Loop-device control surface for BusyBox losetup ──
 * Eight pre-registered loop block devices (/dev/loop0../dev/loop7, created by
 * blk_create_dev_nodes) plus /dev/loop-control. LOOP_SET_FD associates an open
 * file's backing node; the device's byte reads then route through
 * loop_read_blocks. */
#define NUM_LOOPS 8
static struct loop_device g_loops[NUM_LOOPS];
static int g_loops_inited;

void loop_init(void) {
  if (g_loops_inited)
    return;
  g_loops_inited = 1;
  for (int i = 0; i < NUM_LOOPS; i++) {
    char *nm = kmalloc(8);
    if (!nm)
      return;
    snprintf(nm, 8, "loop%d", i);
    g_loops[i].backing_node = 0;
    g_loops[i].readonly = 0;
    g_loops[i].offset = 0;
    g_loops[i].sizelimit = 0;
    g_loops[i].flags = 0;
    g_loops[i].file_name[0] = '\0';
    g_loops[i].bdev.name = nm;
    g_loops[i].bdev.bus = BLK_BUS_LOOP;
    g_loops[i].bdev.block_size = 512;
    g_loops[i].bdev.block_count = 0;
    g_loops[i].bdev.read_blocks = loop_read_blocks;
    g_loops[i].bdev.write_blocks = loop_write_blocks;
    g_loops[i].bdev.flush = loop_flush;
    g_loops[i].in_io = 0;
    g_loops[i].bdev.priv = &g_loops[i];
    blk_register(&g_loops[i].bdev);
  }
  loop_register_nodes();
}

/* Node (re)registration, separate from the one-time device setup above: nodes
 * created during early boot live on the initramfs root and become unreachable
 * once the real root is mounted over "/", so this is called again from
 * vfs_repopulate_after_root_mount(). */
void loop_register_nodes(void) {
  struct vfs_node *ctl = vfs_add_node("/dev/loop-control", VFS_DEVICE, 0, 0, 0);
  if (ctl && !IS_ERR(ctl))
    ctl->inode->mode = 0660;
}

int loop_ioctl(struct vfs_node *node, u64 request, void *arg) {
  if (strcmp(node->name, "loop-control") == 0) {
    if (request == 0x4C82) { /* LOOP_CTL_GET_FREE */
      for (int i = 0; i < NUM_LOOPS; i++)
        if (!g_loops[i].backing_node)
          return i;
      return -ENOSPC;
    }
    return -ENOTTY;
  }
  if (strncmp(node->name, "loop", 4) != 0)
    return -ENOTTY;
  int idx = 0;
  for (const char *c = node->name + 4; *c >= '0' && *c <= '9'; c++)
    idx = idx * 10 + (*c - '0');
  if (idx < 0 || idx >= NUM_LOOPS)
    return -ENXIO;
  struct loop_device *lo = &g_loops[idx];
  switch (request) {
  case 0x4C00: { /* LOOP_SET_FD: arg is the backing file descriptor */
    int backing_fd = (int)(usize)arg;
    struct vfs_handle *h = scheduler_fd_get(backing_fd);
    if (!h || !h->node || !h->node->inode)
      return -EBADF;
    /* Only a regular file may back a loop device — a device node (especially
     * the loop's own /dev/loopN) would recurse loop_read_blocks -> blkdev read
     * -> loop_read_blocks into a stack overflow; a pipe/tty would block inside
     * the block layer. */
    if (h->node->inode->type != VFS_FILE)
      return -EINVAL;
    if (lo->backing_node)
      return -EBUSY; /* already associated — CLR_FD first */
    /* Pin the backing node: the setup process's fd will be closed (and the
     * file may be unlinked) while the loop device lives on. Without this ref
     * loop_read_blocks would dereference a freed node (UAF). */
    lo->backing_node = vfs_node_get(h->node);
    lo->offset = 0;
    lo->sizelimit = 0;
    lo->flags = 0;
    /* Read-only follows the descriptor, so `losetup -r` really produces a
     * device that refuses writes. */
    lo->readonly = (h->flags & (B1NIX_O_WRONLY | B1NIX_O_RDWR)) == 0;
    loop_set_name_from_fd(lo, backing_fd);
    lo->bdev.block_count = (h->node->inode->size + 511) / 512;
    /* Drop any cached blocks keyed to this bdev from a previous association. */
    blk_cache_invalidate(&lo->bdev);
    loop_refresh_node_size(idx, lo);
    return 0;
  }
  case 0x4C01: { /* LOOP_CLR_FD */
    struct vfs_node *old = lo->backing_node;
    /* Drain first, tear down second. Clearing backing_node up front meant every
     * dirty block the invalidate below wrote back re-entered loop_write_blocks
     * with no association and was dropped on the floor — detaching a loop
     * device silently lost whatever had not been flushed yet. */
    if (old) {
      blk_cache_flush(&lo->bdev);
      loop_flush(&lo->bdev);
    }
    lo->backing_node = 0;
    lo->bdev.block_count = 0;
    lo->offset = 0;
    lo->sizelimit = 0;
    lo->flags = 0;
    lo->readonly = 0;
    lo->file_name[0] = '\0';
    blk_cache_invalidate(&lo->bdev);
    loop_refresh_node_size(idx, lo);
    if (old)
      vfs_node_put(old);
    return 0;
  }
  case 0x4C05: { /* LOOP_GET_STATUS64 */
    if (!lo->backing_node)
      return -ENXIO;
    if (!arg)
      return -EFAULT;
    struct loop_info64 info;
    memset(&info, 0, sizeof(info));
    info.lo_inode = lo->backing_node->inode ? lo->backing_node->inode->ino : 0;
    info.lo_offset = lo->offset;
    info.lo_sizelimit = lo->sizelimit;
    info.lo_number = (u32)idx;
    info.lo_flags = lo->flags | (lo->readonly ? LO_FLAGS_READ_ONLY : 0);
    strncpy((char *)info.lo_file_name, lo->file_name,
            sizeof(info.lo_file_name) - 1);
    return syscall_copyout(arg, &info, sizeof(info)) < 0 ? -EFAULT : 0;
  }
  case 0x4C03: { /* LOOP_GET_STATUS (the older struct) */
    if (!lo->backing_node)
      return -ENXIO;
    if (!arg)
      return -EFAULT;
    struct loop_info32 info;
    memset(&info, 0, sizeof(info));
    info.lo_number = idx;
    info.lo_inode = lo->backing_node->inode
                        ? (unsigned long)lo->backing_node->inode->ino
                        : 0;
    info.lo_offset = (int)lo->offset;
    info.lo_flags = (int)(lo->flags | (lo->readonly ? LO_FLAGS_READ_ONLY : 0));
    strncpy(info.lo_name, lo->file_name, sizeof(info.lo_name) - 1);
    return syscall_copyout(arg, &info, sizeof(info)) < 0 ? -EFAULT : 0;
  }
  case 0x4C04: { /* LOOP_SET_STATUS64 */
    if (!lo->backing_node)
      return -ENXIO;
    if (!arg)
      return -EFAULT;
    struct loop_info64 info;
    if (syscall_copyin(&info, arg, sizeof(info)) < 0)
      return -EFAULT;
    if (info.lo_encrypt_type != 0)
      return -EOPNOTSUPP; /* there is no crypto transfer function */
    u64 file_size = lo->backing_node->inode ? lo->backing_node->inode->size : 0;
    if (info.lo_offset > file_size)
      return -EINVAL;
    lo->offset = info.lo_offset;
    lo->sizelimit = info.lo_sizelimit;
    lo->flags = info.lo_flags & ~(u32)LO_FLAGS_READ_ONLY;
    if (info.lo_flags & LO_FLAGS_READ_ONLY)
      lo->readonly = 1;
    if (info.lo_file_name[0]) {
      strncpy(lo->file_name, (const char *)info.lo_file_name,
              sizeof(lo->file_name) - 1);
      lo->file_name[sizeof(lo->file_name) - 1] = '\0';
    }
    u64 usable = loop_limit(lo);
    lo->bdev.block_count =
        usable > lo->offset ? (usable - lo->offset + 511) / 512 : 0;
    blk_cache_invalidate(&lo->bdev);
    loop_refresh_node_size(idx, lo);
    return 0;
  }
  case 0x4C02: { /* LOOP_SET_STATUS */
    if (!lo->backing_node)
      return -ENXIO;
    if (!arg)
      return -EFAULT;
    struct loop_info32 info;
    if (syscall_copyin(&info, arg, sizeof(info)) < 0)
      return -EFAULT;
    if (info.lo_encrypt_type != 0)
      return -EOPNOTSUPP;
    if (info.lo_offset < 0)
      return -EINVAL;
    lo->offset = (u64)info.lo_offset;
    if (info.lo_name[0]) {
      strncpy(lo->file_name, info.lo_name, sizeof(lo->file_name) - 1);
      lo->file_name[sizeof(lo->file_name) - 1] = '\0';
    }
    u64 usable = loop_limit(lo);
    lo->bdev.block_count =
        usable > lo->offset ? (usable - lo->offset + 511) / 512 : 0;
    blk_cache_invalidate(&lo->bdev);
    loop_refresh_node_size(idx, lo);
    return 0;
  }
  case 0x4C07: { /* LOOP_SET_CAPACITY: re-read the backing file's size */
    if (!lo->backing_node || !lo->backing_node->inode)
      return -ENXIO;
    u64 usable = loop_limit(lo);
    lo->bdev.block_count =
        usable > lo->offset ? (usable - lo->offset + 511) / 512 : 0;
    blk_cache_invalidate(&lo->bdev);
    loop_refresh_node_size(idx, lo);
    return 0;
  }
  default:
    return -ENOTTY;
  }
}
