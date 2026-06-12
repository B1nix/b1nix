#include <b1nix/arch.h>
#include <b1nix/blk.h>
#include <b1nix/loop.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/ext2.h>
#include <b1nix/fat32.h>
#include <b1nix/filelock.h>
#include <b1nix/initramfs.h>
#include <b1nix/klog.h>
#include <b1nix/mm.h>
#include <b1nix/net.h>
#include <b1nix/lockdep.h>
#include <b1nix/page_cache.h>
#include <b1nix/panic.h>
#include <b1nix/rwlock.h>
#include <b1nix/rtc.h>
#include <b1nix/sched.h>
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
static int split_parent_path(const char *path, char *parent_path, char *name);
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
};
static struct vfs_mount_entry mounts[MAX_MOUNTS];

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
    for (int i = 0; i < MAX_MOUNTS; i++) {
      if (mounts[i].used && curr == mounts[i].root_node) {
        res = &mounts[i];
        goto out;
      }
    }
    curr = curr->parent;
  }
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (mounts[i].used && strcmp(mounts[i].target, "/") == 0) {
      res = &mounts[i];
      goto out;
    }
  }
out:
  vfs_tree_read_release(flags);
  __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
  return res;
}

void vfs_register_fs(struct vfs_fs *fs) {
  fs->next = filesystems;
  filesystems = fs;
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
 * burning ~50 KiB of static memory regardless of machine size. */
#define DCACHE_SIZE_MIN          64
#define DCACHE_SIZE_MAX          4096
#define DCACHE_POOL_MIN          256
#define DCACHE_POOL_MAX          8192

struct dcache_entry {
  struct vfs_node *parent;
  char name[64];
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

static void dcache_acquire(void) {
  while (__atomic_test_and_set(&dcache_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();
}

static void dcache_release(void) {
  __atomic_clear(&dcache_lock, __ATOMIC_RELEASE);
}

/* Slab-like pool for dcache entries to avoid fragmentation and kmalloc overhead
 * Pool + hash table are kzalloc'd at init, sized from total RAM. */
static struct dcache_entry *dcache_pool = 0;
static u32 g_dcache_pool_size = 0;
static struct dcache_entry *dcache_free_list = 0;

static void dcache_init_pool(void) {
  /* Scale to RAM: ~1 entry per 256 KiB usable RAM, clamped. */
  u64 ram_mb = pmm_total_usable_memory() / (1024ULL * 1024ULL);
  u32 pool = (u32)(ram_mb * 4);
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

/* Dentry-cache lookup/insert: a complete LRU path-resolution cache, not yet
 * wired into the lookup path. Kept (marked unused) for future use. */
__attribute__((unused)) static struct vfs_node *dcache_lookup(struct vfs_node *parent,
                                      const char *name) {
  dcache_acquire();
  u32 h = dcache_hash(parent, name);
  struct dcache_entry *e = dcache[h];
  while (e) {
    if (e->parent == parent && strcmp(e->name, name) == 0) {
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
      dcache_release();
      return res;
    }
    e = e->next;
  }
  dcache_release();
  return 0;
}

__attribute__((unused)) static void dcache_insert(struct vfs_node *parent, const char *name,
                          struct vfs_node *node) {
  dcache_acquire();
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
    dcache_release();
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
  dcache_release();
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
  dcache_acquire();
  u32 h = dcache_hash(parent, name);
  struct dcache_entry **prev_ptr = &dcache[h];
  struct dcache_entry *curr = *prev_ptr;
  while (curr) {
    if (curr->parent == parent && strcmp(curr->name, name) == 0) {
      dcache_unlink_locked(prev_ptr, curr);
      dcache_release();
      return;
    }
    prev_ptr = &curr->next;
    curr = *prev_ptr;
  }
  dcache_release();
}

/* Purge every dcache entry that references `node` either as parent or as the
 * cached child. Called before vfs_free_node so a recycled vfs_node address
 * cannot resurrect a stale lookup against the previous tenant. Without this,
 * dcache_lookup(reused_node, name) can return a dangling child pointer from
 * the previous owner's subtree and crash find_child's sibling walk. */
static void dcache_invalidate_node(struct vfs_node *node) {
  if (!node)
    return;
  dcache_acquire();
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
  dcache_release();
}

/* Icache sizing (B2 audit): same RAM-scaled treatment as dcache, smaller
 * per-entry footprint so we use a tighter ratio (1 entry per 512 KiB). */
#define ICACHE_SIZE_MIN          32
#define ICACHE_SIZE_MAX          2048
#define ICACHE_POOL_MIN          128
#define ICACHE_POOL_MAX          4096

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
  u32 pool = (u32)(ram_mb * 2);
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
  LOCKDEP_ACQUIRE_GLOBAL(LOCKDEP_LVL_INODE);
}

static void vfs_inode_unlock_read(struct vfs_inode *inode) {
  LOCKDEP_RELEASE_GLOBAL(LOCKDEP_LVL_INODE);
  if (__atomic_add_fetch(&inode->rw_lock, -1, __ATOMIC_RELEASE) == 0) {
    scheduler_wake_all((void *)&inode->rw_lock);
  }
}

static void vfs_inode_lock_write(struct vfs_inode *inode) {
  if (blk_cache_lock_is_held()) {
    console_write("[LOCK ORDER] vfs_inode_lock_write called by 0x");
    console_write_hex64((u64)(usize)__builtin_return_address(0));
    console_write(" while bcache lock held\n");
    panic("vfs: inode write-lock under block-cache lock");
  }
  while (1) {
    int val = 0;
    if (__atomic_compare_exchange_n(&inode->rw_lock, &val, -1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      break;
    }
    /* SMP-safe block — see vfs_inode_lock_read for the lost-wakeup rationale. */
    scheduler_wait_prepare((void *)&inode->rw_lock);
    val = 0;
    if (__atomic_compare_exchange_n(&inode->rw_lock, &val, -1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      scheduler_wait_cancel();
      break;
    }
    scheduler_wait_commit();
  }
  /* Sleeping lock — see read-lock variant for why this uses _GLOBAL. */
  LOCKDEP_ACQUIRE_GLOBAL(LOCKDEP_LVL_INODE);
}

static void vfs_inode_unlock_write(struct vfs_inode *inode) {
  LOCKDEP_RELEASE_GLOBAL(LOCKDEP_LVL_INODE);
  __atomic_store_n(&inode->rw_lock, 0, __ATOMIC_RELEASE);
  scheduler_wake_all((void *)&inode->rw_lock);
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
  u64 now = vfs_get_unix_time();
  if (mask & VFS_ATIME)
    inode->atime = now;
  if (mask & VFS_MTIME)
    inode->mtime = now;
  if (mask & VFS_CTIME)
    inode->ctime = now;
}

static usize node_count = 0;
static struct vfs_node *root_node = 0;
static char tty_line[TTY_INPUT_SIZE];
static usize tty_line_pos;
static usize tty_line_len;

static struct vfs_node *vfs_cross_root_mount(struct vfs_node *node) {
  if (!node || node != root_node)
    return node;

  struct vfs_node *mounted_root = 0;
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (mounts[i].used && mounts[i].root_node &&
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
#ifndef __aarch64__
extern char ps2_kbd_getc(void);
extern int ps2_kbd_has_data(void);
#endif
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
  if (cred->euid == ROOT_UID)
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

static struct vfs_inode *alloc_inode(void) {
  struct vfs_inode *inode = vfs_alloc_inode();
  if (inode) {
    inode->refcount = 0;
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
  if (__atomic_sub_fetch(&inode->refcount, 1, __ATOMIC_RELAXED) == 0 &&
      inode->nlink == 0) {
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

static struct vfs_node *alloc_node(void) {
  struct vfs_node *n = vfs_alloc_node();
  if (n) {
    n->refcount = 0;
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
  if (__atomic_sub_fetch(&node->refcount, 1, __ATOMIC_RELAXED) == 0 &&
      node->deleted) {
    if (node->inode && node->inode->release_cb) {
      node->inode->release_cb(node);
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

static void split_path(const char *path, char *first_part, usize first_size,
                        const char **rest) {
  if (!path || !first_part || !first_size) {
    if (first_part && first_size) first_part[0] = '\0';
    if (rest) *rest = 0;
    return;
  }
  while (*path == '/')
    path++;
  if (*path == '\0') {
    first_part[0] = '\0';
    *rest = 0;
    return;
  }
  usize i = 0;
  while (path[i] != '\0' && path[i] != '/' && i + 1 < first_size) {
    first_part[i] = path[i];
    i++;
  }
  first_part[i] = '\0';
  *rest = path + i;
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
   * and safe with IRQs disabled.
   *
   * Note: dcache is intentionally bypassed here because dcache_acquire yields
   * on contention, which reintroduces the same race window. dcache_insert
   * after-the-fact is the right place to repopulate when we add a similarly
   * safe lookup path. */
  struct vfs_node *result = 0;
  u64 flags;
  vfs_tree_read_acquire(&flags);
  struct vfs_node *child = parent->first_child;
  while (child) {
    if (!child->deleted && strcmp(child->name, name) == 0) {
      result = child;
      vfs_node_get(result); /* REFCOUNT RULE: caller owns the returned ref */
      break;
    }
    child = child->next_sibling;
  }
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
  if (path[0] == '/') {
    strncpy(combined, path, VFS_MAX_PATH);
  } else {
    const char *cwd = scheduler_get_cwd();
    strncpy(combined, cwd, VFS_MAX_PATH);
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
  strncpy(tmp, combined, VFS_MAX_PATH);
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

  vfs_node_get(root_node);
  struct vfs_node *current = root_node;
  current = vfs_cross_root_mount(current);
  vfs_inode_lock_read(current->inode);

  char part[64];
  const char *rest = curr_path;

restart_traversal:
  while (1) {
    while (*rest == '/')
      rest++;

    split_path(rest, part, sizeof(part), &rest);

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
      for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].used && current == mounts[i].root_node) {
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

      struct vfs_node *parent = current->parent;
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

    struct vfs_node *child = find_child(current, part);
    if (!child) {
      vfs_inode_unlock_read(current->inode);
      vfs_node_put(current);
      kfree(curr_path);
      kfree(parent_path);
      return ERR_PTR(-ENOENT);
    }

    /* find_child() already returns with refcount incremented */
    /* DOWNWARD MOUNT CROSSING */
    for (int i = 0; i < MAX_MOUNTS; i++) {
      if (mounts[i].used && child == mounts[i].mount_point) {
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
    root_node->inode->atime = root_node->inode->mtime =
        root_node->inode->ctime = vfs_get_unix_time();
  }

  char part[64];
  const char *rest = path;
  struct vfs_node *current = root_node;
  vfs_node_get(current);
  current = vfs_cross_root_mount(current);
  /* Note: vfs_cross_root_mount already manages refcount correctly.
   * If it redirected, it got the new root (+1) and put the old one (-1).
   * current now holds one reference valid throughout the loop below. */

  while (1) {
    split_path(rest, part, sizeof(part), &rest);
    if (part[0] == '\0') {
      struct vfs_node *ret = current;
      vfs_node_put(current);
      return ret;
    }

    struct vfs_node *child = find_child(current, part);
    int child_was_found = (child != NULL);
    if (child) {
      for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].used && child == mounts[i].mount_point) {
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

        child->inode->atime = child->inode->mtime = child->inode->ctime =
            vfs_get_unix_time();
      } else if (data != 0 || size != 0 || flags != 0 ||
                 type == VFS_DIRECTORY) {
        child->inode->type = type;
        child->inode->data = data;
        child->inode->size = size;
        child->inode->flags = flags;
        child->inode->mtime = child->inode->ctime = vfs_get_unix_time();
      } else {
        if (child_was_found) {
          vfs_node_put(child);
        }
        struct vfs_node *ret = child;
        vfs_node_put(current);
        return ret;
      }
      if (child_was_found) {
        vfs_node_put(child);
      }
      struct vfs_node *ret = child;
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
        child->inode->atime = child->inode->mtime = child->inode->ctime =
            vfs_get_unix_time();

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

struct vfs_node *vfs_create_node(enum vfs_node_type type) {
  struct vfs_node *n = alloc_node();
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

void vfs_attach_child(struct vfs_node *parent, struct vfs_node *child) {
  if (!parent || !child)
    return;
  u64 flags;
  vfs_tree_write_acquire(&flags);
  child->next_sibling = parent->first_child;
  parent->first_child = child;
  vfs_tree_write_release(flags);
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

  u64 flags;
  vfs_tree_read_acquire(&flags);
  for (struct vfs_node *child = dir->first_child;
       child && count < max_entries; child = child->next_sibling) {
    if (child->deleted)
      continue;
    if (idx >= start) {
      copy_path(buf[count].name, 64, child->name);
      buf[count].type = (u32)child->inode->type;
      buf[count].is_dir = (child->inode->type == VFS_DIRECTORY);
      buf[count].is_exec = 0;
      buf[count].size = child->inode->size;
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
  if (!h || h->used != 1 || h->refcount <= 0)
    return;
  /* SMP-safe dec-and-test: a fork'd parent and child closing a shared socket
   * fd on different CPUs both release the same handle. A non-atomic
   * "if (refcount > 1) refcount--" lost the update under that race, so both
   * fell through to ->release → kfree(socket_state) twice → kheap double-free
   * ("bucket_unlink ... magic 0x...dead110c"). The atomic sub-and-test makes
   * exactly one releaser observe 0 and run teardown. */
  if (__atomic_sub_fetch(&h->refcount, 1, __ATOMIC_ACQ_REL) > 0)
    return;

  h->used = 0;
  if (h->ops && h->ops->release) {
    h->ops->release(h);
  } else if (h->kind == VFS_HANDLE_NODE && h->node) {
    vfs_node_put(h->node);
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
    for (usize i = 0; i < size; i++)
      buffer[i] = tty_getc_blocking();
    return (isize)size;
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

  for (usize i = 0; i < size; i++) {
    if ((console.termios.c_oflag & B1NIX_OPOST) && buffer[i] == '\n')
      console_putc('\r');
    console_putc(buffer[i]);
  }
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

static void null_init_node(void) {
  struct vfs_node *n = add_node("/dev/null", VFS_DEVICE, 0, 0, 0);
  if (n) {
    n->inode->read_cb = null_read;
    n->inode->write_cb = null_write;
    n->inode->poll_cb = null_poll;
    n->inode->mode =
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
  memset(mounts, 0, sizeof(mounts));

  root_node = alloc_node();
  root_node->inode = alloc_inode();
  strcpy(root_node->name, "/");
  root_node->inode->type = VFS_DIRECTORY;
  root_node->inode->mode = 0755;
  root_node->inode->atime = root_node->inode->mtime = root_node->inode->ctime =
      vfs_get_unix_time();
  root_node->inode->fs_id = 1;
  next_fs_id = 2;

  add_node("/dev", VFS_DIRECTORY, 0, 0, 0);
  add_node("/home", VFS_DIRECTORY, 0, 0, 0);
  add_node("/tmp", VFS_DIRECTORY, 0, 0, 0);
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

  add_node("/dev/console", VFS_DEVICE, 0, 0, 0);
  add_node("/dev/virtio-blk0", VFS_DEVICE, 0, 0, 0);
  /* M32b pseudo-terminals: /dev/ptmx + the /dev/pts mountpoint directory. Both
   * opens are intercepted in vfs_open_flags; the nodes exist so stat()/ls and
   * ptsname() paths resolve. */
  pty_init();
  add_node("/dev/ptmx", VFS_DEVICE, 0, 0, 0);
  add_node("/dev/pts", VFS_DIRECTORY, 0, 0, 0);
  serial_tty_register_nodes();
  vfs_create("/tmp/hello", 0644);
  vfs_mount("initramfs", "/", "initramfs", 0);
  tty_init_node();
  null_init_node();
  vfs_init_stdio();

#ifndef __aarch64__
  virtio_blk_init();
#endif

  for (usize i = 0; i < blk_count(); i++) {
    struct block_device *dev = blk_at(i);
    char dev_path[64];
    strcpy(dev_path, "/dev/");
    strcat(dev_path, dev->name);
    add_node(dev_path, VFS_DEVICE, 0, 0, 0);
  }

#ifndef __aarch64__
  struct block_device *blk = blk_get("virtio-blk0");
  if (blk)
    fat32_mount(blk, "/mnt");
#endif

  console_write(
      "vfs: full featured initialized (POSIX+, Refcounting, Mount Crossing)\n");
}

void vfs_repopulate_after_root_mount(void) {
  struct vfs_node *node;

  node = add_node("/dev", VFS_DIRECTORY, 0, 0, 0);
  if (node && !IS_ERR(node)) vfs_node_put(node);

  node = add_node("/dev/console", VFS_DEVICE, 0, 0, 0);
  if (node && !IS_ERR(node)) {
    node->inode->mode = 0620;
    node->inode->uid = 0;
    node->inode->gid = 5; // group tty
    vfs_node_put(node);
  }

  node = add_node("/dev/ptmx", VFS_DEVICE, 0, 0, 0);
  if (node && !IS_ERR(node)) {
    node->inode->mode = 0666;
    node->inode->uid = 0;
    node->inode->gid = 5; // group tty
    vfs_node_put(node);
  }

  node = add_node("/dev/pts", VFS_DIRECTORY, 0, 0, 0);
  if (node && !IS_ERR(node)) {
    node->inode->mode = 0755;
    node->inode->uid = 0;
    node->inode->gid = 0;
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
    vfs_node_put(node);
  }

  tty_init_node();
  serial_tty_register_nodes();

  node = add_node("/home", VFS_DIRECTORY, 0, 0, 0);
  if (node && !IS_ERR(node)) vfs_node_put(node);

  node = add_node("/tmp", VFS_DIRECTORY, 0, 0, 0);
  if (node && !IS_ERR(node)) {
    node->inode->mode = 01777; // Sticky bit + rwxrwxrwx
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

  for (usize i = 0; i < blk_count(); i++) {
    struct block_device *dev = blk_at(i);
    if (!dev || !dev->name)
      continue;
    char dev_path[64];
    strcpy(dev_path, "/dev/");
    strcat(dev_path, dev->name);
    node = add_node(dev_path, VFS_DEVICE, 0, 0, 0);
    if (node && !IS_ERR(node)) vfs_node_put(node);
  }

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
  int res = 0;
  if (!path)
    return -EINVAL;
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

  struct vfs_node *node = vfs_find_node_internal(resolved, 1, 0);
  if (IS_ERR(node)) {
    if (PTR_ERR(node) == -ENOENT && (flags & B1NIX_O_CREAT)) {
      /* Use internal version to avoid redundant resolution/logging */
      int err = vfs_create_at_internal(resolved, 0666);
      if (err != 0) {
        res = err;
        goto out;
      }
      node = vfs_find_node_internal(resolved, 1, 0);
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
      vfs_node_put(node);
      goto out;
    }
  }

  if (node->inode && node->inode->ino && node->inode->fs_id) {
    struct vfs_inode *cached = icache_get(node->inode->fs_id, node->inode->ino);
    if (!cached) {
      icache_insert(node->inode->fs_id, node->inode->ino, node->inode);
    }
  }

  if ((flags & B1NIX_O_DIRECTORY) && node->inode->type != VFS_DIRECTORY) {
    res = -ENOTDIR;
    vfs_node_put(node);
    goto out;
  }
  /* POSIX: writing to a directory descriptor is not permitted */
  if (node->inode->type == VFS_DIRECTORY &&
      (flags & (B1NIX_O_WRONLY | B1NIX_O_RDWR))) {
    res = -EISDIR;
    vfs_node_put(node);
    goto out;
  }

  int access_mask = 0;
  if (flags & (B1NIX_O_WRONLY | B1NIX_O_RDWR))
    access_mask |= W_OK;
  if ((flags & 3) == B1NIX_O_RDONLY || (flags & B1NIX_O_RDWR))
    access_mask |= R_OK;

  res = vfs_check_access(node, access_mask);
  if (res != 0) {
    vfs_node_put(node);
    goto out;
  }

  if ((flags & B1NIX_O_TRUNC) && node->inode->type == VFS_FILE) {
    /* O_TRUNC requires write permission regardless of open mode */
    res = vfs_check_access(node, W_OK);
    if (res != 0) {
      vfs_node_put(node);
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

  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_NODE);
  if (!h) {
    res = -ENFILE;
    goto out;
  }
  h->node = node; /* Already has ref from find_node */
  extern const struct vfs_file_ops node_file_ops;
  h->ops = &node_file_ops;
  h->flags = flags;
  h->offset = (flags & B1NIX_O_APPEND) ? node->inode->size : 0;

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

static isize node_read(struct vfs_handle *h, char *buf, usize size) {
  if (!h->node)
    return -EBADF;
  struct vfs_node *node = vfs_node_get(h->node);
  u64 offset = h->offset;
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
          page = page_cache_get_page(node->inode, page_aligned);
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
      h->offset += total_read;
    }
  } else if (node->inode->read_cb) {
    res = node->inode->read_cb(node, offset, buf, size, h->flags);
    if (res > 0)
      h->offset += (usize)res;
  } else if (node->inode->type == VFS_FILE) {
    usize rem = node->inode->size > offset ? node->inode->size - offset : 0;
    usize to_r = size < rem ? size : rem;
    if (to_r > 0) {
      memcpy(buf, (const char *)node->inode->data + offset, to_r);
      h->offset += to_r;
    }
    res = (isize)to_r;
  }
  vfs_inode_unlock(node->inode);
  vfs_node_put(node);
  return res;
}

static isize node_write(struct vfs_handle *h, const char *buf, usize size) {
  if (!h->node)
    return -EBADF;
  struct vfs_node *node = vfs_node_get(h->node);
  vfs_inode_lock(node->inode);
  /* O_APPEND: sample the size under the exclusive inode lock. Reading it
   * before the lock loses concurrent appends — a writer blocked on the lock
   * would rewind to a stale EOF and overwrite what the lock holder appended. */
  if (h->flags & B1NIX_O_APPEND)
    h->offset = node->inode->size;
  u64 offset = h->offset;
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
          page = page_cache_get_page(node->inode, page_aligned);
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
      h->offset += total_written;
      if (h->offset > node->inode->size) {
        node->inode->size = h->offset;
      }
      vfs_update_times(node->inode, VFS_MTIME | VFS_CTIME);
    }
  } else if (node->inode->write_cb) {
    res = node->inode->write_cb(node, offset, buf, size, h->flags);
    if (res > 0) {
      vfs_update_times(node->inode, VFS_MTIME | VFS_CTIME);
      h->offset += (usize)res;
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
        memcpy(new_data, node->inode->data, node->inode->size);
        if (node->inode->flags & VFS_NODE_OWNS_DATA)
          kfree(node->inode->data);
      }
      node->inode->data = new_data;
      node->inode->capacity = new_cap;
      node->inode->flags |= VFS_NODE_OWNS_DATA;
    }
    memcpy((char *)node->inode->data + offset, buf, size);
    if (offset + size > node->inode->size)
      node->inode->size = (usize)(offset + size);
    vfs_update_times(node->inode, VFS_MTIME | VFS_CTIME);
    if (node->inode->setattr_cb)
      node->inode->setattr_cb(node);
    h->offset += size;
    res = (isize)size;
  }
  vfs_inode_unlock(node->inode);
  vfs_node_put(node);
  return res;
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
  char name[64];
  if (!p_path)
    return -ENOMEM;
  if (split_parent_path(resolved_path, p_path, name) < 0) {
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
  copy_path(node->name, 64, name);
  node->inode->type = VFS_FILE;
  node->parent = parent;

  u16 umask = scheduler_get_current_umask();
  node->inode->mode = mode & ~umask;
  node->inode->uid = cred ? cred->euid : ROOT_UID;
  node->inode->gid = cred ? cred->egid : ROOT_GID;
  node->inode->atime = node->inode->mtime = node->inode->ctime =
      vfs_get_unix_time();

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

struct vfs_node *vfs_find_node_by_fd(int fd) {
  struct vfs_handle *h = get_handle(fd);
  if (!h || h->kind != VFS_HANDLE_NODE)
    return ERR_PTR(-EBADF);
  return h->node;
}

static int vfs_mkdir_at_internal(const char *resolved_path, u32 mode) {
  if (strcmp(resolved_path, "/") == 0) {
    return -EEXIST;
  }
  int res = 0;
  char *p_path = kmalloc(VFS_MAX_PATH);
  char name[64];
  if (!p_path)
    return -ENOMEM;
  if (split_parent_path(resolved_path, p_path, name) < 0) {
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
  copy_path(node->name, 64, name);
  node->inode->type = VFS_DIRECTORY;
  node->parent = parent;

  u16 umask = scheduler_get_current_umask();
  node->inode->mode = mode & ~umask;
  node->inode->uid = cred ? cred->euid : ROOT_UID;
  node->inode->gid = cred ? cred->egid : ROOT_GID;
  node->inode->atime = node->inode->mtime = node->inode->ctime =
      vfs_get_unix_time();

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
  if (node->inode->type == VFS_DEVICE)
    return B1NIX_S_IFCHR;
  if (node->inode->type == VFS_SYMLINK)
    return B1NIX_S_IFLNK;
  return B1NIX_S_IFREG;
}
static int vfs_stat_node(struct vfs_node *node, struct b1nix_stat *st) {
  if (!node || !node->inode)
    return -ENOENT;
  if (!st)
    return -EINVAL;

  struct vfs_inode *inode = node->inode;
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
  st->st_mtim.tv_sec = inode->mtime;
  st->st_ctim.tv_sec = inode->ctime;

  st->st_dev = inode->fs_id;
  if (inode->type == VFS_DEVICE) {
    st->st_rdev = (u64)inode->data;
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

int vfs_statfs(const char *path, struct b1nix_statfs *st) {
  struct vfs_node *node = vfs_find_node(path);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);

  int res = 0;
  if (node->inode->statfs_cb) {
    res = node->inode->statfs_cb(node, st);
  } else {
    res = -ENOSYS;
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
  if (!h || h->kind != VFS_HANDLE_NODE)
    return -EBADF;
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

static int split_parent_path(const char *path, char *parent_path, char *name) {
  if (!path || path[0] == '\0')
    return -1;

  char local_path[VFS_MAX_PATH];
  strncpy(local_path, path, VFS_MAX_PATH);
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
    parent_path[0] = '/';
    parent_path[1] = '\0';
    memcpy(name, local_path, len + 1);
    return 0;
  }

  if ((usize)last_slash == len - 1)
    return -1;
  if (last_slash == 0) {
    parent_path[0] = '/';
    parent_path[1] = '\0';
  } else {
    memcpy(parent_path, local_path, (usize)last_slash);
    parent_path[last_slash] = '\0';
  }
  memcpy(name, local_path + last_slash + 1, len - (usize)last_slash);
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
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (mounts[i].used && strcmp(mounts[i].target, r_path) == 0)
      return -EBUSY;
  }

  struct vfs_node *prev = 0, *child = parent->first_child;
  while (child) {
    if (!child->deleted && strcmp(child->name, name) == 0) {
      if (cred && (parent->inode->mode & B1NIX_S_ISVTX)) {
        if (cred->euid != ROOT_UID && cred->euid != child->inode->uid && cred->euid != parent->inode->uid)
          return -EACCES;
      }
      if (is_rmdir) {
        if (child->inode->type != VFS_DIRECTORY)
          return -ENOTDIR;
        if (child->first_child)
          return -ENOTEMPTY;
      } else {
        if (child->inode->type == VFS_DIRECTORY)
          return -EISDIR;
      }
      if (parent->inode->unlink_cb && !is_rmdir) {
        int err = parent->inode->unlink_cb(parent, name);
        if (err < 0)
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
  char p_path[VFS_MAX_PATH], name[64];
  split_parent_path(r_path, p_path, name);
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    return -EINVAL;

  struct vfs_node *parent = vfs_find_node(p_path);
  if (IS_ERR(parent))
    return (int)PTR_ERR(parent);

  vfs_inode_lock(parent->inode);
  int res = vfs_remove_child_locked(parent, r_path, name, is_rmdir);
  vfs_inode_unlock(parent->inode);
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

  char parent_path[VFS_MAX_PATH], name[64];
  if (split_parent_path(link_path, parent_path, name) < 0) {
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

  copy_path(new_node->name, 64, name);
  new_node->inode = target_node->inode;
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
      new_node->inode = 0;
      /* The fresh node was allocated with refcount 0; give it the reference
       * we are about to drop and mark it deleted so vfs_node_put's 0+deleted
       * check actually frees it (a bare put underflowed to -1 and leaked). */
      new_node->deleted = 1;
      __atomic_store_n(&new_node->refcount, 1, __ATOMIC_RELAXED);
      vfs_node_put(new_node);
    }
  }
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

  char parent_path[VFS_MAX_PATH], name[64];
  if (split_parent_path(link_path, parent_path, name) < 0)
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

  copy_path(node->name, 64, name);
  node->inode->type = VFS_SYMLINK;
  node->inode->data = target_copy;
  node->inode->size = len;
  node->inode->flags = VFS_NODE_OWNS_DATA;
  node->inode->mode = 0777;
  node->inode->uid = cred ? cred->euid : ROOT_UID;
  node->inode->gid = cred ? cred->egid : ROOT_GID;
  node->inode->atime = node->inode->mtime = node->inode->ctime =
      vfs_get_unix_time();
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
  vfs_inode_unlock(parent->inode);
  vfs_node_put(parent);
  return res;
}

isize vfs_readlink(const char *path, char *buffer, usize size) {
  isize res = 0;
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
  if (node->inode->type != VFS_SYMLINK || !node->inode->data) {
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

  char old_p[VFS_MAX_PATH], old_n[64], new_p[VFS_MAX_PATH], new_n[64];
  split_parent_path(old_res, old_p, old_n);
  split_parent_path(new_res, new_p, new_n);

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

  copy_path(node->name, 64, new_n);
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
  return vfs_rename_internal(old_path, new_path);
}

int vfs_rmdir(const char *path) {
  int res = vfs_remove_node(path, 1);
  return res;
}

int vfs_fstat(int fd, struct b1nix_stat *st) {
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

int vfs_fsync(int fd) {
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h || h->kind != VFS_HANDLE_NODE)
    return -EBADF;
  struct vfs_node *node = h->node;

  /* Inode lock across the flush — see vfs_close_handle (flush vs a concurrent
   * truncate's in-place page zeroing). */
  vfs_inode_lock(node->inode);
  page_cache_flush_inode(node->inode);
  vfs_inode_unlock(node->inode);

  if (node->inode->fsync_cb) {
    int err = node->inode->fsync_cb(node);
    if (err < 0)
      return err;
  }

  if (node->inode->blk_dev)
    blk_cache_flush(node->inode->blk_dev);
  return 0;
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

  /* Claim and initialize the slot under vfs_mount_lock — the unlocked scan
   * let two concurrent mounts pick the same index, and lookups walking
   * mounts[] could observe a half-written entry. The fs->mount() callback
   * itself runs outside the lock (it sleeps on block I/O). */
  while (__atomic_test_and_set(&vfs_mount_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();

  int midx = -1;
  for (usize i = 0; i < MAX_MOUNTS; i++) {
    if (!mounts[i].used) {
      midx = (int)i;
      break;
    }
  }

  if (midx == -1) {
    __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
    vfs_node_put(target_node);
    return -ENOMEM;
  }

  // Pre-register slot so mount crossing works during populate_vfs
  mounts[midx].used = 1;
  mounts[midx].mount_point = target_node;
  mounts[midx].root_node = NULL;
  copy_path(mounts[midx].source, sizeof(mounts[midx].source), source ? source : "");
  copy_path(mounts[midx].target, sizeof(mounts[midx].target), target);
  copy_path(mounts[midx].fstype, sizeof(mounts[midx].fstype), fstype);
  mounts[midx].flags = flags;
  __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);

  currently_mounting = &mounts[midx];
  struct vfs_node *root_node = fs->mount(source, flags, (void *)target);
  currently_mounting = NULL;

  if (IS_ERR(root_node)) {
    mounts[midx].used = 0;
    vfs_node_put(target_node);
    return (int)PTR_ERR(root_node);
  }

  while (__atomic_test_and_set(&vfs_mount_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();
  mounts[midx].root_node = root_node;
  root_node->inode->fs_id = next_fs_id++;
  __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);

  return 0;
}

int vfs_umount(const char *target) {
  if (!target)
    return -EINVAL;

  while (__atomic_test_and_set(&vfs_mount_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();

  for (usize i = 0; i < MAX_MOUNTS; i++) {
    if (mounts[i].used && strcmp(mounts[i].target, target) == 0) {
      if (strcmp(target, "/") == 0) {
        __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
        return -EBUSY;
      }

      /* Basic busy check: if root_node has other refs than our mount entry.
       * Acquire-load: pairs with the atomic refcount updates so a ref taken
       * on another CPU just before umount is observed. */
      if (__atomic_load_n(&mounts[i].root_node->refcount, __ATOMIC_ACQUIRE) > 1) {
        __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
        return -EBUSY;
      }

      struct vfs_node *root = mounts[i].root_node;
      struct vfs_node *mp = mounts[i].mount_point;
      u32 fs_id = (root && root->inode) ? root->inode->fs_id : 0;

      /* Call filesystem umount callback if available (e.g. JBD RECOVER flag) */
      if (mounts[i].fstype[0]) {
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

      mounts[i].used = 0;
      __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);

      if (root && root->inode && root->inode->blk_dev) {
        blk_cache_flush(root->inode->blk_dev);
        blk_cache_invalidate(root->inode->blk_dev);
      }

      vfs_node_put(root);
      vfs_node_put(mp);
      icache_invalidate_fs(fs_id);
      return 0;
    }
  }
  __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
  return -EINVAL;
}

isize vfs_mounts(struct b1nix_mount_entry *out, usize max_entries) {
  if (!out && max_entries > 0)
    return -EFAULT;

  usize count = 0;
  for (usize i = 0; i < MAX_MOUNTS; i++) {
    if (!mounts[i].used)
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

  if (dir->inode->readdir_cb) {
    res = dir->inode->readdir_cb(dir, offset, buf, max_entries);
    if (res > 0) {
      h->offset += (usize)res;
    }
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
      buf[count].type = (u32)child->inode->type;
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
  if (minfd < 0 || (usize)minfd >= SCHED_MAX_FD_LIMIT)
    return -EINVAL;

  usize limit = SCHED_MAX_FD_LIMIT;
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
  if (newfd < 0 || (usize)newfd >= SCHED_MAX_FD_LIMIT)
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
  while (curr && count < 128) {
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

int vfs_ftruncate(int fd, u64 length) {
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

  vfs_inode_lock(inode);

  if (inode->type != VFS_FILE) {
    vfs_inode_unlock(inode);
    return -EINVAL;
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
    if (length > inode->size && inode->write_cb) {
      char *zeroes = kzalloc(4096);
      if (!zeroes) {
        vfs_inode_unlock(inode);
        return -ENOMEM;
      }
      u64 off = inode->size;
      while (off < length) {
        usize chunk = (usize)(length - off);
        if (chunk > 4096)
          chunk = 4096;
        isize written = inode->write_cb(node, off, zeroes, chunk, h->flags);
        if (written < 0) {
          kfree(zeroes);
          vfs_inode_unlock(inode);
          return (int)written;
        }
        if (written == 0) {
          kfree(zeroes);
          vfs_inode_unlock(inode);
          return -EIO;
        }
        off += (u64)written;
      }
      kfree(zeroes);
    }

    if (inode->truncate_cb) {
      int res = inode->truncate_cb(node, length);
      vfs_inode_unlock(inode);
      return res;
    }

    inode->size = (usize)length;
    vfs_update_times(inode, VFS_MTIME | VFS_CTIME);
    int res = inode->setattr_cb(node);
    vfs_inode_unlock(inode);
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
      memcpy(new_data, inode->data, inode->size);
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

int vfs_fcntl(int fd, int cmd, u64 arg) {
  struct vfs_handle *h = get_handle(fd);
  if (!h)
    return -1;
  switch (cmd) {
  case B1NIX_F_DUPFD:
    return vfs_dup_min(fd, (int)arg);
  case B1NIX_F_GETFD:
    return scheduler_fd_flags_get(fd);
  case B1NIX_F_SETFD:
    return scheduler_fd_flags_set(fd, (int)arg);
  case B1NIX_F_GETFL:
    return h->flags;
  case B1NIX_F_SETFL:
    h->flags = (int)arg;
    return 0;
  case B1NIX_F_GETLK:
  case B1NIX_F_SETLK:
  case B1NIX_F_SETLKW:
    if (h->kind != VFS_HANDLE_NODE)
      return -EBADF;
    return filelock_set_lock(fd, cmd, (struct flock *)(usize)arg);
  default:
    return -1;
  }
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
  if (node->inode->type != VFS_DEVICE)
    return -EINVAL;

  /* Loop-device control ioctls (BusyBox losetup): the LOOP_* family is type
   * 0x4C, plus the /dev/loop-control node. Handled before the `arg` check
   * because LOOP_CTL_GET_FREE / LOOP_CLR_FD carry no argument. */
  if (((request >> 8) & 0xFF) == 0x4C ||
      strcmp(node->name, "loop-control") == 0)
    return loop_ioctl(node, request, arg);

  if (!arg)
    return -EINVAL;

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
    case 0x5F: /* BLKRRPART: reread partition table — accept */
    case 0x61: /* BLKFLSBUF: flush buffers — accept */
      return 0;
    default:
      return -ENOTTY;
    }
  }

  if (strcmp(node->name, "tty") != 0 && strcmp(node->name, "console") != 0)
    return -ENOTTY;

  if (request == B1NIX_TCGETS) {
    if (!arg || syscall_copyout(arg, &console.termios, sizeof(struct b1nix_termios)) < 0)
      return -EFAULT;
    return 0;
  }
  if (request == B1NIX_TCSETS) {
    if (!arg || syscall_copyin(&console.termios, arg, sizeof(struct b1nix_termios)) < 0)
      return -EFAULT;
    return 0;
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
  return -1;
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

  node->inode->atime = atime;
  node->inode->mtime = mtime;
  node->inode->ctime = vfs_get_unix_time();
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

int vfs_chown(const char *path, u16 uid, u16 gid) {
  struct vfs_node *node = vfs_find_node(path);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);

  int res = 0;
  const struct cred *cred = get_current_cred();
  if (!cred) {
    res = -EACCES;
    goto out;
  }

  /* Only root can change owner */
  if (cred->euid != ROOT_UID && !cred_has_cap(cred, CAP_CHOWN)) {
    res = -EPERM;
    goto out;
  }

  if (uid != (u16)-1)
    node->inode->uid = uid;
  if (gid != (u16)-1)
    node->inode->gid = gid;
  vfs_update_times(node->inode, VFS_CTIME);
  if (node->inode->setattr_cb) {
    res = node->inode->setattr_cb(node);
    goto out;
  }

out:
  vfs_node_put(node);
  return res;
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

  if (cred->euid != ROOT_UID && !cred_has_cap(cred, CAP_CHOWN))
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

  if (handle->node->inode->statfs_cb)
    return handle->node->inode->statfs_cb(handle->node, st);

  return -ENOSYS;
}

int vfs_syncfs(int fd) {
  struct vfs_handle *handle = get_handle(fd);
  if (!handle || !handle->used)
    return -EBADF;
  return vfs_sync();
}
