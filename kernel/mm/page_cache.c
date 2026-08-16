#include <b1nix/page_cache.h>
#include <b1nix/vfs.h>
#include <b1nix/mm.h>
#include <b1nix/errno.h>
#include <b1nix/console.h>
#include <b1nix/sched.h>
#include <string.h>
#include <b1nix/bootinfo.h>

#define PC_HASH_SIZE 1024

static struct page_cache_entry *hash_table[PC_HASH_SIZE];
/* Variant E — two LRU lists. lru_head/lru_tail is the INACTIVE list (pages seen
 * once); active_head/active_tail is the ACTIVE list (the protected working set:
 * pages referenced again or refaulted). New pages land on inactive; a second
 * reference (page_cache_get_page hit) promotes to active. Eviction drains
 * inactive first and only demotes active->inactive when inactive is empty, so a
 * one-shot scan (a source file read once) is reclaimed before clang's text that
 * every TU touches. */
static struct page_cache_entry *lru_head;       /* inactive (oldest at head) */
static struct page_cache_entry *lru_tail;
static struct page_cache_entry *active_head;     /* active (oldest at head) */
static struct page_cache_entry *active_tail;
static volatile int pc_lock = 0;

/* Refault ring: keys (ino,offset) of pages evicted recently. If a page is
 * re-added soon after eviction it was wrongly evicted (working set bigger than
 * the inactive list), so seed it straight onto the active list. A small ring is
 * enough to catch the hot set churning under pressure. */
#define PC_REFAULT_N 256
static struct { u64 ino; u64 offset; } pc_refault[PC_REFAULT_N];
static u32 pc_refault_w;

static void pc_refault_record(u64 ino, u64 offset) {
  pc_refault[pc_refault_w].ino = ino;
  pc_refault[pc_refault_w].offset = offset;
  pc_refault_w = (pc_refault_w + 1) % PC_REFAULT_N;
}
static int pc_refault_hit(u64 ino, u64 offset) {
  for (u32 i = 0; i < PC_REFAULT_N; i++)
    if (pc_refault[i].ino == ino && pc_refault[i].offset == offset)
      return 1;
  return 0;
}

static struct page_cache_entry *to_free_list = 0;

static int is_power_of_two_u64(u64 value) {
  return value && ((value & (value - 1)) == 0);
}

static void m26_diag_task(void) {
  if (current_task && current_task->name) {
    console_write(" task=");
    console_write(current_task->name);
    console_write(" pid=");
    console_write_dec(current_task->id);
  }
}

/* Per-bucket locks, in front of the one cache-wide lock.
 *
 * Everything used to serialise on pc_lock, including the lookup — the hottest
 * path in the kernel, taken by every mapped page of every executable, by every
 * read(), and by the fault handler's read-ahead. Two CPUs faulting on
 * unrelated files queued behind each other for no reason: their entries live
 * in different hash chains and share nothing.
 *
 * The rule that keeps this deadlock-free is one-directional: a reader takes
 * only its bucket, never pc_lock. A mutator takes pc_lock first and the bucket
 * second, and holds the bucket only across the chain edit itself — never
 * across writeback, which drops pc_lock and blocks on I/O.
 */
static volatile int pc_bucket[PC_HASH_SIZE];

static void lock_bucket(u32 h) {
  extern void tlb_shootdown_poll(void);

  while (__sync_lock_test_and_set(&pc_bucket[h], 1)) {
    while (pc_bucket[h]) {
      __asm__ volatile("pause");
      tlb_shootdown_poll();
    }
  }
}

static void unlock_bucket(u32 h) { __sync_lock_release(&pc_bucket[h]); }

static void lock_pc(void) {
  extern void tlb_shootdown_poll(void);
  while (__sync_lock_test_and_set(&pc_lock, 1)) {
    /* Drain TLB shootdowns while spinning — a waiter that entered with IRQs
     * disabled otherwise can't ACK the initiator's IPI (deadlock). */
    while (pc_lock) { __asm__ volatile("pause"); tlb_shootdown_poll(); }
  }
}

static void unlock_pc(void) {
  __sync_lock_release(&pc_lock);
}

static void page_cache_process_deferred_free(void) {
  lock_pc();
  struct page_cache_entry *curr = to_free_list;
  to_free_list = 0;
  unlock_pc();
  
  while (curr) {
    struct page_cache_entry *next = curr->hash_next;
    kfree(curr);
    curr = next;
  }
}

/* Hash on (fs_id, ino), NOT the inode pointer: the inode slab pool reuses
 * addresses, and pointer-keyed entries of a destroyed inode would collide with
 * a later inode landing at the same address (observed: ld.so mapping libOSMesa
 * got another file's cached page for offset 0). ino alone is not enough either
 * — it is only unique within one filesystem, so with initramfs and the ext4
 * root both mounted, two unrelated files sharing an ino number served each
 * other's pages (observed: read() of libpam.so.2 returning another mapping's
 * live pointers instead of its .plt bytes). */
static u32 pc_hash(struct vfs_inode *inode, u64 offset) {
  u64 val = (inode ? inode->ino : 0) ^ ((u64)(inode ? inode->fs_id : 0) << 32) ^
            (offset >> 12);
  val ^= val >> 16;
  return (u32)(val % PC_HASH_SIZE);
}

/* Full cache-key comparison: (fs_id, ino, offset). See the key_fsid comment in
 * page_cache.h for why the filesystem id is part of the identity. */
static int pc_key_eq(const struct page_cache_entry *e, struct vfs_inode *inode,
                     u64 offset) {
  return e->key_ino == inode->ino && e->key_fsid == inode->fs_id &&
         e->offset == offset;
}

/* Unlink from whichever list (active or inactive) the page is currently on —
 * the PAGE_CACHE_ACTIVE flag selects the head/tail pair. */
static void lru_remove(struct page_cache_entry *page) {
  struct page_cache_entry **head = (page->flags & PAGE_CACHE_ACTIVE)
                                       ? &active_head : &lru_head;
  struct page_cache_entry **tail = (page->flags & PAGE_CACHE_ACTIVE)
                                       ? &active_tail : &lru_tail;
  if (page->lru_prev) page->lru_prev->lru_next = page->lru_next;
  else if (*head == page) *head = page->lru_next;

  if (page->lru_next) page->lru_next->lru_prev = page->lru_prev;
  else if (*tail == page) *tail = page->lru_prev;

  page->lru_next = page->lru_prev = 0;
}

/* Append to the INACTIVE list tail (most-recently-added cold page). */
static void lru_append(struct page_cache_entry *page) {
  page->flags &= ~PAGE_CACHE_ACTIVE;
  page->lru_next = 0;
  page->lru_prev = lru_tail;
  if (lru_tail) lru_tail->lru_next = page;
  else lru_head = page;
  lru_tail = page;
}

/* Append to the ACTIVE list tail (working set, MRU end). */
static void active_append(struct page_cache_entry *page) {
  page->flags |= PAGE_CACHE_ACTIVE;
  page->lru_next = 0;
  page->lru_prev = active_tail;
  if (active_tail) active_tail->lru_next = page;
  else active_head = page;
  active_tail = page;
}

/* Demote up to `n` oldest active pages to the inactive list so eviction can
 * reclaim them once the inactive list is drained. Returns how many moved. */
static usize demote_active(usize n) {
  usize moved = 0;
  while (moved < n && active_head) {
    struct page_cache_entry *p = active_head;
    lru_remove(p);       /* unlinks from active (flag still set) */
    lru_append(p);       /* clears ACTIVE, appends to inactive */
    moved++;
  }
  return moved;
}

void page_cache_init(void) {
  memset(hash_table, 0, sizeof(hash_table));
  lru_head = lru_tail = 0;
  active_head = active_tail = 0;
  pc_refault_w = 0;
  for (u32 i = 0; i < PC_REFAULT_N; i++) { pc_refault[i].ino = 0; pc_refault[i].offset = 0; }
  pc_lock = 0;
}

/* ── File-level sequential readahead ────────────────────────────────────────
 * A small per-inode cursor table detects a sequential access pattern in
 * page_cache_get_page. On a sequential cache MISS it prefetches the next
 * RA_PREFETCH pages so a file read sequentially (the in-guest self-host build
 * reading sources/headers, or a sequential mmap scan) stops paying one
 * blocking block read per page. Best-effort by design: a cold table, a
 * non-file inode, no read_cb, or memory pressure simply skips the burst. */
#define RA_STREAMS   256
#define RA_PREFETCH    4
#define RA_WARMUP      1  /* sequential misses seen before the first burst */

struct ra_stream {
  u64 ino;
  u32 fsid;
  u64 next; /* next file page expected for this stream (in PAGE_SIZE units) */
  u32 seq;  /* consecutive sequential misses observed */
};

static struct ra_stream ra_streams[RA_STREAMS];
static int ra_in_prefetch; /* re-entrancy guard (best-effort, mirrors the
                              proactive-evict guard) */

static u32 ra_hash(const struct vfs_inode *inode) {
  u32 h = (u32)(inode->ino ^ (inode->ino >> 32));
  h ^= (u32)inode->fs_id;
  h ^= h >> 10;
  return h % RA_STREAMS;
}

/* Prefetch RA_PREFETCH pages starting at (offset+1). Must be called with
 * pc_lock RELEASED: it allocates frames and issues blocking read_cb I/O. It
 * re-enters page_cache_get_page/page_cache_add_page; those re-entries observe
 * ra_in_prefetch and never schedule a nested burst. */
static void pc_readahead(struct vfs_inode *inode, u64 offset) {
  if (ra_in_prefetch || !inode->read_cb)
    return;
  ra_in_prefetch = 1;
  struct vfs_node dummy;
  memset(&dummy, 0, sizeof(dummy));
  dummy.inode = inode;
  /* offset is a byte offset; the burst walks PAGE NUMBERS.
   *
   * Multiplying the byte offset by PAGE_SIZE again aimed every prefetch at a
   * position PAGE_SIZE times too far into the file: reading page 1 of a binary
   * fetched somewhere past 16 MB. Each page came back under its own correct
   * key, so nothing was corrupted — it simply never prefetched the pages the
   * reader was about to want, and spent a disk command per page to fill the
   * cache with parts of the file nobody asked for. */
  u64 base = offset / PAGE_SIZE;
  for (u64 po = base + 1; po <= base + RA_PREFETCH; po++) {
    u64 poff = po * PAGE_SIZE;
    if (poff >= inode->size)
      break; /* don't read past EOF */
    struct page_cache_entry *pe = page_cache_get_page(inode, poff);
    if (pe) {
      page_cache_put_page(pe); /* already resident — drop the pin */
      continue;
    }
    u64 frame = pmm_alloc_frame();
    if (!frame)
      break; /* memory pressure — drop the rest of the burst */
    void *virt = (void *)(usize)(frame + vmm_direct_map_base());
    memset(virt, 0, PAGE_SIZE);
    if (inode->read_cb(&dummy, poff, virt, PAGE_SIZE, 0) < 0) {
      pmm_free_frame(frame);
      break;
    }
    if (page_cache_add_page(inode, poff, frame) < 0)
      pmm_free_frame(frame); /* raced with another reader — keep their page */
  }
  ra_in_prefetch = 0;
}

struct page_cache_entry *page_cache_get_page(struct vfs_inode *inode, u64 offset) {
  if (to_free_list) {
    page_cache_process_deferred_free();
  }
  u32 h = pc_hash(inode, offset);

  /* The lookup needs its chain, nothing else: the hit path no longer touches
   * the LRU lists (it sets PAGE_CACHE_REFERENCED instead), so the cache-wide
   * lock is not involved at all. */
  lock_bucket(h);
  struct page_cache_entry *curr = hash_table[h];
  while (curr) {
    /* Identity by (fs_id, ino), not the inode pointer — the inode slab pool
     * reuses freed addresses, so a stale entry whose owning inode was already
     * destroyed can still carry a pointer that numerically matches a brand-new,
     * unrelated inode landing at the same address (the exact "ld.so mapping
     * libOSMesa got another file's cached page" class of bug this struct's
     * key_ino field exists to prevent — see page_cache_invalidate_inode). */
    if (pc_key_eq(curr, inode, offset)) {
      curr->refcount++;
      /* Mark it touched and leave the lists alone.
       *
       * Promoting on every hit meant unlinking and re-linking the entry — four
       * pointer writes under the cache's one lock, on the path every mapped
       * page of every executable takes. The bit says the same thing to
       * eviction, which is the only code that needs to know, and it costs one
       * store. Entries still reach the active list: eviction promotes the ones
       * it finds referenced instead of taking them. */
      curr->flags |= PAGE_CACHE_REFERENCED;
      /* Hit: a sequential reader landing on an already-prefetched page advances
       * the cursor (so the next burst fires at the right place) without
       * scheduling — the earlier burst already filled this window. */
      if (!ra_in_prefetch && inode->read_cb && inode->type == VFS_FILE) {
        struct ra_stream *r = &ra_streams[ra_hash(inode)];
        if (r->ino == inode->ino && r->fsid == inode->fs_id &&
            offset / PAGE_SIZE == r->next)
          r->next = offset / PAGE_SIZE + 1;
      }
      unlock_bucket(h);
      return curr;
    }
    curr = curr->hash_next;
  }

  /* Miss: sequential-stream bookkeeping under the bucket lock; the actual
   * prefetch (blocking I/O) runs after it is released. The read-ahead cursors
   * are a heuristic — a rare race between two buckets costs one mispredicted
   * burst, never correctness. */
  int do_ra = 0;
  if (!ra_in_prefetch && inode->read_cb && inode->type == VFS_FILE) {
    struct ra_stream *r = &ra_streams[ra_hash(inode)];
    if (r->ino == inode->ino && r->fsid == inode->fs_id &&
        offset / PAGE_SIZE == r->next) {
      r->next = offset / PAGE_SIZE + 1;
      if (++r->seq >= RA_WARMUP)
        do_ra = 1;
    } else {
      /* New stream or a jump — restart the cursor, no burst yet (warm-up). */
      r->ino = inode->ino;
      r->fsid = inode->fs_id;
      r->next = offset / PAGE_SIZE + 1;
      r->seq = 0;
    }
  }
  unlock_bucket(h);

  if (do_ra)
    pc_readahead(inode, offset);
  return 0;
}

/* Re-entrancy guard for proactive eviction: page_cache_evict() writes dirty
 * pages back through inode->write_cb (ext4), and that write path can itself read
 * + cache blocks, re-entering page_cache_add_page. Without the guard the proactive
 * evict below would recurse. Best-effort across CPUs — a race only costs one extra
 * evict pass (page_cache_evict is internally locked), never corruption. */
static int pc_proactive_evicting;

int page_cache_add_page(struct vfs_inode *inode, u64 offset, u64 frame) {
  if (to_free_list) {
    page_cache_process_deferred_free();
  }

  /* Proactive cap: the page cache holds reclaimable pmm frames, but reactive
   * reclaim only fires on an allocation miss and tops free back up to just
   * total/512 (~1 MB at 512 MB RAM). That razor-thin headroom let the cache grow
   * to the brink, so a burst allocation — clang's staging during the in-guest
   * self-host — outran reclaim and OOM'd at free=0. When free drops below
   * total/16 we trim a small batch of the oldest CLEAN pages before adding a new
   * one, keeping real headroom. Crucially this is clean-ONLY
   * (page_cache_evict_clean): it must never force synchronous dirty .o writeback
   * here. A demand-paged clang faults its text through page_cache_add_page on
   * every new page, and a dirty-writeback in that hot path would drive the slow
   * polled AHCI on each fault and thrash (143 reclaim cycles, OOM). Expensive
   * dirty writeback is deferred to reactive reclaim in pmm_alloc_frames, which
   * only runs on a true allocation miss. On a roomy guest free stays far above
   * the watermark so this never fires. */
  if (!pc_proactive_evicting) {
    usize total_frames = (usize)(pmm_total_usable_memory() / PAGE_SIZE);
    usize watermark = total_frames / 16;
    if (watermark < 512)
      watermark = 512; /* ~2 MB floor so tiny guests still get headroom */
    usize free = pmm_free_frame_count();
    if (free < watermark) {
      usize deficit = watermark - free;
      usize batch = deficit < 32 ? deficit : 32;
      pc_proactive_evicting = 1;
      page_cache_evict_clean(batch);
      pc_proactive_evicting = 0;
    }
  }

  u32 h = pc_hash(inode, offset);

  struct page_cache_entry *new_entry = kmalloc(sizeof(struct page_cache_entry));
  if (!new_entry) return -ENOMEM;

  new_entry->inode = inode;
  new_entry->key_ino = inode->ino;
  new_entry->key_fsid = inode->fs_id;
  new_entry->offset = offset;
  new_entry->frame = frame;
  new_entry->flags = PAGE_CACHE_UPTODATE;
  new_entry->refcount = 0; // Starts at 0, incremented by get_page if needed
  
  /* Mutator order: the cache-wide lock first (the LRU lists below need it),
   * the bucket second, and the bucket only across the chain edit. */
  lock_pc();
  lock_bucket(h);
  // Check if it was added concurrently
  struct page_cache_entry *curr = hash_table[h];
  while (curr) {
    /* (fs_id, ino), not the inode pointer — see the matching comment in
     * page_cache_get_page. */
    if (pc_key_eq(curr, inode, offset)) {
      unlock_bucket(h);
      unlock_pc();
      kfree(new_entry);
      return -EEXIST;
    }
    curr = curr->hash_next;
  }
  
  pmm_ref_frame(new_entry->frame);
  new_entry->hash_next = hash_table[h];
  hash_table[h] = new_entry;
  unlock_bucket(h);

  /* Refault: this page was evicted recently and is already back — the working
   * set is bigger than the inactive list, so seed it straight onto the active
   * list instead of making it climb through inactive again (where it would just
   * be re-evicted). Otherwise it starts cold on the inactive list. */
  if (pc_refault_hit(inode->ino, offset))
    active_append(new_entry);
  else
    lru_append(new_entry);
  unlock_pc();

  return 0;
}

void page_cache_mark_dirty(struct page_cache_entry *page) {
  lock_pc();
  page->flags |= PAGE_CACHE_DIRTY;
  unlock_pc();
}

static void writeback_page_locked(struct page_cache_entry *page) {
  if ((page->flags & PAGE_CACHE_DIRTY) && page->inode && page->inode->write_cb) {
    struct vfs_node dummy;
    memset(&dummy, 0, sizeof(dummy));
    dummy.inode = page->inode;

    usize size = PAGE_SIZE;
    if (page->offset + PAGE_SIZE > page->inode->size) {
      if (page->offset >= page->inode->size) {
        size = 0;
      } else {
        size = page->inode->size - page->offset;
      }
    }

    if (size > 0) {
      /* Pin the entry across the unlocked write_cb: a concurrent
       * page_cache_invalidate_inode must orphan it (refcount != 0), not free
       * it out from under us. */
      page->refcount++;
      unlock_pc();
      void *virt_addr = (void *)(usize)(page->frame + vmm_direct_map_base());
      page->inode->write_cb(&dummy, page->offset, virt_addr, size, 0);
      lock_pc();
      page->refcount--;
      if (page->refcount == 0 && (page->flags & PAGE_CACHE_ORPHAN)) {
        /* Invalidated while we were writing: finish its teardown here. */
        pmm_free_frame(page->frame);
        page->hash_next = to_free_list;
        to_free_list = page;
        return;
      }
    }

    page->flags &= ~PAGE_CACHE_DIRTY;
  }
}

int page_cache_flush_inode(struct vfs_inode *inode) {
  if (!inode) return -1;
  /* Walk the inode's cached pages, not its offsets. Probing offset by offset
   * asked the cache for pages that were never resident, and every miss
   * advanced the sequential cursor and armed read-ahead — so closing a large
   * file read most of it back from disk in order to write a few dirty pages
   * out. page_cache_invalidate_inode already walks the LRU lists this way. */
  lock_pc();
  struct page_cache_entry *heads[2] = { lru_head, active_head };
  for (int li = 0; li < 2; li++) {
    for (struct page_cache_entry *curr = heads[li]; curr; curr = curr->lru_next) {
      if (curr->inode == inode && (curr->flags & PAGE_CACHE_DIRTY))
        writeback_page_locked(curr);
    }
  }
  unlock_pc();
  return 0;
}

void page_cache_invalidate_inode(struct vfs_inode *inode) {
  if (!inode)
    return;

  int invalidated = 0;
  lock_pc();
  /* Walk BOTH LRU lists (inactive then active) — pages of this inode can sit on
   * either after Variant E's promotion. */
  struct page_cache_entry *heads[2] = { lru_head, active_head };
  for (int li = 0; li < 2; li++) {
    struct page_cache_entry *curr = heads[li];
    while (curr) {
      struct page_cache_entry *next = curr->lru_next;
      if (curr->inode == inode) {
        u32 h = pc_hash(curr->inode, curr->offset);
        lock_bucket(h);
        struct page_cache_entry **prev = &hash_table[h];
        struct page_cache_entry *hcurr = *prev;
        while (hcurr) {
          if (hcurr == curr) {
            *prev = hcurr->hash_next;
            break;
          }
          prev = &hcurr->hash_next;
          hcurr = hcurr->hash_next;
        }
        unlock_bucket(h);
        lru_remove(curr);
        curr->inode = 0;
        if (curr->refcount == 0) {
          pmm_free_frame(curr->frame);
          curr->hash_next = to_free_list;
          to_free_list = curr;
        } else {
          /* A reader still holds a reference (fault path / writeback). The
           * entry is now unreachable (off hash + LRU); mark it ORPHAN so the
           * last put frees the frame and defers the entry. Leaving it in the
           * hash keyed by a soon-to-be-recycled inode is what used to serve
           * another file's page to a new inode at the same address. */
          curr->flags |= PAGE_CACHE_ORPHAN;
        }
        invalidated++;
      }
      curr = next;
    }
  }
  unlock_pc();

  if (bootinfo_has_flag("b1nix.debug.heap") && invalidated > 0) {
    console_write("[M26DIAG] pc_invalidate inode=0x");
    console_write_hex64((u64)(usize)inode);
    console_write(" pages=");
    console_write_dec(invalidated);
    m26_diag_task();
    console_write("\n");
  }
}

void page_cache_truncate_inode(struct vfs_inode *inode, u64 new_size) {
  if (!inode)
    return;

  lock_pc();
  /* Both LRU lists — a truncated inode's tail pages may be active. */
  struct page_cache_entry *heads[2] = { lru_head, active_head };
  for (int li = 0; li < 2; li++) {
   struct page_cache_entry *curr = heads[li];
   while (curr) {
    struct page_cache_entry *next = curr->lru_next;
    if (curr->inode == inode && curr->offset + PAGE_SIZE > new_size) {
      void *virt = (void *)(usize)(curr->frame + vmm_direct_map_base());
      if (curr->offset >= new_size) {
        /* Page lies fully beyond the new EOF. Drop it so a later re-grow
         * reads zeros instead of resurrecting pre-truncate contents. If a
         * reader still holds a reference, neutralize in place instead. */
        if (curr->refcount == 0) {
          u32 h = pc_hash(curr->inode, curr->offset);
          lock_bucket(h);
          struct page_cache_entry **prev = &hash_table[h];
          struct page_cache_entry *hcurr = *prev;
          while (hcurr) {
            if (hcurr == curr) {
              *prev = hcurr->hash_next;
              break;
            }
            prev = &hcurr->hash_next;
            hcurr = hcurr->hash_next;
          }
          unlock_bucket(h);
          lru_remove(curr);
          pmm_free_frame(curr->frame);
          curr->inode = 0;
          curr->hash_next = to_free_list;
          to_free_list = curr;
        } else {
          memset(virt, 0, PAGE_SIZE);
          curr->flags &= ~PAGE_CACHE_DIRTY;
        }
      } else {
        /* Partial tail page: zero the bytes beyond the new EOF. */
        usize keep = (usize)(new_size - curr->offset);
        memset((u8 *)virt + keep, 0, PAGE_SIZE - keep);
      }
    }
    curr = next;
   }
  }
  unlock_pc();
}

void page_cache_put_page(struct page_cache_entry *page) {
  lock_pc();
  if (page->refcount > 0) {
    page->refcount--;
  }
  if (page->refcount == 0 && (page->flags & PAGE_CACHE_ORPHAN)) {
    /* Inode was destroyed while we held the reference; the entry is already
     * off the hash and LRU — finish its teardown now. */
    pmm_free_frame(page->frame);
    page->hash_next = to_free_list;
    to_free_list = page;
  }
  unlock_pc();
}

/* Evict up to target_pages CLEAN, unreferenced pages from the oldest end of the
 * LRU — never writing anything back. This is the cheap reclaim used in hot paths
 * (the proactive cap in page_cache_add_page, fired on every demand-fault that
 * caches a page): it must NOT trigger synchronous dirty .o writeback over the
 * slow polled AHCI, which otherwise thrashes once a demand-paged clang faults
 * its text against a cache full of dirty build output. Expensive dirty
 * writeback is left to reactive reclaim (page_cache_evict from
 * pmm_alloc_frames), which only runs on a genuine allocation miss. Returns the
 * number of pages actually freed (may be < target if the tail is all dirty or
 * referenced). */
usize page_cache_evict_clean(usize target_pages) {
  if (target_pages == 0)
    target_pages = 1;
  lock_pc();
  usize evicted = 0;
  while (evicted < target_pages) {
    struct page_cache_entry *victim = 0;
    /* Inactive list only — the cold pages. */
    for (struct page_cache_entry *curr = lru_head; curr; curr = curr->lru_next) {
      if (curr->refcount != 0)
        continue;
      if (curr->flags & PAGE_CACHE_DIRTY)
        continue; /* clean-only: never write back here */
      victim = curr;
      break;
    }
    if (!victim) {
      /* Inactive is exhausted of clean victims: demote a batch of the oldest
       * active (working-set) pages to inactive and try again. This is how the
       * working set is eventually reclaimable under sustained pressure without
       * being the FIRST thing evicted. If nothing can be demoted, give up. */
      if (demote_active(target_pages - evicted) == 0)
        break;
      continue;
    }
    u32 h = pc_hash(victim->inode, victim->offset);
    lock_bucket(h);
    struct page_cache_entry **prev = &hash_table[h];
    struct page_cache_entry *hcurr = *prev;
    while (hcurr) {
      if (hcurr == victim) {
        *prev = hcurr->hash_next;
        break;
      }
      prev = &hcurr->hash_next;
      hcurr = hcurr->hash_next;
    }
    unlock_bucket(h);
    lru_remove(victim);
    pc_refault_record(victim->key_ino, victim->offset);
    pmm_free_frame(victim->frame);
    victim->hash_next = to_free_list;
    to_free_list = victim;
    evicted++;
  }
  unlock_pc();
  return evicted;
}

usize page_cache_evict(usize target_pages) {
  static u64 evict_calls;
  if (target_pages == 0)
    target_pages = 1;
  evict_calls++;
  lock_pc();

  usize evicted = 0;
  usize dirty_skipped = 0;
  usize dirty_written = 0;

  /* Each pass walks the LRU from the oldest end and either evicts one clean,
   * unreferenced page or writes back one dirty page so it becomes evictable.
   * The pre-read-ahead behaviour skipped dirty pages outright, so a workload
   * that dirties most of the cache (e.g. the in-guest self-host writing .o
   * files to the ram0 module, on a machine with no swap device) could reclaim
   * NOTHING under memory pressure and OOM/triple-fault — which is why the
   * self-host needed ~16 GiB. Writing dirty pages back caps the cache at the
   * real working set. writeback_page_locked transiently drops pc_lock, so a
   * cursor saved across it could dangle; restart the walk from lru_head each
   * pass instead. Bounded: every dirty page is flushed at most once (writeback
   * clears DIRTY) and every clean page evicted at most once. */
  while (evicted < target_pages) {
    struct page_cache_entry *victim = 0;
    struct page_cache_entry *flush = 0;
    dirty_skipped = 0;
    for (struct page_cache_entry *curr = lru_head; curr;) {
      struct page_cache_entry *next = curr->lru_next;

      if (curr->refcount != 0) {
        curr = next;
        continue;
      }
      /* Touched since the last sweep: clear the bit and promote it instead of
       * taking it. This is where a hit's single store becomes the working-set
       * decision that the hit path used to make by relinking the entry. */
      if (curr->flags & PAGE_CACHE_REFERENCED) {
        curr->flags &= ~PAGE_CACHE_REFERENCED;
        lru_remove(curr);
        active_append(curr);
        curr = next;
        continue;
      }
      if (curr->flags & PAGE_CACHE_DIRTY) {
        if (curr->inode && curr->inode->write_cb) {
          flush = curr; /* oldest flushable dirty page */
          break;
        }
        dirty_skipped++; /* no write_cb — cannot reclaim, leave it in place */
        curr = next;
        continue;
      }
      victim = curr; /* oldest clean, untouched page */
      break;
    }

    if (flush) {
      writeback_page_locked(flush); /* clears DIRTY (drops+retakes pc_lock) */
      dirty_written++;
      continue; /* rescan: the page is now clean and evictable */
    }
    if (!victim) {
      /* Inactive list has nothing reclaimable (all referenced / unflushable):
       * demote a batch of the oldest active working-set pages to inactive and
       * retry, so the working set is reclaimable last rather than never. */
      if (demote_active(target_pages - evicted) == 0)
        break;
      continue;
    }

    /* Evict the clean victim: unlink from the hash chain + LRU, free its frame,
     * and defer the entry's kfree. */
    u32 h = pc_hash(victim->inode, victim->offset);
    lock_bucket(h);
    struct page_cache_entry **prev = &hash_table[h];
    struct page_cache_entry *hcurr = *prev;
    while (hcurr) {
      if (hcurr == victim) {
        *prev = hcurr->hash_next;
        break;
      }
      prev = &hcurr->hash_next;
      hcurr = hcurr->hash_next;
    }
    unlock_bucket(h);
    lru_remove(victim);
    pc_refault_record(victim->key_ino, victim->offset);
    pmm_free_frame(victim->frame);
    victim->hash_next = to_free_list;
    to_free_list = victim;
    evicted++;
  }

  unlock_pc();
  if (bootinfo_has_flag("b1nix.debug.heap") && (evict_calls <= 16 || is_power_of_two_u64(evict_calls) ||
      dirty_skipped > 0 || evicted < target_pages)) {
    console_write("[M26DIAG] pc_evict call=");
    console_write_dec(evict_calls);
    console_write(" target=");
    console_write_dec(target_pages);
    console_write(" evicted=");
    console_write_dec(evicted);
    console_write(" dirty_skipped=");
    console_write_dec(dirty_skipped);
    console_write(" dirty_written=");
    console_write_dec(dirty_written);
    console_write(" free_frames=");
    console_write_dec(pmm_free_frame_count());
    m26_diag_task();
    console_write("\n");
  }
  return evicted;
}
