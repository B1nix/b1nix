/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * memfd_secret(2): memory only the processes that map it can read.
 *
 * A secret file's pages are taken out of every kernel mapping of physical
 * memory (paging_hide_frames_2m), so no kernel code path -- a stray pointer, a
 * speculative gadget, a reader of another process's memory -- can reach them.
 * The mapping processes reach them through their own page tables and nothing
 * else.
 *
 * Frames come from 2 MiB chunks the pool takes whole from the allocator and
 * hides at once: hiding a whole block clears one huge entry per alias instead
 * of splitting it, and a block nobody else uses can be invalidated before
 * anything replaces it. A frame that goes back to the pool is scrubbed through
 * a private one-page window before it is handed out again; a chunk whose
 * frames are all back is made visible, zeroed and returned to the allocator.
 *
 * The file follows Linux's secretmem: shared mappings only, its size can be
 * set once, read(2)/write(2) are refused, and the pages count against
 * RLIMIT_MEMLOCK and are never swapped or dumped.
 */
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/secretmem.h>
#include <b1nix/spinlock.h>
#include <b1nix/uidgid.h>
#include <b1nix/vfs.h>
#include <string.h>

#define SM_CHUNK_PAGES 512ULL
#define SM_CHUNK_SIZE (SM_CHUNK_PAGES * PAGE_SIZE)
#define SM_WORDS 8 /* 512 bits */

extern int paging_hide_frames_2m(u64 base);
extern void paging_unhide_frames_2m(u64 base);
extern u64 paging_reserve_kernel_va(usize size);

struct sm_chunk {
  u64 base;
  u64 used[SM_WORDS];
  /* Handed out before and not scrubbed since. */
  u64 dirty[SM_WORDS];
  u32 nused;
  struct sm_chunk *next;
};

static spinlock_t g_sm_lock = SPINLOCK_INIT;
static struct sm_chunk *g_sm_chunks;
static volatile u32 g_sm_nchunks;

static spinlock_t g_sm_scrub_lock = SPINLOCK_INIT;
static u64 g_sm_scrub_va;

void secretmem_init(void) {
  g_sm_scrub_va = paging_reserve_kernel_va(PAGE_SIZE);
  if (!g_sm_scrub_va)
    return;
  /* Build the tables under the window now, so a scrub never allocates one
   * with a spinlock held. The zero page is only there to be unmapped. */
  vmm_map_page(g_sm_scrub_va, pmm_zero_page(), VMM_NO_EXECUTE);
  vmm_unmap_page(g_sm_scrub_va);
}

int secretmem_frame_is_hidden(u64 frame) {
  if (!__atomic_load_n(&g_sm_nchunks, __ATOMIC_RELAXED))
    return 0;
  u64 base = frame & ~(SM_CHUNK_SIZE - 1);
  u64 flags;
  int hit = 0;
  spin_lock_irqsave(&g_sm_lock, &flags);
  for (struct sm_chunk *c = g_sm_chunks; c; c = c->next) {
    if (c->base == base) {
      hit = 1;
      break;
    }
  }
  spin_unlock_irqrestore(&g_sm_lock, flags);
  return hit;
}

/* Zero a hidden frame through the private window. */
static void sm_scrub(u64 frame) {
  u64 flags;
  spin_lock_irqsave(&g_sm_scrub_lock, &flags);
  vmm_map_page(g_sm_scrub_va, frame, VMM_WRITABLE | VMM_NO_EXECUTE);
  memset((void *)(usize)g_sm_scrub_va, 0, PAGE_SIZE);
  vmm_unmap_page(g_sm_scrub_va);
  spin_unlock_irqrestore(&g_sm_scrub_lock, flags);
}

static int sm_take_bit(struct sm_chunk *c, int *dirty) {
  for (unsigned w = 0; w < SM_WORDS; w++) {
    if (c->used[w] == ~0ULL)
      continue;
    unsigned b = (unsigned)__builtin_ctzll(~c->used[w]);
    c->used[w] |= 1ULL << b;
    *dirty = (c->dirty[w] >> b) & 1;
    c->dirty[w] |= 1ULL << b;
    c->nused++;
    return (int)(w * 64 + b);
  }
  return -1;
}

/* A free 2 MiB block that starts on a 2 MiB boundary: only such a block can be
 * hidden whole. The allocator does not promise alignment for a run, so ask for
 * twice the size and give back what lies outside the aligned block. */
static u64 sm_alloc_block(void) {
  u64 raw = pmm_alloc_frames(2 * SM_CHUNK_PAGES);
  if (!raw)
    return 0;
  u64 base = (raw + SM_CHUNK_SIZE - 1) & ~(SM_CHUNK_SIZE - 1);
  u64 end = raw + 2 * SM_CHUNK_SIZE;
  for (u64 f = raw; f < end; f += PAGE_SIZE)
    if (f < base || f >= base + SM_CHUNK_SIZE)
      pmm_free_frame(f);
  return base;
}

static u64 sm_frame_alloc(void) {
  if (!g_sm_scrub_va)
    return 0;
  u64 flags;
  int dirty = 0;
  u64 frame = 0;

  spin_lock_irqsave(&g_sm_lock, &flags);
  for (struct sm_chunk *c = g_sm_chunks; c && !frame; c = c->next) {
    if (c->nused >= SM_CHUNK_PAGES)
      continue;
    int bit = sm_take_bit(c, &dirty);
    if (bit >= 0)
      frame = c->base + (u64)bit * PAGE_SIZE;
  }
  spin_unlock_irqrestore(&g_sm_lock, flags);
  if (frame) {
    if (dirty)
      sm_scrub(frame);
    return frame;
  }

  /* A new chunk: a whole 2 MiB block, already zeroed by the allocator. */
  struct sm_chunk *c = kzalloc(sizeof(*c));
  if (!c)
    return 0;
  u64 base = sm_alloc_block();
  if (!base) {
    kfree(c);
    return 0;
  }
  c->base = base;
  if (paging_hide_frames_2m(base) != 0) {
    for (u64 i = 0; i < SM_CHUNK_PAGES; i++)
      pmm_free_frame(base + i * PAGE_SIZE);
    kfree(c);
    return 0;
  }
  spin_lock_irqsave(&g_sm_lock, &flags);
  int bit = sm_take_bit(c, &dirty);
  c->next = g_sm_chunks;
  g_sm_chunks = c;
  __atomic_add_fetch(&g_sm_nchunks, 1, __ATOMIC_RELAXED);
  spin_unlock_irqrestore(&g_sm_lock, flags);
  return base + (u64)bit * PAGE_SIZE;
}

static void sm_frame_free(u64 frame) {
  u64 base = frame & ~(SM_CHUNK_SIZE - 1);
  unsigned idx = (unsigned)((frame - base) / PAGE_SIZE);
  struct sm_chunk *empty = 0;
  u64 flags;

  spin_lock_irqsave(&g_sm_lock, &flags);
  for (struct sm_chunk **pp = &g_sm_chunks; *pp; pp = &(*pp)->next) {
    struct sm_chunk *c = *pp;
    if (c->base != base)
      continue;
    if (c->used[idx / 64] & (1ULL << (idx % 64))) {
      c->used[idx / 64] &= ~(1ULL << (idx % 64));
      if (--c->nused == 0) {
        *pp = c->next;
        __atomic_sub_fetch(&g_sm_nchunks, 1, __ATOMIC_RELAXED);
        empty = c;
      }
    }
    break;
  }
  spin_unlock_irqrestore(&g_sm_lock, flags);
  if (!empty)
    return;

  /* Visible again, and wiped while it is, before anyone else can have it. A
   * frame some mapping still references would be recycled under it: keep the
   * whole chunk out of circulation instead. */
  paging_unhide_frames_2m(base);
  memset((void *)(usize)(base + vmm_direct_map_base()), 0, SM_CHUNK_SIZE);
  for (u64 i = 0; i < SM_CHUNK_PAGES; i++) {
    if (pmm_get_refcount(base + i * PAGE_SIZE) != 1) {
      console_write("secretmem: chunk 0x");
      console_write_hex64(base);
      console_write(" still referenced on release; leaking it\n");
      kfree(empty);
      return;
    }
  }
  for (u64 i = 0; i < SM_CHUNK_PAGES; i++)
    pmm_free_frame(base + i * PAGE_SIZE);
  kfree(empty);
}

/* ── the file ──────────────────────────────────────────────────────── */

/* Page index -> frame, as a four-level radix of 512-entry tables: 2^48 bytes of
 * file, sparse, with no memory spent on pages never touched. */
#define SM_LEVELS 4
#define SM_FANOUT 512
#define SM_MAX_PAGES (1ULL << (9 * SM_LEVELS))

struct sm_file {
  struct vfs_inode *inode;
  spinlock_t lock;
  u64 *root;
  struct sm_file *next;
};

static spinlock_t g_sm_files_lock = SPINLOCK_INIT;
static struct sm_file *g_sm_files;

static struct sm_file *sm_file_of(struct vfs_inode *inode) {
  u64 flags;
  struct sm_file *f;
  spin_lock_irqsave(&g_sm_files_lock, &flags);
  for (f = g_sm_files; f && f->inode != inode; f = f->next)
    ;
  spin_unlock_irqrestore(&g_sm_files_lock, flags);
  return f;
}

/* The slot for page `pg`, creating the tables on the way when `create`. The
 * tables are allocated before the file lock is taken: `spare` holds one. */
static u64 *sm_slot(struct sm_file *f, u64 pg, u64 **spare) {
  u64 **link = &f->root;
  for (int level = SM_LEVELS - 1;; level--) {
    if (!*link) {
      if (!spare || !*spare)
        return 0;
      *link = *spare;
      *spare = 0;
    }
    u64 *table = *link;
    usize i = (usize)((pg >> (9 * level)) & (SM_FANOUT - 1));
    if (level == 0)
      return &table[i];
    link = (u64 **)&table[i];
  }
}

static int sm_fault(struct vfs_node *node, u64 file_page, u64 *out_phys) {
  struct vfs_inode *inode = node->inode;
  struct sm_file *f = sm_file_of(inode);
  u64 pg = file_page / PAGE_SIZE;

  if (!f || file_page >= inode->size || pg >= SM_MAX_PAGES)
    return -EFAULT; /* past the end: SIGBUS */

  u64 frame = 0;
  int rc;
  for (;;) {
    u64 flags;
    spin_lock_irqsave(&f->lock, &flags);
    u64 *slot = sm_slot(f, pg, 0);
    if (slot && (*slot || frame)) {
      u64 spare_frame = 0;
      if (!*slot)
        *slot = frame;
      else
        spare_frame = frame; /* another fault filled it first */
      pmm_ref_frame(*slot); /* the mapping's reference */
      *out_phys = *slot;
      spin_unlock_irqrestore(&f->lock, flags);
      if (spare_frame)
        sm_frame_free(spare_frame);
      return 0;
    }
    spin_unlock_irqrestore(&f->lock, flags);

    /* Allocate outside the lock: a table on the way, or the page itself. */
    if (!slot) {
      u64 *spare = kzalloc(SM_FANOUT * sizeof(u64));
      if (!spare) {
        rc = -ENOMEM;
        break;
      }
      spin_lock_irqsave(&f->lock, &flags);
      (void)sm_slot(f, pg, &spare);
      spin_unlock_irqrestore(&f->lock, flags);
      if (spare)
        kfree(spare);
      continue;
    }
    frame = sm_frame_alloc();
    if (!frame) {
      rc = -ENOMEM;
      break;
    }
  }
  if (frame)
    sm_frame_free(frame);
  return rc;
}

static void sm_free_tree(u64 *table, int level) {
  if (!table)
    return;
  for (usize i = 0; i < SM_FANOUT; i++) {
    if (!table[i])
      continue;
    if (level == 0)
      sm_frame_free(table[i]);
    else
      sm_free_tree((u64 *)(usize)table[i], level - 1);
  }
  kfree(table);
}

static void sm_release(struct vfs_node *node) {
  struct sm_file *f = 0;
  u64 flags;
  spin_lock_irqsave(&g_sm_files_lock, &flags);
  for (struct sm_file **pp = &g_sm_files; *pp; pp = &(*pp)->next) {
    if ((*pp)->inode == node->inode) {
      f = *pp;
      *pp = f->next;
      break;
    }
  }
  spin_unlock_irqrestore(&g_sm_files_lock, flags);
  if (!f)
    return;
  sm_free_tree(f->root, SM_LEVELS - 1);
  kfree(f);
}

static isize sm_read(struct vfs_node *node, u64 offset, char *buffer,
                     usize size, int flags) {
  (void)node, (void)offset, (void)buffer, (void)size, (void)flags;
  return -EINVAL;
}

static isize sm_write(struct vfs_node *node, u64 offset, const char *buffer,
                      usize size, int flags) {
  (void)node, (void)offset, (void)buffer, (void)size, (void)flags;
  return -EINVAL;
}

/* Linux lets the size be set exactly once: pages already handed to a mapping
 * can then never fall beyond the end of the file. */
static int sm_truncate(struct vfs_node *node, u64 length) {
  if (node->inode->size)
    return -EINVAL;
  if (length > SM_MAX_PAGES * PAGE_SIZE)
    return -EFBIG;
  node->inode->size = (usize)length;
  return 0;
}

int secretmem_attach(struct vfs_node *node) {
  struct sm_file *f = kzalloc(sizeof(*f));
  if (!f)
    return -ENOMEM;
  f->inode = node->inode;
  node->inode->flags |= VFS_NODE_SECRETMEM;
  node->inode->read_cb = sm_read;
  node->inode->write_cb = sm_write;
  node->inode->truncate_cb = sm_truncate;
  node->inode->release_cb = sm_release;
  node->inode->mmap_fault_cb = sm_fault;
  u64 flags;
  spin_lock_irqsave(&g_sm_files_lock, &flags);
  f->next = g_sm_files;
  g_sm_files = f;
  spin_unlock_irqrestore(&g_sm_files_lock, flags);
  return 0;
}

int secretmem_available(void) { return g_sm_scrub_va != 0; }

int secretmem_vma(const struct vm_area *v) {
  return v && v->node && v->node->inode &&
         (v->node->inode->flags & VFS_NODE_SECRETMEM);
}

/* mmap-time checks: shared only, and inside RLIMIT_MEMLOCK together with the
 * process's other secret mappings (they are locked memory, as on Linux). */
int secretmem_mmap_check(struct task *t, u64 length, int map_flags) {
  if (!(map_flags & MAP_SHARED))
    return -EINVAL;
  struct rlimit lim;
  if (scheduler_getrlimit(RLIMIT_MEMLOCK, &lim) != 0 ||
      lim.rlim_cur == RLIM_INFINITY ||
      cred_has_cap(t->cred, CAP_IPC_LOCK))
    return 0;
  u64 locked = length;
  vma_walker_enter();
  for (struct vm_area *v = t->vma_list; v; v = v->next)
    if (secretmem_vma(v))
      locked += v->end - v->start;
  vma_walker_exit();
  return locked > lim.rlim_cur ? -EAGAIN : 0;
}
