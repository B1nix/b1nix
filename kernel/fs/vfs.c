#include <b1nix/termios_abi.h>
#include <b1nix/arch.h>
#include <b1nix/blk.h>
#include <b1nix/loop.h>
#include <b1nix/md.h>
#include <b1nix/mtd.h>
#include <b1nix/nbd.h>
#include <b1nix/vt.h>
#include <b1nix/kmsg.h>
#include <b1nix/rtc.h>
#include <b1nix/watchdog.h>
#include <b1nix/i2c.h>
#include <b1nix/vt.h>
#include <b1nix/kmsg.h>
#include <b1nix/rtc.h>
#include <b1nix/watchdog.h>
#include <b1nix/i2c.h>
#include <b1nix/console.h>
#include <b1nix/drm.h>
#include <b1nix/errno.h>
#include <b1nix/ext2.h>
#include <b1nix/fat32.h>
#include <b1nix/filelock.h>
#include <b1nix/initramfs.h>
#include <b1nix/input.h>
#include <b1nix/inotify.h>
#include <b1nix/klog.h>
#include <b1nix/mm.h>
#include <b1nix/module.h>
#include <b1nix/namespace.h>
#include <b1nix/net.h>
#include <b1nix/lockdep.h>
#include <b1nix/page_cache.h>
#include <b1nix/panic.h>
#include <b1nix/rwlock.h>
#include <b1nix/rtc.h>
#include <b1nix/sched.h>
#include <b1nix/serial.h>
#include <b1nix/serial_tty.h>
#include <b1nix/syscall.h>
#include <b1nix/uidgid.h>
#include <b1nix/vfs.h>
#include <b1nix/posix.h>
#include <stdio.h>
#include <string.h>

/* VFS_NODE_OWNS_DATA now lives in <b1nix/vfs.h> so other filesystems can use it. */
#define MAX_FILE_SIZE (1024 * 1024 * 1024) /* 1 GB limit for now */

/* VFS time update masks */
#define VFS_ATIME 0x01
#define VFS_MTIME 0x02
#define VFS_CTIME 0x04
#define VFS_MAX_SYMLINK_DEPTH 16

static char poll_chan_obj;
void *vfs_poll_chan = &poll_chan_obj;

static volatile int vfs_mount_lock = 0;
static u32 next_fs_id = 1;

/* M28-B: rwlock protecting the parent/sibling chain of every vfs_node — i.e.
 * the in-RAM tree (first_child / next_sibling / parent). Readers are the
 * lookup paths (find_child + ancestor walks in vfs_get_mount_for_node);
 * writers are the sibling-list mutations (add_node, the create/mkdir/link
 * sibling-prepend, and the unlink/rmdir/rename sibling-splice). Independent
 * of the per-inode locks (which guard inode fields, not the chain).
 *
 * Today it runs under the Big Kernel Lock (M28 item 2), so this lock is
 * decorative for the BSP and reader-collisions on APs never happen. Wiring
 * it now is the small, contained diff that lets a future per-subsystem BKL
 * teardown remove cross-CPU races from the VFS one site at a time, instead
 * of one large flag-day patch. */
static rwlock_t vfs_tree_lock = RWLOCK_INIT;

/* Lockdep-traced acquire/release helpers: the bare rw_*_lock_irqsave calls
 * don't know their DAG level, so wrap them here. Inlined to a single
 * instruction sequence in production builds (KERNEL_LOCKDEP undef). */
static inline void vfs_tree_read_acquire(u64 *flags) {
  rw_read_lock_irqsave(&vfs_tree_lock, flags);
  LOCKDEP_ACQUIRE(LOCKDEP_LVL_VFS_TREE);
}
static inline void vfs_tree_read_release(u64 flags) {
  LOCKDEP_RELEASE(LOCKDEP_LVL_VFS_TREE);
  rw_read_unlock_irqrestore(&vfs_tree_lock, flags);
}
static inline void vfs_tree_write_acquire(u64 *flags) {
  rw_write_lock_irqsave(&vfs_tree_lock, flags);
  LOCKDEP_ACQUIRE(LOCKDEP_LVL_VFS_TREE);
}
static inline void vfs_tree_write_release(u64 flags) {
  LOCKDEP_RELEASE(LOCKDEP_LVL_VFS_TREE);
  rw_write_unlock_irqrestore(&vfs_tree_lock, flags);
}

static u32 dcache_hash(struct vfs_node *parent, const char *name);
static struct vfs_node *dcache_lookup(struct vfs_node *parent,
                                      const char *name);
static void dcache_insert(struct vfs_node *parent, const char *name,
                          struct vfs_node *node);
static void dcache_invalidate(struct vfs_node *parent, const char *name);

void serial_init(void);
void serial_putc(char ch);
char serial_getc(void);
int serial_has_data(void);
void serial_write(const char *text);
static void copy_path(char *dst, usize dst_size, const char *src);
static int split_parent_path(const char *path, char *parent_path,
                             usize parent_size, char *name,
                             usize name_size);
static int vfs_create_at_internal(const char *resolved_path, u32 mode);
static int vfs_mkdir_at_internal(const char *resolved_path, u32 mode);

static struct vfs_fs *filesystems = NULL;

struct vfs_mount_entry {
  int used;
  char source[VFS_MAX_PATH];
  char target[VFS_MAX_PATH];
  char fstype[16];
  u64 flags;
  struct vfs_node *root_node;
  struct vfs_node *mount_point;
  /* Reference on the module providing this filesystem type, dropped at umount
   * (NULL for a built-in type). */
  struct module *owner;
  /* M109: the mount namespace this entry belongs to (0 = the initial one).
   * Every scan below skips entries from another namespace, which is what makes
   * a mount made under `unshare -m` invisible outside it. */
  u32 mnt_ns;
  /* How mount events cross this mountpoint: MS_SHARED, MS_SLAVE, MS_PRIVATE or
   * MS_UNBINDABLE. Linux's default for a new mount is private, and systemd's
   * very first act as PID 1 is to make / shared — a call that has to mean
   * something, because everything it later does with per-unit mount namespaces
   * is described in terms of it. `peer_group` is the "shared:N" number
   * /proc/self/mountinfo prints; mounts that were cloned from one another share
   * it, and a new mount under a shared mount is created in every peer's
   * namespace too. */
  u32 propagation;
  u32 peer_group;
  /* Creation order, monotonic and never reused. Several entries can name the
   * same node — a bind mount records its SOURCE as the mount's root, so
   * `ReadOnlyPaths=` and `ProtectSystem=`, which bind a path onto itself and
   * then remount it read-only, leave two entries rooted at one node. Linux
   * consults the mount stacked LAST at that point; scanning the array picked
   * the lowest free slot instead, which is the OLDEST, so the read-only
   * remount was recorded and then never consulted. A slot index cannot answer
   * this because slots are reused (a namespace clone lands in freed ones). */
  u64 seq;
};
/* Monotonic mount counter — see vfs_mount_entry::seq. Never reset, never
 * reused. */
static u64 mount_seq_next = 1;

/* Sized at vfs_init() from RAM, never reallocated — see MAX_MOUNTS in vfs.h for
 * the floor, the ceiling, and why this one is sized rather than grown. Until
 * then the capacity reads as zero, so the scans below are no-ops rather than
 * walks off a null pointer. */
static struct vfs_mount_entry *mounts;
static usize mount_slots;

usize vfs_mount_capacity(void) { return mount_slots; }

/* Allocate the mount table. Called from vfs_init() before any mount happens. */
static void mounts_init_table(void) {
  u64 ram_mb = pmm_total_usable_memory() / (1024ULL * 1024ULL);
  /* One slot per MiB of RAM: a 64 MiB floor for small guests, and a machine
   * big enough to run containers gets room for their namespaces. */
  u32 want = bootinfo_get_u32("b1nix.max-mounts",
                              (u32)(ram_mb > MAX_MOUNTS ? MAX_MOUNTS : ram_mb));

  if (want < MIN_MOUNTS)
    want = MIN_MOUNTS;
  if (want > MAX_MOUNTS)
    want = MAX_MOUNTS;
  mounts = kzalloc((usize)want * sizeof(*mounts));
  if (!mounts) {
    want = MIN_MOUNTS;
    mounts = kzalloc((usize)want * sizeof(*mounts));
  }
  mount_slots = mounts ? want : 0;
}

/* The caller's mount namespace. namespace_active() is a plain global read and
 * is zero on any boot where nothing has unshared, so the whole namespace
 * lookup drops out of the path-resolution fast path. */
static u32 vfs_current_mnt_ns(void) {
  if (!namespace_active())
    return 0;
  return namespace_current_id(NS_MNT);
}

/* Is mounts[i] a live entry the caller can see? */
static int mount_visible(usize i) {
  if (!mounts[i].used)
    return 0;
  if (!namespace_active())
    return 1;
  return mounts[i].mnt_ns == vfs_current_mnt_ns();
}

/* How many mount entries (in any namespace) point at this root node? A cloned
 * namespace holds its own reference per entry, so "busy" has to be measured
 * against that count rather than against 1. */
static usize mount_root_refs(struct vfs_node *root) {
  usize n = 0;
  for (usize i = 0; i < mount_slots; i++)
    if (mounts[i].used && mounts[i].root_node == root)
      n++;
  return n;
}

void vfs_dump_mounts(void) {
  console_write("\n--- Mounted Filesystems ---\n");
  if (!mounts || mount_slots == 0) {
    console_write("  (no mounts initialized)\n");
    console_write("--- End Mounts ---\n");
    return;
  }
  for (usize i = 0; i < mount_slots; i++) {
    if (!mounts[i].used) continue;
    console_write("  ");
    console_write(mounts[i].target[0] ? mounts[i].target : "/");
    console_write(" on ");
    console_write(mounts[i].fstype[0] ? mounts[i].fstype : "unknown");
    console_write(" (dev=");
    console_write(mounts[i].source[0] ? mounts[i].source : "none");
    console_write(", flags=0x");
    console_write_hex64(mounts[i].flags);
    console_write(")\n");
  }
  console_write("--- End Mounts ---\n");
}


static struct vfs_mount_entry *vfs_get_mount_for_node(struct vfs_node *node) {
  if (!node)
    return 0;

  while (__atomic_test_and_set(&vfs_mount_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();

  /* Walk up the parent chain under the VFS tree rwlock (read side) — preemption
   * or a concurrent rmdir mid-walk lets another task free an ancestor and we'd
   * dereference garbage on `curr = curr->parent`. The IRQ-save variant
   * preserves the pre-rwlock cli/sti semantics so a same-CPU writer (entered
   * from a future preemptive timer ISR) can't race us; the mount_lock above
   * only protects the mounts array, not the parent chain. */
  u64 flags;
  vfs_tree_read_acquire(&flags);
  struct vfs_node *curr = node;
  struct vfs_mount_entry *res = 0;
  while (curr) {
    /* The deepest ancestor that is some mount's root wins; among the entries
     * rooted at THAT node, the one mounted last does (see ::seq). */
    for (int i = 0; i < (int)mount_slots; i++) {
      if (mount_visible(i) && curr == mounts[i].root_node &&
          (!res || mounts[i].seq > res->seq))
        res = &mounts[i];
    }
    if (res)
      goto out;
    curr = curr->parent;
  }
  /* Nothing in the chain is a mount root: fall back to the root mount, first
   * match as before. "Newest wins" answers "which mount is stacked on this
   * node"; it is not a rule about a node that is on no mount at all, and
   * applying it here changed which filesystem's flags an unattached node was
   * judged by. */
  for (int i = 0; i < (int)mount_slots; i++) {
    if (mount_visible(i) && strcmp(mounts[i].target, "/") == 0) {
      res = &mounts[i];
      goto out;
    }
  }
out:
  vfs_tree_read_release(flags);
  __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
  return res;
}

/* A mount root's ->parent is NULL: the names above it live on the node it was
 * mounted over. Anything that walks the parent chain to build a path must step
 * across that seam, or it silently produces the path *inside* the mount — e.g.
 * "/cptest" for a directory that is really /run/cptest, which turns an
 * openat(dirfd, "cptest") into a create at the root. Returns the mount point
 * for a mount root, or NULL when `node` is not one.
 *
 * mounts[] is read without vfs_mount_lock here, matching the downward crossing
 * in vfs_find_node_internal: entries are published root_node-last and callers
 * already hold a reference to the node being walked. */
/* Defined below; the mount seam needs to know the machine root. */
static struct vfs_node *root_node;
static void mount_record_target(const char *target, struct vfs_node *node,
                                char *out, usize out_len);

/* The node a mount ROOT is covered by, so a path walk can step across the seam
 * instead of stopping at a filesystem root's NULL parent.
 *
 * Only a filesystem root needs the seam. A node with a parent already has a
 * name in a directory, and that chain IS its canonical path -- following a
 * mount entry from such a node instead is how a bind mount rewrote the name of
 * the file it bound. Binding a file onto ITSELF (which is every
 * ReadOnlyPaths=, ProtectHostname= and ProtectKernelTunables= entry systemd
 * sets up) then made the seam a self-loop: the walk stepped from the node to
 * itself until its step budget ran out and rendered the file's path as "/".
 * That is what put /proc/self/fd/<n> -- the target of every mount systemd
 * makes -- on the root directory, and mounting a unit's private tree over "/"
 * is what turned every later cgroup and /dev/console operation into ENOTDIR.
 *
 * The machine root names itself for the same reason: a recursive bind of "/"
 * somewhere else (systemd's unit-root) must not rename "/" to that place. */
static struct vfs_node *vfs_mount_point_of(const struct vfs_node *node) {
  if (!node || node == root_node || node->parent)
    return 0;
  for (int i = 0; i < (int)mount_slots; i++) {
    if (!mount_visible(i) || mounts[i].root_node != node)
      continue;
    /* The FIRST mount of this root is the one that gives it its name; a later
     * bind of the same filesystem somewhere else does not rename it. Taking a
     * later entry instead makes the walk circular -- the root bound under
     * /run/systemd/unit-root sent every path through
     * /run/systemd/unit-root/run/systemd/unit-root/... until it overflowed and
     * every openat(dirfd, name) in the machine answered ENAMETOOLONG.
     *
     * And a mount whose root IS its own mount point (a bind of a file onto
     * itself, which is every ReadOnlyPaths= entry) crosses nothing: stepping
     * to it lands back where the walk started. */
    return mounts[i].mount_point == node ? 0 : mounts[i].mount_point;
  }
  return 0;
}

void vfs_register_fs(struct vfs_fs *fs) {
  /* The descriptor lives in the registering module's data, so its address
   * names the owner. A built-in filesystem gets NULL and no refcounting. */
  fs->owner = module_owner_of(fs);
  fs->next = filesystems;
  filesystems = fs;
}

usize vfs_list_filesystems(struct vfs_fs_info *out, usize max) {
  usize n = 0;
  for (struct vfs_fs *f = filesystems; f && n < max; f = f->next) {
    out[n].name = f->name;
    out[n].flags = f->flags;
    n++;
  }
  return n;
}

void vfs_unregister_fs(struct vfs_fs *fs) {
  if (!fs)
    return;
  struct vfs_fs **pp = &filesystems;
  while (*pp) {
    if (*pp == fs) {
      *pp = fs->next;
      fs->next = 0;
      return;
    }
    pp = &(*pp)->next;
  }
}

static struct vfs_fs *find_fs(const char *name) {
  struct vfs_fs *curr = filesystems;
  while (curr) {
    if (strcmp(curr->name, name) == 0)
      return curr;
    curr = curr->next;
  }
  return NULL;
}

/* Dcache sizing (B2 audit): pool + hash table are sized to RAM at
 * dcache_init_pool() and lazy-allocated from the kernel heap, instead of
 * burning ~50 KiB of static memory regardless of machine size.
 *
 * The pool and its hash are already derived from RAM (see dcache_init_pool);
 * these are the clamps on that formula. The ceilings, not the formula, were
 * what bound a large machine: at ~1 entry per 256 KiB of RAM an 8 GiB guest
 * asks for 32768 entries and got 8192, and asks for 16384 buckets and got 4096
 * — a load factor of 8, on the cache that every path lookup consults.
 *
 * FLOOR   256 entries / 64 buckets — unchanged, so a small guest is unchanged.
 * CEILING 131072 entries (~13 MiB at 104 bytes an entry) and 65536 buckets
 *         (512 KiB). The RAM rule reaches those only at 32 GiB, which is the
 *         point: the machine decides, not the constant.
 * `b1nix.dcache-entries=N` states the pool size directly. */
#define DCACHE_SIZE_MIN          64
#define DCACHE_SIZE_MAX          65536
#define DCACHE_POOL_MIN          256
#define DCACHE_POOL_MAX          131072

struct dcache_entry {
  struct vfs_node *parent;
  char name[VFS_NAME_MAX];
  struct vfs_node *node;
  struct dcache_entry *next;
  struct dcache_entry *lru_next;
  struct dcache_entry *lru_prev;
};
static struct dcache_entry **dcache = 0;
static u32 g_dcache_size = 0;
static struct dcache_entry *dcache_lru_head = 0;
static struct dcache_entry *dcache_lru_tail = 0;
static int dcache_count = 0;
static volatile int dcache_lock = 0;

/* Spin, never yield. This lock is taken from find_child while the vfs_tree
 * read lock is held; yielding there would drop us into the scheduler with a
 * tree lock in hand, which is exactly the race the old comment above
 * find_child described. Every section under it is a bounded bucket walk plus
 * an LRU splice, so a plain spin with interrupts masked is cheaper than the
 * yield ever was. */
static void dcache_acquire(u64 *flags) {
  *flags = interrupts_save();
  while (__atomic_test_and_set(&dcache_lock, __ATOMIC_ACQUIRE))
    cpu_relax();
}

static void dcache_release(u64 flags) {
  __atomic_clear(&dcache_lock, __ATOMIC_RELEASE);
  interrupts_restore(flags);
}

/* Slab-like pool for dcache entries to avoid fragmentation and kmalloc overhead
 * Pool + hash table are kzalloc'd at init, sized from total RAM. */
static struct dcache_entry *dcache_pool = 0;
static u32 g_dcache_pool_size = 0;
static struct dcache_entry *dcache_free_list = 0;

static void dcache_init_pool(void) {
  /* Scale to RAM: ~1 entry per 256 KiB usable RAM, clamped. */
  u64 ram_mb = pmm_total_usable_memory() / (1024ULL * 1024ULL);
  u32 pool = bootinfo_get_u32(
      "b1nix.dcache-entries",
      (u32)(ram_mb > 0x100000ULL ? 0x100000ULL : ram_mb) * 4u);
  if (pool < DCACHE_POOL_MIN) pool = DCACHE_POOL_MIN;
  if (pool > DCACHE_POOL_MAX) pool = DCACHE_POOL_MAX;
  g_dcache_pool_size = pool;
  /* Hash table half the pool size, also clamped to [MIN, MAX]. */
  u32 table = pool / 2;
  if (table < DCACHE_SIZE_MIN) table = DCACHE_SIZE_MIN;
  if (table > DCACHE_SIZE_MAX) table = DCACHE_SIZE_MAX;
  g_dcache_size = table;

  dcache_pool = kzalloc(g_dcache_pool_size * sizeof(struct dcache_entry));
  dcache = kzalloc(g_dcache_size * sizeof(struct dcache_entry *));
  if (!dcache_pool || !dcache) {
    /* Out of memory at dcache init — the dentry cache is non-critical (its
     * lookup path is currently dead code), so degrade gracefully: zero out
     * counts and leave the free list NULL. dcache_alloc returns 0 then. */
    g_dcache_pool_size = 0;
    g_dcache_size = 0;
    dcache_free_list = 0;
    return;
  }
  for (u32 i = 0; i < g_dcache_pool_size - 1; i++) {
    dcache_pool[i].next = &dcache_pool[i + 1];
  }
  dcache_pool[g_dcache_pool_size - 1].next = 0;
  dcache_free_list = &dcache_pool[0];
}

static struct dcache_entry *dcache_alloc(void) {
  if (!dcache_free_list)
    return 0;
  struct dcache_entry *e = dcache_free_list;
  dcache_free_list = e->next;
  memset(e, 0, sizeof(struct dcache_entry));
  return e;
}

static void dcache_free(struct dcache_entry *e) {
  e->next = dcache_free_list;
  dcache_free_list = e;
}

static u32 dcache_hash(struct vfs_node *parent, const char *name) {
  u32 h = 5381;
  h = ((h << 5) + h) + (u32)(usize)parent;
  while (*name)
    h = ((h << 5) + h) + (u32)(*name++);
  return h % g_dcache_size;
}

/* Dentry cache: name→node, hashed on (parent, name), LRU-evicted.
 *
 * Returns the node with a reference already taken, because the reference has
 * to be acquired under the dcache lock: a node is only freed once it is both
 * unreferenced and marked deleted, and the free path purges the cache under
 * this same lock, so taking the ref here is what closes the window between
 * reading the pointer and the node's memory going back to the pool. A hit on
 * a node already marked deleted is treated as a miss — the caller must see
 * the unlink even if the invalidation has not reached this bucket yet. */
static void dcache_unlink_locked(struct dcache_entry **prev_ptr,
                                 struct dcache_entry *curr);

static struct vfs_node *dcache_lookup(struct vfs_node *parent,
                                      const char *name) {
  if (!dcache || !g_dcache_size)
    return 0;
  u64 dcflags;
  dcache_acquire(&dcflags);
  u32 h = dcache_hash(parent, name);
  struct dcache_entry **prev_ptr = &dcache[h];
  struct dcache_entry *e = dcache[h];
  while (e) {
    if (e->parent == parent && strcmp(e->name, name) == 0) {
      /* Stale entry for a node that has since been unlinked: drop it here so
       * the caller's walk repopulates the bucket with one entry, not two. */
      if (!e->node || e->node->deleted) {
        dcache_unlink_locked(prev_ptr, e);
        dcache_release(dcflags);
        return 0;
      }
      /* Move to LRU head */
      if (e != dcache_lru_head) {
        if (e == dcache_lru_tail)
          dcache_lru_tail = e->lru_prev;
        if (e->lru_prev)
          e->lru_prev->lru_next = e->lru_next;
        if (e->lru_next)
          e->lru_next->lru_prev = e->lru_prev;
        e->lru_next = dcache_lru_head;
        e->lru_prev = 0;
        if (dcache_lru_head)
          dcache_lru_head->lru_prev = e;
        dcache_lru_head = e;
        if (!dcache_lru_tail)
          dcache_lru_tail = e;
      }
      struct vfs_node *res = e->node;
      vfs_node_get(res); /* REFCOUNT RULE: caller owns the returned ref */
      dcache_release(dcflags);
      return res;
    }
    prev_ptr = &e->next;
    e = e->next;
  }
  dcache_release(dcflags);
  return 0;
}

static void dcache_insert(struct vfs_node *parent, const char *name,
                          struct vfs_node *node) {
  if (!dcache || !g_dcache_size || !g_dcache_pool_size)
    return;
  u64 dcflags;
  dcache_acquire(&dcflags);
  if ((u32)dcache_count >= g_dcache_pool_size) {
    /* Evict LRU tail */
    struct dcache_entry *victim = dcache_lru_tail;
    if (victim) {
      /* Invalidate will handle locking if we call it carefully,
         but here we are already holding the lock.
         Let's manually remove the tail to avoid deadlock. */

      /* Remove from hash table */
      u32 vh = dcache_hash(victim->parent, victim->name);
      struct dcache_entry **prev_ptr = &dcache[vh];
      while (*prev_ptr && *prev_ptr != victim)
        prev_ptr = &(*prev_ptr)->next;
      if (*prev_ptr)
        *prev_ptr = victim->next;

      /* Remove from LRU */
      if (victim == dcache_lru_head)
        dcache_lru_head = victim->lru_next;
      if (victim == dcache_lru_tail)
        dcache_lru_tail = victim->lru_prev;
      if (victim->lru_prev)
        victim->lru_prev->lru_next = victim->lru_next;
      if (victim->lru_next)
        victim->lru_next->lru_prev = victim->lru_prev;

      dcache_free(victim);
      dcache_count--;
    }
  }

  u32 h = dcache_hash(parent, name);
  struct dcache_entry *e = dcache_alloc();
  if (!e) {
    dcache_release(dcflags);
    return;
  }
  e->parent = parent;
  copy_path(e->name, 64, name);
  e->node = node;

  /* Insert into hash table */
  e->next = dcache[h];
  dcache[h] = e;

  /* Insert into LRU head */
  e->lru_next = dcache_lru_head;
  e->lru_prev = 0;
  if (dcache_lru_head)
    dcache_lru_head->lru_prev = e;
  dcache_lru_head = e;
  if (!dcache_lru_tail)
    dcache_lru_tail = e;

  dcache_count++;
  dcache_release(dcflags);
}

static void dcache_unlink_locked(struct dcache_entry **prev_ptr,
                                 struct dcache_entry *curr) {
  *prev_ptr = curr->next;
  if (curr == dcache_lru_head)
    dcache_lru_head = curr->lru_next;
  if (curr == dcache_lru_tail)
    dcache_lru_tail = curr->lru_prev;
  if (curr->lru_prev)
    curr->lru_prev->lru_next = curr->lru_next;
  if (curr->lru_next)
    curr->lru_next->lru_prev = curr->lru_prev;
  dcache_free(curr);
  dcache_count--;
}

static void dcache_invalidate(struct vfs_node *parent, const char *name) {
  if (!dcache || !g_dcache_size)
    return;
  u64 dcflags;
  dcache_acquire(&dcflags);
  u32 h = dcache_hash(parent, name);
  struct dcache_entry **prev_ptr = &dcache[h];
  struct dcache_entry *curr = *prev_ptr;
  while (curr) {
    if (curr->parent == parent && strcmp(curr->name, name) == 0) {
      dcache_unlink_locked(prev_ptr, curr);
      dcache_release(dcflags);
      return;
    }
    prev_ptr = &curr->next;
    curr = *prev_ptr;
  }
  dcache_release(dcflags);
}

/* Purge every dcache entry that references `node` either as parent or as the
 * cached child. Called before vfs_free_node so a recycled vfs_node address
 * cannot resurrect a stale lookup against the previous tenant. Without this,
 * dcache_lookup(reused_node, name) can return a dangling child pointer from
 * the previous owner's subtree and crash find_child's sibling walk. */
static void dcache_invalidate_node(struct vfs_node *node) {
  if (!node || !dcache || !g_dcache_size)
    return;
  u64 dcflags;
  dcache_acquire(&dcflags);
  for (u32 h = 0; h < g_dcache_size; h++) {
    struct dcache_entry **prev_ptr = &dcache[h];
    struct dcache_entry *curr = *prev_ptr;
    while (curr) {
      if (curr->parent == node || curr->node == node) {
        dcache_unlink_locked(prev_ptr, curr);
        curr = *prev_ptr;
      } else {
        prev_ptr = &curr->next;
        curr = *prev_ptr;
      }
    }
  }
  dcache_release(dcflags);
}

/* Icache sizing (B2 audit): same RAM-scaled treatment as dcache, smaller
 * per-entry footprint so we use a tighter ratio (1 entry per 512 KiB).
 *
 * Clamps on the RAM-derived sizing in icache_init, raised for the same reason
 * as the dcache above: at ~1 entry per 512 KiB an 8 GiB guest asks for 16384
 * inodes and was given 4096.
 *
 * FLOOR   128 entries / 32 buckets — unchanged for a small guest.
 * CEILING 65536 entries and 32768 buckets, reached at 32 GiB.
 * `b1nix.icache-entries=N` states the pool size directly. */
#define ICACHE_SIZE_MIN          32
#define ICACHE_SIZE_MAX          32768
#define ICACHE_POOL_MIN          128
#define ICACHE_POOL_MAX          65536

struct icache_entry {
  u32 fs_id;
  u64 ino;
  struct vfs_inode *inode;
  struct icache_entry *next;
  struct icache_entry *lru_next;
  struct icache_entry *lru_prev;
};

static struct icache_entry *icache_pool = 0;
static u32 g_icache_pool_size = 0;
static struct icache_entry *icache_free_list = 0;
static struct icache_entry **icache_buckets = 0;
static u32 g_icache_size = 0;
static struct icache_entry *icache_lru_head = 0;
static struct icache_entry *icache_lru_tail = 0;
static int icache_count = 0;
static volatile int icache_lock = 0;

static void icache_acquire(void) {
  while (__atomic_test_and_set(&icache_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();
}

static void icache_release(void) {
  __atomic_clear(&icache_lock, __ATOMIC_RELEASE);
}

static u32 icache_hash(u32 fs_id, u64 ino) {
  return (u32)((fs_id * 2654435761ULL + ino) % g_icache_size);
}

void icache_init(void) {
  /* Scale to RAM: ~1 entry per 512 KiB usable RAM, clamped. */
  u64 ram_mb = pmm_total_usable_memory() / (1024ULL * 1024ULL);
  u32 pool = bootinfo_get_u32(
      "b1nix.icache-entries",
      (u32)(ram_mb > 0x100000ULL ? 0x100000ULL : ram_mb) * 2u);
  if (pool < ICACHE_POOL_MIN) pool = ICACHE_POOL_MIN;
  if (pool > ICACHE_POOL_MAX) pool = ICACHE_POOL_MAX;
  g_icache_pool_size = pool;
  u32 table = pool / 2;
  if (table < ICACHE_SIZE_MIN) table = ICACHE_SIZE_MIN;
  if (table > ICACHE_SIZE_MAX) table = ICACHE_SIZE_MAX;
  g_icache_size = table;

  icache_pool = kzalloc(g_icache_pool_size * sizeof(struct icache_entry));
  icache_buckets = kzalloc(g_icache_size * sizeof(struct icache_entry *));
  if (!icache_pool || !icache_buckets) {
    g_icache_pool_size = 0;
    g_icache_size = 0;
    icache_free_list = 0;
    icache_lru_head = 0;
    icache_lru_tail = 0;
    icache_count = 0;
    return;
  }
  for (u32 i = 0; i < g_icache_pool_size - 1; i++)
    icache_pool[i].next = &icache_pool[i + 1];
  icache_pool[g_icache_pool_size - 1].next = 0;
  icache_free_list = &icache_pool[0];
  icache_lru_head = 0;
  icache_lru_tail = 0;
  icache_count = 0;
}

static struct icache_entry *icache_alloc(void) {
  if (!icache_free_list)
    return 0;
  struct icache_entry *e = icache_free_list;
  icache_free_list = e->next;
  memset(e, 0, sizeof(struct icache_entry));
  return e;
}

static void icache_free(struct icache_entry *e) {
  e->next = icache_free_list;
  icache_free_list = e;
}

struct vfs_inode *icache_get(u32 fs_id, u64 ino) {
  if (!fs_id || !ino)
    return 0;
  icache_acquire();
  u32 h = icache_hash(fs_id, ino);
  struct icache_entry *e = icache_buckets[h];
  while (e) {
    if (e->fs_id == fs_id && e->ino == ino) {
      if (e != icache_lru_head) {
        if (e == icache_lru_tail)
          icache_lru_tail = e->lru_prev;
        if (e->lru_prev)
          e->lru_prev->lru_next = e->lru_next;
        if (e->lru_next)
          e->lru_next->lru_prev = e->lru_prev;
        e->lru_next = icache_lru_head;
        e->lru_prev = 0;
        if (icache_lru_head)
          icache_lru_head->lru_prev = e;
        icache_lru_head = e;
        if (!icache_lru_tail)
          icache_lru_tail = e;
      }
      struct vfs_inode *res = e->inode;
      icache_release();
      return res;
    }
    e = e->next;
  }
  icache_release();
  return 0;
}

void icache_insert(u32 fs_id, u64 ino, struct vfs_inode *inode) {
  if (!fs_id || !ino || !inode)
    return;
  icache_acquire();
  u32 h = icache_hash(fs_id, ino);
  struct icache_entry *existing = icache_buckets[h];
  while (existing) {
    if (existing->fs_id == fs_id && existing->ino == ino) {
      existing->inode = inode;
      icache_release();
      return;
    }
    existing = existing->next;
  }

  if ((u32)icache_count >= g_icache_pool_size) {
    struct icache_entry *victim = icache_lru_tail;
    if (victim) {
      u32 vh = icache_hash(victim->fs_id, victim->ino);
      struct icache_entry **prev_ptr = &icache_buckets[vh];
      while (*prev_ptr && *prev_ptr != victim)
        prev_ptr = &(*prev_ptr)->next;
      if (*prev_ptr)
        *prev_ptr = victim->next;

      if (victim == icache_lru_head)
        icache_lru_head = victim->lru_next;
      if (victim == icache_lru_tail)
        icache_lru_tail = victim->lru_prev;
      if (victim->lru_prev)
        victim->lru_prev->lru_next = victim->lru_next;
      if (victim->lru_next)
        victim->lru_next->lru_prev = victim->lru_prev;
      icache_free(victim);
      icache_count--;
    }
  }

  struct icache_entry *e = icache_alloc();
  if (!e) {
    icache_release();
    return;
  }
  e->fs_id = fs_id;
  e->ino = ino;
  e->inode = inode;
  e->next = icache_buckets[h];
  icache_buckets[h] = e;
  e->lru_next = icache_lru_head;
  e->lru_prev = 0;
  if (icache_lru_head)
    icache_lru_head->lru_prev = e;
  icache_lru_head = e;
  if (!icache_lru_tail)
    icache_lru_tail = e;
  icache_count++;
  icache_release();
}

void icache_invalidate(u32 fs_id, u64 ino) {
  if (!fs_id || !ino)
    return;
  icache_acquire();
  u32 h = icache_hash(fs_id, ino);
  struct icache_entry **prev_ptr = &icache_buckets[h];
  struct icache_entry *curr = *prev_ptr;
  while (curr) {
    if (curr->fs_id == fs_id && curr->ino == ino) {
      *prev_ptr = curr->next;
      if (curr == icache_lru_head)
        icache_lru_head = curr->lru_next;
      if (curr == icache_lru_tail)
        icache_lru_tail = curr->lru_prev;
      if (curr->lru_prev)
        curr->lru_prev->lru_next = curr->lru_next;
      if (curr->lru_next)
        curr->lru_next->lru_prev = curr->lru_prev;
      icache_free(curr);
      icache_count--;
      icache_release();
      return;
    }
    prev_ptr = &curr->next;
    curr = *prev_ptr;
  }
  icache_release();
}

void icache_invalidate_fs(u32 fs_id) {
  if (!fs_id)
    return;
  icache_acquire();
  for (u32 h = 0; h < g_icache_size; h++) {
    struct icache_entry **prev_ptr = &icache_buckets[h];
    struct icache_entry *curr = *prev_ptr;
    while (curr) {
      if (curr->fs_id == fs_id) {
        struct icache_entry *victim = curr;
        *prev_ptr = curr->next;
        curr = *prev_ptr;
        if (victim == icache_lru_head)
          icache_lru_head = victim->lru_next;
        if (victim == icache_lru_tail)
          icache_lru_tail = victim->lru_prev;
        if (victim->lru_prev)
          victim->lru_prev->lru_next = victim->lru_next;
        if (victim->lru_next)
          victim->lru_next->lru_prev = victim->lru_prev;
        icache_free(victim);
        icache_count--;
      } else {
        prev_ptr = &curr->next;
        curr = *prev_ptr;
      }
    }
  }
  icache_release();
}

/* Record who holds an inode's rwlock, for the watchdog's chan report. */
static void vfs_inode_lock_note(struct vfs_inode *inode, const void *site) {
  struct task *t = current_task;
  __atomic_store_n(&inode->rw_owner, t ? (u64)t->id : 0, __ATOMIC_RELAXED);
  inode->rw_site = site;
}

static void vfs_inode_lock_clear_note(struct vfs_inode *inode) {
  __atomic_store_n(&inode->rw_owner, 0, __ATOMIC_RELAXED);
  inode->rw_site = 0;
}

static void vfs_inode_lock_read(struct vfs_inode *inode) {
  if (blk_cache_lock_is_held()) {
    /* Walking the frame pointer chain into panic() shows just the
     * vfs_inode_lock_read frame; print our caller's return address up-front
     * so the offender is named even if the post-panic backtrace fails. */
    console_write("[LOCK ORDER] vfs_inode_lock_read called by 0x");
    console_write_hex64((u64)(usize)__builtin_return_address(0));
    console_write(" while bcache lock held\n");
    panic("vfs: inode read-lock under block-cache lock");
  }
  while (1) {
    int val = inode->rw_lock;
    if (val >= 0 &&
        __atomic_compare_exchange_n(&inode->rw_lock, &val, val + 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      break;
    }
    /* SMP-safe block (closes the check-then-block lost-wakeup window): publish
     * BLOCKED, then retry the acquire before sleeping. Otherwise a writer's
     * vfs_inode_unlock_write -> scheduler_wake_all(&rw_lock) on another CPU,
     * landing between the failed CAS and the block, is lost and this reader
     * sleeps on the inode forever (a silent -smp wedge on the file read path). */
    scheduler_wait_prepare((void *)&inode->rw_lock);
    val = inode->rw_lock;
    if (val >= 0 &&
        __atomic_compare_exchange_n(&inode->rw_lock, &val, val + 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      scheduler_wait_cancel();
      break;
    }
    scheduler_wait_commit();
  }
  /* INODE rwlock is a sleeping lock — scheduler_block_on can wake the
   * caller on a different CPU than it slept on, so the release-CPU and
   * acquire-CPU may differ. Track via the global-singleton lockdep
   * entry (M28 #2 Variant A) instead of the per-CPU acquisition stack. */
  vfs_inode_lock_note(inode, __builtin_return_address(0));
  LOCKDEP_ACQUIRE_GLOBAL(LOCKDEP_LVL_INODE);
}

static void vfs_inode_unlock_read(struct vfs_inode *inode) {
  LOCKDEP_RELEASE_GLOBAL(LOCKDEP_LVL_INODE);
  if (__atomic_add_fetch(&inode->rw_lock, -1, __ATOMIC_RELEASE) == 0) {
    vfs_inode_lock_clear_note(inode);
    scheduler_wake_all((void *)&inode->rw_lock);
  }
}


/* Time spent waiting for an inode lock, and who was holding it.
 *
 * The profile showed unlink at nearly half a second a call on a cold start and
 * absent on a warm one, which is the shape of waiting rather than working. The
 * inode lock is the plausible suspect, so it is measured: total cycles waited,
 * how many waits, and the worst single wait with the site that held the lock.
 * Read through /proc/b1nix-prof. */
static u64 g_inode_wait_cycles, g_inode_waits, g_inode_wait_worst;
static const void *g_inode_wait_worst_site;

static inline u64 vfs_lock_tsc(void) {
  unsigned lo, hi;
  { u64 c_ = arch_cycles(); lo = (u32)c_; hi = (u32)(c_ >> 32); }
  return ((u64)hi << 32) | lo;
}

static void vfs_inode_wait_note(u64 cycles, const void *holder_site) {
  __atomic_fetch_add(&g_inode_wait_cycles, cycles, __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_inode_waits, 1, __ATOMIC_RELAXED);
  if (cycles > __atomic_load_n(&g_inode_wait_worst, __ATOMIC_RELAXED)) {
    __atomic_store_n(&g_inode_wait_worst, cycles, __ATOMIC_RELAXED);
    __atomic_store_n(&g_inode_wait_worst_site, holder_site, __ATOMIC_RELAXED);
  }
}

void vfs_inode_wait_stats(u64 *cycles, u64 *waits, u64 *worst,
                          const void **site) {
  if (cycles) *cycles = __atomic_load_n(&g_inode_wait_cycles, __ATOMIC_RELAXED);
  if (waits) *waits = __atomic_load_n(&g_inode_waits, __ATOMIC_RELAXED);
  if (worst) *worst = __atomic_load_n(&g_inode_wait_worst, __ATOMIC_RELAXED);
  if (site) *site = __atomic_load_n(&g_inode_wait_worst_site, __ATOMIC_RELAXED);
}

void vfs_inode_wait_reset(void) {
  __atomic_store_n(&g_inode_wait_cycles, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_inode_waits, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_inode_wait_worst, 0, __ATOMIC_RELAXED);
}

static void vfs_inode_lock_write(struct vfs_inode *inode) {
  if (blk_cache_lock_is_held()) {
    console_write("[LOCK ORDER] vfs_inode_lock_write called by 0x");
    console_write_hex64((u64)(usize)__builtin_return_address(0));
    console_write(" while bcache lock held\n");
    panic("vfs: inode write-lock under block-cache lock");
  }
  u64 wait_start = 0;
  while (1) {
    int val = 0;
    if (__atomic_compare_exchange_n(&inode->rw_lock, &val, -1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      if (wait_start)
        vfs_inode_wait_note(vfs_lock_tsc() - wait_start, inode->rw_site);
      break;
    }
    if (!wait_start)
      wait_start = vfs_lock_tsc();
    /* SMP-safe block — see vfs_inode_lock_read for the lost-wakeup rationale. */
    scheduler_wait_prepare((void *)&inode->rw_lock);
    val = 0;
    if (__atomic_compare_exchange_n(&inode->rw_lock, &val, -1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      scheduler_wait_cancel();
      vfs_inode_wait_note(vfs_lock_tsc() - wait_start, inode->rw_site);
      break;
    }
    scheduler_wait_commit();
  }
  vfs_inode_lock_note(inode, __builtin_return_address(0));
  /* Sleeping lock — see read-lock variant for why this uses _GLOBAL. */
  LOCKDEP_ACQUIRE_GLOBAL(LOCKDEP_LVL_INODE);
}

static void vfs_inode_unlock_write(struct vfs_inode *inode) {
  LOCKDEP_RELEASE_GLOBAL(LOCKDEP_LVL_INODE);
  vfs_inode_lock_clear_note(inode);
  __atomic_store_n(&inode->rw_lock, 0, __ATOMIC_RELEASE);
  scheduler_wake_all((void *)&inode->rw_lock);
}

/* The watchdog resolves a blocked task's wait channel to a heap block. When
 * that block is exactly an inode and the channel sits on its rwlock, this names
 * the holder — the difference between "parked on some kernel object" and "task
 * N took this inode's lock in <caller> and never dropped it". */
void vfs_inode_chan_report(u64 chan, u64 payload_base, usize block_size) {
  if (block_size != sizeof(struct vfs_inode))
    return;
  if (chan - payload_base != __builtin_offsetof(struct vfs_inode, rw_lock))
    return;
  struct vfs_inode *inode = (struct vfs_inode *)(usize)payload_base;
  console_write("    inode ino=");
  console_write_dec((u64)inode->ino);
  console_write(" rw_lock=");
  console_write_dec((u64)(int)inode->rw_lock);
  console_write(" holder=");
  console_write_dec(__atomic_load_n(&inode->rw_owner, __ATOMIC_RELAXED));
  console_write(" taken-at=0x");
  console_write_hex64((u64)(usize)inode->rw_site);
  console_write("\n");
}

/* Compatibility wrappers */
static void vfs_inode_lock(struct vfs_inode *inode) {
  vfs_inode_lock_write(inode);
}
static void vfs_inode_unlock(struct vfs_inode *inode) {
  vfs_inode_unlock_write(inode);
}

/* Sleeping mutex for filesystem-wide metadata (ext4/ext2 allocator bitmaps and
 * superblock counters). Uses the same publish-BLOCKED-then-retry pattern as the
 * inode rwlock above so a release on another CPU cannot be lost. The holder
 * sleeps on block I/O, so this must never be a spinlock. */
void vfs_meta_lock_acquire(int *lock) {
  while (1) {
    int expected = 0;
    if (__atomic_compare_exchange_n(lock, &expected, 1, 0, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED))
      return;
    scheduler_wait_prepare((void *)lock);
    expected = 0;
    if (__atomic_compare_exchange_n(lock, &expected, 1, 0, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
      scheduler_wait_cancel();
      return;
    }
    scheduler_wait_commit();
  }
}

void vfs_meta_lock_release(int *lock) {
  __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
  scheduler_wake_all((void *)lock);
}

static const struct cred *get_current_cred(void) {
  return scheduler_get_current_cred();
}

u64 vfs_get_unix_time(void) {
  return rtc_now_unix_seconds();
}

static u16 scheduler_get_current_umask(void) {
  const struct cred *cred = get_current_cred();
  return cred ? cred->umask : 0022;
}

static void vfs_update_times(struct vfs_inode *inode, u32 mask) {
  if (!inode)
    return;
  /* One clock read for all three, and both halves of it. A whole second is
   * too coarse to answer "has this changed since I last looked" — see
   * vfs_inode::mtime_nsec. */
  u64 ns = rtc_now_unix_nanos();
  u64 now = ns / 1000000000ull;
  u32 sub = (u32)(ns % 1000000000ull);
  if (mask & VFS_ATIME) {
    inode->atime = now;
    inode->atime_nsec = sub;
  }
  if (mask & VFS_MTIME) {
    inode->mtime = now;
    inode->mtime_nsec = sub;
  }
  if (mask & VFS_CTIME) {
    inode->ctime = now;
    inode->ctime_nsec = sub;
  }
}

/* All three timestamps set to now — what a freshly created inode gets. */
static void vfs_init_times(struct vfs_inode *inode) {
  vfs_update_times(inode, VFS_ATIME | VFS_MTIME | VFS_CTIME);
}

/* A directory whose set of entries changed.
 *
 * POSIX requires that creating, removing or renaming an entry mark the
 * containing directory as modified — the last-data-modification and
 * last-status-change times both move. Nothing here did that, so a directory's
 * mtime was fixed at the moment it was created and never moved again.
 *
 * The consumer that made this visible is systemd's unit cache: a `systemctl
 * start` of a unit the manager has not loaded re-scans the unit directories
 * only when their mtimes differ from the ones the last scan recorded. With a
 * directory whose mtime never moves, a unit file written after the first
 * `daemon-reload` is invisible forever — "Unit b1nix-notify.service not
 * found" for a file sitting in /run/systemd/system. Every tool that decides
 * whether to re-read a directory this way (make, ccache, package managers)
 * has the same blind spot. */
static void vfs_dir_changed(struct vfs_node *dir) {
  if (dir)
    vfs_update_times(dir->inode, VFS_MTIME | VFS_CTIME);
}

static usize node_count = 0;

usize vfs_active_node_count(void) {
  return node_count;
}

static struct vfs_node *root_node = 0;
static char tty_line[TTY_INPUT_SIZE];
static usize tty_line_pos;
static usize tty_line_len;

static struct vfs_node *vfs_cross_root_mount(struct vfs_node *node) {
  if (!node || node != root_node)
    return node;

  struct vfs_node *mounted_root = 0;
  for (int i = 0; i < (int)mount_slots; i++) {
    if (mount_visible(i) && mounts[i].root_node &&
        strcmp(mounts[i].target, "/") == 0) {
      mounted_root = mounts[i].root_node;
    }
  }

  if (!mounted_root || mounted_root == node)
    return node;

  vfs_node_get(mounted_root);
  vfs_node_put(node);
  return mounted_root;
}

void virtio_blk_init(void);
void virtio_blk_mmio_init(void);
void bcm2711_emmc_init(void);
extern char ps2_kbd_getc(void);
extern int ps2_kbd_has_data(void);
int serial_has_data(void);

int vfs_mount(const char *source, const char *target, const char *fstype,
              u64 flags);

/* ── Permission Helpers ── */

extern struct cred *scheduler_get_current_cred(void);

int vfs_check_access(struct vfs_node *node, int requested_access) {
  if (!node)
    return -ENOENT;
  const struct cred *cred = get_current_cred();
  if (!cred)
    return -EACCES;
  if (vfs_get_node_perm(node, cred, (u32)requested_access)) {
    return 0;
  }
  return -EACCES;
}

int vfs_get_node_perm(const struct vfs_node *node, const struct cred *cred,
                      u32 mask) {
  if (!node || !node->inode || !cred)
    return 0;
  struct vfs_inode *inode = node->inode;
  /* Filesystem access is judged by fsuid, which mirrors euid unless
   * setfsuid(2) moved it. */
  if (cred->fsuid == ROOT_UID)
    return 1;
  if (cred_has_cap(cred, CAP_DAC_OVERRIDE))
    return 1;
  if (!(mask & 2) && cred_has_cap(cred, CAP_DAC_READ_SEARCH))
    return 1;

  if (inode->acl_count > 0) {
    u16 matched_perms = 0;
    int mask_found = 0;
    u16 mask_perms = 0;
    for (int i = 0; i < inode->acl_count; i++) {
      if (inode->acls[i].tag == ACL_MASK) {
        mask_found = 1;
        mask_perms = inode->acls[i].perms;
      }
    }
    for (int i = 0; i < inode->acl_count; i++) {
      switch (inode->acls[i].tag) {
      case ACL_USER_OBJ:
        if (cred->euid == inode->uid) {
          matched_perms = inode->acls[i].perms;
          goto acl_check;
        }
        break;
      case ACL_USER:
        if (cred->euid == inode->acls[i].qualifier) {
          matched_perms = inode->acls[i].perms;
          goto acl_check;
        }
        break;
      case ACL_GROUP_OBJ:
        if (cred->egid == inode->gid) {
          matched_perms = inode->acls[i].perms;
          goto acl_check;
        }
        break;
      case ACL_GROUP:
        if (cred->egid == inode->acls[i].qualifier) {
          matched_perms = inode->acls[i].perms;
          goto acl_check;
        }
        for (int g = 0; g < cred->ngroups; g++) {
          if (cred->groups[g] == inode->acls[i].qualifier) {
            matched_perms = inode->acls[i].perms;
            goto acl_check;
          }
        }
        break;
      }
    }
    matched_perms = inode->mode & 7;

  acl_check:
    if (mask_found)
      matched_perms &= mask_perms;
    return (matched_perms & mask) == mask;
  }
  return cred_can_access(cred, inode->uid, inode->gid, inode->mode, mask);
}

/* ── Node/Inode allocation ── */

/* Filesystem id of the mount currently being set up, or 0. A filesystem builds
 * its whole tree inside its mount callback — before vfs_mount can stamp the
 * root it has not returned yet — so every inode created in that window would
 * otherwise carry fs_id 0. That id is not cosmetic: the page cache keys pages
 * by (fs_id, ino), so two filesystems whose nodes both carry 0 and reuse the
 * same on-disk inode number share cache entries, and one mount reads the
 * other's data. Publishing the id before the callback runs closes that. */
static u32 g_mounting_fs_id;

/* The id of the mount being established, for filesystems that create their
 * inodes lazily. Only inodes allocated while fs->mount() runs pick it up
 * automatically; a driver that builds nodes later (every on-disk one does,
 * on lookup) has to record it at mount time and stamp its own inodes. */
u32 vfs_mounting_fs_id(void) { return g_mounting_fs_id; }

static struct vfs_inode *alloc_inode(void) {
  struct vfs_inode *inode = vfs_alloc_inode();
  if (inode) {
    /* One reference: the node this inode is about to be attached to. Every
     * caller here hangs the inode off a fresh vfs_node, and vfs_node_put()
     * releases exactly one inode reference when that node is finally freed --
     * so an inode born at 0 makes its own node's free an underflow. Only
     * vfs_create_node() used to set this, which is why the panic came from the
     * paths that build a node by hand (create/mkdir/mknod/symlink): a
     * `rename(new, existing)` frees the target node and drops an inode
     * reference nobody ever took. */
    inode->refcount = 1;
    inode->fs_id = g_mounting_fs_id;
  }
  return inode;
}

struct vfs_inode *vfs_inode_get(struct vfs_inode *inode) {
  if (inode)
    __atomic_add_fetch(&inode->refcount, 1, __ATOMIC_RELAXED);
  return inode;
}

void vfs_inode_put(struct vfs_inode *inode) {
  if (!inode)
    return;
  int new_ref = __atomic_sub_fetch(&inode->refcount, 1, __ATOMIC_RELAXED);
  if (new_ref < 0) {
    /* Name the inode. "refcount underflow" on its own says a reference was
     * dropped that nobody held, but not which object, and the same call site
     * runs for every file in the system. */
    console_write("vfs: inode underflow ino=");
    console_write_dec(inode->ino);
    console_write(" fs_id=");
    console_write_dec((u64)inode->fs_id);
    console_write(" type=");
    console_write_dec((u64)inode->type);
    console_write(" nlink=");
    console_write_dec((u64)inode->nlink);
    console_write(" size=");
    console_write_dec((u64)inode->size);
    console_write(" refcount=");
    console_write_dec((u64)(u32)new_ref);
    console_write(" caller=0x");
    console_write_hex64((u64)(usize)__builtin_return_address(0));
    console_write("\n");
    panic("vfs: inode refcount underflow");
  }
  if (new_ref == 0 && inode->nlink == 0) {
    page_cache_invalidate_inode(inode);
    /* IC-1: drop any icache entry that maps to this inode BEFORE freeing it,
     * so a later icache_get(fs_id, ino) can't resurrect a dangling pointer
     * into freed slab memory. (The icache currently stores a raw, unreferenced
     * inode pointer; full reference-pinning of cached inodes is deferred.) */
    if (inode->fs_id)
      icache_invalidate(inode->fs_id, inode->ino);
    if (inode->data && (inode->flags & VFS_NODE_OWNS_DATA)) {
      kfree(inode->data);
    }
    vfs_free_xattrs(inode);
    vfs_free_inode(inode);
  }
}

/* Forward declaration: dir_seq's counter lives with the readdir cursor logic
 * further down, but every node must get a sequence number here — see below. */
static u64 dir_seq_next(void);

static struct vfs_node *alloc_node(void) {
  struct vfs_node *n = vfs_alloc_node();
  if (n) {
    n->refcount = 0;
    /* Assign the readdir cursor sequence for EVERY node, not just the ones
     * built through vfs_create_node. The in-memory readdir resumes from
     * "children whose dir_seq is below the last one emitted", and a child left
     * at 0 collapsed that bound to "no bound": the walk restarted from the head
     * of the sibling list on every call, so readdir() on the directory repeated
     * the same entry forever and `ls /run` never returned. */
    n->dir_seq = dir_seq_next();
    __atomic_add_fetch(&node_count, 1, __ATOMIC_RELAXED);
  }
  return n;
}

struct vfs_node *vfs_node_get(struct vfs_node *node) {
  if (node)
    __atomic_add_fetch(&node->refcount, 1, __ATOMIC_RELAXED);
  return node;
}

void vfs_node_put(struct vfs_node *node) {
  if (!node)
    return;
  int new_ref = __atomic_sub_fetch(&node->refcount, 1, __ATOMIC_RELAXED);
  if (new_ref < 0) {
    panic("vfs: node refcount underflow");
  }
  if (new_ref == 0 && node->deleted) {
    if (node->inode && node->inode->release_cb) {
      node->inode->release_cb(node);
    }
    /* The node is about to drop its inode reference. If the inode has none
     * left, this node never owned one -- name it, because "inode refcount
     * underflow" alone does not say which of the nodes sharing that inode is
     * the one accounting for it wrongly. */
    if (node->inode &&
        __atomic_load_n(&node->inode->refcount, __ATOMIC_RELAXED) <= 0) {
      console_write("vfs: node '");
      console_write(node->name[0] ? node->name : "(unnamed)");
      console_write("' releasing an inode it does not hold: ino=");
      console_write_dec(node->inode->ino);
      console_write(" parent='");
      console_write(node->parent && node->parent->name[0] ? node->parent->name
                                                          : "/");
      console_write("'\n");
    }
    vfs_inode_put(node->inode);
    /* Purge any dcache entry that references this node before its memory is
     * returned to the slab pool — otherwise dcache_lookup can resurrect a
     * dangling pointer once the address is recycled. */
    dcache_invalidate_node(node);
    vfs_free_node(node);
    __atomic_sub_fetch(&node_count, 1, __ATOMIC_RELAXED);
  }
}


/* Peel the first component off `path`.
 *
 * Returns 1 when the component did not fit in `first_size` and 0 otherwise.
 *
 * It used to return nothing and truncate silently, and the damage was not the
 * truncation -- it was that `*rest` then resumed in the MIDDLE of the name, so
 * a component longer than the buffer was re-parsed as two components. With a
 * 64-byte buffer, "/tmp/systemd-private-<32 hex>-systemd-logind.service-XXXXXX"
 * (78 characters) was looked up as a 63-character directory containing a
 * 15-character one. Nothing of that shape exists, so every path with a long
 * component answered ENOENT -- while creating it through a different route
 * succeeded, because the name was stored whole. That is what broke PrivateTmp=
 * for every unit that sets it: systemd's mkdtemp() created the directory and
 * the very next mkdir() inside it could not find it.
 *
 * A component that does not fit is ENAMETOOLONG, which is what Linux answers
 * and what the caller can act on -- never a different, shorter path. */
static int split_path(const char *path, char *first_part, usize first_size,
                      const char **rest) {
  if (!path || !first_part || !first_size) {
    if (first_part && first_size) first_part[0] = '\0';
    if (rest) *rest = 0;
    return 0;
  }
  while (*path == '/')
    path++;
  if (*path == '\0') {
    first_part[0] = '\0';
    *rest = 0;
    return 0;
  }
  usize i = 0;
  while (path[i] != '\0' && path[i] != '/' && i + 1 < first_size) {
    first_part[i] = path[i];
    i++;
  }
  first_part[i] = '\0';
  /* Still inside the name: it was too long for the buffer. Step over the rest
   * of it so `*rest` starts at a real boundary rather than mid-name. */
  if (path[i] != '\0' && path[i] != '/') {
    while (path[i] != '\0' && path[i] != '/')
      i++;
    *rest = path + i;
    return 1;
  }
  *rest = path + i;
  return 0;
}

struct vfs_node *find_child(struct vfs_node *parent, const char *name) {
  if (!parent || !parent->inode || parent->inode->type != VFS_DIRECTORY)
    return 0;

  /* M28-B: read-side of vfs_tree_lock — the children list walk must observe a
   * consistent snapshot of first_child / next_sibling. Without this lock, a
   * concurrent unlink/rmdir (which splices a sibling out and frees it) could
   * leave us dereferencing a freed node mid-walk. The IRQ-save variant keeps
   * the pre-rwlock cli/sti semantics so a same-CPU writer entered from a
   * (future) preemptive timer ISR can't race either; vfs_node_get is atomic
   * and safe with IRQs disabled. */
  struct vfs_node *result = 0;
  u64 flags;
  vfs_tree_read_acquire(&flags);

  /* Ask the dentry cache before walking. The sibling list is a plain linked
   * list, so a directory with N entries costs N comparisons per lookup, and a
   * package extract does several lookups per file it creates — the /usr/lib of
   * a desktop install turns that into hundreds of millions of comparisons.
   * The cache is invalidated on unlink, rename, create-over and node free, so
   * a hit is as authoritative as the walk. */
  result = dcache_lookup(parent, name);
  if (result) {
    vfs_tree_read_release(flags);
    return result;
  }

  /* The first character before the call: a directory with a few thousand
   * entries is walked once per lookup, and an entry whose name starts with a
   * different letter cannot match. Comparing that byte inline skips the call
   * for almost all of them. */
  const char first = name[0];
  struct vfs_node *child = parent->first_child;
  while (child) {
    if (!child->deleted && child->name[0] == first &&
        strcmp(child->name, name) == 0) {
      result = child;
      vfs_node_get(result); /* REFCOUNT RULE: caller owns the returned ref */
      break;
    }
    child = child->next_sibling;
  }
  if (result && strlen(name) < 64)
    dcache_insert(parent, name, result);
  vfs_tree_read_release(flags);
  return result;
}

static void append_path_part(char *dst, usize dst_size, const char *part) {
  usize len = strlen(dst);
  if (len == 0 && dst_size > 1) {
    dst[0] = '/';
    dst[1] = '\0';
    len = 1;
  }
  if (len > 1 && len < dst_size - 1) {
    dst[len++] = '/';
    dst[len] = '\0';
  }
  usize i = 0;
  while (part[i] && len < dst_size - 1)
    dst[len++] = part[i++];
  dst[len] = '\0';
}

static void pop_path_part(char *path) {
  usize len = strlen(path);
  if (len <= 1) {
    if (len == 0) {
      path[0] = '/';
      path[1] = '\0';
    }
    return;
  }
  if (path[len - 1] == '/')
    len--;
  while (len > 1 && path[len - 1] != '/')
    len--;
  path[len] = '\0';
}

__attribute__((unused)) static void compose_symlink_path(const char *parent_path, const char *target,
                                 const char *rest, char *out, usize out_size) {
  out[0] = '\0';
  if (!target || target[0] == '\0')
    return;

  if (target[0] == '/') {
    copy_path(out, out_size, target);
  } else {
    copy_path(out, out_size, parent_path && parent_path[0] ? parent_path : "/");
    append_path_part(out, out_size, target);
  }

  if (rest && rest[0]) {
    while (*rest == '/')
      rest++;
    if (*rest)
      append_path_part(out, out_size, rest);
  }
}

void vfs_resolve_path(const char *path, char *out) {
  if (!path || !out)
    return;
  char combined[VFS_MAX_PATH];
  /* strncpy writes no terminator when the source fills the buffer.
   *
   * These three copies passed the full buffer size, so a path exactly
   * VFS_MAX_PATH long (or longer) left `combined` unterminated, and the
   * strlen/strncat/loop below then ran off the end of a stack array — into the
   * caller's own locals. It surfaced as a general-protection fault inside
   * strncpy with a "pointer" made of filename bytes: the path pointer of the
   * frame above had been overwritten by the overrun. A browser reaches these
   * lengths routinely; nothing shorter ever did, which is why it took a
   * 200 MB program to find. */
  if (path[0] == '/') {
    strncpy(combined, path, VFS_MAX_PATH - 1);
    combined[VFS_MAX_PATH - 1] = '\0';
  } else {
    const char *cwd = scheduler_get_cwd();
    strncpy(combined, cwd, VFS_MAX_PATH - 1);
    combined[VFS_MAX_PATH - 1] = '\0';
    usize len = strlen(combined);
    if (len > 0 && combined[len - 1] != '/' && len < VFS_MAX_PATH - 1) {
      combined[len++] = '/';
      combined[len] = '\0';
    }
    strncat(combined, path, VFS_MAX_PATH - len - 1);
  }

  char *parts[64];
  int part_count = 0;
  char tmp[VFS_MAX_PATH];
  strncpy(tmp, combined, VFS_MAX_PATH - 1);
  tmp[VFS_MAX_PATH - 1] = '\0';
  char *curr = tmp;
  while (*curr && part_count < 64) {
    while (*curr == '/')
      curr++;
    if (!*curr)
      break;
    char *start = curr;
    while (*curr && *curr != '/')
      curr++;
    if (*curr) {
      *curr = '\0';
      curr++;
    }
    /* Ignore current directory */
    if (strcmp(start, ".") == 0)
      continue;

    /* Handle parent directory */
    if (strcmp(start, "..") == 0) {
      if (part_count > 0)
        part_count--;
      continue;
    }

    parts[part_count++] = start;
  }
  out[0] = '/';
  out[1] = '\0';
  for (int i = 0; i < part_count; i++) {
    strcat(out, parts[i]);
    if (i < part_count - 1)
      strcat(out, "/");
  }

  /* POSIX: preserve trailing slash for directory resolution */
  usize path_len = strlen(path);
  if (path_len > 0 && path[path_len - 1] == '/' && part_count > 0) {
    usize out_len = strlen(out);
    if (out_len > 0 && out[out_len - 1] != '/' && out_len < VFS_MAX_PATH - 1) {
      out[out_len] = '/';
      out[out_len + 1] = '\0';
    }
  }

  /*
  console_write("VFS: resolve '");
  console_write(path);
  console_write("' -> '");
  console_write(out);
  console_write("'\n");
  */
}

/* POSIX: Iterative path resolution with symlink loop detection to prevent stack
 * overflow */
static struct vfs_node *
vfs_find_node_internal(const char *path, int follow_final, int symlink_depth) {
  if (!root_node || !path)
    return ERR_PTR(-ENOENT);

  if (symlink_depth > VFS_MAX_SYMLINK_DEPTH)
    return ERR_PTR(-ELOOP);

  char *curr_path = kmalloc(VFS_MAX_PATH);
  if (!curr_path)
    return ERR_PTR(-ENOMEM);
  strncpy(curr_path, path, VFS_MAX_PATH - 1);
  curr_path[VFS_MAX_PATH - 1] = '\0';

  char *parent_path = kmalloc(VFS_MAX_PATH);
  if (!parent_path) {
    kfree(curr_path);
    return ERR_PTR(-ENOMEM);
  }
  parent_path[0] = '/';
  parent_path[1] = '\0';

  /* chroot(2): a chrooted task resolves absolute paths from its own root, not
   * the real one. The node is ref-held by the task (kernel/sched/scheduler.c),
   * so it cannot disappear under the walk. */
  struct vfs_node *task_root = scheduler_get_root_node();
  struct vfs_node *start = task_root ? task_root : root_node;
  vfs_node_get(start);
  struct vfs_node *current = start;
  if (!task_root)
    current = vfs_cross_root_mount(current);
  vfs_inode_lock_read(current->inode);

  /* One component, at the size a name may actually be. This was 64 while
   * VFS_NAME_MAX is 256, so every name of 64 characters or more resolved to
   * something that does not exist -- see split_path. */
  char part[VFS_NAME_MAX];
  const char *rest = curr_path;

restart_traversal:
  while (1) {
    while (*rest == '/')
      rest++;

    if (split_path(rest, part, sizeof(part), &rest)) {
      vfs_inode_unlock_read(current->inode);
      vfs_node_put(current);
      kfree(curr_path);
      kfree(parent_path);
      return ERR_PTR(-ENAMETOOLONG);
    }

    if (part[0] == '\0') {
      int orig_len = strlen(path);
      if (orig_len > 0 && path[orig_len - 1] == '/') {
        if (current->inode->type != VFS_DIRECTORY) {
          vfs_inode_unlock_read(current->inode);
          vfs_node_put(current);
          kfree(curr_path);
          kfree(parent_path);
          return ERR_PTR(-ENOTDIR);
        }
      }
      kfree(curr_path);
      kfree(parent_path);
      vfs_inode_unlock_read(current->inode);
      return current;
    }

    if (strcmp(part, ".") == 0)
      continue;

    if (strcmp(part, "..") == 0) {
      while (__atomic_test_and_set(&vfs_mount_lock, __ATOMIC_ACQUIRE))
        scheduler_yield();
      for (int i = 0; i < (int)mount_slots; i++) {
        if (mount_visible(i) && current == mounts[i].root_node) {
          struct vfs_node *mp = vfs_node_get(mounts[i].mount_point);
          __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
          vfs_inode_unlock_read(current->inode);
          vfs_node_put(current);
          current = mp;
          vfs_inode_lock_read(current->inode);
          continue;
        }
      }
      __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);

      /* ".." must not escape a chroot: at the task's root it resolves to
       * itself, exactly as it does at the real filesystem root. */
      struct vfs_node *parent =
          (task_root && current == task_root) ? 0 : current->parent;
      if (parent) {
        vfs_node_get(parent);
        vfs_inode_unlock_read(current->inode);
        vfs_node_put(current);
        current = parent;
        vfs_inode_lock_read(current->inode);
        pop_path_part(parent_path);
      }
      continue;
    }

    /* POSIX: Check directory traversal permission (execute bit) */
    if (vfs_check_access(current, X_OK) != 0) {
      vfs_inode_unlock_read(current->inode);
      vfs_node_put(current);
      kfree(curr_path);
      kfree(parent_path);
      return ERR_PTR(-EACCES);
    }

    if (current->inode->type != VFS_DIRECTORY) {
      vfs_inode_unlock_read(current->inode);
      vfs_node_put(current);
      kfree(curr_path);
      kfree(parent_path);
      return ERR_PTR(-ENOTDIR);
    }

    /* A directory whose children go stale (see lookup_refresh) gets the
     * callback before the lookup, not only when it misses. */
    if (current->inode->lookup_cb && current->inode->lookup_refresh) {
      vfs_inode_unlock_read(current->inode);
      current->inode->lookup_cb(current, part);
      vfs_inode_lock_read(current->inode);
    }
    struct vfs_node *child = find_child(current, part);
    if (!child && current->inode->lookup_cb) {
      /* Synthetic dir (procfs/sysfs) with lazily-materialised children: give it
       * a chance to create `part` on demand, then retry. This is what lets a
       * DIRECT lookup of e.g. /proc/self/fd/N succeed without a prior readdir
       * (musl ttyname()/readlink take exactly that path). Release the inode read
       * lock first — lookup_cb inserts a child under this inode's WRITE lock, and
       * a read→write upgrade on the same inode would deadlock. `current` is
       * refcounted, so it stays valid across the brief unlock. */
      vfs_inode_unlock_read(current->inode);
      current->inode->lookup_cb(current, part);
      vfs_inode_lock_read(current->inode);
      child = find_child(current, part);
    }
    if (!child) {
      vfs_inode_unlock_read(current->inode);
      vfs_node_put(current);
      kfree(curr_path);
      kfree(parent_path);
      return ERR_PTR(-ENOENT);
    }

    /* find_child() already returns with refcount incremented */
    /* DOWNWARD MOUNT CROSSING */
    for (int i = 0; i < (int)mount_slots; i++) {
      if (mount_visible(i) && child == mounts[i].mount_point) {
        struct vfs_node *root = vfs_node_get(mounts[i].root_node);
        vfs_node_put(child);
        child = root;
        break;
      }
    }

    vfs_inode_lock_read(child->inode);
    if (child->inode && child->inode->ino && child->inode->fs_id) {
      struct vfs_inode *cached =
          icache_get(child->inode->fs_id, child->inode->ino);
      if (!cached) {
        icache_insert(child->inode->fs_id, child->inode->ino, child->inode);
      }
    }
    vfs_inode_unlock_read(current->inode);
    vfs_node_put(current);
    current = child;

    int is_final = (!rest || rest[0] == '\0');
    /* A magic link (/proc/<pid>/fd/<n>) names an open file, not a path: step
     * straight to the node the descriptor holds. systemd mounts every API
     * filesystem and every unit sandbox entry by opening the destination
     * O_PATH and passing /proc/self/fd/<n> to mount(2) -- exactly so that the
     * destination cannot be re-resolved -- and re-walking the stored target
     * string is a different operation with a different answer. */
    if (current->inode->type == VFS_SYMLINK && current->inode->magic_link_cb &&
        (follow_final || !is_final)) {
      if (++symlink_depth > VFS_MAX_SYMLINK_DEPTH) {
        vfs_inode_unlock_read(current->inode);
        vfs_node_put(current);
        kfree(curr_path);
        kfree(parent_path);
        return ERR_PTR(-ELOOP);
      }
      struct vfs_node *(*mcb)(struct vfs_node *) = current->inode->magic_link_cb;
      vfs_inode_unlock_read(current->inode);
      struct vfs_node *tgtn = mcb(current);
      if (IS_ERR(tgtn)) {
        vfs_node_put(current);
        kfree(curr_path);
        kfree(parent_path);
        return tgtn;
      }
      if (tgtn) {
        vfs_node_put(current);
        current = tgtn; /* referenced by the callback */
        vfs_inode_lock_read(current->inode);
        /* Anything relative resolved from here on hangs off the node's own
         * place in the tree, not off /proc/<pid>/fd. */
        if (vfs_get_node_path(current, parent_path, VFS_MAX_PATH) != 0) {
          parent_path[0] = '/';
          parent_path[1] = '\0';
        }
        continue; /* the empty-part branch returns it when this was final */
      }
      /* NULL: not a magic link after all -- fall through to the stored
       * target string, which is what a pipe or socket descriptor uses. */
      vfs_inode_lock_read(current->inode);
    }
    if (current->inode->type == VFS_SYMLINK && (follow_final || !is_final)) {
      if (++symlink_depth > VFS_MAX_SYMLINK_DEPTH) {
        vfs_inode_unlock_read(current->inode);
        vfs_node_put(current);
        kfree(curr_path);
        kfree(parent_path);
        return ERR_PTR(-ELOOP);
      }

      char *target_path = kmalloc(VFS_MAX_PATH);
      if (!target_path) {
        vfs_inode_unlock_read(current->inode);
        vfs_node_put(current);
        kfree(curr_path);
        kfree(parent_path);
        return ERR_PTR(-ENOMEM);
      }

      /* Read symlink target */
      isize target_len = 0;
      if (current->inode->read_cb) {
        target_len = current->inode->read_cb(current, 0, target_path,
                                             VFS_MAX_PATH - 1, 0);
      } else if (current->inode->data) {
        target_len = (isize)current->inode->size;
        if (target_len > VFS_MAX_PATH - 1)
          target_len = VFS_MAX_PATH - 1;
        memcpy(target_path, current->inode->data, (usize)target_len);
      } else {
        kfree(target_path);
        vfs_inode_unlock_read(current->inode);
        vfs_node_put(current);
        kfree(curr_path);
        kfree(parent_path);
        return ERR_PTR(-EINVAL);
      }

      if (target_len < 0) {
        kfree(target_path);
        vfs_inode_unlock_read(current->inode);
        vfs_node_put(current);
        kfree(curr_path);
        kfree(parent_path);
        return ERR_PTR((int)target_len);
      }
      target_path[target_len] = '\0';

      /* Path Injection: [target] + [rest] */
      char *new_path = kmalloc(VFS_MAX_PATH);
      if (!new_path) {
        kfree(target_path);
        vfs_inode_unlock_read(current->inode);
        vfs_node_put(current);
        kfree(curr_path);
        kfree(parent_path);
        return ERR_PTR(-ENOMEM);
      }

      if (target_path[0] == '/') {
        strncpy(new_path, target_path, VFS_MAX_PATH - 1);
      } else {
        strncpy(new_path, parent_path, VFS_MAX_PATH - 1);
        append_path_part(new_path, VFS_MAX_PATH, target_path);
      }

      if (!is_final) {
        append_path_part(new_path, VFS_MAX_PATH, rest);
      }
      new_path[VFS_MAX_PATH - 1] = '\0';

      kfree(target_path);
      kfree(curr_path);
      curr_path = new_path;
      rest = curr_path;

      /* Restart traversal from root because new_path is absolute */
      vfs_inode_unlock_read(current->inode);
      vfs_node_put(current);
      vfs_node_get(root_node);
      current = root_node;
      current = vfs_cross_root_mount(current);
      vfs_inode_lock_read(current->inode);
      parent_path[0] = '/';
      parent_path[1] = '\0';

      goto restart_traversal;
    }

    if (!is_final)
      append_path_part(parent_path, VFS_MAX_PATH, part);
  }

  /* Unreachable */
  vfs_inode_unlock_read(current->inode);
  vfs_node_put(current);
  kfree(curr_path);
  kfree(parent_path);
  return ERR_PTR(-EIO);
}

struct vfs_node *vfs_find_node(const char *path) {
  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(path, resolved);
  return vfs_find_node_internal(resolved, 1, 0);
}

static struct vfs_node *vfs_find_node_no_follow(const char *path) {
  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(path, resolved);
  return vfs_find_node_internal(resolved, 0, 0);
}

static struct vfs_node *add_node(const char *path, enum vfs_node_type type,
                                 void *data, usize size, u32 flags) {
  if (!root_node) {
    root_node = alloc_node();
    if (!root_node)
      return ERR_PTR(-ENOMEM);
    root_node->inode = alloc_inode();
    if (!root_node->inode)
      return ERR_PTR(-ENOMEM);

    root_node->name[0] = '/';
    root_node->name[1] = '\0';
    root_node->inode->type = VFS_DIRECTORY;
    root_node->inode->mode = 0755;
    vfs_init_times(root_node->inode);
  }

  char part[VFS_NAME_MAX];
  const char *rest = path;
  struct vfs_node *current = root_node;
  vfs_node_get(current);
  current = vfs_cross_root_mount(current);
  /* Note: vfs_cross_root_mount already manages refcount correctly.
   * If it redirected, it got the new root (+1) and put the old one (-1).
   * current now holds one reference valid throughout the loop below. */

  while (1) {
    if (split_path(rest, part, sizeof(part), &rest)) {
      vfs_node_put(current);
      return ERR_PTR(-ENAMETOOLONG);
    }
    if (part[0] == '\0') {
      return current;
    }


    if (current->inode && current->inode->lookup_cb &&
        current->inode->lookup_refresh)
      current->inode->lookup_cb(current, part);
    struct vfs_node *child = find_child(current, part);
    if (!child && current->inode && current->inode->lookup_cb) {
      current->inode->lookup_cb(current, part);
      child = find_child(current, part);
    }
    int child_was_found = (child != NULL);
    if (child) {
      for (int i = 0; i < (int)mount_slots; i++) {
        if (mount_visible(i) && child == mounts[i].mount_point) {
          vfs_node_put(child); /* Drop ref from find_child */
          child = vfs_node_get(mounts[i].root_node);
          break;
        }
      }
    }
    int is_leaf =
        (!rest || rest[0] == '\0' || (rest[0] == '/' && rest[1] == '\0'));

    if (is_leaf) {
      if (!child) {
        child = alloc_node();
        if (!child) {
          vfs_node_put(current);
          return ERR_PTR(-ENOMEM);
        }
        child->inode = alloc_inode();
        if (!child->inode) {
          vfs_free_node(child);
          __atomic_sub_fetch(&node_count, 1, __ATOMIC_RELAXED);
          vfs_node_put(current);
          return ERR_PTR(-ENOMEM);
        }

        copy_path(child->name, 64, part);
        child->inode->type = type;
        child->inode->data = data;
        child->inode->size = size;
        child->inode->flags = flags;
        child->inode->fs_id = current->inode->fs_id;
        child->parent = current;
        /* Inserting into current->first_child must exclude concurrent readers
         * (vfs_list/find_child) — without the write lock a timer-tick preempt
         * mid-iteration sees a half-linked sibling chain and crashes on a
         * dangling next_sibling. The per-inode write lock guards against other
         * writers of this parent; the vfs_tree_lock write lock (M28-B) is
         * what readers in find_child / vfs_get_mount_for_node observe so they
         * see a consistent next_sibling snapshot. */
        vfs_inode_lock_write(current->inode);
        u64 _tlflags;
        vfs_tree_write_acquire(&_tlflags);
        child->next_sibling = current->first_child;
        current->first_child = child;
        vfs_tree_write_release(_tlflags);
        vfs_inode_unlock_write(current->inode);

        const struct cred *cred = get_current_cred();
        child->inode->uid = cred ? cred->euid : ROOT_UID;
        child->inode->gid = cred ? cred->egid : ROOT_GID;

        u16 umask = scheduler_get_current_umask();
        if (type == VFS_DIRECTORY)
          child->inode->mode = 0777 & ~umask;
        else
          child->inode->mode = 0666 & ~umask;

        if (flags & INITRAMFS_EXECUTABLE)
          child->inode->mode |= VFS_IXUSR | VFS_IXGRP | VFS_IXOTH;
        /* M31: stamp setuid on initramfs files marked SETUID. The file
         * owner is uid 0, so user_execve_current's S_ISUID branch will
         * elevate the new task's euid to root. */
        if (flags & INITRAMFS_SETUID)
          child->inode->mode |= 04000;

        vfs_init_times(child->inode);
      } else if (data != 0 || size != 0 || flags != 0) {
        child->inode->type = type;
        if (data != 0)
          child->inode->data = data;
        if (size != 0)
          child->inode->size = size;
        child->inode->flags = flags;
        vfs_update_times(child->inode, VFS_MTIME | VFS_CTIME);
      }
      struct vfs_node *ret = vfs_node_get(child);
      vfs_node_put(current);
      return ret;

    } else {
      if (!child) {
        child = alloc_node();
        if (!child) {
          vfs_node_put(current);
          return ERR_PTR(-ENOMEM);
        }
        child->inode = alloc_inode();
        if (!child->inode) {
          vfs_free_node(child);
          __atomic_sub_fetch(&node_count, 1, __ATOMIC_RELAXED);
          vfs_node_put(current);
          return ERR_PTR(-ENOMEM);
        }

        copy_path(child->name, 64, part);
        child->inode->type = VFS_DIRECTORY;
        child->inode->fs_id = current->inode->fs_id;

        const struct cred *cred = get_current_cred();
        child->inode->uid = cred ? cred->euid : ROOT_UID;
        child->inode->gid = cred ? cred->egid : ROOT_GID;

        u16 umask = scheduler_get_current_umask();
        child->inode->mode = 0777 & ~umask;
        vfs_init_times(child->inode);

        child->parent = current;
        vfs_inode_lock_write(current->inode);
        u64 _tlflags;
        vfs_tree_write_acquire(&_tlflags);
        child->next_sibling = current->first_child;
        current->first_child = child;
        vfs_tree_write_release(_tlflags);
        vfs_inode_unlock_write(current->inode);
      }
      if (child_was_found) {
        vfs_node_put(current);
        current = child;
      } else {
        vfs_node_get(child);
        vfs_node_put(current);
        current = child;
      }
    }
  }
}

/* Monotonic source for vfs_node::dir_seq. Wrapping would take 2^64 node
 * creations; the counter is deliberately global rather than per-directory so a
 * node keeps its cursor position if it is ever moved between directories. */
static u64 g_dir_seq_next = 1;

static u64 dir_seq_next(void) {
  return __atomic_fetch_add(&g_dir_seq_next, 1, __ATOMIC_RELAXED);
}

struct vfs_node *vfs_create_node(enum vfs_node_type type) {
  struct vfs_node *n = alloc_node(); /* dir_seq assigned there */
  if (!n)
    return NULL;
  n->inode = alloc_inode();
  if (!n->inode) {
    n->refcount = 0;
    return NULL;
  }
  n->inode->type = type;
  n->inode->refcount = 1;
  n->inode->nlink = 1;
  n->refcount = 1;
  return n;
}

/* st_dev of the filesystem this node belongs to. A filesystem that builds its
 * whole tree inside its mount callback does so before vfs_mount can stamp the
 * root (the root node does not exist until the callback returns), so nodes
 * created then carry 0. Resolve it by walking up to the first ancestor that has
 * one — the mount root — and cache it on the way. Without this a file on such a
 * filesystem reports st_dev 0, and /proc/<pid>/maps calls its mapping
 * anonymous. */
u32 vfs_node_dev(struct vfs_node *node) {
  if (!node || !node->inode)
    return 0;
  if (node->inode->dev)
    return node->inode->dev;
  struct vfs_node *p = node->parent;
  for (int depth = 0; p && depth < 64; depth++) {
    if (p->inode && p->inode->dev) {
      node->inode->dev = p->inode->dev;
      return node->inode->dev;
    }
    p = p->parent;
  }
  return 0;
}

void vfs_attach_child(struct vfs_node *parent, struct vfs_node *child) {
  if (!parent || !child)
    return;
  /* st_dev is a property of the filesystem, not of the individual file, so a
   * node inherits it from the directory it is attached to. Doing it here covers
   * every filesystem at once: each one used to have to remember, and the ones
   * that forgot handed out files with st_dev 0 — which a reader of
   * /proc/<pid>/maps treats as "anonymous memory, no backing file". A node that
   * already carries its own id (devpts) keeps it. */
  if (child->inode && parent->inode && !child->inode->dev)
    child->inode->dev = parent->inode->dev;
  u64 flags;
  vfs_tree_write_acquire(&flags);
  /* Appended, not prepended.
   *
   * readdir walks children by position and resumes from an offset, so a name
   * inserted at the head shifts everything the caller has not read yet — and
   * /proc adds a directory per live task on every listing. `ls /proc` returned
   * seven entries on a system running dozens of processes, and the browser's
   * were among the ones that vanished. Appending keeps the positions of the
   * entries already handed out. */
  child->next_sibling = 0;
  if (!parent->first_child) {
    parent->first_child = child;
  } else {
    struct vfs_node *tail = parent->first_child;

    while (tail->next_sibling)
      tail = tail->next_sibling;
    tail->next_sibling = child;
  }
  vfs_tree_write_release(flags);
}

/* The type getdents reports for a child. Same rule as stat: a /proc or /sys
 * pseudo-file is a regular file to userspace even though the VFS serves it
 * through a device-style read callback. */
static u32 vfs_dirent_type(const struct vfs_inode *inode) {
  if (!inode)
    return (u32)VFS_FILE;
  if (inode->type != VFS_DIRECTORY && (inode->flags & VFS_NODE_PSEUDO_REG))
    return (u32)VFS_FILE;
  return (u32)inode->type;
}

void vfs_detach_child(struct vfs_node *parent, struct vfs_node *child) {
  if (!parent || !child)
    return;
  u64 flags;
  vfs_tree_write_acquire(&flags);
  struct vfs_node **pp = &parent->first_child;
  while (*pp) {
    if (*pp == child) {
      *pp = child->next_sibling;
      child->next_sibling = 0;
      break;
    }
    pp = &(*pp)->next_sibling;
  }
  vfs_tree_write_release(flags);
  /* A cached name→node entry would still resolve after the unlink. */
  dcache_invalidate(parent, child->name);
}

isize vfs_readdir_children(struct vfs_node *dir, usize offset,
                           struct dirent *buf, usize max_entries) {
  if (!dir || !buf)
    return -EINVAL;

  usize start = offset, idx = 0, count = 0;
  if (idx >= start && count < max_entries) {
    copy_path(buf[count].name, 64, ".");
    buf[count].type = (u32)VFS_DIRECTORY;
    buf[count].is_dir = 1;
    buf[count].is_exec = 1;
    buf[count].size = 0;
    buf[count].ino = dir->inode ? dir->inode->ino : 0;
    count++;
  }
  idx++;
  if (idx >= start && count < max_entries) {
    copy_path(buf[count].name, 64, "..");
    buf[count].type = (u32)VFS_DIRECTORY;
    buf[count].is_dir = 1;
    buf[count].is_exec = 1;
    buf[count].size = 0;
    buf[count].ino = (dir->parent && dir->parent->inode) ? dir->parent->inode->ino
                     : (dir->inode ? dir->inode->ino : 0);
    count++;
  }
  idx++;

  u64 flags;
  vfs_tree_read_acquire(&flags);
  for (struct vfs_node *child = dir->first_child;
       child && count < max_entries; child = child->next_sibling) {
    if (child->deleted)
      continue;
    if (idx >= start) {
      copy_path(buf[count].name, 64, child->name);
      buf[count].type = vfs_dirent_type(child->inode);
      buf[count].is_dir = (child->inode->type == VFS_DIRECTORY);
      buf[count].is_exec = 0;
      buf[count].size = child->inode->size;
      buf[count].ino = child->inode->ino;
      count++;
    }
    idx++;
  }
  vfs_tree_read_release(flags);
  return (isize)count;
}

struct vfs_node *vfs_add_node(const char *path, enum vfs_node_type type,

                              void *data, usize size, u32 flags) {
  return add_node(path, type, data, size, flags);
}

struct vfs_handle *alloc_raw_handle(enum vfs_handle_kind kind) {
  struct vfs_handle *h = vfs_alloc_handle();
  if (!h)
    return 0;
  h->used = 1;
  h->refcount = 1;
  h->kind = kind;
  h->ns_pin = 0; /* M109: only a /proc/<pid>/ns/<kind> open sets this */
  h->open_path = 0;
  return h;
}

void vfs_handle_retain(struct vfs_handle *h) {
  if (!h || h->used != 1 || h->refcount <= 0)
    return;
  /* SMP-safe: fork retains every shared fd's handle (scheduler.c) on the
   * forking CPU while the original holder may run on another. A non-atomic
   * increment would lose updates, breaking the refcount and ultimately
   * double-freeing the underlying socket/pipe state in ->release. */
  __atomic_add_fetch(&h->refcount, 1, __ATOMIC_RELAXED);
}

static struct vfs_handle *get_handle(int fd) {
  return scheduler_fd_get(fd);
}

static void copy_path(char *dst, usize dst_size, const char *src) {
  if (!dst || dst_size == 0)
    return;
  if (!src)
    src = "";
  usize len = strlen(src);
  if (len >= dst_size)
    len = dst_size - 1;
  memcpy(dst, src, len);
  dst[len] = '\0';
}

void vfs_handle_release(struct vfs_handle *h) {
  if (!h || h->used != 1)
    return;
  int new_ref = __atomic_sub_fetch(&h->refcount, 1, __ATOMIC_ACQ_REL);
  if (new_ref < 0) {
    panic("vfs: handle refcount underflow");
  }
  if (new_ref > 0)
    return;


  h->used = 0;
  if (h->ops && h->ops->release) {
    h->ops->release(h);
  } else if (h->kind == VFS_HANDLE_NODE && h->node) {
    vfs_node_put(h->node);
  }

  if (h->open_path) {
    kfree(h->open_path);
    h->open_path = 0;
  }
  vfs_free_handle(h);
}

static char tty_getc_blocking(void) {
#ifdef __aarch64__
  return 0;
#else
  char c = 0;
  while (c == 0) {
    c = ps2_kbd_getc();
    /* COM1 input belongs to /dev/ttyS0 while that device is open (M39 serial
     * getty); the merged boot console only reads it when unclaimed. */
    if (c == 0 && !serial_tty_claimed(0))
      c = serial_getc();
    if (c == 0)
      scheduler_yield();
  }
  return c;
#endif
}

static isize tty_read(struct vfs_node *node, u64 offset, char *buffer,
                      usize size, int flags) {
  (void)node;
  (void)offset;
  /* Job control check for background reads: */
  if (current_task && console.fg_pgrp > 0) {
    if (current_task->process_group_id != console.fg_pgrp) {
      if ((current_task->blocked_signals & (1ULL << (SIGTTIN - 1))) ||
          current_task->sigactions[SIGTTIN - 1].sa_handler == SIG_IGN) {
        return -EIO;
      } else {
        if (current_task->parent_id == 0 || current_task->parent_id == 1) {
          return -EIO;
        }
        // Process is in the background trying to read from TTY
        scheduler_kill_process_group(current_task->process_group_id, SIGTTIN);
        return -ERESTARTSYS; // Abort read, let scheduler block the task, retry on SIGCONT
      }
    }
  }

  if ((console.termios.c_lflag & B1NIX_ICANON) == 0) {
    usize minimum = console.termios.c_cc[B1NIX_VMIN];
    if (minimum > size)
      minimum = size;
    usize n = 0;
    while (n < size) {
      char c;
      if (n < minimum) {
        c = tty_getc_blocking();
      } else {
        c = ps2_kbd_getc();
        if (c == 0 && !serial_tty_claimed(0))
          c = serial_getc();
        if (c == 0)
          break;
      }
      buffer[n++] = c;
    }
    return (isize)n;
  }

  while (tty_line_pos >= tty_line_len) {
    tty_line_pos = 0;
    tty_line_len = 0;

    while (tty_line_len < sizeof(tty_line) - 1) {
      char c = 0;
      if (flags & B1NIX_O_NONBLOCK) {
        c = ps2_kbd_getc();
        if (c == 0 && !serial_tty_claimed(0))
          c = serial_getc();
        if (c == 0) {
          if (tty_line_len > 0)
            break;
          return -EAGAIN;
        }
      } else {
        c = tty_getc_blocking();
      }
      if (c == 0)
        return 0;
      if (c == 27) {
        char next = tty_getc_blocking();
        if (next == '[') {
          char final = tty_getc_blocking();
          int parameter = 0;
          while (final >= '0' && final <= '9') {
            parameter = parameter * 10 + (final - '0');
            final = tty_getc_blocking();
          }
          if (final == '~' && parameter == 3 && tty_line_len > 0) {
            /*
             * The canonical console does not track a movable cursor yet, so
             * make Delete useful at the prompt by erasing the previous byte.
             * This also handles NumPad '.' with NumLock disabled (CSI 3~).
             */
            tty_line_len--;
            if (console.termios.c_lflag & B1NIX_ECHO)
              console_write("\b \b");
          }
          continue;
        }
        continue;
      }
      if (c == 4)
        break;
      if (c == '\b' || c == 127) {
        if (tty_line_len > 0) {
          tty_line_len--;
          if (console.termios.c_lflag & B1NIX_ECHO)
            console_write("\b \b");
        }
        continue;
      }
      tty_line[tty_line_len++] = c;
      if (console.termios.c_lflag & B1NIX_ECHO)
        console_putc(c);
      if (c == '\n')
        break;
    }
  }

  usize copied = 0;
  while (copied < size && tty_line_pos < tty_line_len)
    buffer[copied++] = tty_line[tty_line_pos++];
  return (isize)copied;
}

static isize tty_write(struct vfs_node *node, u64 offset, const char *buffer,
                       usize size, int flags) {
  (void)flags;
  (void)node;
  (void)offset;
  if (!buffer)
    return -1;

  /* Job control check for background writes: */
  if (current_task && console.fg_pgrp > 0) {
    if (current_task->process_group_id != console.fg_pgrp) {
      if (console.termios.c_lflag & B1NIX_TOSTOP) {
        if ((current_task->blocked_signals & (1ULL << (SIGTTOU - 1))) ||
            current_task->sigactions[SIGTTOU - 1].sa_handler == SIG_IGN) {
          /* POSIX: if blocked/ignored, let the write execute */
        } else {
          if (current_task->parent_id == 0 || current_task->parent_id == 1) {
            return -EIO; /* Orphaned process group, fail immediately */
          }
          scheduler_kill_process_group(current_task->process_group_id, SIGTTOU);
          return -ERESTARTSYS;
        }
      }
    }
  }

  /* Hold console_lock across the whole buffer: console_putc() is the
   * unlocked per-char primitive normally only called from inside
   * console_write()'s own locked loop. Without this, a concurrent
   * console_write() (kernel log) or another tty's write() could land a byte
   * mid-buffer here and vice versa — this is the boot-era merged VGA+serial
   * console (/dev/tty, /dev/console), a separate write path from
   * serial_tty.c's per-instance ttys, and had the same unlocked-UART bug. */
  u64 lock_flags;
  console_lock_acquire_irqsave(&lock_flags);
  /* Raw: a write to /dev/console or /dev/tty is terminal output, not a kernel
   * log record, so it gets no timestamp and no severity prefix. */
  for (usize i = 0; i < size; i++) {
    if ((console.termios.c_oflag & B1NIX_OPOST) && buffer[i] == '\n')
      console_putc_raw('\r');
    console_putc_raw(buffer[i]);
  }
  console_lock_release_irqrestore(lock_flags);
  return (isize)size;
}

/* /dev/null: reads return EOF, writes are discarded, always ready to poll. */
static isize null_read(struct vfs_node *node, u64 offset, char *buffer,
                       usize size, int flags) {
  (void)node;
  (void)offset;
  (void)buffer;
  (void)size;
  (void)flags;
  return 0;
}

static isize null_write(struct vfs_node *node, u64 offset, const char *buffer,
                        usize size, int flags) {
  (void)node;
  (void)offset;
  (void)buffer;
  (void)flags;
  return (isize)size;
}

static int null_poll(struct vfs_node *node, struct b1nix_pollfd *pfd) {
  (void)node;
  pfd->revents = B1NIX_POLLIN | B1NIX_POLLOUT;
  return 0;
}

/* /dev/log: kernel syslog sink. Datagrams written here (by libc syslog()) are
 * forwarded straight to the serial console — so logs from any port land in the
 * kernel log with no userspace syslogd to run.
 * ponytail: a char-device sink, not an AF_UNIX endpoint. If a real syslogd is
 * ever added, give it the AF_UNIX SOCK_DGRAM /dev/log path back. */
static isize log_write(struct vfs_node *node, u64 offset, const char *buffer,
                       usize size, int flags) {
  (void)node;
  (void)offset;
  (void)flags;
  if (!buffer)
    return -1;
  char line[512];
  usize n = size < sizeof(line) - 1 ? size : sizeof(line) - 1;
  memcpy(line, buffer, n);
  while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
    n--;
  line[n] = '\0';
  char out[sizeof(line) + 16];
  snprintf(out, sizeof(out), "/dev/log: %s\n", line);
  console_write(out);
  return (isize)size;
}

/* /dev/urandom and /dev/random: the same CSPRNG getrandom(2) draws from. Both
 * nodes exist because callers ask for one or the other by name — BusyBox shred
 * opens /dev/urandom and fails outright without it — and neither ever blocks:
 * b1nix has one pool, seeded at boot, so a "wait for entropy" distinction would
 * be a fiction. Writing to them is accepted and mixes nothing, as on Linux
 * without CAP_SYS_ADMIN. */
static isize random_read(struct vfs_node *node, u64 offset, char *buffer,
                         usize size, int flags) {
  (void)node;
  (void)offset;
  (void)flags;
  if (!buffer)
    return -EFAULT;
  usize done = 0;
  while (done < size) {
    u64 r = kernel_random_u64();
    usize left = size - done;
    usize n = left < sizeof(r) ? left : sizeof(r);
    memcpy(buffer + done, &r, n);
    done += n;
  }
  return (isize)done;
}

static isize random_write(struct vfs_node *node, u64 offset, const char *buffer,
                          usize size, int flags) {
  (void)node;
  (void)offset;
  (void)buffer;
  (void)flags;
  return (isize)size; /* accepted, mixed into nothing */
}

/* /dev/zero: an endless run of zero bytes, and a sink for writes. Its absence
 * was not visible until something asked for it by name — BusyBox shred opens
 * /dev/zero and /dev/urandom side by side and dies on the first that is
 * missing. */
static isize zero_read(struct vfs_node *node, u64 offset, char *buffer,
                       usize size, int flags) {
  (void)node;
  (void)offset;
  (void)flags;
  if (!buffer)
    return -EFAULT;
  memset(buffer, 0, size);
  return (isize)size;
}

static void zero_init_node(void) {
  struct vfs_node *n = add_node("/dev/zero", VFS_DEVICE, 0, 0, 0);
  if (!n)
    return;
  n->inode->read_cb = zero_read;
  n->inode->write_cb = null_write; /* discarded, like /dev/null */
  n->inode->poll_cb = null_poll;
  n->inode->mode =
      VFS_IRUSR | VFS_IWUSR | VFS_IRGRP | VFS_IWGRP | VFS_IROTH | VFS_IWOTH;
}

static void random_init_nodes(void) {
  static const char *names[2] = {"/dev/urandom", "/dev/random"};
  for (int i = 0; i < 2; i++) {
    struct vfs_node *n = add_node(names[i], VFS_DEVICE, 0, 0, 0);
    if (!n)
      continue;
    n->inode->read_cb = random_read;
    n->inode->write_cb = random_write;
    n->inode->poll_cb = null_poll; /* always readable and writable */
    n->inode->mode =
        VFS_IRUSR | VFS_IWUSR | VFS_IRGRP | VFS_IWGRP | VFS_IROTH | VFS_IWOTH;
  }
}

static void null_init_node(void) {
  struct vfs_node *n = add_node("/dev/null", VFS_DEVICE, 0, 0, 0);
  if (n) {
    n->inode->read_cb = null_read;
    n->inode->write_cb = null_write;
    n->inode->poll_cb = null_poll;
    n->inode->mode =
        VFS_IRUSR | VFS_IWUSR | VFS_IRGRP | VFS_IWGRP | VFS_IROTH | VFS_IWOTH;
  }
  random_init_nodes();
  zero_init_node();
  struct vfs_node *l = add_node("/dev/log", VFS_DEVICE, 0, 0, 0);
  if (l) {
    l->inode->write_cb = log_write;
    l->inode->poll_cb = null_poll;
    l->inode->mode =
        VFS_IRUSR | VFS_IWUSR | VFS_IRGRP | VFS_IWGRP | VFS_IROTH | VFS_IWOTH;
  }
}

static int tty_poll(struct vfs_node *node, struct b1nix_pollfd *pfd) {
  (void)node;
  short revents = B1NIX_POLLOUT; /* the console is always writable */
#ifndef __aarch64__
  if (ps2_kbd_has_data() ||
      (!serial_tty_claimed(0) && serial_has_data()))
    revents |= B1NIX_POLLIN;
#endif
  pfd->revents = revents;
  return 0;
}

static void tty_init_node(void) {
  memset(&console.termios, 0, sizeof(console.termios));
  console.termios.c_lflag = B1NIX_ICANON | B1NIX_ECHO | B1NIX_ISIG;
  console.termios.c_oflag = B1NIX_OPOST;
  console.fg_pgrp = 1; /* Boot group */
  console.session_id = 1; /* Boot session */
  struct vfs_node *tty = add_node("/dev/tty", VFS_DEVICE, 0, 0, 0);
  if (tty) {
    tty->inode->read_cb = tty_read;
    tty->inode->write_cb = tty_write;
    tty->inode->poll_cb = tty_poll;
    tty->inode->mode =
        VFS_IRUSR | VFS_IWUSR | VFS_IRGRP | VFS_IWGRP | VFS_IROTH | VFS_IWOTH;
  }
}

/* Forward declarations for internal VFS metadata operations (thread-unsafe
 * variants) */
static int vfs_rename_internal(const char *old_path, const char *new_path);

/* A private, stable st_dev for /dev/pts, plus st_ino == pty index + 1. musl's
 * ttyname_r() compares stat("/dev/pts/N").{dev,ino} against fstat(slave).{dev,ino}
 * — so both MUST agree. We anchor them on the pts INDEX, not on two independent
 * inode objects, so the /dev/pts/N node (materialised on demand) and the open
 * slave handle report the identical pair. */
#define DEVPTS_FSID 0x70747300u /* "pts\0" — unique, never a real mount fs_id */
static inline u64 devpts_ino(int idx) { return (u64)idx + 1; }

/* On-demand /dev/pts/<N>: /dev/pts opens are string-intercepted in
 * vfs_open_flags, so the slave nodes never physically exist — which means a
 * plain stat()/readlink of /dev/pts/<N> misses. musl's ttyname() readlinks
 * /proc/self/fd/<fd> to "/dev/pts/<N>" and then stat()s it, so that path must
 * resolve. Materialise a char-device node for any live pty slot when the
 * resolver asks; stale nodes for closed ptys are harmless (they just re-open
 * via the interception). */
static int devpts_lookup(struct vfs_node *dir, const char *name) {
  if (!name[0])
    return -1;
  int idx = 0;
  for (const char *q = name; *q; q++) {
    if (*q < '0' || *q > '9')
      return -1;
    idx = idx * 10 + (*q - '0');
  }
  if (!pty_allocated(idx))
    return -1;
  struct vfs_node *existing = find_child(dir, name);
  if (existing) {
    vfs_node_put(existing);
    return 0;
  }
  struct vfs_node *n = vfs_create_node(VFS_DEVICE);
  if (!n)
    return -1;
  usize nl = strlen(name);
  if (nl > 63)
    nl = 63;
  memcpy(n->name, name, nl);
  n->name[nl] = '\0';
  n->inode->type = VFS_DEVICE;
  n->inode->mode = 0620;
  n->inode->nlink = 1;
  n->inode->uid = ROOT_UID;
  n->inode->gid = 5; /* tty */
  n->inode->fs_id = DEVPTS_FSID;
  n->inode->dev = DEVPTS_FSID; /* stat() and ttyname() must agree on st_dev */
  n->inode->ino = devpts_ino(idx);
  n->inode->rdev = ((u64)136 << 8) | (u64)idx;
  n->parent = dir;
  n->refcount++;
  vfs_attach_child(dir, n);
  return 0;
}

static void vfs_init_stdio(void) {
  scheduler_fd_table_init_current();
  int tty = vfs_open_flags("/dev/tty", B1NIX_O_RDWR);
  if (tty < 0)
    return;
  vfs_dup2(tty, 0);
  vfs_dup2(tty, 1);
  vfs_dup2(tty, 2);
}

void vfs_init(void) {
  dcache_init_pool();
  icache_init();
  node_count = 0;
  mounts_init_table(); /* kzalloc'd, so already zeroed */

  root_node = alloc_node();
  root_node->inode = alloc_inode();
  strcpy(root_node->name, "/");
  root_node->inode->type = VFS_DIRECTORY;
  root_node->inode->mode = 0755;
  vfs_init_times(root_node->inode);
  root_node->inode->fs_id = 1;
  root_node->inode->dev = 1; /* anonymous device for the initial RAM root */
  next_fs_id = 2;

  add_node("/dev", VFS_DIRECTORY, 0, 0, 0);
  add_node("/home", VFS_DIRECTORY, 0, 0, 0);
  add_node("/tmp", VFS_DIRECTORY, 0, 0, 0);
  add_node("/dev/shm", VFS_DIRECTORY, 0, 0, 0);
  /* /run is the volatile runtime directory an init system expects (tmpfs on
   * Linux). It is a plain in-memory VFS directory here, which is exactly what
   * FIFOs need — /run/openrc/init.ctl and friends live in RAM and vanish on
   * reboot, as they should. */
  add_node("/run", VFS_DIRECTORY, 0, 0, 0);
  add_node("/var", VFS_DIRECTORY, 0, 0, 0);
  add_node("/mnt", VFS_DIRECTORY, 0, 0, 0);
  add_node("/proc", VFS_DIRECTORY, 0, 0, 0);
  add_node("/mnt/ext1", VFS_DIRECTORY, 0, 0, 0);
  add_node("/mnt/ext2", VFS_DIRECTORY, 0, 0, 0);
  add_node("/mnt/ext3", VFS_DIRECTORY, 0, 0, 0);
  add_node("/mnt/ext4", VFS_DIRECTORY, 0, 0, 0);
  add_node("/mnt/ext4nvme", VFS_DIRECTORY, 0, 0, 0);
  add_node("/mnt/iso", VFS_DIRECTORY, 0, 0, 0);
  add_node("/mnt/root", VFS_DIRECTORY, 0, 0, 0);
  vfs_symlink("/", "/persist");

  /* /dev/console is a terminal, not an empty node. It used to be created with
   * no read/write callbacks at all, so everything written to it was silently
   * discarded — which is why Debian's sysvinit, whose whole output goes to
   * /dev/console, booted the machine without printing one line. It gets the
   * same callbacks as /dev/tty because on this kernel it IS the same device:
   * the boot console. */
  {
    struct vfs_node *con = add_node("/dev/console", VFS_DEVICE, 0, 0, 0);
    if (con && !IS_ERR(con)) {
      con->inode->read_cb = tty_read;
      con->inode->write_cb = tty_write;
      con->inode->poll_cb = tty_poll;
      con->inode->mode = 0620;
      con->inode->gid = 5; /* group tty */
    }
  }
  add_node("/dev/vda", VFS_DEVICE, 0, 0, 0);
  /* M32b pseudo-terminals: /dev/ptmx + the /dev/pts mountpoint directory. Both
   * opens are intercepted in vfs_open_flags; the nodes exist so stat()/ls and
   * ptsname() paths resolve. */
  pty_init();
  add_node("/dev/ptmx", VFS_DEVICE, 0, 0, 0);
  struct vfs_node *ptsdir = add_node("/dev/pts", VFS_DIRECTORY, 0, 0, 0);
  if (ptsdir)
    ptsdir->inode->lookup_cb = devpts_lookup;
  serial_tty_register_nodes();
  /* M107: the VT, kmsg, RTC and watchdog nodes live on the old root too. */
  vt_register_nodes();
  kmsg_register_nodes();
  rtc_dev_register_nodes();
  watchdog_register_nodes();
  i2c_register_nodes();
  vfs_create("/tmp/hello", 0644);
  vfs_mount("initramfs", "/", "initramfs", 0);
  tty_init_node();
  null_init_node();
  vfs_init_stdio();

#ifndef __aarch64__
  virtio_blk_init();
#else
  virtio_blk_mmio_init();
  /* The SD card a Raspberry Pi 4 boots from. Returns immediately on a board
   * whose device tree describes no such controller, which is every other one
   * this port runs on. */
  bcm2711_emmc_init();
#endif

  for (usize i = 0; i < blk_count(); i++) {
    struct block_device *dev = blk_at(i);
    /* The registry can have holes since devices became removable. */
    if (!dev || !dev->name)
      continue;
    char dev_path[64];
    strcpy(dev_path, "/dev/");
    strcat(dev_path, dev->name);
    add_node(dev_path, VFS_DEVICE, 0, 0, 0);
  }

#ifndef __aarch64__
  struct block_device *blk = blk_get("vda");
  if (blk)
    fat32_mount(blk, "/mnt");
#endif

  console_write(
      "vfs: full featured initialized (POSIX+, Refcounting, Mount Crossing)\n");
}

extern void fb_dev_init(void);
extern void input_init(void);
extern void drm_dev_init(void);
extern void drm_card1_init(void);
extern void virtio_gpu_dev_init(void);
extern void sound_module_dev_init(void);
extern void ac97_dev_init(void);

/* Everything under /dev, built from what the drivers have already probed.
 *
 * Split out of vfs_repopulate_after_root_mount so that MOUNTING devtmpfs can
 * run it too. On Linux devtmpfs arrives already populated by the kernel; here
 * it used to be a plain alias for tmpfs, so systemd mounting devtmpfs on /dev
 * replaced every device node in the machine with an empty directory — PID 1
 * lost /dev/console mid-boot and printed nothing further, and no getty could
 * ever open /dev/ttyS0.
 *
 * The node paths are absolute because the drivers that create them say
 * "/dev/...", so this populates the filesystem mounted at /dev and nowhere
 * else. A devtmpfs mounted at another path is an empty directory, which is
 * the honest answer: none of these devices is reachable through it. */
void vfs_populate_dev(void) {
  struct vfs_node *node;

  node = add_node("/dev", VFS_DIRECTORY, 0, 0, 0);
  if (node && !IS_ERR(node)) vfs_node_put(node);

  node = add_node("/dev/console", VFS_DEVICE, 0, 0, 0);
  if (node && !IS_ERR(node)) {
    /* Same callbacks as the boot-time node: without them a write to
     * /dev/console in the real root goes nowhere. */
    node->inode->read_cb = tty_read;
    node->inode->write_cb = tty_write;
    node->inode->poll_cb = tty_poll;
    node->inode->mode = 0620;
    node->inode->uid = 0;
    node->inode->gid = 5; // group tty
    /* The number the Linux ABI fixes for this node. Left at 0, stat() reported
     * st_rdev == 0 -- a device file that is no device -- so anything that
     * identifies a device by its number rather than its path could not see it,
     * /sys/dev/char included. These numbers are not ours to choose. */
    node->inode->rdev = ((u64)5 << 8) | (u64)1; /* /dev/console */
    vfs_node_put(node);
  }

  node = add_node("/dev/ptmx", VFS_DEVICE, 0, 0, 0);
  if (node && !IS_ERR(node)) {
    node->inode->mode = 0666;
    node->inode->uid = 0;
    node->inode->gid = 5; // group tty
    node->inode->rdev = ((u64)5 << 8) | (u64)2; /* /dev/ptmx */
    vfs_node_put(node);
  }

  node = add_node("/dev/pts", VFS_DIRECTORY, 0, 0, 0);
  if (node && !IS_ERR(node)) {
    node->inode->mode = 0755;
    node->inode->uid = 0;
    node->inode->gid = 0;
    node->inode->lookup_cb = devpts_lookup;
    vfs_node_put(node);
  }

  node = add_node("/dev/null", VFS_DEVICE, 0, 0, 0);
  if (node && !IS_ERR(node)) {
    node->inode->read_cb = null_read;
    node->inode->write_cb = null_write;
    node->inode->poll_cb = null_poll;
    node->inode->mode = 0666;
    node->inode->uid = 0;
    node->inode->gid = 0;
    node->inode->rdev = ((u64)1 << 8) | (u64)3; /* /dev/null */
    vfs_node_put(node);
  }

  /* Same reason the nodes above are re-added: the ones made during early boot
   * live on the initramfs root and are unreachable once the real root is
   * mounted over "/". */
  node = add_node("/dev/zero", VFS_DEVICE, 0, 0, 0);
  if (node && !IS_ERR(node)) {
    node->inode->read_cb = zero_read;
    node->inode->write_cb = null_write;
    node->inode->poll_cb = null_poll;
    node->inode->mode = 0666;
    node->inode->uid = 0;
    node->inode->gid = 0;
    node->inode->rdev = ((u64)1 << 8) | (u64)5; /* /dev/zero */
    vfs_node_put(node);
  }

  for (int ri = 0; ri < 2; ri++) {
    node = add_node(ri ? "/dev/random" : "/dev/urandom", VFS_DEVICE, 0, 0, 0);
    if (node && !IS_ERR(node)) {
      node->inode->read_cb = random_read;
      node->inode->write_cb = random_write;
      node->inode->poll_cb = null_poll;
      node->inode->mode = 0666;
      node->inode->uid = 0;
      node->inode->gid = 0;
      vfs_node_put(node);
    }
  }

  /* /dev/fd and the three standard-stream links. bash implements process
   * substitution -- `cmd < <(other)` -- by handing the reader the path
   * /dev/fd/<n> for the pipe it just created. Without these links the open
   * fails with ENOENT, nobody ever reads that pipe, and the writing child
   * blocks in pipe_write() forever: a whole pipeline wedged (neofetch did
   * exactly this) with no error anywhere pointing at the missing node.
   * /proc/self/fd is the real directory behind all four, same as Linux. */
  vfs_unlink("/dev/fd");
  vfs_symlink("/proc/self/fd", "/dev/fd");
  vfs_unlink("/dev/stdin");
  vfs_symlink("/proc/self/fd/0", "/dev/stdin");
  vfs_unlink("/dev/stdout");
  vfs_symlink("/proc/self/fd/1", "/dev/stdout");
  vfs_unlink("/dev/stderr");
  vfs_symlink("/proc/self/fd/2", "/dev/stderr");

  node = add_node("/dev/log", VFS_DEVICE, 0, 0, 0);
  if (node && !IS_ERR(node)) {
    node->inode->write_cb = log_write;
    node->inode->poll_cb = null_poll;
    node->inode->mode = 0666;
    node->inode->uid = 0;
    node->inode->gid = 0;
    vfs_node_put(node);
  }

  tty_init_node();
  serial_tty_register_nodes();

  /* GPU/input device nodes registered during early boot (fb_init, virtio_gpu_init,
   * etc.) land on the initramfs root, which becomes unreachable once "/" redirects
   * to the mounted ext4 root above — same reason /dev/console, /dev/null, ttys
   * and the block-device nodes above all get re-added here. These four are pure
   * VFS-node (re)registration using state already probed at early boot; safe to
   * call again (virtio_gpu_dev_init guards its one-time ctx/buffer allocation). */
  fb_dev_init();
  input_init();
  drm_dev_init();
  drm_card1_init(); /* /dev/dri/card1.. — the imported core's devices */
  virtio_gpu_dev_init();

  /* Sound device nodes (/dev/dsp, /dev/dsp1) created by hda_init/ac97_init
   * land on the initramfs root and are re-registered here, like fb/input.
   *
   * HDA is a PCI device driven entirely through MMIO, so it belongs on every
   * arch that has a PCI bus — which this port does now. AC'97 stays x86: its
   * register file is reached through I/O ports, which do not exist here. */
  sound_module_dev_init();
#if defined(__x86_64__)
  ac97_dev_init();
#endif

  /* M107 device nodes — virtual terminals, /dev/kmsg, /dev/rtc*, /dev/watchdog,
   * /dev/loop-control and the SMBus adapter. Same reason as everything above:
   * these drivers probe at early boot, so without this their nodes stay on the
   * initramfs root and every ioctl against them reports ENOENT. */
  vt_register_nodes();
  kmsg_register_nodes();
  rtc_dev_register_nodes();
  watchdog_register_nodes();
  loop_register_nodes();
  i2c_register_nodes();

  /* /dev/shm — POSIX shared memory. musl's shm_open() opens
   * /dev/shm/<name>, and without the directory every caller fails at the
   * first step: wlroots allocates each output buffer through shm_open, so a
   * compositor came up with no output at all and reported only "Failed to
   * allocate buffer". Same permissions as /tmp (sticky, world-writable). */
  node = add_node("/dev/shm", VFS_DIRECTORY, 0, 0, 0);
  if (node && !IS_ERR(node)) {
    node->inode->mode = 01777;
    node->inode->uid = 0;
    node->inode->gid = 0;
    vfs_node_put(node);
  }

  /* Block-device nodes WITH their blk_dev + read/write callbacks and size.
   * An unbound VFS_DEVICE placeholder reads as an empty file, which broke
   * every disk tool. blk_create_dev_nodes() is the canonical binder. */
  blk_create_dev_nodes();

  /* The flash character device, for the same reason and at the same moment.
   * /dev is rebuilt from this list every time devtmpfs mounts, so a node
   * created once at probe time disappears the moment the real root is in
   * place -- which is exactly what happened to /dev/mtd0: the chip was found
   * and its block face registered, while the interface that can erase it was
   * quietly gone. */
  mtd_create_dev_nodes();
}

void vfs_repopulate_after_root_mount(void) {
  struct vfs_node *node;

  vfs_populate_dev();

  node = add_node("/home", VFS_DIRECTORY, 0, 0, 0);
  if (node && !IS_ERR(node)) vfs_node_put(node);

  node = add_node("/tmp", VFS_DIRECTORY, 0, 0, 0);
  if (node && !IS_ERR(node)) {
    node->inode->mode = 01777; // Sticky bit + rwxrwxrwx
    node->inode->uid = 0;
    node->inode->gid = 0;
    vfs_node_put(node);
  }



  node = add_node("/run", VFS_DIRECTORY, 0, 0, 0);
  if (node && !IS_ERR(node)) {
    node->inode->mode = 0755;
    node->inode->uid = 0;
    node->inode->gid = 0;
    vfs_node_put(node);
  }

  node = add_node("/var", VFS_DIRECTORY, 0, 0, 0);
  if (node && !IS_ERR(node)) vfs_node_put(node);

  node = add_node("/mnt", VFS_DIRECTORY, 0, 0, 0);
  if (node && !IS_ERR(node)) vfs_node_put(node);

  node = add_node("/mnt/ext1", VFS_DIRECTORY, 0, 0, 0);
  if (node && !IS_ERR(node)) vfs_node_put(node);

  node = add_node("/mnt/ext2", VFS_DIRECTORY, 0, 0, 0);
  if (node && !IS_ERR(node)) vfs_node_put(node);

  node = add_node("/mnt/ext3", VFS_DIRECTORY, 0, 0, 0);
  if (node && !IS_ERR(node)) vfs_node_put(node);

  node = add_node("/mnt/ext4", VFS_DIRECTORY, 0, 0, 0);
  if (node && !IS_ERR(node)) vfs_node_put(node);

  node = add_node("/mnt/ext4nvme", VFS_DIRECTORY, 0, 0, 0);
  if (node && !IS_ERR(node)) vfs_node_put(node);

  node = add_node("/mnt/iso", VFS_DIRECTORY, 0, 0, 0);
  if (node && !IS_ERR(node)) vfs_node_put(node);

  node = add_node("/mnt/root", VFS_DIRECTORY, 0, 0, 0);
  if (node && !IS_ERR(node)) vfs_node_put(node);

  vfs_unlink("/persist");
  vfs_rmdir("/persist");
  vfs_symlink("/", "/persist");

  // Shadow, passwd, group nodes (loaded from initramfs, exist on mount point or in VFS)
  node = vfs_find_node("/etc/shadow");
  if (node && !IS_ERR(node)) {
    node->inode->mode = 0600;
    node->inode->uid = 0;
    node->inode->gid = 0;
    vfs_node_put(node);
  }

  node = vfs_find_node("/etc/passwd");
  if (node && !IS_ERR(node)) {
    node->inode->mode = 0644;
    node->inode->uid = 0;
    node->inode->gid = 0;
    vfs_node_put(node);
  }

  node = vfs_find_node("/etc/group");
  if (node && !IS_ERR(node)) {
    node->inode->mode = 0644;
    node->inode->uid = 0;
    node->inode->gid = 0;
    vfs_node_put(node);
  }
}

int vfs_open(const char *path) { return vfs_open_flags(path, B1NIX_O_RDONLY); }

int vfs_open_flags(const char *path, int flags) {
  /* No mode given: the historical 0666 default, still masked by the umask. */
  return vfs_open_flags_mode(path, flags, 0666);
}

/* The real body; the wrapper below reports a refused graphics open. */
static int vfs_open_flags_mode_inner(const char *path, int flags, u16 mode);

int vfs_open_flags_mode(const char *path, int flags, u16 mode) {
  int rc = vfs_open_flags_mode_inner(path, flags, mode);

  /*
   * A refused open of a graphics node, with the flags that were asked for.
   *
   * A compositor reports "failed to open drm device" for anything that goes
   * wrong on the way to a usable fd, and the same node opens perfectly well
   * from a shell — so the difference is in the flags, and the errno is the only
   * thing that says which. Reported here rather than at a syscall, because the
   * callers arrive through open, openat and the kernel's own helpers, and the
   * one that mattered went through the entry the first attempt did not cover.
   */
  if (rc < 0 && path && path[0] == '/' && path[1] == 'd' && path[2] == 'e' &&
      path[3] == 'v' && path[4] == '/' && path[5] == 'd' && path[6] == 'r' &&
      path[7] == 'i' && bootinfo_has_flag("b1nix.drm-debug")) {
    console_write("drm: open ");
    console_write(path);
    console_write(" flags=0x");
    console_write_hex64((u64)(u32)flags);
    console_write(" -> -");
    console_write_dec((u64)(-rc));
    console_write("\n");
  }
  return rc;
}

static int vfs_open_flags_mode_inner(const char *path, int flags, u16 mode) {
  int res = 0;
  if (!path)
    return -EINVAL;
  /* What a graphics client actually looks at while deciding a device exists.
   *
   * Chromium enumerates GPUs through sysfs and reports finding none, while the
   * node it should find answers correctly when asked directly. Guessing which
   * path it reads has cost several rebuilds; this says so outright. Gated on a
   * cmdline flag, and only for /sys, so it costs nothing in a normal boot. */
  /* Every open, when asked for. The browser's threads all wait on one
   * initialiser inside libnss3 and nothing finishes it; what that initialiser
   * touches — a device, a database, a directory that does not exist — is the
   * question, and it is answered by the last file it managed to open. */
  if (bootinfo_has_flag("b1nix.trace-open") && current_task) {
    console_write("open: ");
    console_write(path);
    console_write(" by ");
    console_write(current_task->name);
    console_write("\n");
  }
  if (bootinfo_has_flag("b1nix.trace-sysfs") && current_task &&
      ((path[0] == '/' && path[1] == 's' && path[2] == 'y' && path[3] == 's' &&
        (path[4] == '/' || path[4] == 0)) ||
       /* And /dev/dri, which is where the browser actually looks: it opens
        * every renderD* in turn. Whether it opens ours at all, and when
        * relative to its verdict, cannot be read off the console — its own
        * messages go to a file and are printed later. */
       (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' &&
        path[4] == '/' && path[5] == 'd' && path[6] == 'r' && path[7] == 'i'))) {
    console_write("sysfs-open: ");
    console_write(path);
    console_write(" by ");
    console_write(current_task->name);
    console_write("\n");
  }
  if (strlen(path) >= VFS_MAX_PATH)
    return -ENAMETOOLONG;
  char *resolved = kmalloc(VFS_MAX_PATH);
  if (!resolved)
    return -ENOMEM;
  vfs_resolve_path(path, resolved);

  if (strcmp(resolved, "/dev/tty") == 0) {
    int type = 0;
    int index = 0;
    scheduler_get_ctty(&type, &index);
    if (type == 0) {
      kfree(resolved);
      return -ENXIO;
    }
    if (type == 2) {
      int fd = serial_tty_open(index, flags);
      kfree(resolved);
      return fd;
    }
    if (type == 3) {
      int fd = pty_open_slave(index, flags);
      kfree(resolved);
      return fd;
    }
    /* type == 1 is console, fall through to normal /dev/tty open */
  }

  /* /proc/self/fd/<N> is a magic link on Linux: opening it does not follow the
   * link text ("pipe:[7]" names no file) — it hands back another reference to
   * the SAME open file description. Everything that opens a descriptor by path
   * depends on this, most visibly bash's process substitution, which passes
   * `/dev/fd/63` (-> /proc/self/fd/63) to the reader; following the text there
   * gives ENOENT, nobody ever reads the pipe, and the writing child blocks in
   * pipe_write() until the whole pipeline is wedged. Only the calling task's
   * own descriptors are reachable this way, which is all any of this needs. */
  {
    const char *p = 0;
    if (strncmp(resolved, "/proc/self/fd/", 14) == 0)
      p = resolved + 14;
    else if (strncmp(resolved, "/dev/fd/", 8) == 0)
      p = resolved + 8;
    if (p && *p) {
      int n = 0, ok = 1;
      for (const char *q = p; *q; q++) {
        if (*q < '0' || *q > '9') { ok = 0; break; }
        n = n * 10 + (*q - '0');
      }
      if (ok) {
        kfree(resolved);
        struct vfs_handle *h = scheduler_fd_get(n);
        if (!h)
          return -EBADF;
        /*
         * A descriptor that names a FILE is re-OPENED, not duplicated.
         *
         * The two are not the same thing, and the difference is the whole
         * reason userspace uses this path. dup(2) hands back the same open
         * file description: same flags, same offset. Opening /proc/self/fd/<n>
         * on Linux performs a fresh open of the file that descriptor refers
         * to, with the flags given here -- which is how a program turns an
         * O_PATH reference, which cannot be read, into a descriptor that can.
         *
         * Duplicating instead meant the flags argument was discarded, so the
         * new descriptor was O_PATH too and every read of it answered EBADF.
         * systemd 254 and later open every configuration file that way
         * (chase() returns an O_PATH fd, fd_reopen() upgrades it), so PID 1
         * read none of its own configuration: /etc/systemd/system.conf,
         * /etc/os-release and /etc/machine-id all came back "Bad file
         * descriptor", and the manager ran on defaults having reported each
         * one as a syntax error at line 0.
         *
         * A descriptor with no VFS node behind it -- a pipe, a socket, an
         * eventfd -- keeps the old behaviour, because there is no file to open
         * afresh and another reference to the same description is the only
         * meaningful answer. That is the case the comment above is about, and
         * it is what bash's process substitution depends on.
         */
        if (h->kind == VFS_HANDLE_NODE && h->node) {
          struct vfs_node *rnode = vfs_node_get(h->node);
          /* Permissions are checked again, deliberately: an O_PATH reference
           * must not become a way to open a file for writing that the caller
           * could not have opened for writing by name. */
          int access_mask = 0;
          if (flags & (B1NIX_O_WRONLY | B1NIX_O_RDWR))
            access_mask |= W_OK;
          if ((flags & 3) == B1NIX_O_RDONLY || (flags & B1NIX_O_RDWR))
            access_mask |= R_OK;
          const struct cred *rcred = get_current_cred();
          const int rpath_only = (flags & B1NIX_O_PATH) ? 1 : 0;
          if (!rpath_only && rcred &&
              !vfs_get_node_perm(rnode, rcred, (u32)access_mask)) {
            vfs_node_put(rnode);
            return -EACCES;
          }
          if (rnode->inode->type == VFS_DIRECTORY && !rpath_only &&
              (flags & (B1NIX_O_WRONLY | B1NIX_O_RDWR))) {
            vfs_node_put(rnode);
            return -EISDIR;
          }
          extern const struct vfs_file_ops node_file_ops;
          struct vfs_handle *nh = alloc_raw_handle(VFS_HANDLE_NODE);
          if (!nh) {
            vfs_node_put(rnode);
            return -ENFILE;
          }
          nh->node = rnode;
          nh->ops = &node_file_ops;
          nh->flags = flags;
          nh->offset = (flags & B1NIX_O_APPEND) ? rnode->inode->size : 0;
          if (h->open_path) {
            usize pl = strlen(h->open_path);
            char *op = kmalloc(pl + 1);
            if (op) {
              memcpy(op, h->open_path, pl + 1);
              nh->open_path = op;
            }
          }
          if (rnode->inode->open_cb && !rpath_only) {
            int orc = rnode->inode->open_cb(rnode, nh);
            if (orc < 0) {
              vfs_handle_release(nh);
              return orc;
            }
          }
          int newfd = scheduler_fd_alloc(nh);
          if (newfd < 0) {
            vfs_handle_release(nh);
            return newfd == -ENOMEM ? -ENOMEM : -EMFILE;
          }
          if (flags & B1NIX_O_CLOEXEC)
            scheduler_fd_flags_set(newfd, B1NIX_FD_CLOEXEC);
          return newfd;
        }
        vfs_handle_retain(h);
        int newfd = scheduler_fd_alloc(h);
        if (newfd < 0)
          vfs_handle_release(h);
        return newfd;
      }
    }
  }

  /* M32b pseudo-terminals: /dev/ptmx allocates a fresh master; /dev/pts/<N>
   * binds to that pair's slave. These are dynamic handles, not VFS nodes, so
   * intercept the open before the path lookup. */
  if (strcmp(resolved, "/dev/ptmx") == 0) {
    struct vfs_node *ptmx_node = vfs_find_node("/dev/ptmx");
    if (!IS_ERR(ptmx_node)) {
      int access_mask = 0;
      if (flags & (B1NIX_O_WRONLY | B1NIX_O_RDWR))
        access_mask |= W_OK;
      if ((flags & 3) == B1NIX_O_RDONLY || (flags & B1NIX_O_RDWR))
        access_mask |= R_OK;
      const struct cred *cred = get_current_cred();
      if (cred && !vfs_get_node_perm(ptmx_node, cred, (u32)access_mask)) {
        vfs_node_put(ptmx_node);
        kfree(resolved);
        return -EACCES;
      }
      vfs_node_put(ptmx_node);
    }
    int fd = pty_open_master(flags);
    kfree(resolved);
    return fd;
  }
  /* M39 serial ttys: /dev/ttySn opens bind to the per-port tty (dynamic
   * handles like ptys, with their own line discipline + session state). */
  {
    int sidx = serial_tty_path_index(resolved);
    if (sidx >= 0) {
      struct vfs_node *snode = vfs_find_node(resolved);
      if (!IS_ERR(snode)) {
        int access_mask = 0;
        if (flags & (B1NIX_O_WRONLY | B1NIX_O_RDWR))
          access_mask |= W_OK;
        if ((flags & 3) == B1NIX_O_RDONLY || (flags & B1NIX_O_RDWR))
          access_mask |= R_OK;
        const struct cred *cred = get_current_cred();
        if (cred && !vfs_get_node_perm(snode, cred, (u32)access_mask)) {
          vfs_node_put(snode);
          kfree(resolved);
          return -EACCES;
        }
        vfs_node_put(snode);
      }
      int fd = serial_tty_open(sidx, flags);
      kfree(resolved);
      return fd;
    }
  }
  /* Both DRM nodes take the same route: check the node's permissions, then let
   * the owning driver build the handle. They differ only in which driver that
   * is — card0 is b1nix's own device, card1 the imported core's. */
  if (drm_imported_card_present(resolved)) {
    struct vfs_node *card = vfs_find_node(resolved);
    if (!IS_ERR(card)) {
      int access_mask = 0;
      if (flags & (B1NIX_O_WRONLY | B1NIX_O_RDWR))
        access_mask |= W_OK;
      if ((flags & 3) == B1NIX_O_RDONLY || (flags & B1NIX_O_RDWR))
        access_mask |= R_OK;
      int access = vfs_check_access(card, access_mask);
      vfs_node_put(card);
      if (access < 0) {
        kfree(resolved);
        return access;
      }
    }
    int fd = drm_imported_card_open(resolved, flags);
    kfree(resolved);
    return fd;
  }
  if (strcmp(resolved, "/dev/dri/card0") == 0) {
    struct vfs_node *card = vfs_find_node(resolved);
    if (!IS_ERR(card)) {
      int access_mask = 0;
      if (flags & (B1NIX_O_WRONLY | B1NIX_O_RDWR))
        access_mask |= W_OK;
      if ((flags & 3) == B1NIX_O_RDONLY || (flags & B1NIX_O_RDWR))
        access_mask |= R_OK;
      int access = vfs_check_access(card, access_mask);
      vfs_node_put(card);
      if (access < 0) {
        kfree(resolved);
        return access;
      }
    }
    int fd = drm_dev_open(flags);
    kfree(resolved);
    return fd;
  }
  /* M47 input event devices: /dev/input/eventN opens bind a per-client
   * event queue (raw handles with their own file ops, like the ttys). */
  {
    int iidx = input_path_index(resolved);
    if (iidx >= 0) {
      struct vfs_node *inode_node = vfs_find_node(resolved);
      if (!IS_ERR(inode_node)) {
        int access_mask = 0;
        if (flags & (B1NIX_O_WRONLY | B1NIX_O_RDWR))
          access_mask |= W_OK;
        if ((flags & 3) == B1NIX_O_RDONLY || (flags & B1NIX_O_RDWR))
          access_mask |= R_OK;
        const struct cred *cred = get_current_cred();
        if (cred && !vfs_get_node_perm(inode_node, cred, (u32)access_mask)) {
          vfs_node_put(inode_node);
          kfree(resolved);
          return -EACCES;
        }
        vfs_node_put(inode_node);
      }
      int fd = input_dev_open(iidx, flags);
      kfree(resolved);
      return fd;
    }
  }
  if (strncmp(resolved, "/dev/pts/", 9) == 0) {
    const char *num = resolved + 9;
    if (*num >= '0' && *num <= '9') {
      int idx = 0;
      for (const char *q = num; *q >= '0' && *q <= '9'; q++)
        idx = idx * 10 + (*q - '0');
      struct vfs_node *pts_dir = vfs_find_node("/dev/pts");
      if (!IS_ERR(pts_dir)) {
        int access_mask = X_OK;
        if (flags & (B1NIX_O_WRONLY | B1NIX_O_RDWR))
          access_mask |= W_OK;
        if ((flags & 3) == B1NIX_O_RDONLY || (flags & B1NIX_O_RDWR))
          access_mask |= R_OK;
        const struct cred *cred = get_current_cred();
        if (cred && !vfs_get_node_perm(pts_dir, cred, (u32)access_mask)) {
          vfs_node_put(pts_dir);
          kfree(resolved);
          return -EACCES;
        }
        vfs_node_put(pts_dir);
      }
      int fd = pty_open_slave(idx, flags);
      kfree(resolved);
      return fd;
    }
  }

  /* O_NOFOLLOW stops at the last component; every component before it is still
   * followed, which is what the flag means. */
  const int follow_final = (flags & B1NIX_O_NOFOLLOW) ? 0 : 1;
  struct vfs_node *node = vfs_find_node_internal(resolved, follow_final, 0);
  if (IS_ERR(node)) {
    if (PTR_ERR(node) == -ENOENT && (flags & B1NIX_O_CREAT)) {
      /* Use internal version to avoid redundant resolution/logging */
      /* open(2) with O_CREAT: the file is created with the caller's mode,
       * masked by its umask — ignoring the mode left every new file
       * world-readable (and made setfsuid/permission tests meaningless). */
      const struct cred *ocred = get_current_cred();
      u16 create_mode = (u16)(mode & 07777);
      if (ocred)
        create_mode &= (u16)~ocred->umask;
      /*
       * A symlink pointing at nothing is still a path to create through.
       *
       * open(O_CREAT) creates what the link names, not the link: the lookup
       * above already followed it and said ENOENT, and creating at the link's
       * own path would find the link sitting there and report EEXIST — which
       * is what happened to a package manager whose index was a symlink onto a
       * disk that had not been written yet. Followed here, bounded, and only
       * when the last component really is a link.
       */
      {
        char *link = kmalloc(VFS_MAX_PATH);

        if (link) {
          for (int hop = 0; hop < 8; hop++) {
            struct vfs_node *ln = vfs_find_node_internal(resolved, 0, 0);
            int is_link = !IS_ERR(ln) && ln->inode &&
                          ln->inode->type == VFS_SYMLINK;

            if (!IS_ERR(ln))
              vfs_node_put(ln);
            if (!is_link)
              break;
            isize n = vfs_readlink(resolved, link, VFS_MAX_PATH - 1);
            if (n <= 0)
              break;
            link[n] = 0;
            if (link[0] == '/') {
              strncpy(resolved, link, VFS_MAX_PATH - 1);
            } else {
              char *dir = kmalloc(VFS_MAX_PATH);
              char base[VFS_NAME_MAX];

              if (!dir)
                break;
              if (split_parent_path(resolved, dir, VFS_MAX_PATH, base, sizeof(base)) == 0)
                snprintf(resolved, VFS_MAX_PATH, "%s/%s", dir, link);
              kfree(dir);
            }
            resolved[VFS_MAX_PATH - 1] = 0;
          }
          kfree(link);
        }
      }
      int err = vfs_create_at_internal(resolved, create_mode);
      if (err != 0) {
        res = err;
        goto out;
      }
      node = vfs_find_node_internal(resolved, follow_final, 0);
      if (IS_ERR(node)) {
        res = (int)PTR_ERR(node);
        goto out;
      }
    } else {
      /* Plain ENOENT (or any other open error without O_CREAT) is a *normal*
       * userspace event — gcc/cc1, init scripts, and shells probe many paths
       * that may not exist. Linux doesn't log it; neither should we. The
       * errno reaches userspace via the syscall return, that's enough. */
      res = (int)PTR_ERR(node);
      goto out;
    }
  } else {
    if ((flags & B1NIX_O_CREAT) && (flags & B1NIX_O_EXCL)) {
      res = -EEXIST;
      goto out;
    }
  }

  if (node->inode && node->inode->ino && node->inode->fs_id) {
    struct vfs_inode *cached = icache_get(node->inode->fs_id, node->inode->ino);
    if (!cached) {
      icache_insert(node->inode->fs_id, node->inode->ino, node->inode);
    }
  }

  /*
   * The last component is a symlink and the caller said not to follow it.
   *
   * Plain O_NOFOLLOW is an error -- there is nothing to open, since the file
   * the caller asked for is the link's target and it declined to go there.
   * O_PATH changes that: the descriptor then refers to the LINK, which is the
   * only way to fstat one without a race, and it is how a path is walked by
   * hand. Everything below this point -- the access check, O_TRUNC, the
   * driver's open callback -- is about the file's contents, and an O_PATH
   * descriptor has none, so it skips all of it.
   */
  const int path_only = (flags & B1NIX_O_PATH) ? 1 : 0;
  if (!follow_final && node->inode->type == VFS_SYMLINK && !path_only) {
    res = -ELOOP;
    goto out;
  }
  if ((flags & B1NIX_O_DIRECTORY) && node->inode->type != VFS_DIRECTORY) {
    res = -ENOTDIR;
    goto out;
  }
  if (path_only)
    goto make_handle;
  /* POSIX: writing to a directory descriptor is not permitted */
  if (node->inode->type == VFS_DIRECTORY &&
      (flags & (B1NIX_O_WRONLY | B1NIX_O_RDWR))) {
    res = -EISDIR;
    goto out;
  }

  int access_mask = 0;
  if (flags & (B1NIX_O_WRONLY | B1NIX_O_RDWR))
    access_mask |= W_OK;
  if ((flags & 3) == B1NIX_O_RDONLY || (flags & B1NIX_O_RDWR))
    access_mask |= R_OK;

  /* A read-only mount refuses the OPEN, not the first write. Linux answers
   * EROFS here, and programs act on it: a unit under systemd's
   * `ProtectSystem=strict` expects `open(..., O_WRONLY)` to fail rather than
   * succeed and then hand back a descriptor that cannot be written. Worse,
   * O_TRUNC took effect below with no check at all, so a sandboxed process
   * could empty a file on a filesystem the kernel believed was read-only. */
  if (access_mask & W_OK) {
    struct vfs_mount_entry *wmnt = vfs_get_mount_for_node(node);
    if (wmnt && (wmnt->flags & MS_RDONLY)) {
      res = -EROFS;
      goto out;
    }
  }

  res = vfs_check_access(node, access_mask);
  if (res != 0) {
    goto out;
  }

  /* A FIFO open binds to the shared pipe buffer instead of the node's data —
   * including the POSIX rendezvous, so this must happen after the permission
   * check but before any O_TRUNC handling (truncating a FIFO is a no-op). */
  if (node->inode->type == VFS_FIFO) {
    res = vfs_fifo_open(node, flags);
    vfs_node_put(node);
    node = NULL;
    goto out;
  }

  if ((flags & B1NIX_O_TRUNC) && node->inode->type == VFS_FILE) {
    /* O_TRUNC requires write permission regardless of open mode */
    res = vfs_check_access(node, W_OK);
    if (res != 0) {
      goto out;
    }
    /* M109 chattr: same rule as ftruncate — neither an immutable nor an
     * append-only file may be emptied by an open(). */
    if (node->inode->attr & (VFS_ATTR_IMMUTABLE | VFS_ATTR_APPEND)) {
      res = -EPERM;
      goto out;
    }

    vfs_inode_lock(node->inode);
    /* Drop cached pages first: dirty pages of the discarded content must not
     * be written back, and a later re-grow must read zeros, not stale data. */
    page_cache_truncate_inode(node->inode, 0);
    if (node->inode->truncate_cb) {
      /* Let the filesystem free the data blocks (it also updates size/times),
       * matching ftruncate semantics instead of only zeroing the size field. */
      node->inode->truncate_cb(node, 0);
    } else {
      node->inode->size = 0;
      if (node->inode->data && !node->inode->write_cb && !node->inode->read_cb)
        ((char *)node->inode->data)[0] = '\0';
      if (node->inode->setattr_cb)
        node->inode->setattr_cb(node);
    }
    vfs_inode_unlock(node->inode);
  }

make_handle:;
  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_NODE);
  if (!h) {
    res = -ENFILE;
    goto out;
  }
  h->node = node; /* Already has ref from find_node */
  /* The name this descriptor was opened under (see vfs_handle::open_path).
   * `resolved` is already absolute and lexically normalised. */
  {
    usize rl = strlen(resolved);
    char *op = kmalloc(rl + 1);
    if (op) {
      memcpy(op, resolved, rl + 1);
      h->open_path = op;
    }
  }
  extern const struct vfs_file_ops node_file_ops;
  h->ops = &node_file_ops;
  h->flags = flags;
  h->offset = (flags & B1NIX_O_APPEND) ? node->inode->size : 0;

  if (node->inode->open_cb && !path_only) {
    int orc = node->inode->open_cb(node, h);
    if (orc < 0) {
      vfs_handle_release(h);
      node = NULL;
      res = orc;
      goto out;
    }
  }

  int fd = scheduler_fd_alloc(h);
  if (fd < 0) {
    /* vfs_handle_release() drops the node ref it now owns (node_file_ops has no
     * ->release, so it falls to vfs_node_put(h->node)); NULL it so the out:
     * label does not put the same reference a second time (double-decref UAF). */
    vfs_handle_release(h);
    node = NULL;
    res = -EMFILE;
    goto out;
  }
  if (flags & B1NIX_O_CLOEXEC)
    scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  res = fd;

out:
  if (resolved)
    kfree(resolved);
  if (res < 0 && node && !IS_ERR(node))
    vfs_node_put(node);
  return res;
}

/* Core read. posp == NULL: use and advance the handle's own offset (normal
 * read()). posp != NULL: read at *posp and advance *posp, leaving the handle
 * offset untouched — positioned I/O (pread/sendfile), safe when threads share
 * the open-file description. */
static isize node_read_impl(struct vfs_handle *h, char *buf, usize size,
                            u64 *posp) {
  if (!h->node)
    return -EBADF;
  /* An O_PATH descriptor names a file without opening its contents; reading or
   * writing one is EBADF, which is what tells a caller the difference between
   * a reference and an open file. */
  if (h->flags & B1NIX_O_PATH)
    return -EBADF;
  struct vfs_node *node = vfs_node_get(h->node);
  u64 offset = posp ? *posp : h->offset;
  vfs_inode_lock(node->inode);
  vfs_update_times(node->inode, VFS_ATIME);
  isize res = 0;
  if (node->inode->type == VFS_FILE && node->inode->read_cb) {
    usize remaining = size;
    usize total_read = 0;
    u64 curr_offset = offset;

    while (remaining > 0) {
      if (curr_offset >= node->inode->size) break;

      u64 page_aligned = curr_offset & ~(PAGE_SIZE - 1);
      usize page_offset = curr_offset & (PAGE_SIZE - 1);
      usize chunk = PAGE_SIZE - page_offset;
      if (chunk > remaining) chunk = remaining;
      if (curr_offset + chunk > node->inode->size) {
        chunk = node->inode->size - curr_offset;
      }

      struct page_cache_entry *page = page_cache_get_page(node->inode, page_aligned);
      if (!page) {
        u64 frame = pmm_alloc_frame();
        if (!frame) {
          if (total_read == 0) res = -ENOMEM;
          break;
        }

        void *virt_addr = (void *)(usize)(frame + vmm_direct_map_base());
        memset(virt_addr, 0, PAGE_SIZE);
        isize read_res = node->inode->read_cb(node, page_aligned, virt_addr, PAGE_SIZE, 0);

        if (read_res < 0) {
          pmm_free_frame(frame);
          if (total_read == 0) res = read_res;
          break;
        }

        if (page_cache_add_page(node->inode, page_aligned, frame) < 0) {
          pmm_free_frame(frame);
          page = page_cache_get_page(node->inode, page_aligned);
          if (!page) {
            if (total_read == 0) res = -ENOMEM;
            break;
          }
        } else {
          /* The add succeeded, which does not mean the page is still there:
           * an evictor on another CPU can take it back before this lookup, and
           * the result was dereferenced unchecked. Treat a miss the way the
           * failed-add path above does. */
          page = page_cache_get_page(node->inode, page_aligned);
          if (!page) {
            if (total_read == 0) res = -ENOMEM;
            break;
          }
        }
      }

      void *virt_addr = (void *)(usize)(page->frame + vmm_direct_map_base());
      memcpy(buf + total_read, (char *)virt_addr + page_offset, chunk);
      page_cache_put_page(page);

      total_read += chunk;
      curr_offset += chunk;
      remaining -= chunk;
    }

    if (total_read > 0) {
      res = total_read;
      if (posp) *posp += total_read; else h->offset += total_read;
    }
  } else if (node->inode->read_cb) {
    /* Not under the inode lock: a device read blocks.
     *
     * This called the driver while holding the inode's WRITE lock. For a file
     * the callback returns promptly; for a device it waits for input, and a
     * terminal with a shell sitting on it waits forever -- so every other
     * operation on that node, including open(2), blocked behind a lock nobody
     * was going to release. logind hit exactly that: it opens /dev/tty1 while
     * taking control of a session, the open never returned, and the D-Bus call
     * that was waiting for it timed out with no reply. The watchdog named it
     * outright -- "inode ino=58130 rw_lock=-1 holder=202" with task 202 parked
     * on the VT's input queue.
     *
     * The lock protects this inode's own fields; the driver has its own
     * locking for its device, and the offset it advances lives on the handle,
     * not the inode. So drop it for the duration of the call and take it again
     * afterwards, leaving the single unlock at the end of the function
     * balanced. */
    vfs_inode_unlock(node->inode);
    res = node->inode->read_cb(node, offset, buf, size, h->flags);
    vfs_inode_lock(node->inode);
    if (res > 0) {
      if (posp) *posp += (usize)res; else h->offset += (usize)res;
    }
  } else if (node->inode->type == VFS_FILE) {
    usize rem = node->inode->size > offset ? node->inode->size - offset : 0;
    usize to_r = size < rem ? size : rem;
    if (to_r > 0) {
      if (node->inode->cached_pages == 0) {
        if (node->inode->data)
          memcpy(buf, (const char *)node->inode->data + offset, to_r);
        else
          memset(buf, 0, to_r);
        res = (isize)to_r;
      } else {
        usize done = 0;
        /* An in-memory file may also be mapped. A mapping is served from a
         * page-cache frame seeded from inode->data, and stores through it never
         * come back here — so a read that only looked at inode->data would report
         * what the file held before anyone wrote to the mapping. Where a page is
         * cached, it is the newer of the two, and it is what a reader must see. */
        while (done < to_r) {
          u64 cur = offset + done;
          u64 page_aligned = cur & ~((u64)PAGE_SIZE - 1);
          usize page_off = (usize)(cur & ((u64)PAGE_SIZE - 1));
          usize chunk = PAGE_SIZE - page_off;
          if (chunk > to_r - done)
            chunk = to_r - done;
          struct page_cache_entry *pe =
              page_cache_get_page(node->inode, page_aligned);
          if (pe) {
            const char *src =
                (const char *)(usize)(pe->frame + vmm_direct_map_base());
            memcpy(buf + done, src + page_off, chunk);
            page_cache_put_page(pe);
          } else {
            if (node->inode->data)
              memcpy(buf + done, (const char *)node->inode->data + cur, chunk);
            else
              memset(buf + done, 0, chunk);
          }
          done += chunk;
        }
        res = (isize)done;
      }
      if (posp) *posp += (usize)res; else h->offset += (usize)res;
    } else {
      res = 0;
    }
  }
  vfs_inode_unlock(node->inode);
  vfs_node_put(node);
  return res;
}

/* file_ops .read — normal read using the handle's own offset. */
static isize node_read(struct vfs_handle *h, char *buf, usize size) {
  return node_read_impl(h, buf, size, 0);
}

/* Core write. posp semantics mirror node_read_impl. O_APPEND ignores an explicit
 * position (POSIX: append always goes to EOF). */
static isize node_write_impl(struct vfs_handle *h, const char *buf, usize size,
                             u64 *posp) {
  if (h->flags & B1NIX_O_PATH)
    return -EBADF;
  if (!h->node)
    return -EBADF;
  struct vfs_node *node = vfs_node_get(h->node);
  /* M56 sealing: a sealed-for-write memfd rejects all writes (EPERM). */
  if (node->inode->seals & B1NIX_F_SEAL_WRITE) {
    vfs_node_put(node);
    return -EPERM;
  }
  /* M109 chattr: an immutable file takes no writes at all, and an append-only
   * one takes only writes that extend it — which means a descriptor opened
   * O_APPEND, whose offset the branch below pins to the current end of file.
   * A positioned write (pwrite, posp != NULL) ignores O_APPEND, so it can
   * never be an append and is refused too. */
  if (node->inode->attr & (VFS_ATTR_IMMUTABLE | VFS_ATTR_APPEND)) {
    int is_append = !posp && (h->flags & B1NIX_O_APPEND);
    if ((node->inode->attr & VFS_ATTR_IMMUTABLE) || !is_append) {
      vfs_node_put(node);
      return -EPERM;
    }
  }
  vfs_inode_lock(node->inode);
  /* O_APPEND: sample the size under the exclusive inode lock. Reading it
   * before the lock loses concurrent appends — a writer blocked on the lock
   * would rewind to a stale EOF and overwrite what the lock holder appended.
   * A positioned write (posp != NULL) writes exactly at *posp and ignores
   * O_APPEND. */
  if (!posp && (h->flags & B1NIX_O_APPEND))
    h->offset = node->inode->size;
  u64 offset = posp ? *posp : h->offset;
  isize res = 0;
  if (node->inode->type == VFS_FILE && node->inode->write_cb) {
    usize remaining = size;
    usize total_written = 0;
    u64 curr_offset = offset;

    while (remaining > 0) {
      u64 page_aligned = curr_offset & ~(PAGE_SIZE - 1);
      usize page_offset = curr_offset & (PAGE_SIZE - 1);
      usize chunk = PAGE_SIZE - page_offset;
      if (chunk > remaining) chunk = remaining;

      struct page_cache_entry *page = page_cache_get_page(node->inode, page_aligned);
      if (!page) {
        u64 frame = pmm_alloc_frame();
        if (!frame) {
          if (total_written == 0) res = -ENOMEM;
          break;
        }

        void *virt_addr = (void *)(usize)(frame + vmm_direct_map_base());
        memset(virt_addr, 0, PAGE_SIZE);
        
        if ((chunk < PAGE_SIZE) && (page_aligned < node->inode->size)) {
          if (node->inode->read_cb) {
             node->inode->read_cb(node, page_aligned, virt_addr, PAGE_SIZE, 0);
          }
        }

        if (page_cache_add_page(node->inode, page_aligned, frame) < 0) {
          pmm_free_frame(frame);
          page = page_cache_get_page(node->inode, page_aligned);
          if (!page) {
            if (total_written == 0) res = -ENOMEM;
            break;
          }
        } else {
          /* The add succeeded, which does not mean the page is still there:
           * an evictor on another CPU can take it back before this lookup, and
           * the result was dereferenced unchecked. Treat a miss the way the
           * failed-add path above does. */
          page = page_cache_get_page(node->inode, page_aligned);
          if (!page) {
            if (total_written == 0) res = -ENOMEM;
            break;
          }
        }
      }

      void *virt_addr = (void *)(usize)(page->frame + vmm_direct_map_base());
      memcpy((char *)virt_addr + page_offset, buf + total_written, chunk);
      page_cache_mark_dirty(page);
      page_cache_put_page(page);

      total_written += chunk;
      curr_offset += chunk;
      remaining -= chunk;
    }

    if (total_written > 0) {
      res = total_written;
      u64 newpos = offset + total_written;
      if (posp) *posp = newpos; else h->offset = newpos;
      if (newpos > node->inode->size) {
        node->inode->size = newpos;
      }
      vfs_update_times(node->inode, VFS_MTIME | VFS_CTIME);
    }
  } else if (node->inode->write_cb) {
    /* Tell the block layer whose blocks these are, so a later fsync can
     * write back this file instead of every dirty block on the device. */
    blk_set_dirty_owner(node->inode->fs_id, node->inode->ino);
    res = node->inode->write_cb(node, offset, buf, size, h->flags);
    blk_clear_dirty_owner();
    if (res > 0) {
      vfs_update_times(node->inode, VFS_MTIME | VFS_CTIME);
      if (posp) *posp += (usize)res; else h->offset += (usize)res;
    }
  } else if (node->inode->type == VFS_FILE) {
    if (offset + size > MAX_FILE_SIZE) {
      vfs_inode_unlock(node->inode);
      vfs_node_put(node);
      return -EFBIG;
    }
    if (offset + size > node->inode->capacity) {
      usize new_cap =
          node->inode->capacity == 0 ? 1024 : node->inode->capacity * 2;
      while (new_cap < offset + size)
        new_cap *= 2;
      void *new_data = kzalloc(new_cap);
      if (!new_data) {
        vfs_inode_unlock(node->inode);
        vfs_node_put(node);
        return -ENOMEM;
      }
      if (node->inode->data) {
        /* Preserve what the old buffer held, but never more than the new one
         * can take: a node whose data came from the initramfs carries a real
         * size with capacity 0, and copying that whole file into a buffer
         * sized for the incoming write alone overruns the kernel heap. */
        usize keep = node->inode->size;
        if (keep > new_cap)
          keep = new_cap;
        memcpy(new_data, node->inode->data, keep);
        if (node->inode->flags & VFS_NODE_OWNS_DATA)
          kfree(node->inode->data);
      }
      node->inode->data = new_data;
      node->inode->capacity = new_cap;
      node->inode->flags |= VFS_NODE_OWNS_DATA;
    }
    memcpy((char *)node->inode->data + offset, buf, size);
    /* Mirror the write into any cached page: a mapping of this file is served
     * from the page cache, so a write that only touched inode->data would be
     * invisible through the mapping — the same divergence, in the other
     * direction, that the read path above closes. */
    if (node->inode->cached_pages > 0) {
      for (u64 cur = offset; cur < offset + size;) {
        u64 page_aligned = cur & ~((u64)PAGE_SIZE - 1);
        usize page_off = (usize)(cur & ((u64)PAGE_SIZE - 1));
        usize chunk = PAGE_SIZE - page_off;
        if (chunk > (usize)(offset + size - cur))
          chunk = (usize)(offset + size - cur);
        struct page_cache_entry *pe =
            page_cache_get_page(node->inode, page_aligned);
        if (pe) {
          char *dst = (char *)(usize)(pe->frame + vmm_direct_map_base());
          memcpy(dst + page_off, buf + (usize)(cur - offset), chunk);
          page_cache_mark_dirty(pe);
          page_cache_put_page(pe);
        }
        cur += chunk;
      }
    }
    if (offset + size > node->inode->size)
      node->inode->size = (usize)(offset + size);
    vfs_update_times(node->inode, VFS_MTIME | VFS_CTIME);
    if (node->inode->setattr_cb)
      node->inode->setattr_cb(node);
    if (posp) *posp += size; else h->offset += size;
    res = (isize)size;
  } else {
    /* Nothing here can accept the bytes: a device/pipe-shaped node whose
     * filesystem installed no write_cb. Falling through with res == 0 reported
     * a successful zero-length write, so every read-only procfs/sysfs attribute
     * silently swallowed writes — a 0444 module parameter "accepted" being set
     * and simply kept its old value. Refuse instead. */
    res = -EACCES;
  }
  vfs_inode_unlock(node->inode);
  /* M73 inotify: report a successful write as IN_MODIFY. Called after the inode
   * lock is dropped so the notify path (its own leaf spinlocks) never nests
   * under the inode lock. */
  if (res > 0)
    vfs_inotify_notify(node, IN_MODIFY, 0);
  vfs_node_put(node);
  return res;
}

/* file_ops .write — normal write using the handle's own offset. */
static isize node_write(struct vfs_handle *h, const char *buf, usize size) {
  return node_write_impl(h, buf, size, 0);
}

static int node_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  if (!h || !h->node)
    return -EBADF;
  if (h->node->inode->poll_cb)
    return h->node->inode->poll_cb(h->node, pfd);
  pfd->revents = 0;
  if (h->node->inode->type == VFS_FILE) {
    pfd->revents |= B1NIX_POLLIN | B1NIX_POLLOUT;
  }
  return 0;
}

const struct vfs_file_ops node_file_ops = {
    .read = node_read,
    .write = node_write,
    .poll = node_poll,
};

isize vfs_read(int fd, char *buf, usize size) {
  struct vfs_handle *h = get_handle(fd);
  if (!h || !h->ops || !h->ops->read)
    return -EBADF;
  return h->ops->read(h, buf, size);
}

int vfs_read_is_direct(int fd) {
  struct vfs_handle *h = get_handle(fd);
  return h && h->ops && h->ops->read_user ? 1 : 0;
}

isize vfs_read_user(int fd, void *user_buf, usize size) {
  struct vfs_handle *h = get_handle(fd);
  if (!h || !h->ops || !h->ops->read_user)
    return -EBADF;
  return h->ops->read_user(h, user_buf, size);
}

/* Positioned read/write: read/write at `offset` WITHOUT touching the descriptor's
 * own file offset (POSIX pread/pwrite; also backs sendfile/copy_file_range with
 * explicit offsets). Only seekable regular-file handles qualify — a pipe/socket
 * returns ESPIPE. Thread-safe when several threads share the open-file
 * description, unlike an lseek-save-restore around a plain read/write. */
isize vfs_pread(int fd, char *buf, usize size, u64 offset) {
  struct vfs_handle *h = get_handle(fd);
  if (!h)
    return -EBADF;
  if (h->kind != VFS_HANDLE_NODE || !h->node)
    return -ESPIPE;
  u64 pos = offset;
  return node_read_impl(h, buf, size, &pos);
}

isize vfs_pwrite(int fd, const char *buf, usize size, u64 offset) {
  struct vfs_handle *h = get_handle(fd);
  if (!h)
    return -EBADF;
  if (h->kind != VFS_HANDLE_NODE || !h->node)
    return -ESPIPE;
  struct vfs_mount_entry *mnt = vfs_get_mount_for_node(h->node);
  if (mnt && (mnt->flags & MS_RDONLY))
    return -EROFS;
  u64 pos = offset;
  return node_write_impl(h, buf, size, &pos);
}

/* Positioned I/O on a node with no open-file description behind it. A
 * kernel-internal user of a file (the loop driver) must see and dirty exactly
 * the pages read()/write() would; reaching for inode->read_cb/write_cb instead
 * skips the page cache and reads or writes past whatever it holds. The handle
 * is a borrowed stack one: node_{read,write}_impl only touch ->node, ->flags
 * and ->offset, and take their own reference on the node. */
isize vfs_node_pread(struct vfs_node *node, char *buf, usize size, u64 offset) {
  if (!node || !node->inode)
    return -EBADF;
  struct vfs_handle h;
  memset(&h, 0, sizeof(h));
  h.kind = VFS_HANDLE_NODE;
  h.node = node;
  h.flags = B1NIX_O_RDONLY;
  u64 pos = offset;
  return node_read_impl(&h, buf, size, &pos);
}

isize vfs_node_pwrite(struct vfs_node *node, const char *buf, usize size,
                      u64 offset) {
  if (!node || !node->inode)
    return -EBADF;
  struct vfs_handle h;
  memset(&h, 0, sizeof(h));
  h.kind = VFS_HANDLE_NODE;
  h.node = node;
  h.flags = B1NIX_O_RDWR;
  u64 pos = offset;
  return node_write_impl(&h, buf, size, &pos);
}

/* The fd-free half of vfs_fsync: flush a node's cached pages and let the
 * filesystem push them out. */
int vfs_node_fsync(struct vfs_node *node) {
  if (!node || !node->inode)
    return -EBADF;
  vfs_inode_lock(node->inode);
  page_cache_flush_inode(node->inode);
  vfs_inode_unlock(node->inode);
  if (node->inode->fsync_cb)
    return node->inode->fsync_cb(node);
  return 0;
}

/*
 * A descriptor held by the kernel rather than by a process.
 *
 * nbd-client opens the socket, completes the NBD handshake and hands the
 * descriptor to the kernel, which then does the block I/O on it -- possibly
 * long after the process that opened it has moved on. A raw fd number is no
 * good for that: it is an index into one process's table, and it can be closed
 * or reused underneath us. These take and use a reference on the handle
 * itself.
 */
struct vfs_handle *vfs_handle_acquire(int fd) {
  struct vfs_handle *h = get_handle(fd);

  if (!h)
    return 0;
  __atomic_fetch_add(&h->refcount, 1, __ATOMIC_RELAXED);
  return h;
}

isize vfs_handle_write(struct vfs_handle *h, const void *buf, usize size) {
  if (!h || !h->ops || !h->ops->write)
    return -EBADF;
  return h->ops->write(h, (const char *)buf, size);
}

isize vfs_handle_read(struct vfs_handle *h, void *buf, usize size) {
  if (!h || !h->ops || !h->ops->read)
    return -EBADF;
  return h->ops->read(h, (char *)buf, size);
}

isize vfs_write(int fd, const char *buf, usize size) {
  struct vfs_handle *h = get_handle(fd);
  if (!h || !h->ops || !h->ops->write)
    return -EBADF;

  struct vfs_mount_entry *mnt = vfs_get_mount_for_node(h->node);
  if (mnt && (mnt->flags & MS_RDONLY))
    return -EROFS;

  return h->ops->write(h, buf, size);
}

int vfs_poll(int fd, struct b1nix_pollfd *pfd) {
  struct vfs_handle *h = get_handle(fd);
  if (!h || !h->ops || !h->ops->poll) {
    pfd->revents = B1NIX_POLLNVAL;
    return -1;
  }
  int res = h->ops->poll(h, pfd);
  pfd->revents &= (pfd->events | B1NIX_POLLERR | B1NIX_POLLHUP | B1NIX_POLLNVAL);
  return res;
}

/* Close an open-file handle outside any fd table — the shared close tail of
 * vfs_close and of task teardown freeing the last reference to a CLONE_FILES
 * fd table (where the dying task's fds are no longer reachable by index). */
void vfs_close_handle(struct vfs_handle *h, int owner_pid) {
  if (!h)
    return;

  if (h->kind == VFS_HANDLE_NODE && h->node && h->node->inode) {
    filelock_release_all_by_pid_inode(owner_pid, h->node->inode);

    if (h->flags & (B1NIX_O_WRONLY | B1NIX_O_RDWR)) {
      /* Hold the inode lock across the flush: writeback drops the page-cache
       * lock around write_cb while reading the frame, and a concurrent
       * ftruncate's page_cache_truncate_inode would otherwise memset that live
       * frame mid-DMA. read/write/truncate all serialize on this same lock. */
      vfs_inode_lock(h->node->inode);
      page_cache_flush_inode(h->node->inode);
      vfs_inode_unlock(h->node->inode);
    }
  }

  if (h->ops && h->ops->close)
    h->ops->close(h);

  vfs_handle_release(h);
}

void vfs_close(int fd) {
  /* Take (fetch-and-clear) the slot atomically: two threads racing close()
   * on the same fd of a shared table must not both run the release path. */
  struct vfs_handle *h = scheduler_fd_take(fd);
  if (!h)
    return;

  /* Pass the PROCESS (thread-group) id: POSIX file locks are process-owned, so
   * release must use the same key filelock_set_lock stored. */
  vfs_close_handle(h, current_task ? (int)task_tgid(current_task) : 0);
}

static int vfs_create_at_internal(const char *resolved_path, u32 mode) {
  int res = 0;
  char *p_path = kmalloc(VFS_MAX_PATH);
  char name[VFS_NAME_MAX];
  if (!p_path)
    return -ENOMEM;
  if (split_parent_path(resolved_path, p_path, VFS_MAX_PATH, name, sizeof(name)) < 0) {
    kfree(p_path);
    return -EINVAL;
  }

  struct vfs_node *parent = vfs_find_node_internal(p_path, 1, 0);
  if (IS_ERR(parent)) {
    kfree(p_path);
    return (int)PTR_ERR(parent);
  }
  kfree(p_path);

  struct vfs_node *node = 0;
  vfs_inode_lock(parent->inode);
  struct vfs_mount_entry *mnt = vfs_get_mount_for_node(parent);
  if (mnt && (mnt->flags & MS_RDONLY)) {
    res = -EROFS;
    goto out_unlock;
  }
  struct vfs_node *existing_child = find_child(parent, name);
  if (existing_child) {
    vfs_node_put(existing_child); /* Drop ref from find_child */
    res = -EEXIST;
    goto out_unlock;
  }
  const struct cred *cred = get_current_cred();
  if (cred && !vfs_get_node_perm(parent, cred, 2)) {
    res = -EACCES;
    goto out_unlock;
  }

  node = alloc_node();
  if (!node) {
    res = -ENOMEM;
    goto out_unlock;
  }
  node->inode = alloc_inode();
  if (!node->inode) {
    memset(node, 0, sizeof(*node));
    res = -ENOMEM;
    goto out_node_put;
  }

  node->inode->blk_dev = parent->inode->blk_dev;
  node->inode->fs_id   = parent->inode->fs_id;
  copy_path(node->name, VFS_NAME_MAX, name);
  node->inode->type = VFS_FILE;
  /* One link: the name that was just created.
   *
   * Left at zero, every freshly created file stat'd as already unlinked, and
   * programs check — systemd-journald refuses to append to a journal whose
   * st_nlink is 0 (EIDRM, "the file has been deleted"), so it created the
   * journal, wrote its header, decided the file was gone and removed it, on
   * every start, forever. */
  node->inode->nlink = 1;
  node->parent = parent;

  u16 umask = scheduler_get_current_umask();
  node->inode->mode = mode & ~umask;
  node->inode->uid = cred ? cred->euid : ROOT_UID;
  node->inode->gid = cred ? cred->egid : ROOT_GID;
  vfs_init_times(node->inode);

  {
    u64 _tlflags;
    vfs_tree_write_acquire(&_tlflags);
    node->next_sibling = parent->first_child;
    parent->first_child = node;
    vfs_tree_write_release(_tlflags);
  }

  if (parent->inode->create_cb) {
    int err = parent->inode->create_cb(parent, name, resolved_path,
                                       node->inode->mode);
    if (err < 0) {
      u64 _tlflags;
      vfs_tree_write_acquire(&_tlflags);
      parent->first_child = node->next_sibling;
      vfs_tree_write_release(_tlflags);
      res = err;
      goto out_node_put;
    }
    node->inode->read_cb = parent->inode->read_cb;
    node->inode->write_cb = parent->inode->write_cb;
    node->inode->create_cb = parent->inode->create_cb;
    node->inode->mkdir_cb = parent->inode->mkdir_cb;
    node->inode->unlink_cb = parent->inode->unlink_cb;
    node->inode->rmdir_cb = parent->inode->rmdir_cb;
    node->inode->rename_cb = parent->inode->rename_cb;
    node->inode->link_cb = parent->inode->link_cb;
    node->inode->poll_cb = parent->inode->poll_cb;
  }
  vfs_dir_changed(parent);
  goto out_unlock;

out_node_put:
  if (node) {
    /* The fresh node was allocated with refcount 0; give it the reference we
     * are about to drop and mark it deleted so vfs_node_put's 0+deleted check
     * actually frees it (a bare put underflows to -1 and leaks). Mirrors the
     * vfs_link error path (R3-15). */
    node->deleted = 1;
    __atomic_store_n(&node->refcount, 1, __ATOMIC_RELAXED);
    vfs_node_put(node);
  }
out_unlock:
  vfs_inode_unlock(parent->inode);
  /* M73 inotify: a new entry in `parent` is IN_CREATE on the directory. */
  if (res == 0 && node)
    vfs_inotify_notify(parent, IN_CREATE, name);
  vfs_node_put(parent);
  return res;
}

int vfs_create(const char *path, u32 mode) {
  char *resolved = kmalloc(VFS_MAX_PATH);
  if (!resolved)
    return -ENOMEM;
  vfs_resolve_path(path, resolved);
  int res = vfs_create_at_internal(resolved, mode);
  kfree(resolved);
  return res;
}

/* mknod(2). FIFOs on a VFS-owned directory (/dev, /run, /tmp) are plain
 * in-memory nodes; on a real filesystem the driver's mknod_cb creates a genuine
 * on-disk FIFO inode, so the node survives a remount. A filesystem without a
 * mknod_cb reports -EOPNOTSUPP rather than silently creating a regular file.
 * S_IFREG is forwarded to the ordinary create path.
 *
 * M109: character and block special files are creatable too, because that is
 * the whole of what a hot-plug helper does — mdev reads a uevent and mknod()s
 * the node it names, and refusing it left /dev frozen at whatever boot had
 * created. Such a node is always an in-memory one, never written to the
 * underlying filesystem: a device number is a property of the running kernel
 * (Linux keeps them on devtmpfs for exactly this reason), and the on-disk
 * mknod_cb of every filesystem here writes a FIFO inode, which is not what was
 * asked for. A block node is bound to the device its number names, so it reads
 * and writes like the one the block layer created at boot; an unknown number
 * yields ENODEV rather than a node that looks like a disk and is not one. */
int vfs_mknod(const char *path, u32 mode, u64 dev) {
  if (!path)
    return -EINVAL;
  u32 fmt = mode & B1NIX_S_IFMT;
  if (fmt == 0 || fmt == B1NIX_S_IFREG)
    return vfs_create(path, mode & 07777);
  int is_dev = (fmt == B1NIX_S_IFBLK || fmt == B1NIX_S_IFCHR);
  if (fmt != B1NIX_S_IFIFO && !is_dev)
    return -EPERM;

  char *resolved = kmalloc(VFS_MAX_PATH);
  if (!resolved)
    return -ENOMEM;
  vfs_resolve_path(path, resolved);

  char *p_path = kmalloc(VFS_MAX_PATH);
  char name[VFS_NAME_MAX];
  if (!p_path) {
    kfree(resolved);
    return -ENOMEM;
  }
  if (split_parent_path(resolved, p_path, VFS_MAX_PATH, name, sizeof(name)) < 0) {
    kfree(p_path);
    kfree(resolved);
    return -EINVAL;
  }
  struct vfs_node *parent = vfs_find_node_internal(p_path, 1, 0);
  kfree(p_path);
  kfree(resolved);
  if (IS_ERR(parent))
    return (int)PTR_ERR(parent);

  int res = 0;
  struct vfs_node *node = 0;
  vfs_inode_lock(parent->inode);
  if (parent->inode->type != VFS_DIRECTORY) {
    res = -ENOTDIR;
    goto out_unlock;
  }
  struct vfs_mount_entry *mnt = vfs_get_mount_for_node(parent);
  if (mnt && (mnt->flags & MS_RDONLY)) {
    res = -EROFS;
    goto out_unlock;
  }
  struct vfs_node *existing = find_child(parent, name);
  if (existing) {
    vfs_node_put(existing);
    res = -EEXIST;
    goto out_unlock;
  }
  const struct cred *cred = get_current_cred();
  if (cred && !vfs_get_node_perm(parent, cred, 2)) {
    res = -EACCES;
    goto out_unlock;
  }
  /* Creating a device node is creating an alias for the hardware itself: with
   * no check, any process could mknod a block node for the root disk somewhere
   * it can write and read the filesystem straight out from under its
   * permissions. Linux gates it on CAP_MKNOD and so does this. */
  if (is_dev && cred && cred->euid != ROOT_UID &&
      !cred_has_cap(cred, CAP_MKNOD)) {
    res = -EPERM;
    goto out_unlock;
  }
  if (!is_dev && parent->inode->create_cb && !parent->inode->mknod_cb) {
    res = -EOPNOTSUPP;
    goto out_unlock;
  }

  node = alloc_node();
  if (!node) {
    res = -ENOMEM;
    goto out_unlock;
  }
  node->inode = alloc_inode();
  if (!node->inode) {
    memset(node, 0, sizeof(*node));
    node->deleted = 1;
    __atomic_store_n(&node->refcount, 1, __ATOMIC_RELAXED);
    vfs_node_put(node);
    node = 0;
    res = -ENOMEM;
    goto out_unlock;
  }
  copy_path(node->name, VFS_NAME_MAX, name);
  node->inode->type = is_dev ? VFS_DEVICE : VFS_FIFO;
  node->inode->fs_id = parent->inode->fs_id;
  node->parent = parent;
  node->inode->mode = (mode & 07777) & ~scheduler_get_current_umask();
  node->inode->uid = cred ? cred->euid : ROOT_UID;
  node->inode->gid = cred ? cred->egid : ROOT_GID;
  node->inode->nlink = 1;
  vfs_init_times(node->inode);
  node->inode->blk_dev = parent->inode->blk_dev;
  if (is_dev) {
    /* A special file IS a device; it does not live on the parent's. */
    node->inode->blk_dev = 0;
    node->inode->rdev = dev;
    if (fmt == B1NIX_S_IFBLK && blk_bind_dev_node(node, dev) < 0) {
      node->deleted = 1;
      __atomic_store_n(&node->refcount, 1, __ATOMIC_RELAXED);
      vfs_node_put(node);
      node = 0;
      res = -ENODEV;
      goto out_unlock;
    }
  }
  {
    u64 _tlflags;
    vfs_tree_write_acquire(&_tlflags);
    node->next_sibling = parent->first_child;
    parent->first_child = node;
    vfs_tree_write_release(_tlflags);
  }

  if (!is_dev && parent->inode->mknod_cb) {
    int err = parent->inode->mknod_cb(parent, name, node->inode->mode);
    if (err < 0) {
      u64 _tlflags;
      vfs_tree_write_acquire(&_tlflags);
      parent->first_child = node->next_sibling;
      vfs_tree_write_release(_tlflags);
      node->deleted = 1;
      __atomic_store_n(&node->refcount, 1, __ATOMIC_RELAXED);
      vfs_node_put(node);
      node = 0;
      res = err;
      goto out_unlock;
    }
    /* The driver attached its per-inode state and unlink/setattr hooks; the
     * node type stays VFS_FIFO so opens go to the pipe path, not to the
     * filesystem's read/write callbacks. */
    node->inode->type = VFS_FIFO;
  }

out_unlock:
  if (res == 0 && node)
    vfs_dir_changed(parent);
  vfs_inode_unlock(parent->inode);
  if (res == 0 && node)
    vfs_inotify_notify(parent, IN_CREATE, name);
  vfs_node_put(parent);
  return res;
}

struct vfs_node *vfs_find_node_by_fd(int fd) {
  struct vfs_handle *h = get_handle(fd);
  if (!h || h->kind != VFS_HANDLE_NODE)
    return ERR_PTR(-EBADF);
  /* A node-kind handle need not have a node: the imported DRM core hands out
   * descriptors that carry their object in private_data and nothing else (see
   * lkpi_handle_alloc). Returning the NULL walked straight into a dereference —
   * and because the low physical memory is mapped, reading through it did not
   * even fault where it happened: it read the BIOS interrupt table and used
   * that as a pointer, which is how an fstat on a compositor's descriptor
   * became a general-protection fault. */
  if (!h->node)
    return ERR_PTR(-EBADF);
  return h->node;
}

static int vfs_mkdir_at_internal(const char *resolved_path, u32 mode) {
  if (strcmp(resolved_path, "/") == 0) {
    return -EEXIST;
  }
  int res = 0;
  char *p_path = kmalloc(VFS_MAX_PATH);
  char name[VFS_NAME_MAX];
  if (!p_path)
    return -ENOMEM;
  if (split_parent_path(resolved_path, p_path, VFS_MAX_PATH, name, sizeof(name)) < 0) {
    kfree(p_path);
    return -EINVAL;
  }

  struct vfs_node *parent = vfs_find_node_internal(p_path, 1, 0);
  if (IS_ERR(parent)) {
    kfree(p_path);
    return (int)PTR_ERR(parent);
  }
  kfree(p_path);

  struct vfs_node *node = 0;
  vfs_inode_lock(parent->inode);
  struct vfs_mount_entry *mnt = vfs_get_mount_for_node(parent);
  if (mnt && (mnt->flags & MS_RDONLY)) {
    res = -EROFS;
    goto out_unlock;
  }
  struct vfs_node *existing_child = find_child(parent, name);
  if (existing_child) {
    vfs_node_put(existing_child); /* Drop ref from find_child */
    res = -EEXIST;
    goto out_unlock;
  }
  const struct cred *cred = get_current_cred();
  if (cred && !vfs_get_node_perm(parent, cred, 2)) {
    res = -EACCES;
    goto out_unlock;
  }

  node = alloc_node();
  if (!node) {
    res = -ENOMEM;
    goto out_unlock;
  }
  node->inode = alloc_inode();
  if (!node->inode) {
    memset(node, 0, sizeof(*node));
    res = -ENOMEM;
    goto out_node_put;
  }

  node->inode->blk_dev = parent->inode->blk_dev;
  node->inode->fs_id   = parent->inode->fs_id;
  copy_path(node->name, VFS_NAME_MAX, name);
  node->inode->type = VFS_DIRECTORY;
  /* A directory has two links of its own ("." and its name in the parent), and
   * the parent gains one for the new directory's "..". */
  node->inode->nlink = 2;
  if (parent->inode->nlink > 0)
    parent->inode->nlink++;
  node->parent = parent;

  u16 umask = scheduler_get_current_umask();
  node->inode->mode = mode & ~umask;
  node->inode->uid = cred ? cred->euid : ROOT_UID;
  node->inode->gid = cred ? cred->egid : ROOT_GID;
  vfs_init_times(node->inode);

  {
    u64 _tlflags;
    vfs_tree_write_acquire(&_tlflags);
    node->next_sibling = parent->first_child;
    parent->first_child = node;
    vfs_tree_write_release(_tlflags);
  }

  if (parent->inode->mkdir_cb) {
    int err = parent->inode->mkdir_cb(parent, name, node->inode->mode);
    if (err < 0) {
      u64 _tlflags;
      vfs_tree_write_acquire(&_tlflags);
      parent->first_child = node->next_sibling;
      vfs_tree_write_release(_tlflags);
      res = err;
      goto out_node_put;
    }
    node->inode->read_cb = parent->inode->read_cb;
    node->inode->write_cb = parent->inode->write_cb;
    node->inode->create_cb = parent->inode->create_cb;
    node->inode->mkdir_cb = parent->inode->mkdir_cb;
    node->inode->unlink_cb = parent->inode->unlink_cb;
    node->inode->rmdir_cb = parent->inode->rmdir_cb;
    node->inode->rename_cb = parent->inode->rename_cb;
    node->inode->link_cb = parent->inode->link_cb;
    node->inode->poll_cb = parent->inode->poll_cb;
  }
  vfs_dir_changed(parent);
  goto out_unlock;

out_node_put:
  if (node) {
    /* See vfs_create_at_internal: refcount-0 node needs deleted=1 + refcount=1
     * before the put or it underflows and leaks (R3-15). */
    node->deleted = 1;
    __atomic_store_n(&node->refcount, 1, __ATOMIC_RELAXED);
    vfs_node_put(node);
  }
out_unlock:
  vfs_inode_unlock(parent->inode);
  vfs_node_put(parent);
  return res;
}

int vfs_mkdir(const char *path, u32 mode) {
  char *resolved = kmalloc(VFS_MAX_PATH);
  if (!resolved)
    return -ENOMEM;
  vfs_resolve_path(path, resolved);
  int res = vfs_mkdir_at_internal(resolved, mode);
  kfree(resolved);
  return res;
}

isize vfs_list(const char *dir_path, const char **names, usize max_names) {
  struct vfs_node *dir = vfs_find_node(dir_path);
  if (IS_ERR(dir))
    return PTR_ERR(dir);
  if (dir->inode->type != VFS_DIRECTORY) {
    vfs_node_put(dir);
    return -ENOTDIR;
  }

  int res = vfs_check_access(dir, R_OK);
  if (res != 0) {
    vfs_node_put(dir);
    return 0;
  }

  vfs_inode_lock_read(dir->inode);
  usize count = 0;
  /* The sibling list (first_child / next_sibling) is NOT protected by the
   * inode rwlock — that lock guards inode data (size, timestamps, etc.).
   * A concurrent unlink can splice a child out of the list, set
   * child->deleted = 1, drop its ref, and free it while we walk — a #GP on
   * child->deleted. The bare cli/sti this used before only blocked SAME-CPU
   * preemption; under SMP another core mutates the list in parallel. Take the
   * vfs_tree_lock read side (the rwlock unlink holds for write over its
   * splice), exactly like find_child. It is irqsave, so the old same-CPU
   * guarantee is preserved too. Order: inode lock then tree lock, per the
   * M28 DAG; the walk does not yield. */
  u64 tflags;
  vfs_tree_read_acquire(&tflags);
  struct vfs_node *child = dir->first_child;
  while (child && count < max_names) {
    if (!child->deleted) {
      names[count++] = child->name;
    }
    child = child->next_sibling;
  }
  vfs_tree_read_release(tflags);
  vfs_inode_unlock_read(dir->inode);
  vfs_node_put(dir);
  return (isize)count;
}

static u32 vfs_node_type_mode(const struct vfs_node *node) {
  if (!node || !node->inode)
    return B1NIX_S_IFREG;
  if (node->inode->type == VFS_DIRECTORY)
    return B1NIX_S_IFDIR;
  /* A /proc or /sys file: served by a callback, but a regular file to anyone
   * who stats it (see VFS_NODE_PSEUDO_REG). */
  if (node->inode->flags & VFS_NODE_PSEUDO_REG)
    return B1NIX_S_IFREG;
  if (node->inode->type == VFS_DEVICE)
    /* A device node backed by a block_device is a block device, and tools check
     * that: BusyBox losetup refuses any target that is not S_ISBLK, so with
     * every device reported as a character device `losetup <dev> <file>` died
     * before it ever issued an ioctl. */
    return node->inode->blk_dev ? B1NIX_S_IFBLK : B1NIX_S_IFCHR;
  if (node->inode->type == VFS_SYMLINK)
    return B1NIX_S_IFLNK;
  if (node->inode->type == VFS_FIFO)
    return B1NIX_S_IFIFO;
  if (node->inode->type == VFS_SOCKET)
    return B1NIX_S_IFSOCK;
  return B1NIX_S_IFREG;
}
static int vfs_stat_node(struct vfs_node *node, struct b1nix_stat *st) {
  if (!node || !node->inode)
    return -ENOENT;
  if (!st)
    return -EINVAL;

  struct vfs_inode *inode = node->inode;
  /* Let a synthetic filesystem bring its computed fields up to date first —
   * before the read lock, because refreshing them takes the write side. */
  if (inode->getattr_cb)
    inode->getattr_cb(node);
  if (inode && inode->ino && inode->fs_id) {
    struct vfs_inode *cached = icache_get(inode->fs_id, inode->ino);
    if (!cached) {
      icache_insert(inode->fs_id, inode->ino, inode);
    }
  }
  vfs_inode_lock_read(inode);
  memset(st, 0, sizeof(*st));
  st->st_ino = inode->ino;
  st->st_uid = inode->uid;
  st->st_gid = inode->gid;
  st->st_size = inode->size;
  st->st_blksize = 512;
  st->st_blocks = (inode->size + 511) / 512;
  st->st_nlink = (u32)inode->nlink;
  st->st_mode = vfs_node_type_mode(node) | (inode->mode & 07777);

  st->st_atim.tv_sec = inode->atime;
  st->st_atim.tv_nsec = inode->atime_nsec;
  st->st_mtim.tv_sec = inode->mtime;
  st->st_mtim.tv_nsec = inode->mtime_nsec;
  st->st_ctim.tv_sec = inode->ctime;
  st->st_ctim.tv_nsec = inode->ctime_nsec;

  /* vfs_node_dev, not a raw field read: a filesystem that populated its tree
   * inside its own mount callback left these nodes unstamped. */
  st->st_dev = vfs_node_dev(node);
  if (inode->type == VFS_DEVICE) {
    st->st_rdev = inode->rdev;
  }
  vfs_inode_unlock_read(inode);
  return 0;
}

int vfs_stat(const char *path, struct b1nix_stat *st) {
  if (!path || !st)
    return -EINVAL;
  struct vfs_node *node = vfs_find_node(path);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);
  int res = vfs_stat_node(node, st);
  vfs_node_put(node);
  return res;
}

/* statfs describes the FILESYSTEM a node lives on, not the node. Only mount
 * roots carry a statfs_cb, so asking about any other path used to report
 * ENOSYS — which is not "this filesystem has no answer", it is "this kernel
 * does not implement statfs", and callers believe it. systemd decides whether
 * the machine has a unified cgroup hierarchy by statfs()ing /sys/fs/cgroup and
 * comparing f_type; an ENOSYS there sends it down the cgroup v1 path, which
 * this kernel does not have, and PID 1 freezes. Resolve the node's mount and
 * ask its root. */
static int vfs_statfs_node(struct vfs_node *node, struct b1nix_statfs *st) {
  if (node->inode->statfs_cb)
    return node->inode->statfs_cb(node, st);
  struct vfs_mount_entry *mnt = vfs_get_mount_for_node(node);
  if (mnt && mnt->root_node && mnt->root_node->inode &&
      mnt->root_node->inode->statfs_cb)
    return mnt->root_node->inode->statfs_cb(mnt->root_node, st);
  return -ENOSYS;
}

int vfs_statfs(const char *path, struct b1nix_statfs *st) {
  struct vfs_node *node = vfs_find_node(path);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);

  int res = vfs_statfs_node(node, st);
  if (bootinfo_has_flag("b1nix.trace-statfs")) {
    char line[192];
    snprintf(line, sizeof(line),
             "statfs: '%s' -> node %p '%s' cb=%d res=%d type=0x%llx", path,
             (void *)node, node->name, node->inode->statfs_cb ? 1 : 0, res,
             res == 0 ? (unsigned long long)st->f_type : 0ull);
    klog_info(line);
  }
  vfs_node_put(node);
  return res;
}

int vfs_lstat(const char *path, struct b1nix_stat *st) {
  if (!path || !st)
    return -EINVAL;
  struct vfs_node *node = vfs_find_node_no_follow(path);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);
  int res = vfs_stat_node(node, st);
  vfs_node_put(node);
  return res;
}

isize vfs_lseek(int handle, isize offset, int whence) {
  struct vfs_handle *h = get_handle(handle);
  if (!h)
    return -EBADF;
  /* A pipe, socket or anonymous object has no file position. POSIX says
   * ESPIPE, not EBADF — GNU head asks to seek backwards on standard input
   * and takes EBADF as "this descriptor is broken", which is how a plain
   * `cmd | head -1` came out as an error rather than a short read. */
  if (h->kind != VFS_HANDLE_NODE)
    return h->kind == VFS_HANDLE_NONE ? -EBADF : -ESPIPE;
  isize base = 0;
  if (whence == B1NIX_SEEK_SET)
    base = 0;
  else if (whence == B1NIX_SEEK_CUR)
    base = (isize)h->offset;
  else if (whence == B1NIX_SEEK_END)
    base = (h->node && h->node->inode) ? (isize)h->node->inode->size : 0;
  else
    return -EINVAL;

  isize next = base + offset;
  if (next < 0)
    return -EINVAL;
  h->offset = (usize)next;
  return next;
}

/* Split "/a/b/c" into "/a/b" and "c".
 *
 * Both destinations are bounded now. They were not: `name` was copied with a
 * memcpy sized from the path, into buffers every caller declares as 64 bytes.
 * A final component longer than that — a browser's cache files are — wrote
 * past the end of a caller's stack array and over whatever followed it, which
 * showed up as a general-protection fault inside strncpy on a "pointer" made
 * of filename characters. A name that does not fit is now ENAMETOOLONG, which
 * is also the truth: a b1nix directory entry holds 63 characters. */
static int split_parent_path(const char *path, char *parent_path,
                             usize parent_size, char *name, usize name_size) {
  if (!path || path[0] == '\0' || !parent_path || !name || parent_size < 2 ||
      name_size < 2)
    return -1;

  char local_path[VFS_MAX_PATH];
  strncpy(local_path, path, VFS_MAX_PATH - 1);
  local_path[VFS_MAX_PATH - 1] = '\0';

  usize len = strlen(local_path);
  while (len > 1 && local_path[len - 1] == '/') {
    local_path[len - 1] = '\0';
    len--;
  }

  if (len == 0 || len >= 256)
    return -1;
  isize last_slash = -1;
  for (isize i = (isize)len - 1; i >= 0; i--) {
    if (local_path[i] == '/') {
      last_slash = i;
      break;
    }
  }

  if (last_slash < 0) {
    if (len + 1 > name_size)
      return -ENAMETOOLONG;
    parent_path[0] = '/';
    parent_path[1] = '\0';
    memcpy(name, local_path, len + 1);
    return 0;
  }

  if ((usize)last_slash == len - 1)
    return -1;
  usize name_len = len - (usize)last_slash - 1;
  if (name_len + 1 > name_size)
    return -ENAMETOOLONG;
  if (last_slash == 0) {
    parent_path[0] = '/';
    parent_path[1] = '\0';
  } else {
    if ((usize)last_slash + 1 > parent_size)
      return -ENAMETOOLONG;
    memcpy(parent_path, local_path, (usize)last_slash);
    parent_path[last_slash] = '\0';
  }
  memcpy(name, local_path + last_slash + 1, name_len + 1);
  return 0;
}

/* Core child-removal logic. The caller MUST already hold parent->inode's lock
 * and a ref on parent; this neither locks/unlocks the inode nor drops the
 * parent ref. `r_path` is the fully-resolved path of the child (used only for
 * the mount-point guard); `name` is its final component. Shared by
 * vfs_remove_node (which takes the lock) and vfs_rename_internal, which already
 * holds the new-parent lock when replacing an existing rename target — calling
 * the lock-taking vfs_remove_node there self-deadlocks on the parent inode. */
static int vfs_remove_child_locked(struct vfs_node *parent, const char *r_path,
                                   const char *name, int is_rmdir) {
  struct vfs_mount_entry *mnt = vfs_get_mount_for_node(parent);
  if (mnt && (mnt->flags & MS_RDONLY))
    return -EROFS;
  const struct cred *cred = get_current_cred();
  if (cred && !vfs_get_node_perm(parent, cred, 2))
    return -EACCES;

  /* Защита точек монтирования */
  for (int i = 0; i < (int)mount_slots; i++) {
    if (mount_visible(i) && strcmp(mounts[i].target, r_path) == 0) {
      if (bootinfo_has_flag("b1nix.trace-mount")) {
        char bl[320];
        snprintf(bl, sizeof(bl), "unlink EBUSY: '%s' is mount target %d", r_path, i);
        klog_info(bl);
      }
      return -EBUSY;
    }
  }

  struct vfs_node *prev = 0, *child = parent->first_child;
  while (child) {
    if (!child->deleted && strcmp(child->name, name) == 0) {
      if (cred && (parent->inode->mode & B1NIX_S_ISVTX)) {
        if (cred->euid != ROOT_UID && cred->euid != child->inode->uid && cred->euid != parent->inode->uid)
          return -EACCES;
      }
      /* M109 chattr: an immutable or append-only file keeps its name — Linux
       * refuses to unlink either one, and so does this. */
      if (child->inode->attr & (VFS_ATTR_IMMUTABLE | VFS_ATTR_APPEND))
        return -EPERM;
      if (is_rmdir) {
        if (child->inode->type != VFS_DIRECTORY)
          return -ENOTDIR;
        /* A directory whose children are the filesystem's own control files
         * (a cgroup) is never "not empty" because of them — its rmdir_cb makes
         * that call. See VFS_NODE_CTRL_CHILDREN. */
        if (child->first_child &&
            !(child->inode->flags & VFS_NODE_CTRL_CHILDREN))
          return -ENOTEMPTY;
      } else {
        if (child->inode->type == VFS_DIRECTORY)
          return -EISDIR;
      }
      if (parent->inode->unlink_cb && !is_rmdir) {
        int err = parent->inode->unlink_cb(parent, name);
        /* -ENOENT means the filesystem has no directory entry of this name,
         * which is not a failure to remove: the entry exists ONLY in memory.
         * Device nodes are exactly that — mknod(2) keeps a character or block
         * special file in memory (a device number is a property of the running
         * kernel, never of the image), and the kernel plants /dev/<disk> the
         * same way at boot. With the filesystem's ENOENT vetoing the removal,
         * nothing could ever unlink one of those: `rm /dev/loop0` failed, and
         * so did mdev removing the node for a device that had gone away, which
         * is half of what a hot-plug helper does. Every other error still
         * stops the unlink. */
        if (err < 0 && err != -ENOENT)
          return err;
      } else if (parent->inode->rmdir_cb && is_rmdir) {
        int err = parent->inode->rmdir_cb(parent, name);
        if (err < 0)
          return err;
      }

      vfs_node_get(child);
      child->deleted = 1;
      child->inode->nlink--;
      if (child->inode)
        icache_invalidate(child->inode->fs_id, child->inode->ino);
      {
        u64 _tlflags;
        vfs_tree_write_acquire(&_tlflags);
        if (prev)
          prev->next_sibling = child->next_sibling;
        else
          parent->first_child = child->next_sibling;
        vfs_tree_write_release(_tlflags);
      }
      vfs_node_put(child);
      dcache_invalidate(parent, name);
      vfs_dir_changed(parent);
      return 0;
    }
    prev = child;
    child = child->next_sibling;
  }
  return -ENOENT;
}

static int vfs_remove_node(const char *path, int is_rmdir) {
  char r_path[VFS_MAX_PATH];
  vfs_resolve_path(path, r_path);
  char p_path[VFS_MAX_PATH], name[VFS_NAME_MAX];
  split_parent_path(r_path, p_path, sizeof(p_path), name, sizeof(name));
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    return -EINVAL;

  struct vfs_node *parent = vfs_find_node(p_path);
  if (IS_ERR(parent))
    return (int)PTR_ERR(parent);

  /* Reference the entry before it is unlinked so IN_DELETE_SELF can name it
   * after the removal has already dropped it from the tree. */
  struct vfs_node *victim = 0;
  vfs_inode_lock(parent->inode);
  victim = find_child(parent, name);
  if (victim)
    victim = vfs_node_get(victim);
  int res = vfs_remove_child_locked(parent, r_path, name, is_rmdir);
  vfs_inode_unlock(parent->inode);
  /* M73 inotify: a removed entry is IN_DELETE on the parent directory. M107:
   * and IN_DELETE_SELF on the entry itself, which is what a watch placed
   * directly on a file (rather than on its directory) is waiting for. */
  if (res == 0) {
    vfs_inotify_notify(parent, IN_DELETE | (is_rmdir ? IN_ISDIR : 0), name);
    if (victim)
      vfs_inotify_notify(victim, IN_DELETE_SELF | (is_rmdir ? IN_ISDIR : 0), 0);
  }
  if (victim)
    vfs_node_put(victim);
  vfs_node_put(parent);
  return res;
}

int vfs_unlink(const char *path) {
  return vfs_remove_node(path, 0);
}

int vfs_link(const char *target, const char *link_path) {
  int res = 0;
  struct vfs_node *target_node = vfs_find_node(target);
  struct vfs_node *parent = 0;
  if (IS_ERR(target_node)) {
    res = (int)PTR_ERR(target_node);
    goto out;
  }
  if (target_node->inode->type == VFS_DIRECTORY) {
    res = -EPERM;
    goto out;
  }

  struct vfs_node *existing = vfs_find_node_no_follow(link_path);
  if (!IS_ERR(existing)) {
    vfs_node_put(existing);
    res = -EEXIST;
    goto out;
  }

  char parent_path[VFS_MAX_PATH], name[VFS_NAME_MAX];
  if (split_parent_path(link_path, parent_path, sizeof(parent_path), name, sizeof(name)) < 0) {
    res = -EINVAL;
    goto out;
  }

  parent = vfs_find_node(parent_path);
  if (IS_ERR(parent)) {
    res = (int)PTR_ERR(parent);
    goto out;
  }
  if (parent->inode->type != VFS_DIRECTORY) {
    res = -ENOTDIR;
    goto out;
  }

  vfs_inode_lock(parent->inode);
  {
    struct vfs_mount_entry *lmnt = vfs_get_mount_for_node(parent);
    if (lmnt && (lmnt->flags & MS_RDONLY)) {
      vfs_inode_unlock(parent->inode);
      res = -EROFS;
      goto out;
    }
  }
  res = vfs_check_access(parent, W_OK);
  if (res != 0) {
    vfs_inode_unlock(parent->inode);
    goto out;
  }

  struct vfs_node *new_node = alloc_node();
  if (!new_node) {
    vfs_inode_unlock(parent->inode);
    res = -ENOMEM;
    goto out;
  }

  copy_path(new_node->name, VFS_NAME_MAX, name);
  /* A hard link is a second node over one inode, and each node releases one
   * inode reference when it is freed -- so the second node has to take one. */
  new_node->inode = vfs_inode_get(target_node->inode);
  new_node->inode->nlink++;
  new_node->parent = parent;
  {
    u64 _tlflags;
    vfs_tree_write_acquire(&_tlflags);
    new_node->next_sibling = parent->first_child;
    parent->first_child = new_node;
    vfs_tree_write_release(_tlflags);
  }

  if (parent->inode->link_cb) {
    res = parent->inode->link_cb(target_node, parent, name);
    if (res < 0) {
      u64 _tlflags;
      vfs_tree_write_acquire(&_tlflags);
      parent->first_child = new_node->next_sibling;
      vfs_tree_write_release(_tlflags);
      new_node->inode->nlink--;
      vfs_inode_put(new_node->inode); /* the ref taken just above */
      new_node->inode = 0;
      /* The fresh node was allocated with refcount 0; give it the reference
       * we are about to drop and mark it deleted so vfs_node_put's 0+deleted
       * check actually frees it (a bare put underflowed to -1 and leaked). */
      new_node->deleted = 1;
      __atomic_store_n(&new_node->refcount, 1, __ATOMIC_RELAXED);
      vfs_node_put(new_node);
    }
  }
  if (res == 0)
    vfs_dir_changed(parent);
  vfs_inode_unlock(parent->inode);

out:
  if (parent && !IS_ERR(parent))
    vfs_node_put(parent);
  if (target_node && !IS_ERR(target_node))
    vfs_node_put(target_node);
  return res;
}

int vfs_symlink(const char *target, const char *link_path) {
  int res = 0;
  if (!target || target[0] == '\0')
    return -EINVAL;

  char parent_path[VFS_MAX_PATH], name[VFS_NAME_MAX];
  if (split_parent_path(link_path, parent_path, sizeof(parent_path), name, sizeof(name)) < 0)
    return -EINVAL;

  struct vfs_node *parent = vfs_find_node(parent_path);
  if (IS_ERR(parent))
    return (int)PTR_ERR(parent);
  if (parent->inode->type != VFS_DIRECTORY) {
    vfs_node_put(parent);
    return -ENOTDIR;
  }

  struct vfs_node *node = 0;
  vfs_inode_lock(parent->inode);
  struct vfs_node *existing_child = find_child(parent, name);
  if (existing_child) {
    vfs_node_put(existing_child); /* Drop ref from find_child */
    res = -EEXIST;
    goto out_unlock;
  }

  {
    struct vfs_mount_entry *smnt = vfs_get_mount_for_node(parent);
    if (smnt && (smnt->flags & MS_RDONLY)) {
      res = -EROFS;
      goto out_unlock;
    }
  }
  const struct cred *cred = get_current_cred();
  if (cred && !vfs_get_node_perm(parent, cred, 2)) {
    res = -EACCES;
    goto out_unlock;
  }

  usize len = strlen(target);
  if (len >= VFS_MAX_PATH) {
    res = -ENAMETOOLONG;
    goto out_unlock;
  }
  char *target_copy = kmalloc(len + 1);
  if (!target_copy) {
    res = -ENOMEM;
    goto out_unlock;
  }
  memcpy(target_copy, target, len + 1);

  node = alloc_node();
  if (!node) {
    kfree(target_copy);
    res = -ENOMEM;
    goto out_unlock;
  }
  node->inode = alloc_inode();
  if (!node->inode) {
    kfree(target_copy);
    vfs_node_put(node);
    res = -ENOMEM;
    goto out_unlock;
  }

  copy_path(node->name, VFS_NAME_MAX, name);
  node->inode->type = VFS_SYMLINK;
  node->inode->data = target_copy;
  node->inode->size = len;
  node->inode->flags = VFS_NODE_OWNS_DATA;
  node->inode->nlink = 1; /* the name just created */
  node->inode->mode = 0777;
  node->inode->uid = cred ? cred->euid : ROOT_UID;
  node->inode->gid = cred ? cred->egid : ROOT_GID;
  vfs_init_times(node->inode);
  node->parent = parent;
  {
    u64 _tlflags;
    vfs_tree_write_acquire(&_tlflags);
    node->next_sibling = parent->first_child;
    parent->first_child = node;
    vfs_tree_write_release(_tlflags);
  }

  if (parent->inode->symlink_cb) {
    int err = parent->inode->symlink_cb(parent, name, target);
    if (err < 0) {
      u64 _tlflags;
      vfs_tree_write_acquire(&_tlflags);
      parent->first_child = node->next_sibling;
      vfs_tree_write_release(_tlflags);
      /* refcount-0 node: deleted=1 + refcount=1 before the put or it underflows
       * and leaks (R3-15, mirrors vfs_link). */
      node->deleted = 1;
      __atomic_store_n(&node->refcount, 1, __ATOMIC_RELAXED);
      vfs_node_put(node);
      res = err;
      goto out_unlock;
    }
  }

out_unlock:
  if (res == 0)
    vfs_dir_changed(parent);
  vfs_inode_unlock(parent->inode);
  vfs_node_put(parent);
  return res;
}

isize vfs_readlink(const char *path, char *buffer, usize size) {
  isize res = 0;
  {
    /* Once, not per readlink: bootinfo_has_flag scans the command line. */
    static int trace = -1;
    if (trace < 0)
      trace = bootinfo_has_flag("b1nix.trace-mount") ? 1 : 0;
    if (trace && path) {
      char rl[320];
      snprintf(rl, sizeof(rl), "readlink: '%s'", path);
      klog_info(rl);
    }
  }
  if (!path || !buffer || size == 0) {
    res = -EINVAL;
    goto out;
  }
  struct vfs_node *node = vfs_find_node_no_follow(path);
  if (IS_ERR(node)) {
    res = PTR_ERR(node);
    goto out;
  }

  vfs_inode_lock_read(node->inode);
  if (node->inode->type != VFS_SYMLINK) {
    vfs_inode_unlock_read(node->inode);
    vfs_node_put(node);
    res = -EINVAL;
    goto out;
  }
  /* Dynamic symlink target: a read_cb renders the target per-caller (e.g.
   * procfs /proc/self/exe, which resolves to the calling task's exe path).
   * Call it without the inode lock — the callback may take other locks. */
  if (node->inode->read_cb) {
    isize (*rcb)(struct vfs_node *, u64, char *, usize, int) = node->inode->read_cb;
    vfs_inode_unlock_read(node->inode);
    res = rcb(node, 0, buffer, size, 0);
    vfs_node_put(node);
    goto out;
  }
  if (!node->inode->data) {
    vfs_inode_unlock_read(node->inode);
    vfs_node_put(node);
    res = -EINVAL;
    goto out;
  }

  usize len = node->inode->size;
  if (len > size)
    len = size;
  memcpy(buffer, node->inode->data, len);
  res = (isize)len;
  vfs_inode_unlock_read(node->inode);
  vfs_node_put(node);

out:
  return res;
}

static int vfs_rename_internal(const char *old_path, const char *new_path) {
  int res = 0;
  char old_res[VFS_MAX_PATH], new_res[VFS_MAX_PATH];
  vfs_resolve_path(old_path, old_res);
  vfs_resolve_path(new_path, new_res);
  if (strcmp(old_res, new_res) == 0)
    return 0;

  char old_p[VFS_MAX_PATH], old_n[VFS_NAME_MAX], new_p[VFS_MAX_PATH], new_n[VFS_NAME_MAX];
  split_parent_path(old_res, old_p, sizeof(old_p), old_n, sizeof(old_n));
  split_parent_path(new_res, new_p, sizeof(new_p), new_n, sizeof(new_n));

  struct vfs_node *old_parent = vfs_find_node(old_p);
  if (IS_ERR(old_parent))
    return (int)PTR_ERR(old_parent);
  struct vfs_node *new_parent = vfs_find_node(new_p);
  if (IS_ERR(new_parent)) {
    vfs_node_put(old_parent);
    return (int)PTR_ERR(new_parent);
  }

  struct vfs_node *node = 0;
  struct vfs_node *existing = 0;

  node = find_child(old_parent, old_n);
  if (!node) {
    res = -ENOENT;
    goto out_put_parents;
  }

  /* M109 chattr: renaming an immutable or append-only file changes its name,
   * which is exactly what those two flags pin down. */
  if (node->inode && (node->inode->attr & (VFS_ATTR_IMMUTABLE | VFS_ATTR_APPEND))) {
    res = -EPERM;
    goto out_put_parents;
  }

  const struct cred *cred = get_current_cred();
  if (cred && (old_parent->inode->mode & B1NIX_S_ISVTX)) {
    if (cred->euid != ROOT_UID && cred->euid != node->inode->uid && cred->euid != old_parent->inode->uid) {
      res = -EACCES;
      goto out_put_parents;
    }
  }

  /* Рекурсивная защита */
  struct vfs_node *tmp = new_parent;
  while (tmp) {
    if (tmp == node) {
      res = -EINVAL;
      goto out_put_parents;
    }
    tmp = tmp->parent;
  }

  /* EXDEV check */
  if (old_parent->inode->fs_id != new_parent->inode->fs_id) {
    res = -EXDEV;
    goto out_put_parents;
  }

  res = vfs_check_access(old_parent, W_OK);
  if (res == 0)
    res = vfs_check_access(new_parent, W_OK);
  if (res != 0) {
    goto out_put_parents;
  }

  /* Lock parents in consistent order */
  struct vfs_node *p1 = old_parent, *p2 = new_parent;
  if (p1 > p2) {
    struct vfs_node *t = p1;
    p1 = p2;
    p2 = t;
  }
  vfs_inode_lock(p1->inode);
  if (p1 != p2)
    vfs_inode_lock(p2->inode);

  struct vfs_mount_entry *mnt_old = vfs_get_mount_for_node(old_parent);
  if (mnt_old && (mnt_old->flags & MS_RDONLY)) {
    res = -EROFS;
    goto out_unlock;
  }
  struct vfs_mount_entry *mnt_new = vfs_get_mount_for_node(new_parent);
  if (mnt_new && (mnt_new->flags & MS_RDONLY)) {
    res = -EROFS;
    goto out_unlock;
  }

  /* POSIX type-consistency checks */
  existing = find_child(new_parent, new_n);
  if (existing) {
    if (cred && (new_parent->inode->mode & B1NIX_S_ISVTX)) {
      if (cred->euid != ROOT_UID && cred->euid != existing->inode->uid && cred->euid != new_parent->inode->uid) {
        res = -EACCES;
        goto out_unlock;
      }
    }
    if (node->inode->type == VFS_DIRECTORY &&
        existing->inode->type != VFS_DIRECTORY) {
      res = -ENOTDIR;
      goto out_unlock;
    }
    if (node->inode->type != VFS_DIRECTORY &&
        existing->inode->type == VFS_DIRECTORY) {
      res = -EISDIR;
      goto out_unlock;
    }
    if (existing->inode->type == VFS_DIRECTORY && existing->first_child) {
      res = -ENOTEMPTY;
      goto out_unlock;
    }
    /* Drop the existing target in place: new_parent->inode is already locked
     * here, so the lock-taking vfs_remove_node() would self-deadlock on it
     * (e.g. `rename("/etc/passwd+", "/etc/passwd")`). The `existing` ref is
     * released at out_put_parents. */
    vfs_remove_child_locked(new_parent, new_res, new_n, 0);
  }

  /* Move the node between parents under the vfs_tree_lock write side
   * (M28-B). Unlink from old_parent then link to new_parent — both halves
   * inside the lock so a concurrent find_child can't observe the node
   * "missing" or doubly-linked. */
  {
    u64 _tlflags;
    vfs_tree_write_acquire(&_tlflags);
    struct vfs_node *prev_c = 0, *c = old_parent->first_child;
    while (c) {
      if (c == node) {
        if (prev_c)
          prev_c->next_sibling = c->next_sibling;
        else
          old_parent->first_child = c->next_sibling;
        break;
      }
      prev_c = c;
      c = c->next_sibling;
    }
    vfs_tree_write_release(_tlflags);
  }

  if (old_parent->inode->rename_cb) {
    int err =
        old_parent->inode->rename_cb(old_parent, old_n, new_parent, new_n);
    if (err < 0) {
      u64 _tlflags;
      vfs_tree_write_acquire(&_tlflags);
      node->next_sibling = old_parent->first_child;
      old_parent->first_child = node;
      vfs_tree_write_release(_tlflags);
      res = err;
      goto out_unlock;
    }
  }

  /* The whole name. A directory entry here holds VFS_NAME_MAX-1 characters and
   * every other creation path stores that many; rename alone capped the copy
   * at 64, so `rename(a, b)` with a `b` longer than 63 characters silently
   * renamed the entry to a shorter, different name — the file then existed
   * under a name nobody would ever look up, and the intended one was ENOENT.
   * Atomic "write to a temporary, rename over the target" is how systemd,
   * dpkg and glibc all replace a file, and their temporary names are long. */
  copy_path(node->name, VFS_NAME_MAX, new_n);
  node->parent = new_parent;
  {
    u64 _tlflags;
    vfs_tree_write_acquire(&_tlflags);
    node->next_sibling = new_parent->first_child;
    new_parent->first_child = node;
    vfs_tree_write_release(_tlflags);
  }
  if (node->inode)
    icache_invalidate(node->inode->fs_id, node->inode->ino);
  dcache_invalidate(old_parent, old_n);
  dcache_invalidate(new_parent, new_n);
  vfs_dir_changed(old_parent);
  if (new_parent != old_parent)
    vfs_dir_changed(new_parent);

out_unlock:
  if (p1 != p2)
    vfs_inode_unlock(p2->inode);
  vfs_inode_unlock(p1->inode);
out_put_parents:
  if (existing)
    vfs_node_put(existing);
  if (node)
    vfs_node_put(node);
  vfs_node_put(new_parent);
  vfs_node_put(old_parent);
  return res;
}

int vfs_rename(const char *old_path, const char *new_path) {
  int res = vfs_rename_internal(old_path, new_path);
  /* M107 inotify: a rename is IN_MOVED_FROM on the source directory and
   * IN_MOVED_TO on the destination, sharing a cookie. Resolved after the
   * rename so the destination lookup finds the entry that now exists. */
  if (res == 0) {
    char oldbuf[VFS_MAX_PATH], newbuf[VFS_MAX_PATH];
    vfs_resolve_path(old_path, oldbuf);
    vfs_resolve_path(new_path, newbuf);
    char *old_name = strrchr(oldbuf, '/');
    char *new_name = strrchr(newbuf, '/');
    if (old_name && new_name) {
      *old_name = '\0';
      *new_name = '\0';
      const char *old_dir_path = oldbuf[0] ? oldbuf : "/";
      const char *new_dir_path = newbuf[0] ? newbuf : "/";
      struct vfs_node *od = vfs_find_node(old_dir_path);
      struct vfs_node *nd = vfs_find_node(new_dir_path);
      if (!IS_ERR(od) && !IS_ERR(nd)) {
        struct vfs_node *moved = find_child(nd, new_name + 1);
        int is_dir = moved && moved->inode &&
                     moved->inode->type == VFS_DIRECTORY;
        vfs_inotify_notify_move(od, old_name + 1, nd, new_name + 1, is_dir);
      }
      if (!IS_ERR(od))
        vfs_node_put(od);
      if (!IS_ERR(nd))
        vfs_node_put(nd);
    }
  }
  return res;
}

int vfs_rmdir(const char *path) {
  int res = vfs_remove_node(path, 1);
  return res;
}

int vfs_fstat(int fd, struct b1nix_stat *st) {
  /* Pseudo-terminal slave/master fds have no backing vfs_node, but fstat() must
   * still work on them — musl's ttyname_r() fstat()s the slave and matches its
   * {dev,ino} against stat("/dev/pts/N"). Report the same pts-index identity as
   * devpts_lookup so the two agree. */
  struct vfs_handle *ph = get_handle(fd);
  if (ph && (ph->kind == VFS_HANDLE_PTY_SLAVE ||
             ph->kind == VFS_HANDLE_PTY_MASTER)) {
    int idx = pty_index_of(ph);
    if (idx < 0)
      return -EBADF;
    memset(st, 0, sizeof(*st));
    st->st_dev = DEVPTS_FSID;
    st->st_ino = devpts_ino(idx);
    st->st_mode = B1NIX_S_IFCHR | 0620;
    st->st_nlink = 1;
    st->st_gid = 5; /* tty */
    st->st_rdev = ((u64)136 << 8) | (u64)idx;
    st->st_blksize = 512;
    return 0;
  }
  /* Descriptors that are not files at all — pipes, sockets and the anonymous
   * objects (eventfd, timerfd, signalfd, epoll, inotify). fstat(2) works on
   * every one of them on Linux, and userspace leans on it: GNU head and tail
   * fstat standard input to size their buffer, and reported
   * "cannot fstat 'standard input': Bad file descriptor" for anything read
   * through a pipe, because a handle with no vfs_node fell through to a node
   * lookup that could only fail. */
  if (ph) {
    u32 anon_mode = 0;
    switch (ph->kind) {
    case VFS_HANDLE_PIPE_READ:
    case VFS_HANDLE_PIPE_WRITE:
      anon_mode = B1NIX_S_IFIFO | 0600;
      break;
    case VFS_HANDLE_SOCKET:
      anon_mode = B1NIX_S_IFSOCK | 0777;
      break;
    case VFS_HANDLE_EVENTFD:
    case VFS_HANDLE_TIMERFD:
    case VFS_HANDLE_SIGNALFD:
    case VFS_HANDLE_EPOLL:
    case VFS_HANDLE_INOTIFY:
    case VFS_HANDLE_PIDFD:
      /* Linux backs these with an anonymous inode, reported as a regular
       * file with no name and no size. */
      anon_mode = B1NIX_S_IFREG | 0600;
      break;
    default:
      break;
    }
    if (anon_mode) {
      memset(st, 0, sizeof(*st));
      st->st_mode = anon_mode;
      st->st_nlink = 1;
      st->st_blksize = 4096;
      /* A pidfd's inode number is its identity: a caller stores it to tell one
       * process reference from another and to notice a pid that has been
       * reused. The handle's own address is not that -- a freed handle's
       * address is handed out again -- so the pidfd carries a sequence number
       * that is never issued twice in a boot. */
      st->st_ino = (ph->kind == VFS_HANDLE_PIDFD) ? vfs_pidfd_id(ph)
                                                  : (u64)(usize)ph;
      return 0;
    }
  }

  /* Descriptors with no node of their own — the imported DRM core's objects,
   * which Linux backs with an anonymous inode. fstat on one has to work: a
   * compositor stats every descriptor it is handed, and an error here reads as
   * a broken device rather than as an object that simply has no name. */
  if (ph && ph->kind == VFS_HANDLE_NODE && !ph->node) {
    memset(st, 0, sizeof(*st));
    st->st_mode = B1NIX_S_IFREG | 0600;
    st->st_nlink = 1;
    st->st_blksize = 512;
    return 0;
  }

  struct vfs_node *node = vfs_find_node_by_fd(fd);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);
  if (node->inode && node->inode->ino && node->inode->fs_id) {
    struct vfs_inode *cached = icache_get(node->inode->fs_id, node->inode->ino);
    if (!cached) {
      icache_insert(node->inode->fs_id, node->inode->ino, node->inode);
    }
  }
  return vfs_stat_node(node, st);
}

/* M109: the namespace a /proc/<pid>/ns/<kind> descriptor pinned when it was
 * opened. setns(2) reads it here rather than re-deriving it from the path,
 * which would follow the task instead of the namespace. */
int vfs_fd_ns_pin(int fd, u32 *pin_out) {
  struct vfs_handle *h = get_handle(fd);
  if (!h || h->kind != VFS_HANDLE_NODE)
    return -EBADF;
  if (!(h->ns_pin & VFS_NS_PIN_VALID))
    return -EINVAL;
  if (pin_out)
    *pin_out = h->ns_pin;
  return 0;
}

/* Absolute path of an open fd, written NUL-terminated into buf. Backs the libc
 * *at() emulation (openat/unlinkat with a real dirfd + relative name): b1nix has
 * no per-fd-base path resolver, so libc resolves the dirfd to its path here and
 * joins the relative component. Built by walking node->parent under the VFS tree
 * read lock (a concurrent rmdir could otherwise free an ancestor mid-walk).
 * Returns the path length on success, or a negative errno. */
int vfs_fd_abspath(int fd, char *buf, usize size) {
  if (!buf || size < 2)
    return -EINVAL;
  struct vfs_node *node = vfs_find_node_by_fd(fd);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);

  /* Collect the basename of each ancestor (deepest first), stopping at the root
   * (parent == NULL), then emit them root-first as "/a/b/c". */
  const char *parts[64];
  int n = 0;
  u64 flags;
  vfs_tree_read_acquire(&flags);
  int steps = 0;
  for (struct vfs_node *c = node; c && n < 64 && steps < 256; steps++) {
    /* Crossing a mount seam upwards: the mount root contributes no name of its
     * own — the mount point's name is the one on the path. */
    struct vfs_node *mp = vfs_mount_point_of(c);
    if (mp) {
      c = mp;
      continue;
    }
    if (!c->parent)
      break;
    parts[n++] = c->name;
    c = c->parent;
  }

  usize pos = 0;
  if (n == 0) {
    buf[pos++] = '/'; /* the fd refers to the root itself */
  } else {
    for (int i = n - 1; i >= 0; i--) {
      const char *name = parts[i];
      usize pl = name ? strlen(name) : 0;
      if (pos + 1 + pl + 1 > size) {
        vfs_tree_read_release(flags);
        return -ENAMETOOLONG;
      }
      buf[pos++] = '/';
      memcpy(buf + pos, name, pl);
      pos += pl;
    }
  }
  buf[pos] = '\0';
  vfs_tree_read_release(flags);
  return (int)pos;
}

int vfs_fsync(int fd) {
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h || h->kind != VFS_HANDLE_NODE)
    return -EBADF;
  struct vfs_node *node = h->node;

  /* Inode lock across the flush — see vfs_close_handle (flush vs a concurrent
   * truncate's in-place page zeroing). */
  int err = vfs_node_fsync(node);
  if (err < 0)
    return err;

  if (node->inode->blk_dev) {
    /* Persist THIS file, then flush the device — fsync is about one file, and
     * draining every dirty block in a RAM-sized cache is what made a single
     * call cost seconds. Blocks carry the inode that dirtied them, so the rest
     * stays for the background drain. */
    blk_cache_flush_inode(node->inode->blk_dev, node->inode->fs_id,
                          node->inode->ino);
  }
  return 0;
}

static u32 next_peer_group;
static int path_is_under(const char *path, const char *under);

/* A new mount under a SHARED mount appears in every namespace that holds a peer
 * of that mount. This is the whole observable meaning of MS_SHARED here: a
 * mount namespace is a copy of the table, so "the copies are peers" has to mean
 * that a later mount reaches all of them.
 *
 * Called after the mount is fully published. Peers are found by peer group,
 * which only a mount that was explicitly made shared (or cloned from one) ever
 * has, so on a machine that never unshared a mount namespace this walks the
 * table once and does nothing. */
static void vfs_mount_propagate(int midx) {
  while (__atomic_test_and_set(&vfs_mount_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();

  /* The mount this one was made INSIDE: the longest target that is a prefix of
   * ours, in our own namespace. */
  u32 ns = mounts[midx].mnt_ns;
  int parent = -1;
  usize best = 0;
  for (usize i = 0; i < mount_slots; i++) {
    if (!mounts[i].used || (int)i == midx || mounts[i].mnt_ns != ns)
      continue;
    if (!path_is_under(mounts[midx].target, mounts[i].target))
      continue;
    usize l = strlen(mounts[i].target);
    if (parent < 0 || l > best) {
      parent = (int)i;
      best = l;
    }
  }
  if (parent < 0 || mounts[parent].propagation != MS_SHARED ||
      !mounts[parent].peer_group) {
    __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
    return;
  }

  u32 group = mounts[parent].peer_group;
  u32 child_group = next_peer_group++;
  mounts[midx].propagation = MS_SHARED;
  mounts[midx].peer_group = child_group;

  for (usize i = 0; i < mount_slots; i++) {
    if (!mounts[i].used || mounts[i].peer_group != group ||
        mounts[i].mnt_ns == ns)
      continue;
    int slot = -1;
    for (usize j = 0; j < mount_slots; j++)
      if (!mounts[j].used) {
        slot = (int)j;
        break;
      }
    if (slot < 0)
      break; /* out of table: the peer simply does not get it */
    mounts[slot] = mounts[midx];
    mounts[slot].mnt_ns = mounts[i].mnt_ns;
    mounts[slot].peer_group = child_group;
    mounts[slot].seq = mount_seq_next++;
    mounts[slot].used = 1;
    if (mounts[slot].root_node)
      vfs_node_get(mounts[slot].root_node);
    if (mounts[slot].mount_point)
      vfs_node_get(mounts[slot].mount_point);
    if (mounts[slot].owner)
      (void)try_module_get(mounts[slot].owner);
  }
  __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
}

static struct vfs_mount_entry *currently_mounting = NULL;

void vfs_set_currently_mounting_root(struct vfs_node *root) {
  if (currently_mounting) {
    currently_mounting->root_node = root;
  }
}

int vfs_mount(const char *source, const char *target, const char *fstype,
              u64 flags) {
  if (!target || target[0] == '\0' || !fstype)
    return -EINVAL;

  struct vfs_node *target_node = vfs_find_node(target);
  if (IS_ERR(target_node)) {
    return (int)PTR_ERR(target_node);
  }
  if (IS_ERR(target_node))
    return (int)PTR_ERR(target_node);
  if (target_node->inode->type != VFS_DIRECTORY) {
    vfs_node_put(target_node);
    return -ENOTDIR;
  }

  struct vfs_fs *fs = find_fs(fstype);
  if (!fs) {
    vfs_node_put(target_node);
    return -ENODEV;
  }

  /* Pin the module for as long as the mount lives: without this, rmmod frees
   * the text every operation on the mount calls into. Fails only when the
   * module is already on its way out. */
  if (fs->owner && !try_module_get(fs->owner)) {
    vfs_node_put(target_node);
    return -ENODEV;
  }

  /* Claim and initialize the slot under vfs_mount_lock — the unlocked scan
   * let two concurrent mounts pick the same index, and lookups walking
   * mounts[] could observe a half-written entry. The fs->mount() callback
   * itself runs outside the lock (it sleeps on block I/O). */
  /* Named before the lock: the resolver takes vfs_mount_lock itself. */
  char rectgt[VFS_MAX_PATH];
  mount_record_target(target, target_node, rectgt, sizeof(rectgt));

  while (__atomic_test_and_set(&vfs_mount_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();

  int midx = -1;
  for (usize i = 0; i < mount_slots; i++) {
    if (!mounts[i].used) {
      midx = (int)i;
      break;
    }
  }

  if (midx == -1) {
    __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
    vfs_node_put(target_node);
    module_put(fs->owner);
    return -ENOMEM;
  }

  // Pre-register slot so mount crossing works during populate_vfs
  mounts[midx].used = 1;
  mounts[midx].mount_point = target_node;
  mounts[midx].root_node = NULL;
  copy_path(mounts[midx].source, sizeof(mounts[midx].source), source ? source : "");
  /* The CANONICAL path of the mountpoint, not the string the caller used.
   * systemd mounts through a descriptor — it opens the directory O_PATH and
   * calls mount(..., "/proc/self/fd/4", ...) — so the raw string named a
   * descriptor rather than a place. Every mount in the machine then appeared in
   * /proc/self/mountinfo as "/proc/self/fd/4", nothing could be recognised as
   * already mounted, and systemd mounted /proc, /sys and /dev a second and
   * third time on each pass through its table. */
  copy_path(mounts[midx].target, sizeof(mounts[midx].target), rectgt);
  copy_path(mounts[midx].fstype, sizeof(mounts[midx].fstype), fstype);
  mounts[midx].flags = flags & ~(u64)MS_PROPAGATION_MASK;
  mounts[midx].propagation = MS_PRIVATE; /* Linux's default for a new mount */
  mounts[midx].peer_group = 0;
  mounts[midx].owner = fs->owner;
  mounts[midx].mnt_ns = vfs_current_mnt_ns();
  mounts[midx].seq = mount_seq_next++;
  __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
  if (bootinfo_has_flag("b1nix.trace-mount")) {
    char ml[192];
    snprintf(ml, sizeof(ml), "vfs_mount: type='%s' target='%s' point=%p '%s'",
             fstype, target, (void *)target_node, target_node->name);
    klog_info(ml);
  }

  currently_mounting = &mounts[midx];
  u32 new_fs_id;
  while (__atomic_test_and_set(&vfs_mount_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();
  new_fs_id = next_fs_id++;
  __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
  g_mounting_fs_id = new_fs_id;
  /* M107: filesystems resolve their device with blk_get(), which matches the
   * bare registration name ("sda", "loop0"). Every userspace mounter passes
   * a /dev path, so strip the directory here — this is what made
   * `mount /dev/loop0 /mnt` (and therefore `mount -o loop`) fail with ENODEV
   * while `mount loop0 /mnt` worked. */
  const char *dev_source = source;
  if (dev_source && strncmp(dev_source, "/dev/", 5) == 0)
    dev_source += 5;
  struct vfs_node *root_node = fs->mount(dev_source, flags, (void *)target);
  g_mounting_fs_id = 0;
  currently_mounting = NULL;

  if (IS_ERR(root_node)) {
    mounts[midx].used = 0;
    mounts[midx].owner = 0;
    vfs_node_put(target_node);
    module_put(fs->owner);
    return (int)PTR_ERR(root_node);
  }

  while (__atomic_test_and_set(&vfs_mount_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();
  mounts[midx].root_node = root_node;
  /* st_dev for everything on this mount. A filesystem mounted from a block
   * device reports that device's number, so tools that map a file back to its
   * disk (and /proc/<pid>/maps' dev column) name the real thing. A pseudo or
   * RAM filesystem has no device, so it gets an anonymous number — major 0 with
   * a unique minor — exactly as Linux does for tmpfs and friends. */
  root_node->inode->fs_id = new_fs_id; /* keys the inode cache: per-mount */
  {
    /* st_dev: the block device this filesystem was mounted from, or an
     * anonymous number (major 0, unique minor) for a pseudo/RAM filesystem —
     * the same distinction Linux makes for tmpfs. */
    static u32 next_anon_minor = 1;
    struct block_device *bdev = dev_source ? blk_get(dev_source) : 0;
    u32 devno = bdev ? blk_devno(bdev) : 0;
    root_node->inode->dev = devno ? devno : (next_anon_minor++ & 0xFFu);
  }
  __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);

  vfs_mount_propagate(midx);
  return 0;
}

/* ── mount propagation, bind mounts and remount ────────────────────────────
 *
 * Everything here operates on the mount TABLE; none of it mounts a filesystem.
 *
 * Propagation is what systemd asks for before it does anything else:
 * mount(NULL, "/", NULL, MS_REC|MS_SHARED, NULL). Its per-unit sandboxing is
 * then described relative to it — a unit with PrivateTmp= gets a mount
 * namespace whose root is a slave of the host's, so mounts the host makes
 * afterwards are still visible inside the unit while the unit's own are not
 * visible outside. b1nix models a namespace as a copy of this table, so
 * "shared" means: the copies are peers, and a mount made under one of them is
 * made under all of them.
 */

static u32 next_peer_group = 1;

/* Is `path` at or below `under`? Both are canonical. */
static int path_is_under(const char *path, const char *under) {
  if (under[0] == '/' && under[1] == '\0')
    return 1;
  usize ul = strlen(under);
  if (strncmp(path, under, ul) != 0)
    return 0;
  return path[ul] == '\0' || path[ul] == '/';
}

int vfs_set_propagation(const char *target, u64 flags) {
  u32 type = (u32)(flags & MS_PROPAGATION_MASK);
  /* Exactly one propagation type, as Linux requires. */
  if (type != MS_SHARED && type != MS_SLAVE && type != MS_PRIVATE &&
      type != MS_UNBINDABLE)
    return -EINVAL;

  char canon[VFS_MAX_PATH];
  struct vfs_node *node = vfs_find_node(target);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);
  mount_record_target(target, node, canon, sizeof(canon));
  vfs_node_put(node);

  int recursive = (flags & MS_REC) ? 1 : 0;
  int touched = 0;

  while (__atomic_test_and_set(&vfs_mount_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();
  for (usize i = 0; i < mount_slots; i++) {
    if (!mount_visible(i))
      continue;
    if (recursive ? !path_is_under(mounts[i].target, canon)
                  : strcmp(mounts[i].target, canon) != 0)
      continue;
    mounts[i].propagation = type;
    if (type == MS_SHARED) {
      if (!mounts[i].peer_group)
        mounts[i].peer_group = next_peer_group++;
    } else if (type != MS_SLAVE) {
      mounts[i].peer_group = 0;
    }
    touched = 1;
  }
  __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);

  /* Linux allows a propagation change on any mountpoint; a path that is not
   * one is EINVAL. */
  return touched ? 0 : -EINVAL;
}

int vfs_remount(const char *target, u64 flags) {
  char canon[VFS_MAX_PATH];
  struct vfs_node *node = vfs_find_node(target);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);
  mount_record_target(target, node, canon, sizeof(canon));
  vfs_node_put(node);

  int found = 0;
  while (__atomic_test_and_set(&vfs_mount_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();
  for (usize i = 0; i < mount_slots; i++) {
    if (!mount_visible(i) || strcmp(mounts[i].target, canon) != 0)
      continue;
    /* MS_REMOUNT changes the mount's flags and nothing else. The propagation
     * bits are not mount flags and are not touched by a remount. */
    mounts[i].flags = (flags & ~(u64)(MS_REMOUNT | MS_PROPAGATION_MASK));
    found = 1;
  }
  __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
  if (!found && bootinfo_has_flag("b1nix.trace-mount")) {
    char rl[320];
    snprintf(rl, sizeof(rl), "remount: no mount recorded at '%s'", canon);
    klog_info(rl);
  }
  return found ? 0 : -EINVAL;
}

/* The name to record for a mount point.
 *
 * Not the node's own path: a node reached through a bind mount has a name in
 * two places, and the mount belongs at the one the caller named. systemd
 * mounts everything through /proc/self/fd/<n> (it opens the destination
 * O_PATH so no symlink can be swapped in), and that descriptor knows the name
 * it was opened under -- so when the target is such a link, its readlink text
 * IS the canonical answer. Everything else canonicalises through the node, and
 * the raw string is the last resort.
 *
 * Getting this wrong is not cosmetic: systemd re-reads /proc/self/mountinfo
 * after each bind to check the mount took, and a row filed under a different
 * name means it never sees its own mount. It retries 32 times and then fails
 * the unit with EBUSY, which is what kept systemd-udevd from ever starting. */
static void mount_record_target(const char *target, struct vfs_node *node,
                                char *out, usize out_len) {
  out[0] = '\0';
  if (target && strncmp(target, "/proc/", 6) == 0) {
    char link[VFS_MAX_PATH];
    isize n = vfs_readlink(target, link, sizeof(link) - 1);
    if (n > 0 && link[0] == '/') {
      link[n] = '\0';
      copy_path(out, out_len, link);
      return;
    }
  }
  if (target && target[0]) {
    char norm[VFS_MAX_PATH];
    vfs_resolve_path(target, norm);
    if (norm[0]) {
      copy_path(out, out_len, norm);
      return;
    }
  }
  {
    char canon[VFS_MAX_PATH];
    if (node && vfs_get_node_path(node, canon, sizeof(canon)) == 0 && canon[0]) {
      copy_path(out, out_len, canon);
      return;
    }
  }
  copy_path(out, out_len, target ? target : "");
}

int vfs_bind_mount(const char *source, const char *target, u64 flags) {
  /* A bind needs something to bind. An empty source resolves to the root
   * directory, so accepting one here quietly bind-mounts the whole filesystem
   * over the target -- the loudest possible way to answer a caller that simply
   * passed NULL because it meant a remount. Linux answers EINVAL. */
  if (!source || !source[0])
    return -EINVAL;
  struct vfs_node *src = vfs_find_node(source);
  if (IS_ERR(src))
    return (int)PTR_ERR(src);
  struct vfs_node *tgt = vfs_find_node(target);
  if (IS_ERR(tgt)) {
    vfs_node_put(src);
    return (int)PTR_ERR(tgt);
  }
  if (bootinfo_has_flag("b1nix.trace-mount")) {
    char sp[VFS_MAX_PATH], tp[VFS_MAX_PATH], bl[512];
    if (vfs_get_node_path(src, sp, sizeof(sp)) != 0)
      sp[0] = '\0';
    if (vfs_get_node_path(tgt, tp, sizeof(tp)) != 0)
      tp[0] = '\0';
    char rt[VFS_MAX_PATH];
    mount_record_target(target, tgt, rt, sizeof(rt));
    snprintf(bl, sizeof(bl),
             "bind: '%s'(%p '%s' t=%d) -> '%s'(%p '%s' t=%d) recorded='%s'",
             source, (void *)src, sp, (int)src->inode->type, target,
             (void *)tgt, tp, (int)tgt->inode->type, rt);
    klog_info(bl);
  }
  /* Linux allows a file-to-file bind; both ends must agree about which it is. */
  if ((src->inode->type == VFS_DIRECTORY) !=
      (tgt->inode->type == VFS_DIRECTORY)) {
    /* Named, because a caller only ever hears ENOTDIR and cannot tell which end
     * disagreed -- systemd reports the whole thing as "Failed to set up mount
     * namespacing" against the target path, and the source is the half that has
     * been wrong here. Bounded: a boot that does this once does it hundreds of
     * times. */
    static unsigned told;

    if (told < 8) {
      char line[192];

      told++;
      snprintf(line, sizeof(line),
               "bind: kind mismatch: source %s is a %s, target %s is a %s "
               "(resolved to '%s')\n",
               source, src->inode->type == VFS_DIRECTORY ? "directory" : "file",
               target, tgt->inode->type == VFS_DIRECTORY ? "directory" : "file",
               tgt->name);
      console_write(line);
    }
    vfs_node_put(src);
    vfs_node_put(tgt);
    return -ENOTDIR;
  }

  /* Resolved before the mount lock is taken: naming the target walks the path
   * resolver, which takes that same lock. */
  char rectgt[VFS_MAX_PATH];
  mount_record_target(target, tgt, rectgt, sizeof(rectgt));

  while (__atomic_test_and_set(&vfs_mount_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();
  int midx = -1;
  for (usize i = 0; i < mount_slots; i++)
    if (!mounts[i].used) {
      midx = (int)i;
      break;
    }
  if (midx < 0) {
    __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
    vfs_node_put(src);
    vfs_node_put(tgt);
    return -ENOMEM;
  }
  mounts[midx].used = 1;
  mounts[midx].mount_point = tgt;   /* reference kept by the mount */
  mounts[midx].root_node = src;     /* reference kept by the mount */
  copy_path(mounts[midx].source, sizeof(mounts[midx].source), source);
  copy_path(mounts[midx].target, sizeof(mounts[midx].target), rectgt);
  copy_path(mounts[midx].fstype, sizeof(mounts[midx].fstype), "bind");
  mounts[midx].flags = flags & ~(u64)(MS_BIND | MS_REC | MS_PROPAGATION_MASK);
  mounts[midx].owner = 0;
  mounts[midx].mnt_ns = vfs_current_mnt_ns();
  mounts[midx].seq = mount_seq_next++;
  mounts[midx].propagation = MS_PRIVATE;
  mounts[midx].peer_group = 0;
  __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
  return 0;
}

isize vfs_mounts_info(struct vfs_mount_info *out, usize max_entries) {
  if (!out && max_entries > 0)
    return -EFAULT;
  usize count = 0;
  for (usize i = 0; i < mount_slots; i++) {
    if (!mount_visible(i))
      continue;
    if (count < max_entries) {
      copy_path(out[count].source, sizeof(out[count].source), mounts[i].source);
      copy_path(out[count].target, sizeof(out[count].target), mounts[i].target);
      copy_path(out[count].fstype, sizeof(out[count].fstype), mounts[i].fstype);
      out[count].flags = mounts[i].flags;
      out[count].propagation = mounts[i].propagation;
      out[count].peer_group = mounts[i].peer_group;
    }
    count++;
  }
  return (isize)count;
}

/* Rewrite a mount's target when the directory it hangs under moves: "/old" and
 * everything below it become "/new..." . Returns 0 when `path` is not inside
 * `from`, so the caller can leave that entry alone. */
static int retarget_under(char *path, usize path_size, const char *from,
                          const char *to) {
  usize flen = strlen(from);

  if (strcmp(path, from) == 0) {
    copy_path(path, path_size, to);
    return 1;
  }
  if (strncmp(path, from, flen) != 0 || path[flen] != '/')
    return 0;

  const char *tail = path + flen; /* starts with '/' */
  char rebuilt[VFS_MAX_PATH];

  if (to[0] == '/' && to[1] == '\0')
    copy_path(rebuilt, sizeof(rebuilt), tail);
  else {
    copy_path(rebuilt, sizeof(rebuilt), to);
    strncat(rebuilt, tail, sizeof(rebuilt) - strlen(rebuilt) - 1);
  }
  copy_path(path, path_size, rebuilt);
  return 1;
}

int vfs_move_mount(const char *source, const char *target) {
  if (!source || !source[0] || !target || !target[0])
    return -EINVAL;

  char src[VFS_MAX_PATH];
  char dst[VFS_MAX_PATH];
  vfs_resolve_path(source, src);
  vfs_resolve_path(target, dst);

  if (strcmp(src, "/") == 0)
    return -EBUSY; /* the root mount has nowhere above it to move to */
  if (strcmp(src, dst) == 0)
    return -EINVAL;
  {
    /* Moving a mount underneath itself would make its own path unreachable. */
    usize slen = strlen(src);
    if (strncmp(dst, src, slen) == 0 && dst[slen] == '/')
      return -EINVAL;
  }

  struct vfs_node *src_node = vfs_find_node(src);
  if (IS_ERR(src_node))
    return (int)PTR_ERR(src_node);
  struct vfs_node *dst_node = vfs_find_node(dst);
  if (IS_ERR(dst_node)) {
    vfs_node_put(src_node);
    return (int)PTR_ERR(dst_node);
  }
  if (dst_node->inode->type != VFS_DIRECTORY) {
    vfs_node_put(dst_node);
    vfs_node_put(src_node);
    return -ENOTDIR;
  }

  while (__atomic_test_and_set(&vfs_mount_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();

  int midx = -1;
  for (usize i = 0; i < mount_slots; i++) {
    if (mounts[i].used && mounts[i].root_node == src_node) {
      midx = (int)i;
      break;
    }
  }
  if (midx < 0) {
    __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
    vfs_node_put(dst_node);
    vfs_node_put(src_node);
    return -EINVAL; /* not a mountpoint: nothing to move */
  }

  struct vfs_node *old_mp = mounts[midx].mount_point;

  /* A move is a retarget, not a remount: the filesystem, its root node and
   * every open file on it stay exactly as they are. Only where the tree
   * crosses into it changes — this entry's mountpoint, and the recorded target
   * of every mount nested inside it, whose own mountpoint nodes travel with
   * the subtree and so need no fixing. */
  for (usize i = 0; i < mount_slots; i++) {
    if (!mounts[i].used || (int)i == midx)
      continue;
    retarget_under(mounts[i].target, sizeof(mounts[i].target), src, dst);
  }
  mounts[midx].mount_point = dst_node; /* takes over the lookup reference */
  copy_path(mounts[midx].target, sizeof(mounts[midx].target), dst);
  __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);

  /* A working directory is a directory, not a path, so a process standing
   * inside the moved subtree moves with it. Here the directory is remembered
   * by name, so the name is what has to be rewritten — and switch_root depends
   * on it: the chroot(".") it issues immediately after the move would
   * otherwise resolve a path that no longer exists. */
  for (usize i = 0; i < scheduler_task_slots(); i++) {
    struct task *t = scheduler_task_slot(i);
    if (t)
      retarget_under(t->cwd, sizeof(t->cwd), src, dst);
  }

  /* Cached name→node answers still point through the old mountpoint. */
  dcache_invalidate_node(old_mp);
  dcache_invalidate_node(dst_node);
  vfs_node_put(old_mp);
  vfs_node_put(src_node);
  return 0;
}

/* Is this block device the source of a live mount? A self-test that wants a
 * disk nothing else owns has no other way to ask: on one board the first
 * virtio disk is a scratch image, on another it is the root filesystem, and
 * writing test patterns into the running root is not a mistake worth making
 * twice. */
int vfs_device_is_mounted(const char *name) {
  if (!name || !name[0] || !mounts)
    return 0;
  for (usize i = 0; i < mount_slots; i++) {
    if (mounts[i].used && strcmp(mounts[i].source, name) == 0)
      return 1;
  }
  return 0;
}

int vfs_umount(const char *target) {
  if (!target)
    return -EINVAL;

  while (__atomic_test_and_set(&vfs_mount_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();

  for (usize i = 0; i < mount_slots; i++) {
    if (mount_visible(i) && strcmp(mounts[i].target, target) == 0) {
      if (strcmp(target, "/") == 0) {
        __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
        return -EBUSY;
      }

      /* Basic busy check: if root_node has other refs than our mount entry.
       * Acquire-load: pairs with the atomic refcount updates so a ref taken
       * on another CPU just before umount is observed. */
      if (__atomic_load_n(&mounts[i].root_node->refcount, __ATOMIC_ACQUIRE) >
          (int)mount_root_refs(mounts[i].root_node)) {
        __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
        return -EBUSY;
      }

      struct vfs_node *root = mounts[i].root_node;
      struct vfs_node *mp = mounts[i].mount_point;
      u32 fs_id = (root && root->inode) ? root->inode->fs_id : 0;
      /* The filesystem is only really going away when this was its last mount
       * entry across every namespace — another namespace's copy still uses it,
       * so tearing the superblock down here would pull it out from under the
       * tasks in that namespace. */
      int last_ref = (mount_root_refs(root) <= 1);

      /* Call filesystem umount callback if available (e.g. JBD RECOVER flag) */
      if (last_ref && mounts[i].fstype[0]) {
        struct vfs_fs *fs = filesystems;
        while (fs) {
          if (strcmp(fs->name, mounts[i].fstype) == 0) {
            if (fs->umount && root)
              fs->umount(root);
            break;
          }
          fs = fs->next;
        }
      }

      struct module *owner = mounts[i].owner;
      mounts[i].used = 0;
      mounts[i].owner = 0;
      __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
      module_put(owner);

      if (last_ref && root && root->inode && root->inode->blk_dev) {
        blk_cache_flush(root->inode->blk_dev);
        blk_cache_invalidate(root->inode->blk_dev);
      }

      vfs_node_put(root);
      vfs_node_put(mp);
      if (last_ref)
        icache_invalidate_fs(fs_id);
      return 0;
    }
  }
  __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
  return -EINVAL;
}

/* pivot_root(2) — make new_root the root and park the old one at put_old.
 *
 * What an initramfs does last: it has mounted the real filesystem somewhere
 * under itself, and now wants that filesystem to be "/" without rebooting.
 * That is what busybox switch_root drives, and the reason M109 wanted it.
 *
 * b1nix does not replace root_node on a root mount — it records a mounts[]
 * entry whose target is "/", and every lookup that starts at root_node calls
 * vfs_cross_root_mount() to step onto it. So a pivot is a retarget of two
 * entries rather than a rebuild of the tree: the new root's entry becomes "/",
 * and the old root becomes an ordinary mount at put_old, reached through the
 * put_old directory node exactly like any other mount point.
 *
 * The old root is not always an entry already: booted on the initramfs there
 * is no "/" mount at all, only the synthetic root_node. In that case an entry
 * is created for it, so the old root really is still reachable at put_old
 * afterwards (and can be umounted there) rather than merely being orphaned.
 */
/* A mount at "/" is attached to the global root node — that is the node a path
 * walk arrives at, and the one vfs_mount records when the target is "/". An
 * entry that becomes the root through a pivot has to be re-pointed at it, or
 * it keeps naming the directory it used to be mounted on and every path built
 * by walking up out of that filesystem (getcwd, /proc/<pid>/cwd) is prefixed
 * with a place the root is no longer at. */
static void vfs_pivot_reroot_mp(struct vfs_mount_entry *m) {
  struct vfs_node *prev = m->mount_point;
  m->mount_point = vfs_node_get(root_node);
  if (prev)
    vfs_node_put(prev);
}

int vfs_pivot_root(const char *new_root, const char *put_old) {
  if (!new_root || !put_old || !new_root[0] || !put_old[0])
    return -EINVAL;

  char *new_abs = kmalloc(VFS_MAX_PATH);
  char *old_abs = kmalloc(VFS_MAX_PATH);
  if (!new_abs || !old_abs) {
    kfree(new_abs);
    kfree(old_abs);
    return -ENOMEM;
  }
  vfs_resolve_path(new_root, new_abs);
  vfs_resolve_path(put_old, old_abs);

  int rc = 0;
  if (strcmp(new_abs, "/") == 0)
    rc = -EBUSY; /* the new root must not be the current one */
  /* put_old has to live under new_root, or unmounting the old root later
   * would have to reach through the old root to find it. */
  usize nlen = strlen(new_abs);
  if (!rc && (strncmp(old_abs, new_abs, nlen) != 0 || old_abs[nlen] != '/'))
    rc = -EINVAL;
  if (rc) {
    kfree(new_abs);
    kfree(old_abs);
    return rc;
  }

  struct vfs_node *old_mp = vfs_find_node(old_abs);
  if (IS_ERR(old_mp) || !old_mp) {
    kfree(new_abs);
    kfree(old_abs);
    return IS_ERR(old_mp) ? (int)PTR_ERR(old_mp) : -ENOENT;
  }
  if (old_mp->inode->type != VFS_DIRECTORY) {
    vfs_node_put(old_mp);
    kfree(new_abs);
    kfree(old_abs);
    return -ENOTDIR;
  }
  /* If something is already mounted at put_old, the lookup above crossed into
   * it and returned that filesystem's root. What the old root has to be
   * attached to is the DIRECTORY put_old names, not the root of whatever is
   * covering it — attaching to the covering root would leave the directory
   * with no mount pointing at it, and the path would stop resolving. */
  for (struct vfs_node *mp = vfs_mount_point_of(old_mp); mp;
       mp = vfs_mount_point_of(old_mp)) {
    vfs_node_get(mp);
    vfs_node_put(old_mp);
    old_mp = mp;
  }

  while (__atomic_test_and_set(&vfs_mount_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();

  int nidx = -1, oidx = -1, freeidx = -1;
  for (usize i = 0; i < mount_slots; i++) {
    if (!mounts[i].used) {
      if (freeidx < 0)
        freeidx = (int)i;
      continue;
    }
    if (strcmp(mounts[i].target, new_abs) == 0 && mounts[i].root_node)
      nidx = (int)i;
    else if (strcmp(mounts[i].target, "/") == 0)
      oidx = (int)i;
  }

  if (nidx < 0) {
    /* Linux says EINVAL when new_root is not a mount point, and it means it:
     * pivoting onto a plain directory would leave "/" naming a subtree of the
     * filesystem it is supposed to be replacing. */
    __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
    vfs_node_put(old_mp);
    kfree(new_abs);
    kfree(old_abs);
    return -EINVAL;
  }

  /* Every target string is a path in the OLD root's namespace, and the pivot
   * renames the whole tree: what was <new_root>/x is about to be /x, and what
   * was /y is about to be <put_old>/y. A target that keeps its old spelling
   * is not cosmetic — umount() matches on it, and /proc/mounts is read from
   * it — so they are all rewritten. `put_old_new` is what put_old is called
   * once the pivot has happened. */
  const char *put_old_new = old_abs + nlen; /* begins with '/' by the check above */

  char *tmp = kmalloc(VFS_MAX_PATH);
  if (!tmp) {
    __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
    vfs_node_put(old_mp);
    kfree(new_abs);
    kfree(old_abs);
    return -ENOMEM;
  }

  for (usize i = 0; i < mount_slots; i++) {
    if (!mounts[i].used || (int)i == nidx || (int)i == oidx)
      continue;
    if (strcmp(mounts[i].target, new_abs) == 0) {
      /* Another entry naming the new root itself — the same doubling as
       * below, on the other side of the pivot. */
      tmp[0] = '/';
      tmp[1] = '\0';
      vfs_pivot_reroot_mp(&mounts[i]);
    } else if (strncmp(mounts[i].target, new_abs, nlen) == 0 &&
               mounts[i].target[nlen] == '/') {
      /* Was under the new root: drop the prefix. */
      strncpy(tmp, mounts[i].target + nlen, VFS_MAX_PATH - 1);
    } else if (strcmp(mounts[i].target, "/") == 0) {
      /* Another entry claiming the old root — b1nix can have two, the
       * initramfs and the filesystem mounted over it. Concatenating would
       * spell it "<put_old>/", which is not a path anything matches. */
      strncpy(tmp, put_old_new, VFS_MAX_PATH - 1);
    } else {
      /* Was on the old root: it moves with it. */
      snprintf(tmp, VFS_MAX_PATH, "%s%s", put_old_new, mounts[i].target);
    }
    tmp[VFS_MAX_PATH - 1] = '\0';
    strncpy(mounts[i].target, tmp, VFS_MAX_PATH - 1);
    mounts[i].target[VFS_MAX_PATH - 1] = '\0';
  }
  kfree(tmp);

  if (oidx >= 0) {
    /* The old root was a real mount: it keeps its filesystem, its source and
     * its module reference, and only changes where it is attached. */
    struct vfs_node *prev_mp = mounts[oidx].mount_point;
    strncpy(mounts[oidx].target, put_old_new, VFS_MAX_PATH - 1);
    mounts[oidx].target[VFS_MAX_PATH - 1] = '\0';
    mounts[oidx].mount_point = vfs_node_get(old_mp);
    if (prev_mp)
      vfs_node_put(prev_mp);
  } else if (freeidx >= 0) {
    /* Booted on the initramfs: the old root is the synthetic root_node and has
     * no entry. Give it one so it stays reachable at put_old. */
    mounts[freeidx].used = 1;
    strncpy(mounts[freeidx].source, "rootfs", sizeof(mounts[freeidx].source) - 1);
    mounts[freeidx].source[sizeof(mounts[freeidx].source) - 1] = '\0';
    strncpy(mounts[freeidx].target, put_old_new, VFS_MAX_PATH - 1);
    mounts[freeidx].target[VFS_MAX_PATH - 1] = '\0';
    strncpy(mounts[freeidx].fstype, "rootfs", sizeof(mounts[freeidx].fstype) - 1);
    mounts[freeidx].fstype[sizeof(mounts[freeidx].fstype) - 1] = '\0';
    mounts[freeidx].flags = 0;
    mounts[freeidx].root_node = vfs_node_get(root_node);
    mounts[freeidx].mount_point = vfs_node_get(old_mp);
    mounts[freeidx].owner = 0;
  } else {
    __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
    vfs_node_put(old_mp);
    kfree(new_abs);
    kfree(old_abs);
    return -ENOMEM;
  }

  /* Last, so no lookup ever sees two entries claiming "/". */
  mounts[nidx].target[0] = '/';
  mounts[nidx].target[1] = '\0';
  vfs_pivot_reroot_mp(&mounts[nidx]);

  __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
  vfs_node_put(old_mp);
  kfree(new_abs);
  kfree(old_abs);

  /* Same duty a root mount has: the device nodes made during early boot lived
   * on the old root and are not reachable from the new one. */
  vfs_repopulate_after_root_mount();
  return 0;
}

/* ── M109: mount namespaces ───────────────────────────────────────────────
 * A mount namespace is a private copy of this table. Cloning one duplicates
 * every entry of the source namespace, taking a reference on both nodes (and
 * on the module providing the filesystem) so the copy keeps the mount alive on
 * its own; destroying one drops exactly those references again. The underlying
 * superblock is shared, which is what Linux does too: unshare(CLONE_NEWNS)
 * copies the mount tree, it does not re-mount the filesystems. */
int vfs_mnt_ns_clone(u32 from_ns, u32 to_ns) {
  if (to_ns == from_ns)
    return -EINVAL;

  while (__atomic_test_and_set(&vfs_mount_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();

  /* Count first: a partially copied namespace would be a namespace missing its
   * root, so the copy is all-or-nothing. */
  usize need = 0, have = 0;
  for (usize i = 0; i < mount_slots; i++) {
    if (mounts[i].used && mounts[i].mnt_ns == from_ns)
      need++;
    else if (!mounts[i].used)
      have++;
  }
  if (need > have) {
    __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
    return -ENOMEM;
  }

  for (usize i = 0; i < mount_slots; i++) {
    if (!mounts[i].used || mounts[i].mnt_ns != from_ns)
      continue;
    for (usize j = 0; j < mount_slots; j++) {
      if (mounts[j].used)
        continue;
      mounts[j] = mounts[i];
      mounts[j].mnt_ns = to_ns;
      /* A shared mount's copy is its PEER: that is what "shared" means, and it
       * is why a later mount under either of them shows up under both. A
       * private mount's copy is unrelated to the original. */
      if (mounts[i].propagation != MS_SHARED) {
        mounts[j].propagation = MS_PRIVATE;
        mounts[j].peer_group = 0;
      }
      if (mounts[j].root_node)
        vfs_node_get(mounts[j].root_node);
      if (mounts[j].mount_point)
        vfs_node_get(mounts[j].mount_point);
      if (mounts[j].owner && !try_module_get(mounts[j].owner))
        mounts[j].owner = 0;
      break;
    }
  }

  __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
  return 0;
}

/* Entries detached per pass in vfs_mnt_ns_destroy.
 *
 * The references cannot be dropped under the mount lock, so detached entries
 * are carried out of the locked scan and released after it. That buffer used to
 * be three MAX_MOUNTS arrays on the stack, which was survivable only while
 * MAX_MOUNTS was 64 — at the current ceiling it would be 96 KiB on a 32 KiB
 * kernel stack. Draining in fixed batches keeps the stack cost constant (under
 * a kilobyte) whatever the table capacity, and needs no allocation on a
 * teardown path that must not fail. */
#define MNT_RELEASE_BATCH 32

void vfs_mnt_ns_destroy(u32 ns) {
  if (ns == 0)
    return; /* the initial namespace outlives everything */

  for (;;) {
    struct vfs_node *roots[MNT_RELEASE_BATCH];
    struct vfs_node *mps[MNT_RELEASE_BATCH];
    struct module *owners[MNT_RELEASE_BATCH];
    usize n = 0;

    while (__atomic_test_and_set(&vfs_mount_lock, __ATOMIC_ACQUIRE))
      scheduler_yield();

    for (usize i = 0; i < mount_slots && n < MNT_RELEASE_BATCH; i++) {
      if (!mounts[i].used || mounts[i].mnt_ns != ns)
        continue;
      /* Last entry anywhere for this filesystem: let it shut down properly, the
       * same way vfs_umount() does. A filesystem another namespace still mounts
       * is left alone. */
      if (mount_root_refs(mounts[i].root_node) <= 1 && mounts[i].fstype[0]) {
        for (struct vfs_fs *fs = filesystems; fs; fs = fs->next) {
          if (strcmp(fs->name, mounts[i].fstype) != 0)
            continue;
          if (fs->umount && mounts[i].root_node)
            fs->umount(mounts[i].root_node);
          break;
        }
      }
      roots[n] = mounts[i].root_node;
      mps[n] = mounts[i].mount_point;
      owners[n] = mounts[i].owner;
      n++;
      mounts[i].used = 0;
      mounts[i].owner = 0;
      mounts[i].root_node = 0;
      mounts[i].mount_point = 0;
    }

    __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);

    if (n == 0)
      return; /* nothing left in this namespace */

    for (usize i = 0; i < n; i++) {
      module_put(owners[i]);
      vfs_node_put(roots[i]);
      vfs_node_put(mps[i]);
    }
  }
}

isize vfs_mounts(struct b1nix_mount_entry *out, usize max_entries) {
  if (!out && max_entries > 0)
    return -EFAULT;

  usize count = 0;
  for (usize i = 0; i < mount_slots; i++) {
    if (!mount_visible(i))
      continue;
    if (count < max_entries) {
      copy_path(out[count].source, sizeof(out[count].source), mounts[i].source);
      copy_path(out[count].target, sizeof(out[count].target), mounts[i].target);
      copy_path(out[count].fstype, sizeof(out[count].fstype), mounts[i].fstype);
      out[count].flags = mounts[i].flags;
    }
    count++;
  }
  return (isize)count;
}

/* The mount id of the mount a path lives on: the number /proc/<pid>/mountinfo
 * prints in its first field for that mount, which is the only thing a mount id
 * means to userspace.
 *
 * Reporting a constant — this used to answer 0 for every path, on the grounds
 * that there is one mount namespace — is not a simplification, it is an answer
 * no line of mountinfo carries. systemd asks for the mount id of /dev and then
 * looks for the mountinfo row with that id to read its filesystem type; with 0
 * it finds no row, concludes /dev is not a devtmpfs, and (with udev not yet
 * running) DISABLES its device monitor for the rest of the boot. No .device
 * unit can activate after that, whatever udev goes on to do.
 *
 * The id is the mount's position in the visible list, +1, exactly as
 * r_mountinfo numbers the rows it prints. The mount a path lives on is the
 * visible mount whose target is the longest prefix of it, latest wins.
 * Returns 0 when nothing matches, which cannot happen once / is mounted. */
int vfs_mount_id_for_path(const char *path) {
  if (!path || path[0] != '/')
    return 0;
  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(path, resolved);

  int best_id = 0;
  usize best_len = 0;
  usize index = 0;
  for (usize i = 0; i < mount_slots; i++) {
    if (!mount_visible(i))
      continue;
    index++;
    const char *tgt = mounts[i].target;
    usize tlen = strlen(tgt);
    if (tlen == 0)
      continue;
    if (strncmp(resolved, tgt, tlen) != 0)
      continue;
    /* "/devices" is not under "/dev": a prefix match must end on a component
     * boundary. The root is the one target that ends in '/' already. */
    if (tlen > 1 && resolved[tlen] != '\0' && resolved[tlen] != '/')
      continue;
    if (tlen >= best_len) {
      best_len = tlen;
      best_id = (int)index;
    }
  }
  return best_id;
}

/* Is this path the root of a mount -- the directory a filesystem is mounted
 * ON, rather than somewhere below it?
 *
 * This is the question statx(2) answers with STATX_ATTR_MOUNT_ROOT, and since
 * systemd 256 it is the ONLY way systemd asks it: the older routes through
 * name_to_handle_at(2) and /proc/self/mountinfo were dropped, and a kernel
 * that does not report the bit gets `-EUNATCH` -- "Failed to determine whether
 * /proc is a mount point". PID 1 asks that about every API filesystem before
 * it will mount anything, so on this kernel it asked five times, gave up, and
 * exited before printing its own version banner. */
int vfs_path_is_mount_root(const char *path) {
  if (!path || path[0] != '/')
    return 0;
  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(path, resolved);
  /* A trailing slash names the same directory; the root is "/" and keeps it. */
  usize rlen = strlen(resolved);
  while (rlen > 1 && resolved[rlen - 1] == '/')
    resolved[--rlen] = '\0';

  for (usize i = 0; i < mount_slots; i++) {
    if (!mount_visible(i))
      continue;
    if (strcmp(mounts[i].target, resolved) == 0)
      return 1;
  }
  return 0;
}

/* Directory cursor, high bit: the filesystem's half of a merged listing is
 * done and the rest of the cursor is the dir_seq of the last in-memory child
 * handed out. Below it the cursor is whatever the filesystem made it. */
#define VFS_DIR_MEM_CURSOR ((usize)1 << (sizeof(usize) * 8 - 2))

/* The live child of `dir` with the smallest dir_seq above `bound`, or NULL when
 * there is none left.
 *
 * The sibling list is NOT ordered by dir_seq: vfs_attach_child appends to it
 * while add_node (initramfs, and every filesystem that builds its tree through
 * it) prepends. A cursor that means "every child up to this sequence has been
 * handed out" therefore cannot resume at the next list position — against a
 * prepending directory the first child returned carries the HIGHEST sequence
 * and every remaining sibling is then below the bound, which is why such a
 * directory listed exactly one entry. Picking the next sequence each time is
 * independent of the order the list happens to be in. */
static struct vfs_node *next_child_by_seq(struct vfs_node *dir, u64 bound) {
  struct vfs_node *best = 0;

  for (struct vfs_node *c = dir->first_child; c; c = c->next_sibling) {
    if (c->deleted || !c->inode)
      continue;
    if (c->dir_seq == 0)
      c->dir_seq = dir_seq_next(); /* alloc_node assigns one; belt and braces */
    if (bound && c->dir_seq <= bound)
      continue;
    if (!best || c->dir_seq < best->dir_seq)
      best = c;
  }
  return best;
}

/* Remove from a filesystem's batch every name that an in-memory child of `dir`
 * already owns, so a merged listing reports each name once and the in-memory
 * node is the one it reports. Returns how many entries are left. */
static isize vfs_drop_shadowed_entries(struct vfs_node *dir, struct dirent *buf,
                                       isize count) {
  isize kept = 0;
  u64 tflags;

  vfs_tree_read_acquire(&tflags);
  for (isize i = 0; i < count; i++) {
    int shadowed = 0;
    for (struct vfs_node *c = dir->first_child; c; c = c->next_sibling) {
      if (c->deleted || !c->inode)
        continue;
      if (strcmp(c->name, buf[i].name) == 0) {
        shadowed = 1;
        break;
      }
    }
    if (shadowed)
      continue;
    if (kept != i)
      buf[kept] = buf[i];
    kept++;
  }
  vfs_tree_read_release(tflags);
  return kept;
}

isize vfs_getdents(int fd, struct dirent *buf, usize max_entries) {
  isize res = 0;
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h || h->kind != VFS_HANDLE_NODE) {
    return -EBADF;
  }
  struct vfs_node *dir = h->node;
  if (!dir || dir->inode->type != VFS_DIRECTORY || !buf) {
    return -EINVAL;
  }

  vfs_handle_retain(h);
  vfs_node_get(dir);
  usize offset = h->offset;

  /* Cursor-based readdir wins when the filesystem offers it: the handle then
   * carries an opaque resume cookie instead of a live-entry index, which is
   * what keeps a delete-as-you-go walk (rm -rf) from skipping entries. */
  if (!dir->inode->readdir_at_cb && !dir->inode->readdir_cb) {
    /* In-memory tree (tmpfs/ramfs/devtmpfs and every synthetic dir): resume by
     * vfs_node::dir_seq, which no unlink renumbers — unlike a positional index,
     * which shifts every surviving entry down when one is removed and so makes
     * a delete-as-you-go walk (rm -rf) skip one entry per deletion.
     *
     * The cursor is a lower bound: emit children whose seq is above the last
     * one emitted. Cookies 0 and 1 are "." and "..", and 2 means "children, no
     * bound yet"; a child cookie is seq+2, which is always >= 3.
     *
     * The walk does NOT assume the sibling list is in dir_seq order. Most
     * children are appended (vfs_attach_child) and so ascend, but several
     * paths — vfs_add_node's intermediate directories, rename, and the
     * mkdir/create helpers — PREPEND, which puts a high seq at the head. Read
     * as "skip anything at or below the bound", such a list stopped dead after
     * its newest child: `ls /dev` listed the four entries created last and none
     * of the device nodes. So each step picks the smallest seq still above the
     * bound, which is the same order regardless of how the list is linked. */
    u64 cookie = (u64)offset;
    usize count = 0;
    if (cookie == 0 && count < max_entries) {
      copy_path(buf[count].name, 64, ".");
      buf[count].type = (u32)VFS_DIRECTORY;
      buf[count].is_dir = 1;
      buf[count].is_exec = 1;
      buf[count].size = 0;
      count++;
      cookie = 1;
    }
    if (cookie == 1 && count < max_entries) {
      copy_path(buf[count].name, 64, "..");
      buf[count].type = (u32)VFS_DIRECTORY;
      buf[count].is_dir = 1;
      buf[count].is_exec = 1;
      buf[count].size = 0;
      count++;
      cookie = 2;
    }
    u64 seq_above = (cookie > 2) ? (cookie - 2) : 0; /* 0 = no bound */
    u64 tflags;
    vfs_tree_read_acquire(&tflags);
    struct vfs_node *child = next_child_by_seq(dir, seq_above);
    while (child && count < max_entries) {
      copy_path(buf[count].name, 64, child->name);
      buf[count].type = vfs_dirent_type(child->inode);
      buf[count].is_dir = (child->inode->type == VFS_DIRECTORY);
      buf[count].is_exec = 0;
      buf[count].size = child->inode->size;
      buf[count].ino = child->inode->ino;
      count++;
      seq_above = child->dir_seq;
      cookie = child->dir_seq + 2;
      child = next_child_by_seq(dir, seq_above);
    }
    vfs_tree_read_release(tflags);
    if (count > 0)
      h->offset = (usize)cookie;
    res = (isize)count;
    goto out;
  }

  if (dir->inode->readdir_at_cb || dir->inode->readdir_cb) {
    /* A directory on a real filesystem can still carry in-memory children:
     * that is what /dev is after the root switch — the block-device nodes are
     * attached to the directory in RAM while the names on the image come off
     * the disk. Listing only one of the two halves loses the other, and it was
     * the device nodes that were lost, so `blkid` and `findfs` found nothing
     * to scan.
     *
     * Both halves are handed out, the in-memory one last, with a name that
     * exists in both served from memory only — the devtmpfs-over-disk layout.
     *
     * The two halves share ONE cursor, h->offset, because that is the only
     * thing a caller can save and restore: the getdents shim pushes an entry
     * that does not fit back with an lseek to the cursor it read before it, and
     * a second cursor kept beside it would not come back with it. Below
     * VFS_DIR_MEM_CURSOR the cursor is the filesystem's own opaque cookie;
     * above it, the dir_seq of the last in-memory child handed out. */
    int merge = !dir->inode->readdir_lists_children;

    while (!(h->offset & VFS_DIR_MEM_CURSOR)) {
      if (dir->inode->readdir_at_cb) {
        u64 next = (u64)h->offset;
        res = dir->inode->readdir_at_cb(dir, (u64)h->offset, buf, max_entries,
                                        &next);
        if (res > 0)
          h->offset = (usize)next;
      } else {
        res = dir->inode->readdir_cb(dir, h->offset, buf, max_entries);
        if (res > 0)
          h->offset += (usize)res;
      }
      if (res < 0)
        goto out;
      if (res == 0) {
        h->offset = VFS_DIR_MEM_CURSOR; /* filesystem exhausted */
        break;
      }
      if (!merge)
        goto out;
      res = vfs_drop_shadowed_entries(dir, buf, res);
      if (res > 0)
        goto out;
      /* Every name in this batch was shadowed by an in-memory child. Returning
       * zero here would read as end-of-directory, so fetch the next batch. */
    }

    if (!merge) {
      res = 0;
      goto out;
    }

    u64 seq_above = (u64)(h->offset & ~VFS_DIR_MEM_CURSOR); /* 0 = no bound */
    usize count = 0;
    u64 tflags;
    vfs_tree_read_acquire(&tflags);
    struct vfs_node *child = next_child_by_seq(dir, seq_above);
    while (child && count < max_entries) {
      copy_path(buf[count].name, 64, child->name);
      buf[count].type = vfs_dirent_type(child->inode);
      buf[count].is_dir = (child->inode->type == VFS_DIRECTORY);
      buf[count].is_exec = 0;
      buf[count].size = child->inode->size;
      buf[count].ino = child->inode->ino;
      count++;
      seq_above = child->dir_seq;
      h->offset = VFS_DIR_MEM_CURSOR | (usize)seq_above;
      child = next_child_by_seq(dir, seq_above);
    }
    vfs_tree_read_release(tflags);
    res = (isize)count;
    goto out;
  }

  /* Fallback to in-memory Ramfs-style readdir.
   *
   * h->offset is the ABSOLUTE index of the next directory entry to return:
   * index 0 = ".", 1 = "..", and 2+ = the (n-2)th non-deleted child. We walk
   * from the start each call (O(n) per batch) emitting entries whose absolute
   * index is >= start, until the buffer fills. Crucially the cursor `idx` is
   * a SEPARATE counter from the emitted `count`, so emitting never perturbs
   * the resume position — an earlier version reused the running offset as the
   * skip target, which made it emit every other entry and duplicate across
   * batches once a directory exceeded one getdents batch. */
  usize start = offset;
  usize idx = 0;
  usize count = 0;

  if (idx >= start && count < max_entries) {
    copy_path(buf[count].name, 64, ".");
    buf[count].type = (u32)VFS_DIRECTORY;
    buf[count].is_dir = 1;
    buf[count].is_exec = 1;
    buf[count].size = 0;
    count++;
  }
  idx++;
  if (idx >= start && count < max_entries) {
    copy_path(buf[count].name, 64, "..");
    buf[count].type = (u32)VFS_DIRECTORY;
    buf[count].is_dir = 1;
    buf[count].is_exec = 1;
    buf[count].size = 0;
    count++;
  }
  idx++;

  /* Same race as vfs_list: the sibling list is NOT protected by any inode
   * lock, and a concurrent unlink on another CPU can free a child mid-walk.
   * The bare cli/sti this used before only blocked same-CPU preemption; take
   * the vfs_tree_lock read side (held for write by unlink's splice) like
   * find_child. irqsave, so the same-CPU snapshot guarantee is preserved. */
  u64 tflags;
  vfs_tree_read_acquire(&tflags);
  struct vfs_node *child = dir->first_child;
  while (child && count < max_entries) {
    if (child->deleted) {
      child = child->next_sibling;
      continue;
    }
    if (idx >= start) {
      copy_path(buf[count].name, 64, child->name);
      buf[count].type = vfs_dirent_type(child->inode);
      buf[count].is_dir = (child->inode->type == VFS_DIRECTORY);
      buf[count].is_exec = 0;
      buf[count].size = child->inode->size;
      count++;
    }
    idx++;
    child = child->next_sibling;
  }
  vfs_tree_read_release(tflags);

  h->offset = start + count;
  res = (isize)count;

out:
  vfs_node_put(dir);
  vfs_handle_release(h);
  return res;
}

int vfs_sync(void) {
  /* Flush in-memory filesystem structures to block cache first */
  ext2_sync_all_fs();
  fat32_sync_all_fs();

  /* Then flush the entire block cache to physical hardware */
  blk_sync_all();
  return 0;
}

int vfs_node_is_readonly(struct vfs_node *node) {
  struct vfs_mount_entry *mnt = vfs_get_mount_for_node(node);
  if (mnt && (mnt->flags & MS_RDONLY))
    return 1;
  return 0;
}

int vfs_dup(int oldfd) {
  struct vfs_handle *old_handle = scheduler_fd_get(oldfd);
  if (!old_handle)
    return -EBADF;

  int newfd = scheduler_fd_alloc(old_handle);
  if (newfd < 0)
    return newfd;

  vfs_handle_retain(old_handle);
  return newfd;
}

static int vfs_dup_min(int oldfd, int minfd) {
  struct vfs_handle *old_handle = scheduler_fd_get(oldfd);
  if (!old_handle)
    return -EBADF;
  if (minfd < 0 || (usize)minfd >= sched_fd_limit())
    return -EINVAL;

  usize limit = sched_fd_limit();
  struct rlimit rlim;
  if (scheduler_getrlimit(RLIMIT_NOFILE, &rlim) == 0 && rlim.rlim_cur < limit)
    limit = rlim.rlim_cur;
  if ((usize)minfd >= limit)
    return -EMFILE;

  for (usize fd = (usize)minfd; fd < limit; fd++) {
    if (scheduler_fd_get((int)fd) == 0) {
      if (scheduler_fd_set((int)fd, old_handle) < 0)
        return -EMFILE;
      vfs_handle_retain(old_handle);
      return (int)fd;
    }
  }
  return -EMFILE;
}

int vfs_dup2(int oldfd, int newfd) {
  struct vfs_handle *old_handle = scheduler_fd_get(oldfd);
  if (!old_handle)
    return -EBADF;
  if (newfd < 0 || (usize)newfd >= sched_fd_limit())
    return -EBADF;

  struct rlimit rlim;
  if (scheduler_getrlimit(RLIMIT_NOFILE, &rlim) == 0) {
    if ((usize)newfd >= rlim.rlim_cur)
      return -EBADF;
  }

  if (oldfd == newfd)
    return newfd;

  if (scheduler_fd_get(newfd) != 0)
    vfs_close(newfd);
  if (scheduler_fd_set(newfd, old_handle) < 0)
    return -EMFILE;
  vfs_handle_retain(old_handle);
  return newfd;
}

int vfs_get_node_path(struct vfs_node *node, char *buf, usize buf_len) {
  if (!node || !buf || buf_len == 0)
    return -EINVAL;

  struct vfs_node *path_nodes[128];
  int count = 0;
  struct vfs_node *curr = node;
  int steps = 0;
  while (curr && count < 128 && steps < 256) {
    steps++;
    /* Same mount seam as in vfs_fd_abspath: step from a mount root to the node
     * it covers instead of stopping at its NULL parent. */
    struct vfs_node *mp = vfs_mount_point_of(curr);
    if (mp) {
      curr = mp;
      continue;
    }
    path_nodes[count++] = curr;
    if (curr->parent == curr || curr->parent == NULL) {
      break;
    }
    curr = curr->parent;
  }

  usize pos = 0;
  buf[0] = '\0';
  for (int i = count - 1; i >= 0; i--) {
    struct vfs_node *n = path_nodes[i];
    if (n->parent == NULL) {
      continue;
    }
    usize name_len = strlen(n->name);
    if (pos + name_len + 2 > buf_len)
      return -ENAMETOOLONG;
    buf[pos++] = '/';
    memcpy(buf + pos, n->name, name_len);
    pos += name_len;
    buf[pos] = '\0';
  }
  if (pos == 0) {
    buf[0] = '/';
    buf[1] = '\0';
  }
  return 0;
}

/* Where an ftruncate spends itself: the filesystem's own truncate, the page
 * cache work around it, or the zero-filling fallback. The browser calls this
 * for every shared-memory region it creates, so the split matters.
 * `b1nix.trace-ftruncate`. */
static inline u64 ftr_now(void) {
  u32 lo, hi;

  { u64 c_ = arch_cycles(); lo = (u32)c_; hi = (u32)(c_ >> 32); }
  return ((u64)hi << 32) | lo;
}

int vfs_ftruncate(int fd, u64 length) {
  u64 ftr_t0 = ftr_now();
  struct vfs_handle *h = get_handle(fd);
  if (!h || !h->used)
    return -EBADF;

  if (h->kind != VFS_HANDLE_NODE || !h->node || !h->node->inode)
    return -EINVAL;

  struct vfs_node *node = h->node;
  struct vfs_inode *inode = node->inode;

  struct vfs_mount_entry *mnt = vfs_get_mount_for_node(node);
  if (mnt && (mnt->flags & MS_RDONLY))
    return -EROFS;

  if ((h->flags & 3) == B1NIX_O_RDONLY)
    return -EINVAL;

  /* M109 chattr: truncation changes the contents and shortens the file, which
   * both immutable and append-only forbid. */
  if (inode->attr & (VFS_ATTR_IMMUTABLE | VFS_ATTR_APPEND))
    return -EPERM;

  vfs_inode_lock(inode);

  if (inode->type != VFS_FILE) {
    vfs_inode_unlock(inode);
    return -EINVAL;
  }

  /* M56 sealing: F_SEAL_SHRINK forbids reducing size, F_SEAL_GROW forbids
   * increasing it. F_SEAL_WRITE also blocks any size change. */
  if ((inode->seals & B1NIX_F_SEAL_SHRINK) && length < (u64)inode->size) {
    vfs_inode_unlock(inode);
    return -EPERM;
  }
  if ((inode->seals & (B1NIX_F_SEAL_GROW | B1NIX_F_SEAL_WRITE)) &&
      length > (u64)inode->size) {
    vfs_inode_unlock(inode);
    return -EPERM;
  }

  if (length > MAX_FILE_SIZE) {
    vfs_inode_unlock(inode);
    return -EFBIG;
  }

  /* Invalidate cached pages beyond min(old, new) size BEFORE the fs callback
   * runs: dirty pages past the new EOF must never reach the disk, and the
   * stale tail of the partial page must read back as zeros after a
   * shrink-then-grow (POSIX) instead of resurrecting pre-truncate contents. */
  page_cache_truncate_inode(inode,
                            length < (u64)inode->size ? length
                                                      : (u64)inode->size);

  if (inode->truncate_cb || inode->setattr_cb) {
    /* Zero-fill by writing only when the filesystem has no truncate of its
     * own. Growing a file through its write path asks it to write past its own
     * end, which a backing store that refuses exactly that (an in-memory file
     * knows nothing beyond its size) answers with an error — so ftruncate
     * failed for the one case it was meant to serve: making a fresh anonymous
     * region big enough to use. A truncate_cb sets the size itself, and the
     * new bytes read as zeroes because that is what growing a file means. */
    if (length > inode->size && inode->write_cb && !inode->truncate_cb) {
      /* One page per write_cb call made growing a file a page-at-a-time walk
       * through the whole filesystem write path; the buffer is on the heap, so
       * it may as well be the 64 KiB Linux moves per iteration in its own
       * zero-filling loops. A page is the fallback when the heap is tight. */
      usize zsize = 64 * 1024;
      char *zeroes = kzalloc(zsize);
      if (!zeroes) {
        zsize = 4096;
        zeroes = kzalloc(zsize);
      }
      if (!zeroes) {
        vfs_inode_unlock(inode);
        return -ENOMEM;
      }
      u64 off = inode->size;
      while (off < length) {
        usize chunk = (usize)(length - off);
        if (chunk > zsize)
          chunk = zsize;
        isize written = inode->write_cb(node, off, zeroes, chunk, h->flags);
        if (written < 0) {
          kfree(zeroes);
          vfs_inode_unlock(inode);
          char b[80];
          snprintf(b, sizeof(b), "vfs_ftruncate: write_cb returned %ld\n", (long)written);
          console_write(b);
          return (int)written;
        }
        if (written == 0) {
          kfree(zeroes);
          vfs_inode_unlock(inode);
          console_write("vfs_ftruncate: write_cb returned 0 -> -EIO\n");
          return -EIO;
        }
        off += (u64)written;
      }
      kfree(zeroes);
    }

    if (inode->truncate_cb) {
      u64 t_cb0 = ftr_now();
      int res = inode->truncate_cb(node, length);
      u64 t_cb1 = ftr_now();

      if (bootinfo_has_flag("b1nix.trace-ftruncate") &&
          (t_cb1 - ftr_t0) > 10000000ULL) {
        console_write("ftruncate: len=");
        console_write_dec(length / 1024);
        console_write("K total=");
        console_write_dec((t_cb1 - ftr_t0) / 1000000);
        console_write("M cb=");
        console_write_dec((t_cb1 - t_cb0) / 1000000);
        console_write("M\n");
      }
      vfs_inode_unlock(inode);
      if (res < 0) {
        char b[80];
        snprintf(b, sizeof(b), "vfs_ftruncate: truncate_cb returned %d\n", res);
        console_write(b);
      }
      return res;
    }

    inode->size = (usize)length;
    vfs_update_times(inode, VFS_MTIME | VFS_CTIME);
    int res = inode->setattr_cb(node);
    vfs_inode_unlock(inode);
    if (res < 0) {
      char b[80];
      snprintf(b, sizeof(b), "vfs_ftruncate: setattr_cb returned %d\n", res);
      console_write(b);
    }
    return res;
  }

  if (length > inode->capacity) {
    usize new_cap = inode->capacity == 0 ? 1024 : inode->capacity * 2;
    while (new_cap < length)
      new_cap *= 2;
    void *new_data = kzalloc(new_cap);
    if (!new_data) {
      vfs_inode_unlock(inode);
      return -ENOMEM;
    }
    if (inode->data) {
      /* Same clamp as vfs_write: copy no more than the new buffer holds. */
      usize keep = inode->size;
      if (keep > new_cap)
        keep = new_cap;
      memcpy(new_data, inode->data, keep);
      if (inode->flags & VFS_NODE_OWNS_DATA)
        kfree(inode->data);
    }
    inode->data = new_data;
    inode->capacity = new_cap;
    inode->flags |= VFS_NODE_OWNS_DATA;
  } else if (length > inode->size) {
    if (inode->data) {
      memset((char *)inode->data + inode->size, 0, (usize)(length - inode->size));
    }
  }

  inode->size = (usize)length;
  vfs_update_times(inode, VFS_MTIME | VFS_CTIME);
  vfs_inode_unlock(inode);
  return 0;
}

static isize memfd_read(struct vfs_node *node, u64 offset, char *buffer,
                        usize size, int flags) {
  (void)flags;
  if (!node || !node->inode || offset >= node->inode->size)
    return 0;
  usize available = node->inode->size - (usize)offset;
  usize count = size < available ? size : available;
  /* Beyond what is backed, the file reads as zeroes.
   *
   * A hole is not an error and not a short read: ftruncate no longer allocates
   * (see memfd_truncate), so most of a freshly sized region has no buffer
   * behind it at all. Returning the count without touching the caller's buffer
   * would hand back whatever was already in it. */
  if (count > 0) {
    usize backed = node->inode->capacity > (usize)offset
                       ? node->inode->capacity - (usize)offset
                       : 0;
    usize from_data = backed < count ? backed : count;

    if (from_data && node->inode->data)
      memcpy(buffer, (const char *)node->inode->data + offset, from_data);
    else
      from_data = 0;
    if (count > from_data)
      memset(buffer + from_data, 0, count - from_data);
  }
  return (isize)count;
}

static int memfd_truncate(struct vfs_node *node, u64 length);
static int memfd_reserve(struct vfs_inode *inode, u64 length);

static isize memfd_write(struct vfs_node *node, u64 offset,
                         const char *buffer, usize size, int flags) {
  (void)flags;
  if (!node || !node->inode)
    return 0;
  /* A write past the end extends the file, as it does on any other file.
   * Refusing it made an in-memory file the only one that could not be
   * appended to, and left it stuck at whatever size it was created with. */
  if (offset + size > (u64)node->inode->size) {
    int rc = memfd_truncate(node, offset + size);
    if (rc < 0)
      return rc;
  }
  usize available = node->inode->size - (usize)offset;
  usize count = size < available ? size : available;
  if (count > 0) {
    int rc = memfd_reserve(node->inode, offset + count);

    if (rc < 0)
      return rc;
    memcpy((char *)node->inode->data + offset, buffer, count);
  }
  return (isize)count;
}

/* Resize an in-memory file.
 *
 * ftruncate on a memfd is not an edge case: it is how every anonymous shared
 * region gets its size, right after creation and before anyone maps it. The
 * buffer grows to hold the new length and the bytes added read as zeroes. */
static int memfd_truncate(struct vfs_node *node, u64 length) {
  if (!node || !node->inode)
    return -EINVAL;
  struct vfs_inode *inode = node->inode;

  /* Setting the size allocates nothing.
   *
   * This is what makes an anonymous shared region affordable, and it is what
   * Linux does: ftruncate on a memfd records a length, and pages appear as
   * they are touched. Allocating the whole length here made a compositor's
   * 512 MiB buffer pool cost 512 MiB of contiguous kernel memory the instant
   * it was created -- and, because the capacity doubled rather than matching
   * the request, a 260 MiB pool cost the same. Four such regions exhausted a
   * 4 GiB machine about twenty seconds into a KDE session, after which the
   * failures were whatever ran next: a lazy page with no frame to back it, a
   * shootdown stalled behind a starved CPU, the OOM killer taking the shell.
   *
   * Mapping the region is unaffected -- an mmap faults page by page through
   * the page cache, which calls read_cb and gets zeroes for a hole. Only a
   * write(2) into the file needs a real buffer, and memfd_write reserves it
   * then, for as much of the file as it is about to write.
   */
  if (length > inode->size && inode->data &&
      inode->capacity > (usize)inode->size)
    memset((char *)inode->data + inode->size, 0,
           (usize)((length < inode->capacity ? length : inode->capacity) -
                   inode->size));
  inode->size = (usize)length;
  return 0;
}

/* Give an in-memory file a buffer covering at least `length` bytes.
 *
 * Called from the write path only: everything else either reads (holes are
 * zeroes) or maps (the page cache owns the pages). The growth still doubles,
 * because a file being written a piece at a time would otherwise reallocate on
 * every call -- but it is now driven by bytes actually written rather than by
 * a size someone declared. */
static int memfd_reserve(struct vfs_inode *inode, u64 length) {
  if (!inode)
    return -EINVAL;
  if (length <= inode->capacity && inode->data)
    return 0;

  {
    usize new_cap = inode->capacity ? inode->capacity : 1024;
    while (new_cap < length) {
      if (new_cap > (usize)1 << 40)
        return -EFBIG;
      new_cap *= 2;
    }
    /* How long a large region takes to materialise, and how large it was.
     * Chromium creates shared-memory regions on the browser's UI thread, so
     * this allocation is on the critical path of every mojo data pipe.
     * `b1nix.trace-memfd`. */
    u32 t_lo, t_hi;

    { u64 c_ = arch_cycles(); t_lo = (u32)c_; t_hi = (u32)(c_ >> 32); }
    u64 t0 = ((u64)t_hi << 32) | t_lo;
    void *new_data = kzalloc(new_cap);
    if (!new_data)
      return -ENOMEM;
    if (new_cap >= (1u << 20) && bootinfo_has_flag("b1nix.trace-memfd")) {
      u32 e_lo, e_hi;

      { u64 c_ = arch_cycles(); e_lo = (u32)c_; e_hi = (u32)(c_ >> 32); }
      console_write("memfd: grew to ");
      console_write_dec(new_cap / 1024);
      console_write(" KiB in ");
      console_write_dec(((((u64)e_hi << 32) | e_lo) - t0) / 1000000);
      console_write(" Mcycles\n");
    }
    if (inode->data) {
      /* By capacity, not by size.
       *
       * Since ftruncate stopped allocating, the size is a declaration and the
       * capacity is the buffer -- and the size can be enormously larger. A
       * file declared 512 MiB whose first write is four bytes has a 1 KiB
       * buffer; copying `size` bytes out of it reads half a gigabyte past the
       * source and writes it past the destination, which faulted in kernel
       * memcpy exactly one page after the new allocation ended. */
      usize keep = inode->capacity < new_cap ? inode->capacity : new_cap;

      memcpy(new_data, inode->data, keep);
      if (inode->flags & VFS_NODE_OWNS_DATA)
        kfree(inode->data);
    }
    inode->data = new_data;
    inode->capacity = new_cap;
    inode->flags |= VFS_NODE_OWNS_DATA;
  }

  return 0;
}

int vfs_memfd_create(const char *name, u32 flags) {
  /* MFD_NOEXEC_SEAL and MFD_EXEC decide whether the file may ever be mapped
   * executable; MFD_HUGETLB asks for huge pages. b1nix never maps a memfd
   * executable, so NOEXEC_SEAL is the behaviour it already has and EXEC is the
   * one thing here it cannot honour. Huge pages are a performance request, not
   * a semantic one, and are satisfied with ordinary pages.
   *
   * Rejecting these outright is what a kernel older than Linux 6.3 does, and
   * modern userspace passes MFD_NOEXEC_SEAL by default: glibc, pulseaudio and
   * Chromium all failed to create shared memory here, which in Chromium's case
   * left its discardable-memory allocator uninstantiated and killed the
   * browser the first time Skia cached a blur. */
  if (flags & ~(u32)(B1NIX_MFD_CLOEXEC | B1NIX_MFD_ALLOW_SEALING |
                     B1NIX_MFD_HUGETLB | B1NIX_MFD_NOEXEC_SEAL |
                     B1NIX_MFD_EXEC))
    return -EINVAL;
  /* Linux refuses the pair: a file cannot be sealed non-executable and
   * executable at once. */
  if ((flags & B1NIX_MFD_NOEXEC_SEAL) && (flags & B1NIX_MFD_EXEC))
    return -EINVAL;

  struct vfs_node *node = vfs_create_node(VFS_FILE);
  if (!node)
    return -ENOMEM;

  copy_path(node->name, sizeof(node->name), name && name[0] ? name : "memfd");
  node->deleted = 1;
  node->inode->nlink = 0;
  node->inode->mode = 0600;
  /* M56: only memfds created with MFD_ALLOW_SEALING accept F_ADD_SEALS. */
  node->inode->seals_allowed = (flags & B1NIX_MFD_ALLOW_SEALING) ? 1 : 0;
  node->inode->seals = 0;
  const struct cred *cred = get_current_cred();
  node->inode->uid = cred ? cred->euid : ROOT_UID;
  node->inode->gid = cred ? cred->egid : ROOT_GID;
  node->inode->read_cb = memfd_read;
  node->inode->write_cb = memfd_write;
  node->inode->truncate_cb = memfd_truncate;
  /* write_cb stays, because write(2) still has to work; the flag is what tells
   * reclaim not to call it for a page it already holds. */
  node->inode->flags |= VFS_NODE_MEMORY_BACKED;
  vfs_init_times(node->inode);

  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_NODE);
  if (!h) {
    vfs_node_put(node);
    return -ENFILE;
  }
  h->node = node;
  h->ops = &node_file_ops;
  h->flags = B1NIX_O_RDWR;

  int fd = scheduler_fd_alloc(h);
  if (fd < 0) {
    vfs_handle_release(h);
    return fd == -ENOMEM ? -ENOMEM : -EMFILE;
  }
  if (flags & B1NIX_MFD_CLOEXEC)
    scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  return fd;
}

/* How often each fcntl command is asked for.
 *
 * A thread was found spinning here — 72 million context switches deep, its
 * process unable to finish exit_group because that thread never dies. The
 * command is supported (nothing reports EINVAL), so the question is which one
 * it repeats: a call that returns the same unhelpful answer forever looks
 * exactly like a working call from inside the kernel. */
static u64 g_fcntl_calls[64];

void vfs_fcntl_dump_counts(void) {
  console_write("fcntl calls:");
  for (unsigned i = 0; i < 64; i++) {
    if (!g_fcntl_calls[i])
      continue;
    console_write(" cmd");
    console_write_dec(i);
    console_write("=");
    console_write_dec(g_fcntl_calls[i]);
  }
  console_write("\n");
}

int vfs_fcntl(int fd, int cmd, u64 arg) {
  if (cmd >= 0 && cmd < 64)
    g_fcntl_calls[cmd]++;
  struct vfs_handle *h = get_handle(fd);
  if (!h)
    return -EBADF; /* POSIX: a closed fd is EBADF, not the bare -1 that
                    * userspace decodes as EPERM. BusyBox ash saves any open
                    * fd 3 with fcntl(3, F_DUPFD, 10) before a redirection and
                    * treats EBADF as "nothing to save"; EPERM aborted the
                    * redirection instead, which broke `exec 3>&1` in every
                    * OpenRC init script. */
  switch (cmd) {
  case B1NIX_F_DUPFD:
    return vfs_dup_min(fd, (int)arg);
  case B1NIX_F_DUPFD_CLOEXEC: {
    /* Like F_DUPFD, but the new descriptor has FD_CLOEXEC set. The new fd is a
     * fresh table slot (fd_flags zeroed by scheduler_fd_set/alloc), so we set
     * the flag explicitly after the dup. Used by the multiprocess broker model
     * (base/posix) to hand a child a descriptor that won't leak across exec. */
    int newfd = vfs_dup_min(fd, (int)arg);
    if (newfd >= 0)
      scheduler_fd_flags_set(newfd, B1NIX_FD_CLOEXEC);
    return newfd;
  }
  case B1NIX_F_GETFD:
    return scheduler_fd_flags_get(fd);
  case B1NIX_F_SETFD:
    return scheduler_fd_flags_set(fd, (int)arg);
  case B1NIX_F_GETFL:
    return h->flags;
  case B1NIX_F_SETFL: {
    /* F_SETFL sets only the status flags. The access mode and the flags that
     * only mean something at open() time (O_CREAT, O_EXCL, O_TRUNC, O_CLOEXEC,
     * O_DIRECTORY) are fixed for the life of the description and are ignored
     * here — POSIX says so, and assigning the argument wholesale instead turned
     * the routine fcntl(fd, F_SETFL, O_NONBLOCK) that every event loop performs
     * into a silent downgrade of a read-write descriptor to O_RDONLY. */
    const int settable = B1NIX_O_APPEND | B1NIX_O_NONBLOCK;
    h->flags = (h->flags & ~settable) | ((int)arg & settable);
    return 0;
  }
  case B1NIX_F_ADD_SEALS:
    return vfs_fcntl_add_seals(fd, (u32)arg);
  case B1NIX_F_GET_SEALS:
    return vfs_fcntl_get_seals(fd);
  case B1NIX_F_GETLK:
  case B1NIX_F_SETLK:
  case B1NIX_F_SETLKW:
    if (h->kind != VFS_HANDLE_NODE)
      return -EBADF;
    return filelock_set_lock(fd, cmd, (struct flock *)(usize)arg);
  default:
    /* Name the command we are refusing.
     *
     * A caller that does not expect EINVAL here retries, and a retry loop is
     * indistinguishable from a hang: a thread was found spinning on fcntl 72
     * million context switches deep, which kept its process from ever
     * finishing exit_group. Say which command, once per distinct value. */
    {
      static u32 seen[16];
      static unsigned nseen;
      unsigned i;
      for (i = 0; i < nseen; i++)
        if (seen[i] == (u32)cmd)
          break;
      if (i == nseen && nseen < 16) {
        seen[nseen++] = (u32)cmd;
        console_write("fcntl: unsupported command ");
        console_write_dec((u64)(u32)cmd);
        console_write(" -> EINVAL\n");
      }
    }
    return -EINVAL;
  }
}

/* ── M109: FS_IOC_GETFLAGS / FS_IOC_SETFLAGS (chattr, lsattr) ──────────────
 * Linux spells the argument `long` in the ioctl number but its own handlers
 * copy an `int`, and both e2fsprogs and BusyBox pass an int — so this copies
 * four bytes, matching what the callers actually hand over. */
static int vfs_ioctl_fsflags(struct vfs_node *node, int nr, void *arg) {
  struct vfs_inode *inode = node->inode;
  if (!arg)
    return -EFAULT;
  /* Attributes belong to files and directories. A device node has none. */
  if (inode->type != VFS_FILE && inode->type != VFS_DIRECTORY)
    return -ENOTTY;

  if (nr == 1) { /* FS_IOC_GETFLAGS */
    int flags = (int)(inode->attr & VFS_ATTR_USER_MASK);
    return syscall_copyout(arg, &flags, sizeof(flags)) < 0 ? -EFAULT : 0;
  }

  int want = 0;
  if (syscall_copyin(&want, arg, sizeof(want)) < 0)
    return -EFAULT;
  u32 attr = (u32)want;
  /* Everything above the low byte is the filesystem's own business (extents,
   * inline data, ...). Refuse to pretend we stored a flag we cannot. */
  if (attr & ~VFS_ATTR_USER_MASK)
    return -EOPNOTSUPP;

  struct vfs_mount_entry *mnt = vfs_get_mount_for_node(node);
  if (mnt && (mnt->flags & MS_RDONLY))
    return -EROFS;

  const struct cred *cred = get_current_cred();
  if (cred && cred->euid != ROOT_UID && cred->euid != inode->uid)
    return -EPERM;
  /* Only root may raise or lower immutable/append-only. Letting the owner do
   * it would make both flags decorative: the owner would simply take the flag
   * off and then do whatever it was meant to prevent. */
  if (cred && cred->euid != ROOT_UID &&
      ((attr ^ inode->attr) & (VFS_ATTR_IMMUTABLE | VFS_ATTR_APPEND)))
    return -EPERM;

  /* A filesystem that cannot store the flags says so rather than accepting
   * them and forgetting at umount: a chattr that silently does not stick is
   * worse than one that fails. GETFLAGS above still answers for everyone. */
  if (!inode->setflags_cb)
    return -EOPNOTSUPP;

  u32 old = inode->attr;
  inode->attr = attr;
  int err = inode->setflags_cb(node, attr);
  if (err < 0) {
    inode->attr = old;
    return err;
  }
  return 0;
}

/* FITRIM (fstrim): hand every free range of the filesystem this node lives on
 * back to the device. Issued on the mount point, which is a directory — hence
 * handled before vfs_ioctl's "device nodes only" rule. */
static int vfs_ioctl_fitrim(struct vfs_node *node, void *arg) {
  struct fstrim_range_k {
    u64 start;
    u64 len;
    u64 minlen;
  } range;
  if (!arg || syscall_copyin(&range, arg, sizeof(range)) < 0)
    return -EFAULT;
  const struct cred *cred = get_current_cred();
  if (cred && cred->euid != ROOT_UID)
    return -EPERM;
  if (!node->inode->fitrim_cb)
    return -EOPNOTSUPP;
  u64 trimmed = 0;
  int err = node->inode->fitrim_cb(node, range.start, range.len, range.minlen,
                                   &trimmed);
  if (err < 0)
    return err;
  /* fstrim prints the returned len as "N bytes were trimmed". */
  range.len = trimmed;
  return syscall_copyout(arg, &range, sizeof(range)) < 0 ? -EFAULT : 0;
}

int vfs_ioctl(int fd, u64 request, void *arg) {
  /* Handles with their own ioctl op (pty master/slave) take priority — they
   * are raw handles with no backing VFS node. */
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (h && h->ops && h->ops->ioctl)
    return h->ops->ioctl(h, request, arg);

  struct vfs_node *node = vfs_find_node_by_fd(fd);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);
  if (node->inode->type != VFS_DEVICE) {
    /* M109: the two ioctl families that are issued on ordinary files and
     * directories rather than on device nodes — inode attributes
     * (chattr/lsattr) and FITRIM (fstrim). Matched on ioctl type + command
     * number, ignoring the size/direction bits, as the block ioctls below are.
     * Kept inside the non-device branch so no device's own ioctl hook can be
     * shadowed by a type byte that happens to collide. */
    u8 type = (u8)((request >> 8) & 0xFF);
    u8 nr = (u8)(request & 0xFF);
    if (type == 'f' && (nr == 1 || nr == 2))
      return vfs_ioctl_fsflags(node, nr, arg);
    if (type == 'X' && nr == 121)
      return vfs_ioctl_fitrim(node, arg);
    return -EINVAL;
  }

  /* Devices with their own ioctl hook (M47 /dev/fb0) dispatch before the
   * legacy name-based special cases below. */
  if (node->inode->ioctl_cb)
    return node->inode->ioctl_cb(node, request, arg);

  /* Loop-device control ioctls (BusyBox losetup): the LOOP_* family is type
   * 0x4C, plus the /dev/loop-control node. Handled before the `arg` check
   * because LOOP_CTL_GET_FREE / LOOP_CLR_FD carry no argument. */
  if (((request >> 8) & 0xFF) == 0x4C ||
      strcmp(node->name, "loop-control") == 0)
    return loop_ioctl(node, request, arg);

  /* The tty ioctls whose argument is a VALUE, not a pointer.
   *
   * TIOCSCTTY takes 0 or 1 and TIOCNOTTY takes nothing at all, so the blanket
   * "no argument means EINVAL" below rejected them out of hand. systemd opens
   * the console for a getty and calls ioctl(fd, TIOCSCTTY, 0); the EINVAL came
   * back as "Failed to set up standard input: Invalid argument" and no login
   * prompt was ever printed. */
  /* TIOCCONS: print the kernel console on THIS terminal from now on.
   *
   * Linux's rule, kept here: issued on /dev/console it cancels any
   * redirection; issued on a terminal it redirects to that terminal. Only
   * root may move the console, because the machine's log is not something an
   * ordinary user gets to capture. */
  if (request == B1NIX_TIOCCONS) {
    struct cred *c = scheduler_get_current_cred();

    if (!cred_has_cap(c, CAP_SYS_ADMIN))
      return -EPERM;
    if (strcmp(node->name, "console") == 0)
      return console_redirect_set(0) == 0 ? 0 : -EIO;
    return console_redirect_set(node) == 0 ? 0 : -EIO;
  }

  if ((strcmp(node->name, "tty") == 0 || strcmp(node->name, "console") == 0)) {
    if (request == B1NIX_TIOCSCTTY) {
      /* A session leader claims the console: its group becomes the foreground
       * one, so ^C and job control reach the right processes. */
      if (current_task) {
        console.session_id = current_task->session_id;
        console.fg_pgrp = current_task->process_group_id;
        if (current_task->session_id == current_task->id)
          scheduler_set_ctty(current_task, 1, 0);
      }
      return 0;
    }
    if (request == B1NIX_TIOCNOTTY)
      return 0;

    /* TIOCVHANGUP / TCFLSH / TCSBRK / TIOCEXCL / TIOCNXCL: nothing to do on a
     * console with no line discipline state to throw away, and each is
     * "succeeded" rather than "unsupported" because that is the truth — there
     * is no buffered output to drain and no exclusive-open state to set. */
    if (request == 0x5437 /* TIOCVHANGUP */ || request == 0x540B /* TCFLSH */ ||
        request == 0x5409 /* TCSBRK */ || request == 0x540C /* TIOCEXCL */ ||
        request == 0x540D /* TIOCNXCL */)
      return 0;
  }

  /* These come BEFORE the "an ioctl with no argument is a mistake" rule
   * below: RAID_AUTORUN, NBD_DO_IT, NBD_CLEAR_SOCK and NBD_DISCONNECT all
   * carry no argument, and the blanket rejection turned every one of them into
   * "ioctl 0x914 failed: Invalid argument" without the handler ever running. */
  /* The nbd ioctls (0xab00..0xab0a), as nbd-client issues them.
   *
   * The client has already connected and completed the handshake; what reaches
   * here is the geometry it learned and the socket it learned it on. The
   * device is named by the node the ioctl was issued on -- /dev/nbd0 is
   * device 0 -- so the kernel does not have to guess which export is meant.
   */
  if ((request & 0xFF00) == 0xab00) {
    struct cred *c = scheduler_get_current_cred();
    unsigned index = 0;
    const char *nm = node->name;

    if (!cred_has_cap(c, CAP_SYS_ADMIN))
      return -EPERM;
    if (strncmp(nm, "nbd", 3) != 0)
      return -ENOTTY;
    for (const char *p = nm + 3; *p >= '0' && *p <= '9'; p++)
      index = index * 10 + (unsigned)(*p - '0');

    struct nbd_device *nd = nbd_device_at(index);
    if (!nd)
      return -ENODEV;

    switch (request & 0xFF) {
    case 0x00: { /* NBD_SET_SOCK: the connected socket, by descriptor */
      struct vfs_handle *sock = vfs_handle_acquire((int)(isize)arg);

      if (!sock)
        return -EBADF;
      int rc = nbd_set_socket(nd, sock);
      if (rc != 0)
        vfs_handle_release(sock);
      return rc;
    }
    case 0x01: /* NBD_SET_BLKSIZE */
      return nbd_set_geometry(nd, (u32)(usize)arg, nbd_block_count(nd));
    case 0x02: /* NBD_SET_SIZE: bytes */
      return nbd_set_geometry(nd, nbd_block_size(nd),
                              (u64)(usize)arg / (nbd_block_size(nd) ?: 512));
    case 0x07: /* NBD_SET_SIZE_BLOCKS */
      return nbd_set_geometry(nd, nbd_block_size(nd) ?: 512, (u64)(usize)arg);
    case 0x03: /* NBD_DO_IT: serve until disconnected */
      return nbd_run(nd);
    case 0x04: /* NBD_CLEAR_SOCK */
    case 0x05: /* NBD_CLEAR_QUE — nothing is queued here; clearing is the same */
      return nbd_clear_socket(nd);
    case 0x08: /* NBD_DISCONNECT */
      return nbd_disconnect(nd);
    case 0x06: /* NBD_PRINT_DEBUG */
    case 0x09: /* NBD_SET_TIMEOUT */
    case 0x0a: /* NBD_SET_FLAGS */
      return 0;
    default:
      return -ENOTTY;
    }
  }

  /* RAID_AUTORUN: "find the arrays and start them".
   *
   * The number is _IO(MD_MAJOR=9, 0x14) = 0x0914, which is what raidautorun
   * actually sends -- an invented one produced "ioctl 0x914 failed: Invalid
   * argument" and no assembly at all.
   *
   * This is what `raidautorun /dev/md0` asks for, and it is the only assembly
   * path here -- there is no mdadm on this system, so an array is described by
   * the superblocks its members carry and brought up by this scan. Requires
   * CAP_SYS_ADMIN: assembling an array publishes a new block device.
   */
  if (request == 0x0914) {
    struct cred *c = scheduler_get_current_cred();

    if (!cred_has_cap(c, CAP_SYS_ADMIN))
      return -EPERM;
    return md_autorun() > 0 ? 0 : -ENODEV;
  }


  if (!arg)
    return -EINVAL;

  /* HDIO_GETGEO: mkfs.vfat and fdisk want a CHS geometry for the boot sector.
   * No device b1nix drives addresses by cylinder, so the numbers are derived
   * from the capacity with Linux's own convention (255 heads, 63 sectors) —
   * the same synthesis Linux performs for every modern disk. */
  if (node->inode->blk_dev && request == 0x0301) {
    struct block_device *bd = node->inode->blk_dev;
    u64 sectors = ((u64)bd->block_size * bd->block_count) / 512;
    struct {
      u8 heads;
      u8 sectors;
      u16 cylinders;
      unsigned long start;
    } geo;
    geo.heads = 255;
    geo.sectors = 63;
    u64 cyl = sectors / (255ull * 63ull);
    geo.cylinders = (u16)(cyl > 0xFFFF ? 0xFFFF : cyl);
    geo.start = 0;
    return syscall_copyout(arg, &geo, sizeof(geo)) < 0 ? -EFAULT : 0;
  }

  /* Block-device size ioctls for BusyBox fdisk. Match on the Linux ioctl
   * "type 0x12" + command number, ignoring the size/dir bits that differ
   * between the 32- and 64-bit ABIs. */
  if (node->inode->blk_dev && ((request >> 8) & 0xFF) == 0x12) {
    struct block_device *bd = node->inode->blk_dev;
    u64 bytes = (u64)bd->block_size * bd->block_count;
    switch (request & 0xFF) {
    case 0x60: { /* BLKGETSIZE: size in 512-byte sectors (unsigned long) */
      unsigned long sectors = (unsigned long)(bytes / 512);
      return syscall_copyout(arg, &sectors, sizeof(sectors)) < 0 ? -EFAULT : 0;
    }
    case 0x68: { /* BLKSSZGET: logical sector size (int) */
      int ss = (int)(bd->block_size ? bd->block_size : 512);
      return syscall_copyout(arg, &ss, sizeof(ss)) < 0 ? -EFAULT : 0;
    }
    case 0x70: { /* BLKBSZGET: block size (size_t) */
      unsigned long bs = (unsigned long)(bd->block_size ? bd->block_size : 512);
      return syscall_copyout(arg, &bs, sizeof(bs)) < 0 ? -EFAULT : 0;
    }
    case 0x72: /* BLKGETSIZE64: size in bytes (u64) */
      return syscall_copyout(arg, &bytes, sizeof(bytes)) < 0 ? -EFAULT : 0;
    case 0x5F: /* BLKRRPART: reread partition table */
      return blk_rescan_partitions(bd) == 0 ? 0 : -EIO;
    case 0x61: /* BLKFLSBUF: flush buffers — accept */
      return 0;
    case 0x77: { /* BLKDISCARD: byte range {start, len} */
      u64 r[2];
      if (syscall_copyin(r, arg, sizeof(r)) < 0)
        return -EFAULT;
      if ((r[0] % 512) || (r[1] % 512) || r[1] == 0)
        return -EINVAL;
      if (r[0] + r[1] > bytes || r[0] + r[1] < r[0])
        return -EINVAL;
      return blk_discard_blocks(bd, r[0] / 512, (u32)(r[1] / 512));
    }
    case 0x7D: /* BLKSECDISCARD: a secure erase b1nix cannot promise. */
      return -EOPNOTSUPP;
    case 0x7F: { /* BLKZEROOUT: byte range {start, len} */
      u64 r[2];
      if (syscall_copyin(r, arg, sizeof(r)) < 0)
        return -EFAULT;
      if ((r[0] % 512) || (r[1] % 512) || r[1] == 0)
        return -EINVAL;
      if (r[0] + r[1] > bytes || r[0] + r[1] < r[0])
        return -EINVAL;
      return blk_zero_blocks(bd, r[0] / 512, (u32)(r[1] / 512)) == 0 ? 0 : -EIO;
    }
    case 0x7C: { /* BLKDISCARDZEROES: does a discard leave zeroes behind? */
      /* No: nothing here promises the content of a discarded range, which is
       * exactly why blk_discard_blocks has no zero-writing fallback. */
      int z = 0;
      return syscall_copyout(arg, &z, sizeof(z)) < 0 ? -EFAULT : 0;
    }
    case 0x7E: { /* BLKROTATIONAL */
      int rot = bd->rotational ? 1 : 0;
      return syscall_copyout(arg, &rot, sizeof(rot)) < 0 ? -EFAULT : 0;
    }
    case 0x78:   /* BLKIOMIN */
    case 0x79:   /* BLKIOOPT */
    case 0x7B: { /* BLKPBSZGET: physical block size */
      /* One 512-byte block in, one out: no device here reports a physical
       * block larger than its logical one, and inventing a number would make
       * mkfs align to a boundary that does not exist. */
      unsigned int v = (unsigned int)(bd->block_size ? bd->block_size : 512);
      return syscall_copyout(arg, &v, sizeof(v)) < 0 ? -EFAULT : 0;
    }
    case 0x7A: { /* BLKALIGNOFF: alignment offset */
      int off = 0;
      return syscall_copyout(arg, &off, sizeof(off)) < 0 ? -EFAULT : 0;
    }
    case 0x5E: { /* BLKROGET: is the device read-only? */
      /* Real information rather than a constant: a loop device associated
       * through a read-only descriptor refuses writes, and blockdev --getro
       * should say so. */
      int ro = bd->write_blocks ? 0 : 1;
      return syscall_copyout(arg, &ro, sizeof(ro)) < 0 ? -EFAULT : 0;
    }
    default:
      return -ENOTTY;
    }
  }

  if (strcmp(node->name, "tty") != 0 && strcmp(node->name, "console") != 0)
    return -ENOTTY;

  if (request == B1NIX_TCGETS) {
    return tty_termios_copyout(arg, &console.termios);
  }
  /* TCGETS2/TCSETS2: the same operations in the layout glibc 2.42 and later
   * use for every tcgetattr(3) and tcsetattr(3) call. This is the one that
   * matters most on /dev/console: isatty(3) IS tcgetattr succeeding, and a
   * console that refuses it is not a terminal to anything built against a libc
   * that new -- including an init system, which then has nowhere to print the
   * reason it is giving up. */
  if (request == B1NIX_TCGETS2) {
    return tty_termios2_copyout(arg, &console.termios);
  }
  if (request == B1NIX_TCSETS || request == B1NIX_TCSETSW ||
      request == B1NIX_TCSETSF) {
    /* TCSADRAIN/TCSAFLUSH (TCSETSW/TCSETSF): no output buffering, apply as TCSETS. */
    return tty_termios_copyin(&console.termios, arg);
  }
  if (tty_is_termios2_set(request)) {
    return tty_termios2_copyin(&console.termios, arg);
  }
  if (request == B1NIX_TIOCGPGRP) {
    /* The user buffer is a pid_t (32-bit) — copying sizeof(usize) would
     * read/write 4 bytes of adjacent user stack on x86_64. */
    int fg32 = (int)console.fg_pgrp;
    if (!arg || syscall_copyout(arg, &fg32, sizeof(fg32)) < 0)
      return -EFAULT;
    return 0;
  }
  if (request == B1NIX_TIOCSPGRP) {
    int fg32;
    if (!arg || syscall_copyin(&fg32, arg, sizeof(fg32)) < 0)
      return -EFAULT;
    /* The strict POSIX same-session check (current_task->session_id ==
     * console.session_id) stays disabled: the boot console predates every
     * session and is never claimed via TIOCSCTTY. */
    console.fg_pgrp = (usize)fg32;
    return 0;
  }
  if (request == B1NIX_TIOCNOTTY) {
    /* Detach-from-controlling-tty: accepted as a no-op on the boot console
     * (getty's setsid-fallback path calls this and only needs success). */
    return 0;
  }
  /* An ioctl this terminal does not implement is ENOTTY, which is what Linux
   * answers and what every caller tests for. It used to be a bare -1, i.e.
   * EPERM -- "operation not permitted" for a request the console had simply
   * never heard of. A program that probes for an optional ioctl reads EPERM as
   * a permission problem worth reporting, or worth giving up over, and the two
   * are not the same fact. */
  return -ENOTTY;
}

void vfs_close_on_exec(void) {
  if (!current_task)
    return;
  for (usize i = 0; i < current_task->fd_capacity; i++) {
    int flags = scheduler_fd_flags_get((int)i);
    if (flags >= 0 && (flags & B1NIX_FD_CLOEXEC) != 0) {
      vfs_close((int)i);
    }
  }
}

/* ── Permission Management Functions ── */

int vfs_chmod(const char *path, u16 mode) {
  struct vfs_node *node = vfs_find_node(path);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);

  int res = 0;
  const struct cred *cred = get_current_cred();
  if (!cred) {
    res = -EACCES;
    goto out;
  }

  if (cred->euid != ROOT_UID && cred->euid != node->inode->uid) {
    if (!cred_has_cap(cred, CAP_FOWNER)) {
      res = -EPERM;
      goto out;
    }
  }

  node->inode->mode = (node->inode->mode & ~07777) | (mode & 07777);
  vfs_update_times(node->inode, VFS_CTIME);
  vfs_inotify_notify(node, IN_ATTRIB, 0); /* M107 */
  if (node->inode->setattr_cb) {
    res = node->inode->setattr_cb(node);
    goto out;
  }

out:
  vfs_node_put(node);
  return res;
}

int vfs_utime(const char *path, u64 atime, u64 mtime) {
  struct vfs_node *node = vfs_find_node(path);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);

  int res = 0;
  const struct cred *cred = get_current_cred();
  if (!cred) {
    res = -EACCES;
    goto out;
  }

  /* POSIX: setting explicit times needs ownership (or CAP_FOWNER). */
  if (cred->euid != ROOT_UID && cred->euid != node->inode->uid) {
    if (!cred_has_cap(cred, CAP_FOWNER)) {
      res = -EPERM;
      goto out;
    }
  }

  /* utimes(2) names whole seconds; the sub-second halves it does not name are
   * zeroed rather than left over from the last write. */
  node->inode->atime = atime;
  node->inode->atime_nsec = 0;
  node->inode->mtime = mtime;
  node->inode->mtime_nsec = 0;
  vfs_update_times(node->inode, VFS_CTIME);
  vfs_inotify_notify(node, IN_ATTRIB, 0); /* M107 */
  if (node->inode->setattr_cb) {
    res = node->inode->setattr_cb(node);
    goto out;
  }

out:
  vfs_node_put(node);
  return res;
}

int vfs_fchmod(int fd, u16 mode) {
  struct vfs_handle *handle = get_handle(fd);
  if (!handle || !handle->used)
    return -EBADF;
  if (handle->kind != VFS_HANDLE_NODE || !handle->node)
    return -EINVAL;

  const struct cred *cred = get_current_cred();
  if (!cred)
    return -EACCES;

  if (cred->euid != ROOT_UID && cred->euid != handle->node->inode->uid) {
    if (!cred_has_cap(cred, CAP_FOWNER))
      return -EPERM;
  }

  handle->node->inode->mode =
      (handle->node->inode->mode & ~07777) | (mode & 07777);
  vfs_update_times(handle->node->inode, VFS_CTIME);
  if (handle->node->inode->setattr_cb)
    return handle->node->inode->setattr_cb(handle->node);
  return 0;
}

/* Shared body of chown/lchown: `nofollow` selects whether a trailing symlink is
 * resolved (chown) or is itself the target (lchown). */
static int vfs_chown_common(const char *path, u16 uid, u16 gid, int nofollow) {
  struct vfs_node *node =
      nofollow ? vfs_find_node_no_follow(path) : vfs_find_node(path);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);

  int res = 0;
  const struct cred *cred = get_current_cred();
  if (!cred) {
    res = -EACCES;
    goto out;
  }

  /* Only root can change owner */
  if (!cred_has_cap(cred, CAP_CHOWN)) {
    res = -EPERM;
    goto out;
  }

  if (uid != (u16)-1)
    node->inode->uid = uid;
  if (gid != (u16)-1)
    node->inode->gid = gid;
  vfs_update_times(node->inode, VFS_CTIME);
  vfs_inotify_notify(node, IN_ATTRIB, 0); /* M107 */
  if (node->inode->setattr_cb) {
    res = node->inode->setattr_cb(node);
    goto out;
  }

out:
  vfs_node_put(node);
  return res;
}

int vfs_chown(const char *path, u16 uid, u16 gid) {
  return vfs_chown_common(path, uid, gid, 0);
}

/* lchown(2): change the ownership of a symlink itself. */
int vfs_lchown(const char *path, u16 uid, u16 gid) {
  return vfs_chown_common(path, uid, gid, 1);
}

int vfs_set_acl(struct vfs_node *node, const struct acl_entry *acl) {
  if (!node || !acl)
    return -1;

  const struct cred *cred = get_current_cred();
  if (!cred)
    return -1;

  if (cred->euid != ROOT_UID && cred->euid != node->inode->uid) {
    if (!cred_has_cap(cred, CAP_FOWNER))
      return -1;
  }

  if (node->inode->acl_count >= ACL_MAX_ENTRIES)
    return -1;
  node->inode->acls[node->inode->acl_count++] = *acl;
  return 0;
}

int vfs_get_acl(struct vfs_node *node, struct acl_entry *out_acl,
                int max_entries) {
  if (!node || !out_acl)
    return -1;
  int count = node->inode->acl_count < max_entries ? node->inode->acl_count
                                                   : max_entries;
  for (int i = 0; i < count; i++)
    out_acl[i] = node->inode->acls[i];
  return count;
}

/* ── Extended attributes ──
 * Per-inode in-memory name/value list. Path-based; the syscall layer copies
 * names/values in and out, so these take kernel pointers. */

void vfs_free_xattrs(struct vfs_inode *inode) {
  if (!inode)
    return;
  struct vfs_xattr *x = inode->xattrs;
  while (x) {
    struct vfs_xattr *next = x->next;
    if (x->value)
      kfree(x->value);
    kfree(x);
    x = next;
  }
  inode->xattrs = 0;
}

static struct vfs_node *xattr_lookup(const char *path, int nofollow) {
  return nofollow ? vfs_find_node_no_follow(path) : vfs_find_node(path);
}

/* Modifying xattrs needs ownership (or CAP_FOWNER), matching chmod/utime. */
static int xattr_check_write(struct vfs_node *node) {
  const struct cred *cred = get_current_cred();
  if (!cred)
    return -EACCES;
  if (cred->euid != ROOT_UID && cred->euid != node->inode->uid &&
      !cred_has_cap(cred, CAP_FOWNER))
    return -EPERM;
  return 0;
}

isize vfs_setxattr(const char *path, const char *name, const void *value,
                   usize size, int flags, int nofollow) {
  if (!name)
    return -EFAULT;
  usize nlen = strlen(name);
  if (nlen == 0 || nlen > XATTR_NAME_MAX)
    return -ERANGE;
  if (size > XATTR_VALUE_MAX)
    return -E2BIG;

  struct vfs_node *node = xattr_lookup(path, nofollow);
  if (IS_ERR(node))
    return (isize)PTR_ERR(node);

  isize ret = xattr_check_write(node);
  if (ret != 0)
    goto out;

  /* Exclusive inode lock: the xattr list is shared mutable inode state; a
   * concurrent setxattr/removexattr on another CPU would corrupt the list and
   * a concurrent getxattr could walk a freed node (UAF). */
  vfs_inode_lock(node->inode);

  struct vfs_xattr **pp = &node->inode->xattrs;
  struct vfs_xattr *existing = 0;
  while (*pp) {
    if (strcmp((*pp)->name, name) == 0) {
      existing = *pp;
      break;
    }
    pp = &(*pp)->next;
  }
  if (existing && (flags & XATTR_CREATE)) {
    ret = -EEXIST;
    goto out_unlock;
  }
  if (!existing && (flags & XATTR_REPLACE)) {
    ret = -ENODATA;
    goto out_unlock;
  }

  void *newval = 0;
  if (size) {
    newval = kmalloc(size);
    if (!newval) {
      ret = -ENOMEM;
      goto out_unlock;
    }
    memcpy(newval, value, size);
  }

  if (existing) {
    if (existing->value)
      kfree(existing->value);
    existing->value = newval;
    existing->size = size;
  } else {
    struct vfs_xattr *x = kmalloc(sizeof(struct vfs_xattr));
    if (!x) {
      if (newval)
        kfree(newval);
      ret = -ENOMEM;
      goto out_unlock;
    }
    x->next = 0;
    memcpy(x->name, name, nlen);
    x->name[nlen] = '\0';
    x->value = newval;
    x->size = size;
    *pp = x; /* pp points at the tail's NULL link */
  }
  vfs_update_times(node->inode, VFS_CTIME);
  ret = 0;

out_unlock:
  vfs_inode_unlock(node->inode);
out:
  vfs_node_put(node);
  return ret;
}

isize vfs_getxattr(const char *path, const char *name, void *value,
                   usize size, int nofollow) {
  if (!name)
    return -EFAULT;
  struct vfs_node *node = xattr_lookup(path, nofollow);
  if (IS_ERR(node))
    return (isize)PTR_ERR(node);

  isize ret = -ENODATA;
  vfs_inode_lock_read(node->inode);
  for (struct vfs_xattr *x = node->inode->xattrs; x; x = x->next) {
    if (strcmp(x->name, name) != 0)
      continue;
    if (size == 0) {
      ret = (isize)x->size; /* size query */
    } else if (size < x->size) {
      ret = -ERANGE;
    } else {
      if (x->size)
        memcpy(value, x->value, x->size);
      ret = (isize)x->size;
    }
    break;
  }
  vfs_inode_unlock_read(node->inode);
  vfs_node_put(node);
  return ret;
}

isize vfs_listxattr(const char *path, char *list, usize size, int nofollow) {
  struct vfs_node *node = xattr_lookup(path, nofollow);
  if (IS_ERR(node))
    return (isize)PTR_ERR(node);

  vfs_inode_lock_read(node->inode);
  usize total = 0;
  for (struct vfs_xattr *x = node->inode->xattrs; x; x = x->next)
    total += strlen(x->name) + 1;

  isize ret;
  if (size == 0) {
    ret = (isize)total; /* size query */
  } else if (size < total) {
    ret = -ERANGE;
  } else {
    usize off = 0;
    for (struct vfs_xattr *x = node->inode->xattrs; x; x = x->next) {
      usize n = strlen(x->name) + 1;
      memcpy(list + off, x->name, n);
      off += n;
    }
    ret = (isize)total;
  }
  vfs_inode_unlock_read(node->inode);
  vfs_node_put(node);
  return ret;
}

isize vfs_removexattr(const char *path, const char *name, int nofollow) {
  if (!name)
    return -EFAULT;
  struct vfs_node *node = xattr_lookup(path, nofollow);
  if (IS_ERR(node))
    return (isize)PTR_ERR(node);

  isize ret = xattr_check_write(node);
  if (ret != 0)
    goto out;

  ret = -ENODATA;
  vfs_inode_lock(node->inode);
  struct vfs_xattr **pp = &node->inode->xattrs;
  while (*pp) {
    if (strcmp((*pp)->name, name) == 0) {
      struct vfs_xattr *dead = *pp;
      *pp = dead->next;
      if (dead->value)
        kfree(dead->value);
      kfree(dead);
      vfs_update_times(node->inode, VFS_CTIME);
      ret = 0;
      break;
    }
    pp = &(*pp)->next;
  }
  vfs_inode_unlock(node->inode);

out:
  vfs_node_put(node);
  return ret;
}

int vfs_fchown(int fd, u16 uid, u16 gid) {
  struct vfs_handle *handle = get_handle(fd);
  if (!handle || !handle->used)
    return -EBADF;
  if (handle->kind != VFS_HANDLE_NODE || !handle->node)
    return -EINVAL;

  const struct cred *cred = get_current_cred();
  if (!cred)
    return -EACCES;

  if (!cred_has_cap(cred, CAP_CHOWN))
    return -EPERM;

  if (uid != (u16)-1)
    handle->node->inode->uid = uid;
  if (gid != (u16)-1)
    handle->node->inode->gid = gid;
  handle->node->inode->ctime = vfs_get_unix_time();
  if (handle->node->inode->setattr_cb)
    return handle->node->inode->setattr_cb(handle->node);
  return 0;
}

int vfs_fstatfs(int fd, struct b1nix_statfs *st) {
  struct vfs_handle *handle = get_handle(fd);
  if (!handle || !handle->used)
    return -EBADF;
  if (handle->kind != VFS_HANDLE_NODE || !handle->node)
    return -EINVAL;

  return vfs_statfs_node(handle->node, st);
}

int vfs_syncfs(int fd) {
  struct vfs_handle *handle = get_handle(fd);
  if (!handle || !handle->used)
    return -EBADF;
  return vfs_sync();
}
