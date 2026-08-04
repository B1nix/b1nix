#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/page_cache.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/arch.h>
#include <b1nix/lapic.h>   /* struct percpu + MAX_CPUS for the per-CPU PCP cache */
#include <string.h>
#include <b1nix/bootinfo.h>

#define BITS_PER_BYTE 8

extern u8 __kernel_start[];
extern u8 __kernel_end[];

struct pmm_state {
  u64 kernel_start;
  u64 kernel_end;
  u64 max_address;
  u64 total_usable;   /* RAM the kernel can actually use (clamped to direct map) */
  u64 phys_total;     /* installed RAM reported by firmware (uncapped, for display) */
  u64 free_frames;
  usize bitmap_bytes;
  u8 *bitmap;
  u16 *frame_refcounts;
  /* Free-page inventory lives in the buddy allocator (see below): free blocks
   * hang off per-order free lists, and the used-bitmap stays the authoritative
   * "free vs used" map that reclaim / watermarks / free_frames all read.
   * buddy_heads has one bit per frame, set exactly while that frame is the
   * head of a block currently linked into free_area[] — it makes "is this a
   * live free block?" an O(1) test instead of a per-page bitmap walk. */
  u8 *buddy_heads;
};

static struct pmm_state pmm;
static spinlock_t pmm_lock = SPINLOCK_INIT;

/* Variant C — reclaim re-entrancy guard. Page-cache eviction writes dirty pages
 * back through write_cb (ext4), which kmallocs a block buffer and can grow the
 * kheap -> vmm_map_page -> pmm_alloc. If that nested allocation also fails and
 * tried to reclaim again, it would re-enter the dirty-writeback path while the
 * outer kheap_grow still holds heap_lock — a lock-ordering deadlock (observed as
 * a hard wedge when reclaim is driven hard). pmm_reclaim_depth is raised around
 * the writeback eviction; while it is >0 a nested pmm_alloc does only CLEAN-only
 * reclaim (no write_cb, no kmalloc) and otherwise fails gracefully, so the
 * writeback path can always make forward progress or back off without deadlock.
 * Counter precision under SMP isn't critical — it only steers a rare nested
 * alloc onto the safe path. */
static volatile int pmm_reclaim_depth;
static void pmm_enter_reclaim(void) {
  __atomic_add_fetch(&pmm_reclaim_depth, 1, __ATOMIC_RELAXED);
}
static void pmm_leave_reclaim(void) {
  __atomic_sub_fetch(&pmm_reclaim_depth, 1, __ATOMIC_RELAXED);
}
static int pmm_in_reclaim(void) {
  return __atomic_load_n(&pmm_reclaim_depth, __ATOMIC_RELAXED) > 0;
}

/* Variant B — background reclaim (kswapd). Reactive reclaim runs ON the
 * allocation hot path: a task that hits free==0 stalls while it evicts pages
 * synchronously. kswapd moves that work into a dedicated kernel thread that
 * keeps a HIGH-watermark of free frames ready, so ordinary allocations almost
 * always hit the fast path and a bursty allocator (clang's per-TU heap) finds
 * headroom instead of cliff-diving into synchronous reclaim. kswapd runs in a
 * top-level (no heap_lock/vmm_lock held) context, so it can safely write dirty
 * pages back; it still raises pmm_reclaim_depth so its own eviction's nested
 * allocs take the clean-only path. It blocks on kswapd_chan and is woken
 * whenever an allocation has to fall into reactive reclaim (free crossed the
 * low watermark), with a periodic timeout as a missed-wakeup safety net. */
static char kswapd_chan;

static usize kswapd_high_watermark(void) {
  usize total = (usize)(pmm.total_usable / PAGE_SIZE);
  usize high = total / 16;        /* ~6% of RAM kept free */
  if (high < 512) high = 512;     /* 2 MiB floor on tiny guests */
  if (high > 16384) high = 16384; /* 64 MiB cap so big guests don't over-reserve */
  return high;
}

static void pmm_scrub_quarantine(void);

static void kswapd_thread(void *arg) {
  (void)arg;
  for (;;) {
    /* Idle-time quarantine scrub: age freed pages out of quarantine and into
     * the pre-zeroed pool, catching UAF/OOB poison corruption. Runs before
     * reclaim so promotion never competes with it for this iteration. */
    pmm_scrub_quarantine();
    usize high = kswapd_high_watermark();
    /* Reclaim toward the high watermark in bounded batches, yielding between
     * them so kswapd never monopolises a CPU. Stop when nothing more is
     * reclaimable (all cache referenced / dirty-unflushable). */
    while ((usize)pmm.free_frames < high) {
      usize deficit = high - (usize)pmm.free_frames;
      usize batch = deficit < 256 ? deficit : 256;
      pmm_enter_reclaim();
      usize ev = page_cache_evict(batch);
      pmm_leave_reclaim();
      if (ev == 0)
        break;
      scheduler_yield();
    }
    /* Sleep until an allocation wakes us on low free, or 500 ms elapses
     * (TIMER_HZ=100 -> 50 ticks) so slow growth is still trimmed. */
    scheduler_block_on_timeout(&kswapd_chan, 50);
  }
}

static void kswapd_wake(void) { scheduler_wake_all(&kswapd_chan); }

void kswapd_init(void) {
  kthread_create("kswapd", kswapd_thread, 0);
}

#include <b1nix/lockdep.h>
static inline void pmm_acquire(u64 *flags) {
  spin_lock_irqsave(&pmm_lock, flags);
  LOCKDEP_ACQUIRE(LOCKDEP_LVL_PMM);
}
static inline void pmm_release(u64 flags) {
  LOCKDEP_RELEASE(LOCKDEP_LVL_PMM);
  spin_unlock_irqrestore(&pmm_lock, flags);
}

/* Runtime size of the kernel's direct map (see mm.h). Starts at the
 * compile-time ceiling so any pre-pmm_init reference is a safe over-estimate;
 * pmm_init shrinks it to the actual top-of-RAM clamped into [MIN, MAX]. */
u64 g_direct_map_size = DIRECT_MAP_MAX;

static void m26_diag_task(void) {
  if (current_task && current_task->name) {
    console_write(" task=");
    console_write(current_task->name);
    console_write(" pid=");
    console_write_dec(current_task->id);
  }
}

static int is_power_of_two_u64(u64 value) {
  return value && ((value & (value - 1)) == 0);
}

static usize pmm_reclaim_target_frames(usize count, u64 free_snapshot) {
  usize needed = count;
  if (free_snapshot < count)
    needed = count - (usize)free_snapshot;

  usize total_frames = (usize)(pmm.total_usable / PAGE_SIZE);
  usize low_watermark = total_frames / 512;
  if (low_watermark < count)
    low_watermark = count;

  if (free_snapshot < low_watermark) {
    usize watermark_needed = low_watermark - (usize)free_snapshot;
    if (watermark_needed > needed)
      needed = watermark_needed;
  }

  return needed ? needed : 1;
}

static u64 align_up_u64(u64 value, u64 alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

static u64 align_down_u64(u64 value, u64 alignment) {
  return value & ~(alignment - 1);
}

static usize frame_index(u64 frame) { return (usize)(frame / PAGE_SIZE); }

static u64 frame_from_index(usize index) { return (u64)index * PAGE_SIZE; }

static int bitmap_get(usize index) {
  return (pmm.bitmap[index / BITS_PER_BYTE] &
          (1u << (index % BITS_PER_BYTE))) != 0;
}

static void bitmap_set(usize index) {
  pmm.bitmap[index / BITS_PER_BYTE] |= (u8)(1u << (index % BITS_PER_BYTE));
}

static void bitmap_clear(usize index) {
  pmm.bitmap[index / BITS_PER_BYTE] &= (u8) ~(1u << (index % BITS_PER_BYTE));
}

/* ─────────────────────────── Buddy allocator ─────────────────────────────
 * The intrusive LIFO free stack and the O(N) bitmap scan for contiguous runs
 * are replaced by a classic buddy allocator:
 *   - free blocks of order k (2^k pages) hang off free_area[k] as a doubly
 *     linked list; the block's first page (via the direct map) stores next /
 *     prev frame links, its own order and a magic canary.
 *   - alloc pops the smallest suitable block, splitting higher-order blocks as
 *     needed; a request for `count` pages that is not a power of two gets the
 *     block's excess tail pages released back to the tree.
 *   - free returns the block, then merges it with its buddy (frame XOR block
 *     size) while the buddy is also a free block of the same order. Merging
 *     always yields a correctly aligned, larger block (buddy pairs share an
 *     aligned boundary by construction).
 * The used-bitmap stays the authoritative "free vs used" source for reclaim,
 * watermarks and free_frames; the buddy lists are a consistent cache of free
 * runs on top of it, kept non-stale by reseeding whenever the bitmap-scan
 * fallback claims pages behind the tree's back. "Is this block free?" is then
 * a single head-bit + header check (buddy_is_free_block) rather than a walk
 * over the block's pages — which matters because merging tests one block per
 * order and a per-page walk made a high-order merge chain O(2^k). A
 * magic/order mismatch in a block header is treated as corruption: the list is
 * truncated and the caller falls back to the bitmap scan, so no free memory is
 * ever lost (same graceful-degradation guarantee the old free stack had).
 * ──────────────────────────────────────────────────────────────────────── */
#define BUDDY_MAX_ORDER 18   /* 2^18 pages = 1 GiB largest free block */
#define BUDDY_MAGIC 0xB000500ULL

struct buddy_block {
  u64 next;   /* frame of the next free block, or 0 */
  u64 prev;   /* frame of the previous free block, or 0 */
  u64 order;  /* this block's order */
  u64 magic;
};

static u64 free_area[BUDDY_MAX_ORDER + 1]; /* head frame of each order's list */
static int buddy_ready;

/* Shared zero page for zero-page dedup: one reserved physical frame mapped
 * read-only (+COW) into every fresh anonymous page's PTE until first write.
 * Allocated in pmm_switch_to_direct_map, freed never. */
static u64 zero_page_frame;

static usize buddy_pages(int order) { return (usize)1 << order; }
static u64 buddy_bytes(int order) { return (u64)buddy_pages(order) * PAGE_SIZE; }

static struct buddy_block *buddy_hdr(u64 frame) {
  return (struct buddy_block *)(usize)(frame + DIRECT_MAP_BASE);
}

/* A block is a valid free-node site only if it is block-aligned, within usable
 * RAM and reachable through the direct map (its first page holds the intrusive
 * list header). Validating before dereference turns a corrupt free-list entry
 * into a logged, survivable event instead of a fatal GP fault. */
static int buddy_region_valid(u64 frame, int order) {
  if (order < 0 || order > BUDDY_MAX_ORDER) return 0;
  u64 bytes = buddy_bytes(order);
  if (frame & (bytes - 1)) return 0;             /* must be block-aligned */
  if (frame + bytes > pmm.max_address) return 0;
  if (frame + bytes > g_direct_map_size) return 0;
  if (frame_index(frame) + buddy_pages(order) >
      (usize)pmm.bitmap_bytes * BITS_PER_BYTE) return 0;
  return 1;
}

static int head_get(usize index) {
  return (pmm.buddy_heads[index / BITS_PER_BYTE] &
          (1u << (index % BITS_PER_BYTE))) != 0;
}

static void head_set(usize index) {
  pmm.buddy_heads[index / BITS_PER_BYTE] |= (u8)(1u << (index % BITS_PER_BYTE));
}

static void head_clear(usize index) {
  pmm.buddy_heads[index / BITS_PER_BYTE] &=
      (u8) ~(1u << (index % BITS_PER_BYTE));
}

static void buddy_insert(u64 frame, int order) {
  struct buddy_block *b = buddy_hdr(frame);
  b->order = (u64)order;
  b->magic = BUDDY_MAGIC;
  b->next = free_area[order];
  b->prev = 0;
  if (b->next) buddy_hdr(b->next)->prev = frame;
  free_area[order] = frame;
  head_set(frame_index(frame));
}

static void buddy_unlink(u64 frame, int order) {
  struct buddy_block *b = buddy_hdr(frame);
  if (b->prev) buddy_hdr(b->prev)->next = b->next;
  else if (free_area[order] == frame) free_area[order] = b->next;
  if (b->next) buddy_hdr(b->next)->prev = b->prev;
  b->next = b->prev = 0;
  head_clear(frame_index(frame));
}

/* Is `frame` the head of a live order-k block in the tree? O(1): the head-bit
 * map is maintained by buddy_insert/buddy_unlink, so no per-page bitmap walk is
 * needed (that walk made every merge O(2^k) — up to ~500K bit tests under
 * pmm_lock when a high-order chain merged). The header magic/order and the
 * head page's used-bit are cross-checked as a cheap corruption net: the
 * bitmap-scan fallback can claim pages out from under the tree, which is why it
 * reseeds the tree afterwards (see bitmap_scan_alloc). Caller holds pmm_lock. */
static int buddy_is_free_block(u64 frame, int order) {
  if (!buddy_region_valid(frame, order)) return 0;
  usize idx = frame_index(frame);
  if (!head_get(idx) || bitmap_get(idx)) return 0;
  const struct buddy_block *b = buddy_hdr(frame);
  return b->magic == BUDDY_MAGIC && b->order == (u64)order;
}

/* Merge `frame`'s order-k block with a free same-order buddy while possible,
 * then insert it. Does NOT touch the bitmap — the caller has already marked
 * the block's pages free. Idempotent: a frame that is already linked (a
 * double free reaching the drain fallback) is left alone rather than linked
 * twice, which would cycle the list. Caller holds pmm_lock. */
static void buddy_merge_insert(u64 frame, int order) {
  if (!buddy_region_valid(frame, order)) return;
  if (head_get(frame_index(frame))) return; /* already in the tree */
  while (order < BUDDY_MAX_ORDER) {
    u64 buddy = frame ^ buddy_bytes(order);
    if (!buddy_is_free_block(buddy, order)) break;
    buddy_unlink(buddy, order);
    if (buddy < frame) frame = buddy;
    order++;
  }
  buddy_insert(frame, order);
}

/* Mark an order-k block's pages free in the bitmap/refcounts and return it to
 * the tree. Returns 0 on success, -1 if any page was already free (double
 * free / corrupt state — nothing is modified). Caller holds pmm_lock. */
static int buddy_release(u64 frame, int order) {
  if (!buddy_region_valid(frame, order)) return -1;
  usize idx = frame_index(frame);
  usize npages = buddy_pages(order);
  for (usize i = 0; i < npages; i++)
    if (!bitmap_get(idx + i)) return -1;  /* page already free (double free) */
  for (usize i = 0; i < npages; i++) {
    bitmap_clear(idx + i);
    if (pmm.frame_refcounts) pmm.frame_refcounts[idx + i] = 0;
  }
  pmm.free_frames += npages;
  buddy_merge_insert(frame, order);
  return 0;
}

/* Return one frame from a per-CPU bucket to the global tree. buddy_release
 * handles the normal case; it refuses a frame whose bitmap bit is already
 * clear (a double free that slipped past the marker check), and then we still
 * put the frame back on the tree — buddy_merge_insert is idempotent, so a
 * frame that is already linked is left alone instead of cycling the list.
 * Caller holds pmm_lock. */
static void pmm_return_frame(u64 frame) {
  if (buddy_release(frame, 0) == 0) return;
  usize idx = frame_index(frame);
  if (bitmap_get(idx)) {
    bitmap_clear(idx);
    pmm.free_frames++;
  }
  if (pmm.frame_refcounts) pmm.frame_refcounts[idx] = 0;
  buddy_merge_insert(frame, 0);
}

/* Allocate a block of `order` pages, splitting higher-order blocks as needed.
 * Claims the pages (bitmap set, refcount=1, free_frames decremented). Returns
 * the block's base frame, or 0 if no block is available. On free-list
 * corruption the offending list is truncated (the bitmap is authoritative) and
 * 0 is returned so the caller can fall back to the bitmap scan. Caller holds
 * pmm_lock. */
static u64 buddy_alloc(int order) {
  if (order < 0 || order > BUDDY_MAX_ORDER) return 0;
  for (int k = order; k <= BUDDY_MAX_ORDER; k++) {
    /* Pop each order's list head. A block whose header is corrupt truncates
     * the list — the bitmap is authoritative, so the caller's bitmap-scan
     * fallback recovers every free page and nothing is lost. The head check is
     * O(1) (head-bit + header), so this costs nothing on the hot path. */
    while (free_area[k] != 0) {
      u64 frame = free_area[k];
      if (!buddy_is_free_block(frame, k)) {
        static int reported;
        if (!reported) {
          reported = 1;
          console_write("pmm: buddy free-list corruption at order ");
          console_write_dec(k);
          console_write(" — truncating; bitmap scan recovers free frames\n");
        }
        head_clear(frame_index(frame));
        free_area[k] = 0;
        break;
      }
      buddy_unlink(frame, k);
      while (k > order) {
        k--;
        buddy_insert(frame + buddy_bytes(k), k); /* top half becomes free */
      }
      usize idx = frame_index(frame);
      usize npages = buddy_pages(order);
      for (usize i = 0; i < npages; i++) {
        bitmap_set(idx + i);
        if (pmm.frame_refcounts) pmm.frame_refcounts[idx + i] = 1;
      }
      pmm.free_frames -= npages;
      return frame;
    }
  }
  return 0;
}

/* Rebuild the whole tree from the authoritative bitmap: drop every list, then
 * decompose each contiguous free run into maximal aligned blocks (largest
 * order first) — the canonical buddy representation. Whole all-used 64-bit
 * bitmap words are skipped, which for typical layouts (low memory, the kernel,
 * the bitmap/refcount pages) cuts the walk a lot at large RAM under TCG.
 *
 * Used once at direct-map switch-over to seed the tree, and again after the
 * bitmap-scan fallback claims pages — the scan claims arbitrary runs straight
 * out of the bitmap and can orphan blocks the tree still links, and reseeding
 * is what lets every other path assume the tree is never stale (which is what
 * makes the O(1) buddy_is_free_block test sound). Caller holds pmm_lock. */
static void buddy_seed_from_bitmap(void) {
  for (int i = 0; i <= BUDDY_MAX_ORDER; i++) free_area[i] = 0;
  memset(pmm.buddy_heads, 0, pmm.bitmap_bytes);

  usize frame_count = (usize)(pmm.max_address / PAGE_SIZE);
  const u64 *words = (const u64 *)pmm.bitmap;
  usize idx = 0;
  while (idx < frame_count) {
    if ((idx & 63) == 0 && idx + 64 <= frame_count && words[idx / 64] == ~0ULL) {
      idx += 64;
      continue;
    }
    if (bitmap_get(idx)) {
      idx++;
      continue;
    }
    usize end = idx + 1;
    while (end < frame_count && !bitmap_get(end)) end++;
    usize pos = idx;
    while (pos < end) {
      int order = 0;
      while (order < BUDDY_MAX_ORDER) {
        usize pages = (usize)1 << (order + 1);
        if (pos + pages > end) break;
        if (pos & (pages - 1)) break;
        order++;
      }
      buddy_insert(frame_from_index(pos), order);
      pos += (usize)1 << order;
    }
    idx = end;
  }
}

static void mark_frame_free(u64 frame) {
  usize index = frame_index(frame);

  if (bitmap_get(index)) {
    bitmap_clear(index);
    pmm.free_frames++;
    if (buddy_ready) {
      buddy_merge_insert(frame, 0);
    }
  }
}

/* Bulk version of mark_frame_free for pmm_init's "mark whole region free"
 * walk. The naive per-frame loop dominated boot under TCG with large RAM
 * (~2M iterations for 8 GB ⇒ tens of seconds before the shell appeared).
 * Pre-condition: every bitmap bit in [start_idx, end_idx) is currently SET
 * (the bitmap was just memset(0xff)'d in pmm_init); the bulk clear is the
 * inverse — memset(0) for the byte-aligned interior, bit-clear at the edges.
 * Caller may not depend on buddy_ready here (it is false during init). */
static void mark_frames_free_range(usize start_idx, usize end_idx) {
  if (end_idx <= start_idx) return;
  usize count = end_idx - start_idx;
  /* Leading partial byte */
  while (start_idx < end_idx && (start_idx & 7)) {
    bitmap_clear(start_idx++);
  }
  /* Aligned middle bytes — single memset, the fast path */
  usize byte_start = start_idx >> 3;
  usize byte_end = end_idx >> 3;
  if (byte_end > byte_start) {
    memset(&pmm.bitmap[byte_start], 0, byte_end - byte_start);
    start_idx = byte_end << 3;
  }
  /* Trailing partial byte */
  while (start_idx < end_idx) {
    bitmap_clear(start_idx++);
  }
  pmm.free_frames += count;
}

static void mark_frame_used(u64 frame) {
  usize index = frame_index(frame);

  if (!bitmap_get(index)) {
    bitmap_set(index);
    pmm.free_frames--;
  }
}

static int region_contains(u64 base, u64 length, u64 address, u64 size) {
  return address >= base && address + size <= base + length;
}

static u64 find_early_mem(const struct boot_info *boot_info, usize size) {
  /* __kernel_end is a (possibly higher-half) virtual symbol; the PMM works in
   * physical addresses, so convert via KERNEL_VMA (0 on the identity-mapped
   * 32-bit port). */
  u64 search_addr =
      align_up_u64((u64)(usize)__kernel_end - KERNEL_VMA, PAGE_SIZE);

  for (usize i = 0; i < boot_info->memory_region_count; i++) {
    const struct boot_memory_region *region = &boot_info->memory_regions[i];

    if (region->type != BOOT_MEMORY_AVAILABLE) {
      continue;
    }

    u64 start = align_up_u64(region->base, PAGE_SIZE);
    u64 end = align_down_u64(region->base + region->length, PAGE_SIZE);

    if (search_addr < start) {
      search_addr = start;
    }

    if (boot_info->has_ramdisk) {
      u64 rd_start = align_down_u64(boot_info->ramdisk_addr, PAGE_SIZE);
      u64 rd_end = align_up_u64(boot_info->ramdisk_addr + boot_info->ramdisk_size, PAGE_SIZE);
      if (!(search_addr + size <= rd_start || search_addr >= rd_end)) {
        search_addr = rd_end;
      }
    }

    if (search_addr + size > 0x100000000ULL) {
      continue;
    }

    if (region_contains(start, end - start, search_addr,
                        align_up_u64(size, PAGE_SIZE))) {
      return search_addr;
    }
  }

  panic("no space for early physical memory allocation");
}


/* Size the direct map to actual hardware: walk usable regions, take the top
 * end, align UP to a 2 MiB boundary (vmm_init maps in 2 MiB hugepages), then
 * clamp into [DIRECT_MAP_MIN, DIRECT_MAP_MAX]. Must run before any code
 * (including the rest of pmm_init) reads DIRECT_MAP_SIZE. */
static void size_direct_map(const struct boot_info *boot_info) {
  u64 top = 0;
  for (usize i = 0; i < boot_info->memory_region_count; i++) {
    const struct boot_memory_region *r = &boot_info->memory_regions[i];
    if (r->type != BOOT_MEMORY_AVAILABLE) continue;
    u64 end = r->base + r->length;
    if (end > top) top = end;
  }
  /* 2 MiB hugepage alignment so the vmm_init loop covers the whole region. */
  u64 huge = 0x200000ULL;
  top = (top + huge - 1ULL) & ~(huge - 1ULL);
  if (top < DIRECT_MAP_MIN) top = DIRECT_MAP_MIN;
  if (top > DIRECT_MAP_MAX) top = DIRECT_MAP_MAX;
  g_direct_map_size = top;
  console_write("pmm: direct map sized to 0x");
  console_write_hex64(g_direct_map_size);
  console_write(" (");
  console_write_dec(g_direct_map_size / (1024ULL * 1024ULL));
  console_write(" MiB)\n");
}

void pmm_init(const struct boot_info *boot_info) {
  /* Convert the (higher-half) kernel symbols to physical addresses; the two
   * reservation loops below then cover [0, kernel_end_phys). KERNEL_VMA is 0 on
   * the identity-mapped 32-bit port, so this is a no-op there. */
  pmm.kernel_start = (u64)(usize)__kernel_start - KERNEL_VMA;
  pmm.kernel_end = align_up_u64((u64)(usize)__kernel_end - KERNEL_VMA, PAGE_SIZE);
  pmm.max_address = 0;
  pmm.total_usable = 0;
  pmm.phys_total = 0;
  pmm.free_frames = 0;

  /* Compute the runtime direct-map size before the loops below consult it. */
  size_direct_map(boot_info);

  for (usize i = 0; i < boot_info->memory_region_count; i++) {
    const struct boot_memory_region *region = &boot_info->memory_regions[i];
    if (region->type != BOOT_MEMORY_AVAILABLE) {
      continue;
    }

    /* Installed RAM as the firmware reports it — uncapped, for display only.
     * The kernel can only *use* what fits in the direct map (clamped below),
     * but a 32-bit box with 16 GiB should still report 16 GiB, not the 1 GiB
     * direct-map ceiling. */
    pmm.phys_total += region->length;

    u64 end = align_down_u64(region->base + region->length, PAGE_SIZE);

    /* Only memory reachable through the direct map is usable: the kernel
     * dereferences any frame as (frame + DIRECT_MAP_BASE), and that mapping
     * exists only for [0, DIRECT_MAP_SIZE). RAM above the limit is dropped. */
    if (end > DIRECT_MAP_SIZE) {
      end = DIRECT_MAP_SIZE;
    }

    if (end > pmm.max_address) {
      pmm.max_address = end;
    }
  }

  usize frame_count = (usize)(pmm.max_address / PAGE_SIZE);
  pmm.bitmap_bytes = align_up_u64((frame_count + 7) / 8, PAGE_SIZE);
  usize refcounts_bytes = align_up_u64(frame_count * sizeof(u16), PAGE_SIZE);

  for (int i = 0; i <= BUDDY_MAX_ORDER; i++) free_area[i] = 0;
  buddy_ready = 0;

  u64 early_mem =
      find_early_mem(boot_info, pmm.bitmap_bytes * 2 + refcounts_bytes);
  pmm.bitmap = (u8 *)(usize)early_mem;
  pmm.buddy_heads = (u8 *)(usize)(early_mem + pmm.bitmap_bytes);
  pmm.frame_refcounts = (u16 *)(usize)(early_mem + pmm.bitmap_bytes * 2);

  memset(pmm.bitmap, 0xff, pmm.bitmap_bytes);
  memset(pmm.buddy_heads, 0, pmm.bitmap_bytes);
  memset(pmm.frame_refcounts, 0, refcounts_bytes);

  console_write("pmm: kernel 0x");
  console_write_hex64(pmm.kernel_start);
  console_write("-0x");
  console_write_hex64(pmm.kernel_end);
  console_write("\n");

  for (usize i = 0; i < boot_info->memory_region_count; i++) {
    const struct boot_memory_region *region = &boot_info->memory_regions[i];

    if (region->type != BOOT_MEMORY_AVAILABLE) {
      continue;
    }

    u64 start = align_up_u64(region->base, PAGE_SIZE);
    u64 end = align_down_u64(region->base + region->length, PAGE_SIZE);

    /* Drop RAM above the direct-map limit (see the max_address loop above). */
    if (end > DIRECT_MAP_SIZE) {
      end = DIRECT_MAP_SIZE;
    }

    if (end <= start) {
      continue;
    }

    pmm.total_usable += end - start;

    /* Bulk-free the whole region. Replaces a per-frame loop that ran ~2M
     * times for 8 GB — under TCG that walk added tens of seconds to the
     * boot-to-shell time, making "more RAM → slower boot" feel pathological. */
    mark_frames_free_range(start / PAGE_SIZE, end / PAGE_SIZE);

    console_write("pmm: usable 0x");
    console_write_hex64(start);
    console_write("-0x");
    console_write_hex64(end);
    console_write("\n");
  }

  for (u64 frame = pmm.kernel_start; frame < pmm.kernel_end;
       frame += PAGE_SIZE) {
    mark_frame_used(frame);
  }

  for (u64 frame = 0; frame < pmm.kernel_start; frame += PAGE_SIZE) {
    mark_frame_used(frame);
  }

  for (u64 frame = (u64)(usize)pmm.bitmap;
       frame < (u64)(usize)pmm.bitmap + pmm.bitmap_bytes; frame += PAGE_SIZE) {
    mark_frame_used(frame);
  }

  for (u64 frame = (u64)(usize)pmm.buddy_heads;
       frame < (u64)(usize)pmm.buddy_heads + pmm.bitmap_bytes;
       frame += PAGE_SIZE) {
    mark_frame_used(frame);
  }

  for (u64 frame = (u64)(usize)pmm.frame_refcounts;
       frame < (u64)(usize)pmm.frame_refcounts + refcounts_bytes; frame += PAGE_SIZE) {
    mark_frame_used(frame);
  }

  if (boot_info->has_ramdisk) {
    u64 start_frame = align_down_u64(boot_info->ramdisk_addr, PAGE_SIZE);
    u64 end_frame = align_up_u64(boot_info->ramdisk_addr + boot_info->ramdisk_size, PAGE_SIZE);
    for (u64 frame = start_frame; frame < end_frame; frame += PAGE_SIZE) {
      mark_frame_used(frame);
    }
  }

  console_write("pmm: bitmap 0x");
  console_write_hex64((u64)(usize)pmm.bitmap);
  console_write("-0x");
  console_write_hex64((u64)(usize)pmm.bitmap + pmm.bitmap_bytes);
  console_write("\n");

  console_write("pmm: total usable bytes 0x");
  console_write_hex64(pmm.total_usable);
  console_write("\n");

  /* One-line MiB summary so a real-hardware screen unambiguously shows where
   * the numbers come from: firmware-reported RAM (sum of e820 AVAILABLE),
   * what the kernel can actually use (clamped to the direct map), and the
   * direct-map ceiling. If "firmware" > the box's physical RAM, the BIOS e820
   * is over-reporting (e.g. shared-GPU/remapped regions) — the kernel still
   * only ever hands out frames it freed from real regions. */
  console_write("pmm: firmware RAM ");
  console_write_dec(pmm.phys_total / (1024 * 1024));
  console_write(" MiB, usable ");
  console_write_dec(pmm.total_usable / (1024 * 1024));
  console_write(" MiB, direct-map cap ");
  console_write_dec(DIRECT_MAP_SIZE / (1024 * 1024));
  console_write(" MiB\n");
}

void pmm_ref_frame(u64 frame) {
  u64 flags;
  pmm_acquire(&flags);
  usize idx = frame / PAGE_SIZE;
  if (pmm.frame_refcounts) {
    pmm.frame_refcounts[idx]++;
  }
  pmm_release(flags);
}

/* ─────────────────────────── Per-CPU PMM cache (PCP) ───────────────────────
 *
 * Single-frame alloc/free is the dominant path under any SMP build (fork,
 * mmap, kheap growth — all of it). Going through the global pmm_lock for
 * every page makes that path serialize across all CPUs: with 8 vCPUs +
 * make -j8 the lock-acquire dominated wall-clock, and the per-RAM watermark
 * (low_watermark = total_frames / 512) made bigger guests *slower* because
 * a single reclaim batch held both pmm_lock and pc_lock for thousands of
 * frames at a time.
 *
 * Same trick Linux uses (per-CPU pageset, "PCP") and FreeBSD uses (UMA
 * per-CPU buckets): each CPU keeps a small private intrusive stack of
 * recently-freed-but-not-redistributed frames. alloc pops local, free
 * pushes local — IRQs disabled to pin to the current CPU, no global lock.
 * When local empties we refill a batch from global; when local overflows
 * we drain a batch back. The global lock is hit ~1/PCP_REFILL times.
 *
 * Bitmap invariant: a frame sitting in *any* per-CPU cache is bitmap-USED
 * (refilling claims it via bitmap_set); pmm.free_frames counts only the
 * *globally-free* stash. Free-frame consumers (page-cache evict, reclaim
 * watermark) therefore see a slightly stale view that ignores cache
 * inventory — fine in steady state; under hard pressure we drain caches
 * before declaring OOM.
 * ──────────────────────────────────────────────────────────────────────── */
/* Each CPU keeps three private buckets:
 *   - head/count: the plain free stack (frames to be zeroed by the caller).
 *   - zero_head/zero_count: pre-zeroed frames, returned without a memset.
 *   - q_head/q_tail/q_count: the QUARANTINE queue. Every freed page lands here
 *     FIRST, poisoned with a canary pattern, and only becomes reallocatable
 *     after the kswapd scrubber ages it out (or the bucket overflows). UAF
 *     reads see poison, double-frees trip the canary check, and OOB writes
 *     into a parked page are reported by the scrubber — instead of the old
 *     silent "GP fault in freelist_pop" corruption.
 * The quarantine is a FIFO (q_tail) so the head is always the OLDEST victim:
 * aging "N allocs" is implemented by only promoting the head once Q_AGE more
 * frames have piled up behind it. */
struct pmm_pcp {
  u64 head;       /* intrusive stack head, same on-frame link layout as global */
  u32 count;      /* number of frames currently parked */
  u32 _pad;
  u64 zero_head;  /* intrusive stack head of PRE-ZEROED frames (zeroed by scrubber) */
  u32 zero_count; /* number of pre-zeroed frames parked */
  u32 _pad2;
  u64 q_head;     /* oldest quarantined frame, or 0 (promote from here) */
  u64 q_tail;     /* newest quarantined frame, or 0 (new frees go here) */
  u32 q_count;    /* number of frames currently quarantined */
  u32 _pad3;
} __attribute__((aligned(64)));

#define PMM_PCP_LIMIT    128  /* drain to global once cache exceeds */
#define PMM_PCP_REFILL    32  /* pull this many on miss */
#define PMM_PCP_ZERO_LIMIT 64 /* zeroed-bucket cap; overflow drains to global */
#define PMM_PCP_ZERO_DRAIN 32
#define PMM_PCP_Q_LIMIT   32  /* quarantine bucket cap per CPU */
#define PMM_PCP_Q_AGE     16  /* promote head once Q_AGE+1 frees piled behind it */
#define PMM_PCP_Q_SCRUB    8  /* max promotions per scrubber pass */

/* Marker layout inside a parked frame (frame + DIRECT_MAP_BASE):
 *   [0,  8)  intrusive list link (next frame or FIFO tail)
 *   [8, 16)  PMM_POISON_CANARY — double-free trips this on a second free
 *   [16,24)  PMM_ZERO_MAGIC — set while parked in the zero bucket, so freeing
 *            a page that is still parked there is also caught
 *   [24,4096) filled with PMM_POISON_BYTE — only when poisoning is enabled
 * Both markers are cleared by the memset every allocator hands a page out with,
 * so a legitimately-owned frame can never carry them.
 *
 * The full-page poison fill is a DEBUG facility, off by default: it costs a
 * 4 KiB write in the free path on top of the scrubber's zeroing, i.e. two page
 * writes per page instead of one, which cancels out the pre-zeroed pool's whole
 * reason to exist. Enabled with `b1nix.pmm-poison` on the kernel cmdline, it
 * makes UAF reads see 0xDF and lets the scrubber report OOB writes into parked
 * pages. The 8-byte markers are always written (one store, not a page fill), so
 * double-free detection works either way. */
#define PMM_POISON_BYTE       0xDF
#define PMM_POISON_CANARY     0xCAFE0DF00DFACEFEULL
#define PMM_ZERO_MAGIC        0x5A1DE4E4E4E45A11ULL
#define PMM_POISON_CANARY_OFF 8
#define PMM_ZERO_MAGIC_OFF    16

static struct pmm_pcp pmm_pcp[MAX_CPUS];
static int pmm_poison_enabled; /* set from b1nix.pmm-poison at direct-map switch */

static inline int pmm_pcp_ready(void) {
  if (!buddy_ready) return 0;
  struct percpu *p = get_percpu();
  return p && p->cpu_id < MAX_CPUS;
}

static inline u64 irq_save_cli(void) {
  return interrupts_save();
}
static inline void irq_restore(u64 f) {
  interrupts_restore(f);
}

static void pmm_pcp_refill(struct pmm_pcp *pcp) {
  u64 flags;
  pmm_acquire(&flags);
  for (int i = 0; i < PMM_PCP_REFILL && pcp->count < PMM_PCP_LIMIT; i++) {
    u64 frame = buddy_alloc(0);   /* claims bitmap + refcount=1 */
    if (frame == 0) break;
    *(u64 *)(usize)(frame + DIRECT_MAP_BASE) = pcp->head;
    pcp->head = frame;
    pcp->count++;
  }
  pmm_release(flags);
}

static void pmm_pcp_zero_drain(struct pmm_pcp *pcp) {
  u64 flags;
  pmm_acquire(&flags);
  for (int i = 0; i < PMM_PCP_ZERO_DRAIN && pcp->zero_count > 0; i++) {
    u64 frame = pcp->zero_head;
    pcp->zero_head = *(u64 *)(usize)(frame + DIRECT_MAP_BASE);
    pcp->zero_count--;
    pmm_return_frame(frame);
  }
  pmm_release(flags);
}

/* FIFO push/pop for the quarantine queue. The list link lives in the frame's
 * first 8 bytes (direct map); the poison canary at offset 8 and the zero
 * marker at offset 16 are never overwritten by these. Caller must hold IRQs
 * off (per-CPU ownership). */
static void q_push(struct pmm_pcp *pcp, u64 frame) {
  *(u64 *)(usize)(frame + DIRECT_MAP_BASE) = 0;
  if (pcp->q_tail) {
    *(u64 *)(usize)(pcp->q_tail + DIRECT_MAP_BASE) = frame;
  } else {
    pcp->q_head = frame;
  }
  pcp->q_tail = frame;
  pcp->q_count++;
}

static u64 q_pop(struct pmm_pcp *pcp) {
  if (!pcp->q_head) return 0;
  u64 frame = pcp->q_head;
  pcp->q_head = *(u64 *)(usize)(frame + DIRECT_MAP_BASE);
  if (!pcp->q_head) pcp->q_tail = 0;
  pcp->q_count--;
  return frame;
}

/* LIFO push onto the pre-zeroed stack (frame must already be all-zeros). */
static void zero_push(struct pmm_pcp *pcp, u64 frame) {
  *(u64 *)(usize)(frame + DIRECT_MAP_BASE) = pcp->zero_head;
  pcp->zero_head = frame;
  pcp->zero_count++;
}

static void pmm_pcp_drain_all(void) {
  for (int c = 0; c < MAX_CPUS; c++) {
    struct pmm_pcp *pcp = &pmm_pcp[c];
    if (pcp->count == 0 && pcp->zero_count == 0 && pcp->q_count == 0) continue;
    u64 flags;
    pmm_acquire(&flags);
    while (pcp->count > 0) {
      u64 frame = pcp->head;
      pcp->head = *(u64 *)(usize)(frame + DIRECT_MAP_BASE);
      pcp->count--;
      pmm_return_frame(frame);
    }
    while (pcp->q_count > 0) {
      u64 frame = q_pop(pcp);
      /* Quarantined pages are poisoned — zero them before they reach the
       * global pool so no one can read the poison through a fresh allocation
       * window. */
      memset((void *)(usize)(frame + DIRECT_MAP_BASE), 0, PAGE_SIZE);
      pmm_return_frame(frame);
    }
    while (pcp->zero_count > 0) {
      u64 frame = pcp->zero_head;
      pcp->zero_head = *(u64 *)(usize)(frame + DIRECT_MAP_BASE);
      pcp->zero_count--;
      pmm_return_frame(frame);
    }
    pmm_release(flags);
  }
}

/* Background quarantine scrubber. Runs from kswapd's idle time (every ~500 ms
 * loop) and handles ONLY the CPU it currently runs on: the bucket is owned by
 * that CPU under IRQs-off, so this is race-free with no lock. On every other
 * CPU aging is therefore driven purely by bucket overflow (Q_LIMIT) — kswapd's
 * own CPU is the only one that ages pages out on a timer. That is deliberate:
 * a cross-CPU scrub would need an IPI or a per-CPU thread, and overflow already
 * bounds how long a page can sit parked.
 *
 * Pages are aged out oldest-first once Q_AGE more frees have piled up behind
 * them (FIFO head = oldest), zeroed, and promoted to the zero bucket — never
 * released to global, so the "parked before reallocatable" guarantee holds
 * through the handover. This is also where the pre-zeroed pool earns its keep:
 * the zeroing happens on an idle CPU instead of in the allocation path.
 * With poisoning on, a page whose canary is broken was written into after free
 * (UAF/OOB) — reported, then recycled. */
static void pmm_scrub_quarantine(void) {
  if (!pmm_pcp_ready()) return;
  u64 flags = irq_save_cli();
  struct percpu *p = get_percpu();
  struct pmm_pcp *pcp = &pmm_pcp[p->cpu_id];
  int done = 0;
  while (pcp->q_count > PMM_PCP_Q_AGE && done < PMM_PCP_Q_SCRUB) {
    u64 frame = q_pop(pcp);
    u64 *marker = (u64 *)(usize)(frame + DIRECT_MAP_BASE);
    if (marker[PMM_POISON_CANARY_OFF / 8] != PMM_POISON_CANARY) {
      static unsigned corrupted;
      if (corrupted < 8) {
        console_write("pmm: quarantine canary corrupted at 0x");
        console_write_hex64(frame);
        console_write("\n");
        klog_warn("pmm: quarantine canary corrupted (UAF/OOB write into freed page)");
        corrupted++;
      }
    }
    /* The zeroing the pre-zeroed pool is built around — deliberately here, on
     * an idle CPU, and not in the free path. */
    memset((void *)(usize)(frame + DIRECT_MAP_BASE), 0, PAGE_SIZE);
    marker[PMM_ZERO_MAGIC_OFF / 8] = PMM_ZERO_MAGIC;
    if (pcp->zero_count >= PMM_PCP_ZERO_LIMIT) {
      pmm_pcp_zero_drain(pcp);
    }
    zero_push(pcp, frame);
    done++;
  }
  irq_restore(flags);
}

void pmm_free_frame(u64 frame) {
  if (zero_page_frame && frame == zero_page_frame) {
    /* Shared zero page: reserved for the kernel's whole lifetime (its single
     * allocation refcount is never released). Unmaps of read-only+COW
     * zero-page PTEs — from any address space — land here. */
    return;
  }
  if ((frame & (PAGE_SIZE - 1)) != 0 || frame >= pmm.max_address) {
    /* Out-of-range or misaligned frames reach here legitimately: unmapping a
     * shared/device mapping (e.g. the virtio-gpu framebuffer at ~0xfe000000,
     * which the device owns) hits a PTE whose frame is above usable RAM. We
     * correctly skip freeing it. Rate-limit the warning — a memory-heavy
     * process (Mesa) unmaps such pages constantly, and a per-call serial
     * klog_warn throttles it to a crawl. */
    static unsigned warned = 0;
    if (warned < 16)
      klog_warn("pmm_free_frame: invalid frame (skipped, device/shared?)");
    warned++;
    return;
  }
  usize idx = frame / PAGE_SIZE;

  /* Refcount step still goes through the global lock — it's a tiny
   * critical section and we need atomicity vs pmm_ref_frame for fork CoW. */
  u64 flags;
  pmm_acquire(&flags);
  int now_zero = 1;
  if (pmm.frame_refcounts) {
    if (pmm.frame_refcounts[idx] > 0) {
      pmm.frame_refcounts[idx]--;
    }
    now_zero = (pmm.frame_refcounts[idx] == 0);
  }
  pmm_release(flags);

  if (!now_zero) return;  /* still referenced elsewhere (CoW sibling) */

  /* Refcount hit 0 — park the frame in our per-CPU cache rather than
   * touching the global free list. Drain a batch back if the cache is
   * full so memory doesn't pile up on one CPU. */
  if (pmm_pcp_ready()) {
    u64 ifl = irq_save_cli();
    struct percpu *p = get_percpu();
    struct pmm_pcp *pcp = &pmm_pcp[p->cpu_id];

    /* Double-free check: a page that is still parked somewhere (quarantine or
     * zero bucket) carries its poison/zero marker. It is bitmap-USED and owned
     * by a bucket, so freeing it again would corrupt the lists — refuse the
     * free and report instead. Any genuinely owned frame has been memset by
     * the allocator, so its marker slots are 0. */
    u64 *marker = (u64 *)(usize)(frame + DIRECT_MAP_BASE);
    if (marker[PMM_POISON_CANARY_OFF / 8] == PMM_POISON_CANARY ||
        marker[PMM_ZERO_MAGIC_OFF / 8] == PMM_ZERO_MAGIC) {
      irq_restore(ifl);
      static unsigned df_reported;
      if (df_reported < 8) {
        console_write("pmm: double free detected at 0x");
        console_write_hex64(frame);
        console_write("\n");
        klog_warn("pmm_free_frame: double free (page already parked in a bucket)");
        df_reported++;
      }
      return;
    }

    /* Park in the quarantine queue, NOT the zero bucket: a freed page must age
     * out of quarantine before it is reallocatable, and the scrubber zeroes it
     * on the way out. Only the 8-byte canary is written here — the full-page
     * poison fill is debug-only (b1nix.pmm-poison), because paying a 4 KiB
     * write in the free path on top of the scrubber's zeroing would mean two
     * page writes per page where one is enough. */
    if (pmm_poison_enabled)
      memset((void *)(usize)(frame + DIRECT_MAP_BASE), PMM_POISON_BYTE, PAGE_SIZE);
    marker[PMM_POISON_CANARY_OFF / 8] = PMM_POISON_CANARY;
    q_push(pcp, frame);
    if (pcp->q_count > PMM_PCP_Q_LIMIT) {
      /* Bucket full — promote the OLDEST victim (the head) to the zero bucket
       * instead of releasing it to global: it stays withheld from allocation,
       * only its parking spot changes. */
      u64 oldest = q_pop(pcp);
      memset((void *)(usize)(oldest + DIRECT_MAP_BASE), 0, PAGE_SIZE);
      *(u64 *)(usize)(oldest + DIRECT_MAP_BASE + PMM_ZERO_MAGIC_OFF) =
          PMM_ZERO_MAGIC;
      if (pcp->zero_count >= PMM_PCP_ZERO_LIMIT) {
        pmm_pcp_zero_drain(pcp);
      }
      zero_push(pcp, oldest);
    }
    irq_restore(ifl);
    return;
  }

  /* No per-CPU cache yet (early boot) — fall back to the global free list. */
  pmm_acquire(&flags);
  mark_frame_free(frame);
  pmm_release(flags);
}

void pmm_unref_frame(u64 frame) {
  pmm_free_frame(frame);
}

u16 pmm_get_refcount(u64 frame) {
  u64 flags;
  pmm_acquire(&flags);
  usize idx = frame / PAGE_SIZE;
  u16 val = 0;
  if (pmm.frame_refcounts) {
    val = pmm.frame_refcounts[idx];
  }
  pmm_release(flags);
  return val;
}

u64 pmm_alloc_frame(void) {
  if (!pmm_pcp_ready()) return pmm_alloc_frames(1);

  /* Try the per-CPU cache. IRQs off to prevent migration mid-pop; no spinlock,
   * the cache belongs only to this CPU. */
  u64 flags = irq_save_cli();
  struct percpu *p = get_percpu();
  struct pmm_pcp *pcp = &pmm_pcp[p->cpu_id];

  /* Pre-zeroed bucket first: frames parked here were memset at free time, so
   * this fast path skips the per-allocation memset entirely — the zero-page
   * COW write path, calloc, and fresh anonymous pages all benefit. */
  if (pcp->zero_count > 0) {
    u64 frame = pcp->zero_head;
    pcp->zero_head = *(u64 *)(usize)(frame + DIRECT_MAP_BASE);
    pcp->zero_count--;
    /* Clear the "parked in zero bucket" marker so a later free of this page
     * is not mistaken for a double free. One store vs a full memset. */
    *(u64 *)(usize)(frame + DIRECT_MAP_BASE + PMM_ZERO_MAGIC_OFF) = 0;
    if (pmm.frame_refcounts) pmm.frame_refcounts[frame_index(frame)] = 1;
    irq_restore(flags);
    return frame;  /* already zeroed by the scrubber / overflow promotion */
  }

  if (pcp->count == 0) {
    /* Local empty — refill under global lock. pmm_pcp_refill itself does
     * pushfq/cli inside spin_lock_irqsave; we are already cli, so the
     * combined push/popf restores to "still cli" — correct. */
    pmm_pcp_refill(pcp);
  }
  if (pcp->count > 0) {
    u64 frame = pcp->head;
    pcp->head = *(u64 *)(usize)(frame + DIRECT_MAP_BASE);
    pcp->count--;
    /* Set refcount=1 (transfer ownership from cache to caller). The frame is
     * already bitmap-USED from refill time, so no bitmap touch needed. */
    if (pmm.frame_refcounts) pmm.frame_refcounts[frame_index(frame)] = 1;
    irq_restore(flags);
    /* Zero outside the cli window — memset can take a while and we don't
     * want to mask IRQs longer than necessary. */
    memset((void *)(usize)(frame + DIRECT_MAP_BASE), 0, PAGE_SIZE);
    return frame;
  }
  irq_restore(flags);

  /* Cache still empty after refill — global is truly out. Fall back to the
   * slow path which runs reclaim/swap. */
  return pmm_alloc_frames(1);
}

static void claim_frame(usize index) {
  mark_frame_used(frame_from_index(index));
  if (pmm.frame_refcounts) {
    pmm.frame_refcounts[index] = 1;
  }
}

static void zero_frames(u64 frame, usize count) {
  void *ptr = (void *)(usize)frame;
  if (direct_map_ready) {
    ptr = (void *)(usize)(frame + DIRECT_MAP_BASE);
  }
  memset(ptr, 0, count * PAGE_SIZE);
}

/* Find the first run of `count` free frames, skipping whole all-used 64-bit
 * bitmap words. Returns the run's base frame (claimed + zeroed), or 0. Used for
 * contiguous (count>1) requests and as the count==1 fallback before the free
 * list is built. Caller holds pmm_lock. */
static u64 bitmap_scan_alloc(usize count) {
  usize frame_count = (usize)(pmm.max_address / PAGE_SIZE);
  if (count == 0 || count > frame_count) {
    return 0;
  }

  const u64 *words = (const u64 *)pmm.bitmap;
  usize run = 0;
  usize run_start = 0;

  for (usize idx = 0; idx < frame_count;) {
    if ((idx % 64) == 0 && idx + 64 <= frame_count && words[idx / 64] == ~0ULL) {
      run = 0;
      idx += 64;
      continue;
    }
    if (!bitmap_get(idx)) {
      if (run == 0) {
        run_start = idx;
      }
      if (++run >= count) {
        for (usize j = 0; j < count; j++) {
          claim_frame(run_start + j);
        }
        /* The run was claimed straight out of the bitmap, so any tree block
         * overlapping it is now stale. Reseed so the tree stays a faithful
         * image of the bitmap — every other path depends on that (see
         * buddy_seed_from_bitmap). The scan is already O(N) and only runs as a
         * degraded fallback, so this is a constant-factor cost on a cold path,
         * not a new complexity class. */
        if (buddy_ready) buddy_seed_from_bitmap();
        u64 frame = frame_from_index(run_start);
        zero_frames(frame, count);
        return frame;
      }
    } else {
      run = 0;
    }
    idx++;
  }
  return 0;
}

/* Global pressure tracker. Per-call retry counters are blind to the real
 * thrash pattern: each pmm_alloc_frames returns after 1 reclaim, but hundreds
 * of distinct calls each need that reclaim — the system as a whole is in
 * sustained pressure even though no single call loops. So count cumulative
 * reclaim *attempts* (not "evictions per call"); reset on a sustained
 * stretch of no-reclaim allocs (low_pressure_run). Same intuition as Linux
 * PSI / systemd-oomd: the metric is "how often does reclaim happen",
 * normalized over a window of allocations. */
static u64 pmm_total_reclaims = 0;     /* cumulative reclaim iterations */
static u64 pmm_clean_alloc_streak = 0; /* successful allocs without reclaim */
static u64 pmm_warned_pressure = 0;    /* one-shot per pressure episode */
static int pmm_oom_reported = 0;       /* one-shot OOM diag per pressure episode */

/* M99: allocate `count` contiguous frames whose LAST byte is <= `limit`.
 *
 * This is the zone a bounce buffer comes from. A device whose address window is
 * narrower than the memory a caller happens to hold cannot be handed that
 * memory, and without an IOMMU the only remedy is to copy through a buffer the
 * device *can* reach — which first requires being able to ask for one. The
 * ordinary allocator has no notion of an address ceiling, so this walks the
 * authoritative used-bitmap (as bitmap_scan_alloc does) over the frames below
 * the ceiling and claims a run there, reseeding the buddy tree afterwards so it
 * stays a faithful image of the bitmap.
 *
 * O(limit / PAGE_SIZE) and only on the bounce path, which is by definition the
 * slow one. Returns 0 when no run below the ceiling is free. */
u64 pmm_alloc_frames_below(usize count, u64 limit) {
  if (count == 0 || limit < PAGE_SIZE)
    return 0;

  u64 flags;
  pmm_acquire(&flags);

  usize frame_count = (usize)(pmm.max_address / PAGE_SIZE);
  /* A frame is usable only if its whole page fits under the ceiling. */
  usize cap = (usize)((limit + 1) / PAGE_SIZE);
  if (cap < frame_count)
    frame_count = cap;

  usize run = 0, run_start = 0;
  u64 frame = 0;
  for (usize idx = 0; idx < frame_count; idx++) {
    if (bitmap_get(idx)) {
      run = 0;
      continue;
    }
    if (run == 0)
      run_start = idx;
    if (++run >= count) {
      for (usize j = 0; j < count; j++)
        claim_frame(run_start + j);
      if (buddy_ready)
        buddy_seed_from_bitmap();
      frame = frame_from_index(run_start);
      zero_frames(frame, count);
      break;
    }
  }

  pmm_release(flags);
  return frame;
}

u64 pmm_alloc_frames(usize count) {
  u64 flags;
  u64 reclaim_attempts = 0;
  int pcp_drained = 0;  /* one-shot: drain per-CPU caches at most once per call */

  /* Retry via an explicit loop, never by recursion. Under memory pressure the
   * recovery path can run many eviction rounds; a tail-recursive retry would
   * add a stack frame per round and overflow the 16KB kernel stack (a likely
   * cause of the in-guest-build wedge at low RAM). Each iteration either
   * satisfies the request, makes progress by freeing >=1 frame, or gives up. */
  for (;;) {
    u64 free_snapshot = 0;
    pmm_acquire(&flags);

    /* Single-frame fast path: pop an order-0 block off the buddy tree in
     * O(1) amortised. */
    if (count == 1 && buddy_ready) {
      u64 frame = buddy_alloc(0);
      if (frame != 0) {
        zero_frames(frame, 1);
        pmm_release(flags);
        if (reclaim_attempts == 0) {
          /* Healthy fast-path success. After a stretch of these, the
           * pressure episode is over — re-arm the warning. */
          if (++pmm_clean_alloc_streak >= 128) {
            pmm_warned_pressure = 0;
            pmm_oom_reported = 0;
            pmm_total_reclaims = 0;
            pmm_clean_alloc_streak = 0;
          }
        }
        return frame;
      }
      /* Buddy tree empty. If the bitmap still shows free frames, the tree
       * missed some (it must not): fall back to the authoritative scan. Skip
       * the scan when nothing is free so the OOM path stays O(1) instead of
       * O(N). */
      if (pmm.free_frames > 0) {
        frame = bitmap_scan_alloc(1);
        if (frame != 0) {
          pmm_release(flags);
          if (reclaim_attempts == 0) {
          /* Healthy fast-path success. After a stretch of these, the
           * pressure episode is over — re-arm the warning. */
          if (++pmm_clean_alloc_streak >= 128) {
            pmm_warned_pressure = 0;
            pmm_oom_reported = 0;
            pmm_total_reclaims = 0;
            pmm_clean_alloc_streak = 0;
          }
        }
          return frame;
        }
      }
    } else if (count > 1) {
      /* Contiguous multi-frame request: buddy alloc of the smallest power of
       * two covering `count`, then release the excess tail pages back to the
       * tree (they merge into it). */
      int order = 0;
      while (((usize)1 << order) < count) order++;
      u64 frame = buddy_ready ? buddy_alloc(order) : 0;
      if (frame != 0) {
        usize npages = (usize)1 << order;
        if (npages > count) {
          for (usize e = count; e < npages; e++) {
            usize eidx = frame_index(frame) + e;
            bitmap_clear(eidx);
            if (pmm.frame_refcounts) pmm.frame_refcounts[eidx] = 0;
            pmm.free_frames++;
            buddy_merge_insert(frame + e * PAGE_SIZE, 0);
          }
        }
        zero_frames(frame, count);
        pmm_release(flags);
        if (reclaim_attempts == 0) {
          /* Healthy fast-path success. After a stretch of these, the
           * pressure episode is over — re-arm the warning. */
          if (++pmm_clean_alloc_streak >= 128) {
            pmm_warned_pressure = 0;
            pmm_oom_reported = 0;
            pmm_total_reclaims = 0;
            pmm_clean_alloc_streak = 0;
          }
        }
        return frame;
      }
      if (pmm.free_frames > 0) {
        frame = bitmap_scan_alloc(count);
        if (frame != 0) {
          pmm_release(flags);
          if (reclaim_attempts == 0) {
          /* Healthy fast-path success. After a stretch of these, the
           * pressure episode is over — re-arm the warning. */
          if (++pmm_clean_alloc_streak >= 128) {
            pmm_warned_pressure = 0;
            pmm_oom_reported = 0;
            pmm_total_reclaims = 0;
            pmm_clean_alloc_streak = 0;
          }
        }
          return frame;
        }
      }
    } else {
      /* count == 1 before the buddy tree exists (early boot): the bitmap scan
       * is the only allocator. */
      u64 frame = bitmap_scan_alloc(1);
      if (frame != 0) {
        pmm_release(flags);
        if (reclaim_attempts == 0) {
          /* Healthy fast-path success. After a stretch of these, the
           * pressure episode is over — re-arm the warning. */
          if (++pmm_clean_alloc_streak >= 128) {
            pmm_warned_pressure = 0;
            pmm_oom_reported = 0;
            pmm_total_reclaims = 0;
            pmm_clean_alloc_streak = 0;
          }
        }
        return frame;
      }
    }

    free_snapshot = pmm.free_frames;
    pmm_release(flags);

    // OOM. Reclaim and retry. Try evicting clean page-cache pages first.
    reclaim_attempts++;
    if (bootinfo_has_flag("b1nix.debug.heap") && (reclaim_attempts <= 16 || is_power_of_two_u64(reclaim_attempts))) {
      console_write("[M26DIAG] pmm_reclaim attempt=");
      console_write_dec(reclaim_attempts);
      console_write(" count=");
      console_write_dec(count);
      console_write(" free_frames=");
      console_write_dec(free_snapshot);
      m26_diag_task();
      console_write("\n");
    }
    /* This call needed reclaim — break any clean-streak. */
    pmm_clean_alloc_streak = 0;
    /* Pressure: nudge kswapd to refill the free pool in the background for the
     * following allocations. Cheap no-op if it is already running or not yet
     * created; skipped when this is kswapd's own nested allocation. */
    if (!pmm_in_reclaim())
      kswapd_wake();
    pmm_total_reclaims++;
    enum { PMM_PRESSURE_WARN = 32 };
    if (pmm_total_reclaims == PMM_PRESSURE_WARN && !pmm_warned_pressure) {
      pmm_warned_pressure = 1;
      klog_warn("pmm: memory pressure — 32 cumulative reclaim cycles "
                "(too many parallel tasks for available RAM; suggest "
                "fewer make -j N jobs, more memory, or enable swap)");
    }

    usize reclaim_target = pmm_reclaim_target_frames(count, free_snapshot);
    /* If this allocation is itself nested inside an eviction's writeback
     * (pmm_in_reclaim — e.g. ext4 write_cb -> kmalloc -> kheap_grow -> here),
     * only do CLEAN-only reclaim: re-entering the dirty-writeback path would
     * kmalloc again and deadlock on the heap_lock the outer kheap_grow holds.
     * If no clean page can be freed, fail gracefully (return 0) so the nested
     * write backs off instead of wedging — the dirty page just stays cached for
     * a later flush, no data loss. */
    if (pmm_in_reclaim()) {
      usize ce = page_cache_evict_clean(reclaim_target);
      if (ce > 0)
        continue;
      return 0;
    }
    /* Top-level reclaim: full eviction (writes dirty pages back). Run it inside
     * the reclaim context so a nested alloc takes the clean-only path above. */
    pmm_enter_reclaim();
    usize pc_evicted = page_cache_evict(reclaim_target);
    pmm_leave_reclaim();
    if (pc_evicted > 0) {
      continue;
    }

    /* Before declaring OOM, drain per-CPU caches back to the global pool —
     * frames sitting in another CPU's pcp are bitmap-USED and invisible to
     * bitmap_scan_alloc. One-shot per call so we don't busy-loop draining
     * an empty cache. */
    if (pmm_pcp_ready() && !pcp_drained) {
      pmm_pcp_drain_all();
      pcp_drained = 1;
      continue;
    }

    // Then try evicting a process page to swap.
    extern int swap_active(void);
    if (swap_active() && interrupts_enabled() && !pmm_in_reclaim()) {
      extern u64 swap_evict_page(void);
      pmm_enter_reclaim();
      u64 evicted_frame = swap_evict_page();
      pmm_leave_reclaim();
      if (evicted_frame != 0) {
        if (bootinfo_has_flag("b1nix.debug.heap")) {
          console_write("[M26DIAG] swap_evict frame=0x");
          console_write_hex64(evicted_frame);
          m26_diag_task();
          console_write("\n");
        }
        pmm_free_frame(evicted_frame);
        continue;
      }
    }

    // Nothing left to reclaim — genuinely out of memory. Tell the user
    // exactly what we tried and what would make this work, then return 0 so
    // the caller can panic with a meaningful upstream message. This is the
    // closest analogue to Linux's "Out of memory: Killed process X" dmesg
    // line — observable, actionable, not a silent hang.
    /* Throttle the OOM diagnostic to once per pressure episode. A userspace
     * allocation storm (e.g. a JS engine GC-thrashing near the RAM ceiling)
     * hits this path thousands of times; printing the multi-line dump each
     * time floods the console and makes the box look wedged. The flag is reset
     * once a clean-alloc streak shows the episode is over (see fast path). */
    if (!pmm_oom_reported) {
      pmm_oom_reported = 1;
      extern void kheap_dump_large_allocs(void);
      kheap_dump_large_allocs();
      console_write("[OUT OF MEMORY] pmm: cannot satisfy ");
      console_write_dec(count);
      console_write("-frame request after ");
      console_write_dec(pmm_total_reclaims);
      console_write(" reclaim cycles. Total RAM=");
      console_write_dec(pmm.total_usable / (1024 * 1024));
      console_write(" MB free=");
      console_write_dec(pmm.free_frames * PAGE_SIZE / (1024 * 1024));
      console_write(" MB. SUGGEST: increase -m or reduce make -j N.");
      m26_diag_task();
      console_write("\n");
      klog_warn("pmm: out of contiguous physical memory");
    }
    /* OOM reclaim of last resort: SIGKILL the userspace task demanding the
     * memory we cannot supply, so its address space is torn down and its frames
     * reclaimed — ending the ENOMEM/page-fault retry storm instead of returning
     * 0 forever. Kernel-thread / init allocations are spared and fall through to
     * the 0 return (the caller handles failure). Async: the victim dies at its
     * next return-to-user. */
    extern int scheduler_oom_kill_current(void);
    scheduler_oom_kill_current();
    /* Reset pressure flags so the NEXT episode can warn again. */
    pmm_warned_pressure = 0;
    pmm_total_reclaims = 0;
    pmm_clean_alloc_streak = 0;
    return 0;
  }
}

u64 pmm_total_usable_memory(void) { return pmm.total_usable; }
u64 pmm_phys_total_memory(void) { return pmm.phys_total; }

u64 pmm_free_memory_estimate(void) { return pmm.free_frames * PAGE_SIZE; }

usize pmm_free_frame_count(void) { return (usize)pmm.free_frames; }

u64 pmm_zero_page(void) { return zero_page_frame; }

void pmm_switch_to_direct_map(void) {
  if (pmm.bitmap) {
    pmm.bitmap = (u8 *)((u64)(usize)pmm.bitmap + DIRECT_MAP_BASE);
  }
  if (pmm.buddy_heads) {
    pmm.buddy_heads = (u8 *)((u64)(usize)pmm.buddy_heads + DIRECT_MAP_BASE);
  }
  if (pmm.frame_refcounts) {
    pmm.frame_refcounts = (u16 *)((u64)(usize)pmm.frame_refcounts + DIRECT_MAP_BASE);
  }

  pmm_poison_enabled = bootinfo_has_flag("b1nix.pmm-poison");
  if (pmm_poison_enabled)
    console_write("pmm: page poisoning enabled (debug, costs a page write per free)\n");

  /* The direct map now exists, so free blocks can hold their intrusive list
   * header. Seed the buddy tree with every currently-free frame; from here on
   * alloc/free maintain it incrementally. */
  u64 flags;
  pmm_acquire(&flags);
  buddy_seed_from_bitmap();
  buddy_ready = 1;
  pmm_release(flags);

  /* Reserve the shared zero page now that the buddy tree is live. It gets a
   * normal allocation (refcount 1) and is never released — pmm_free_frame
   * ignores it, so it can never be reallocated or swapped (it is also never
   * registered in the eviction ring). */
  zero_page_frame = pmm_alloc_frame();
  if (!zero_page_frame)
    panic("pmm: failed to reserve shared zero page");
}
