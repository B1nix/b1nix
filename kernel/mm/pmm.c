#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/page_cache.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/arch.h>
#include <string.h>

#define BITS_PER_BYTE 8

extern u8 __kernel_start[];
extern u8 __kernel_end[];

struct pmm_state {
  u64 kernel_start;
  u64 kernel_end;
  u64 max_address;
  u64 total_usable;
  u64 free_frames;
  usize bitmap_bytes;
  u8 *bitmap;
  u16 *frame_refcounts;
  /* O(1) single-frame allocator. free_list_head is an intrusive LIFO stack of
   * free frames: each free frame's first 8 bytes (via the direct map) hold the
   * next frame's physical address; 0 terminates (frame 0 is always reserved, so
   * it is a safe sentinel). free_list_bitmap tracks which frames are currently
   * linked so a push is idempotent: a frame taken by a contiguous bitmap scan
   * stays linked (stale) until popped, and re-freeing it must not link it twice
   * (which would create a cycle). The used-bitmap stays the source of truth for
   * "free vs used"; a popped frame whose used-bit is set is stale and skipped. */
  u64 free_list_head;
  u8 *free_list_bitmap;
  int free_list_ready;
};

static struct pmm_state pmm;
static spinlock_t pmm_lock = SPINLOCK_INIT;

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

static int onlist_get(usize index) {
  return (pmm.free_list_bitmap[index / BITS_PER_BYTE] &
          (1u << (index % BITS_PER_BYTE))) != 0;
}

static void onlist_set(usize index) {
  pmm.free_list_bitmap[index / BITS_PER_BYTE] |= (u8)(1u << (index % BITS_PER_BYTE));
}

static void onlist_clear(usize index) {
  pmm.free_list_bitmap[index / BITS_PER_BYTE] &=
      (u8) ~(1u << (index % BITS_PER_BYTE));
}

/* Link a frame onto the free stack. Idempotent: a frame already on the list is
 * left untouched, so it can never be linked twice. Requires the direct map. */
static void freelist_push(u64 frame) {
  usize index = frame_index(frame);
  if (onlist_get(index)) {
    return;
  }
  *(u64 *)(usize)(frame + DIRECT_MAP_BASE) = pmm.free_list_head;
  pmm.free_list_head = frame;
  onlist_set(index);
}

/* Pop the next genuinely-free frame, discarding stale entries (frames that were
 * linked but later taken by a contiguous bitmap scan). Returns 0 if empty. */
static u64 freelist_pop(void) {
  while (pmm.free_list_head != 0) {
    u64 frame = pmm.free_list_head;
    usize index = frame_index(frame);
    pmm.free_list_head = *(u64 *)(usize)(frame + DIRECT_MAP_BASE);
    onlist_clear(index);
    if (!bitmap_get(index)) {
      return frame;
    }
  }
  return 0;
}

static void mark_frame_free(u64 frame) {
  usize index = frame_index(frame);

  if (bitmap_get(index)) {
    bitmap_clear(index);
    pmm.free_frames++;
  }
  if (pmm.free_list_ready) {
    freelist_push(frame);
  }
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
  u64 search_addr = align_up_u64((u64)(usize)__kernel_end, PAGE_SIZE);

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

void pmm_init(const struct boot_info *boot_info) {
  pmm.kernel_start = (u64)(usize)__kernel_start;
  pmm.kernel_end = align_up_u64((u64)(usize)__kernel_end, PAGE_SIZE);
  pmm.max_address = 0;
  pmm.total_usable = 0;
  pmm.free_frames = 0;

  for (usize i = 0; i < boot_info->memory_region_count; i++) {
    const struct boot_memory_region *region = &boot_info->memory_regions[i];
    if (region->type != BOOT_MEMORY_AVAILABLE) {
      continue;
    }

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

  pmm.free_list_head = 0;
  pmm.free_list_ready = 0;

  u64 early_mem = find_early_mem(
      boot_info, pmm.bitmap_bytes * 2 + refcounts_bytes);
  pmm.bitmap = (u8 *)(usize)early_mem;
  pmm.free_list_bitmap = (u8 *)(usize)(early_mem + pmm.bitmap_bytes);
  pmm.frame_refcounts =
      (u16 *)(usize)(early_mem + pmm.bitmap_bytes * 2);

  memset(pmm.bitmap, 0xff, pmm.bitmap_bytes);
  memset(pmm.free_list_bitmap, 0, pmm.bitmap_bytes);
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

    for (u64 frame = start; frame < end; frame += PAGE_SIZE) {
      mark_frame_free(frame);
    }

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

  for (u64 frame = (u64)(usize)pmm.free_list_bitmap;
       frame < (u64)(usize)pmm.free_list_bitmap + pmm.bitmap_bytes;
       frame += PAGE_SIZE) {
    mark_frame_used(frame);
  }

  for (u64 frame = (u64)(usize)pmm.frame_refcounts;
       frame < (u64)(usize)pmm.frame_refcounts + refcounts_bytes; frame += PAGE_SIZE) {
    mark_frame_used(frame);
  }

  console_write("pmm: bitmap 0x");
  console_write_hex64((u64)(usize)pmm.bitmap);
  console_write("-0x");
  console_write_hex64((u64)(usize)pmm.bitmap + pmm.bitmap_bytes);
  console_write("\n");

  console_write("pmm: total usable bytes 0x");
  console_write_hex64(pmm.total_usable);
  console_write("\n");
}

void pmm_ref_frame(u64 frame) {
  u64 flags;
  spin_lock_irqsave(&pmm_lock, &flags);
  usize idx = frame / PAGE_SIZE;
  if (pmm.frame_refcounts) {
    pmm.frame_refcounts[idx]++;
  }
  spin_unlock_irqrestore(&pmm_lock, flags);
}

void pmm_free_frame(u64 frame) {
  if ((frame & (PAGE_SIZE - 1)) != 0 || frame >= pmm.max_address) {
    klog_warn("pmm_free_frame: invalid frame");
    return;
  }

  u64 flags;
  spin_lock_irqsave(&pmm_lock, &flags);
  usize idx = frame / PAGE_SIZE;
  if (pmm.frame_refcounts) {
    if (pmm.frame_refcounts[idx] > 0) {
      pmm.frame_refcounts[idx]--;
    }
    if (pmm.frame_refcounts[idx] == 0) {
      mark_frame_free(frame);
    }
  } else {
    mark_frame_free(frame);
  }
  spin_unlock_irqrestore(&pmm_lock, flags);
}

void pmm_unref_frame(u64 frame) {
  pmm_free_frame(frame);
}

u16 pmm_get_refcount(u64 frame) {
  u64 flags;
  spin_lock_irqsave(&pmm_lock, &flags);
  usize idx = frame / PAGE_SIZE;
  u16 val = 0;
  if (pmm.frame_refcounts) {
    val = pmm.frame_refcounts[idx];
  }
  spin_unlock_irqrestore(&pmm_lock, flags);
  return val;
}

u64 pmm_alloc_frame(void) { return pmm_alloc_frames(1); }

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

u64 pmm_alloc_frames(usize count) {
  u64 flags;
  static u64 reclaim_attempts;

  /* Retry via an explicit loop, never by recursion. Under memory pressure the
   * recovery path can run many eviction rounds; a tail-recursive retry would
   * add a stack frame per round and overflow the 16KB kernel stack (a likely
   * cause of the in-guest-build wedge at low RAM). Each iteration either
   * satisfies the request, makes progress by freeing >=1 frame, or gives up. */
  for (;;) {
    u64 free_snapshot = 0;
    spin_lock_irqsave(&pmm_lock, &flags);

    /* Single-frame fast path: pop the free-list stack in O(1). */
    if (count == 1 && pmm.free_list_ready) {
      u64 frame = freelist_pop();
      if (frame != 0) {
        claim_frame(frame_index(frame));
        zero_frames(frame, 1);
        spin_unlock_irqrestore(&pmm_lock, flags);
        return frame;
      }
      /* List empty. If the bitmap still shows free frames, the list missed
       * some (it must not): fall back to the authoritative scan. Skip the scan
       * when nothing is free so the OOM path stays O(1) instead of O(N). */
      if (pmm.free_frames > 0) {
        frame = bitmap_scan_alloc(1);
        if (frame != 0) {
          spin_unlock_irqrestore(&pmm_lock, flags);
          return frame;
        }
      }
    } else {
      u64 frame = bitmap_scan_alloc(count);
      if (frame != 0) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        return frame;
      }
    }

    free_snapshot = pmm.free_frames;
    spin_unlock_irqrestore(&pmm_lock, flags);

    // OOM. Reclaim and retry. Try evicting clean page-cache pages first.
    reclaim_attempts++;
    if (reclaim_attempts <= 16 || is_power_of_two_u64(reclaim_attempts)) {
      console_write("[M26DIAG] pmm_reclaim attempt=");
      console_write_dec(reclaim_attempts);
      console_write(" count=");
      console_write_dec(count);
      console_write(" free_frames=");
      console_write_dec(free_snapshot);
      m26_diag_task();
      console_write("\n");
    }
    usize reclaim_target = pmm_reclaim_target_frames(count, free_snapshot);
    usize pc_evicted = page_cache_evict(reclaim_target);
    if (pc_evicted > 0) {
      continue;
    }

    // Then try evicting a process page to swap.
    extern int swap_active(void);
    if (swap_active() && interrupts_enabled()) {
      extern u64 swap_evict_page(void);
      u64 evicted_frame = swap_evict_page();
      if (evicted_frame != 0) {
        console_write("[M26DIAG] swap_evict frame=0x");
        console_write_hex64(evicted_frame);
        m26_diag_task();
        console_write("\n");
        pmm_free_frame(evicted_frame);
        continue;
      }
    }

    // Nothing left to reclaim — genuinely out of memory.
    extern void kheap_dump_large_allocs(void);
    kheap_dump_large_allocs();
    console_write("[M26DIAG] pmm_reclaim_failed count=");
    console_write_dec(count);
    m26_diag_task();
    console_write("\n");
    klog_warn("pmm: out of contiguous physical memory");
    return 0;
  }
}

u64 pmm_total_usable_memory(void) { return pmm.total_usable; }

u64 pmm_free_memory_estimate(void) { return pmm.free_frames * PAGE_SIZE; }

usize pmm_free_frame_count(void) { return (usize)pmm.free_frames; }

void pmm_switch_to_direct_map(void) {
  if (pmm.bitmap) {
    pmm.bitmap = (u8 *)((u64)(usize)pmm.bitmap + DIRECT_MAP_BASE);
  }
  if (pmm.free_list_bitmap) {
    pmm.free_list_bitmap =
        (u8 *)((u64)(usize)pmm.free_list_bitmap + DIRECT_MAP_BASE);
  }
  if (pmm.frame_refcounts) {
    pmm.frame_refcounts = (u16 *)((u64)(usize)pmm.frame_refcounts + DIRECT_MAP_BASE);
  }

  /* The direct map now exists, so free frames can hold the intrusive list link.
   * Seed the stack with every currently-free frame (one-time O(frame_count)
   * walk); from here on alloc/free maintain it incrementally. */
  u64 flags;
  spin_lock_irqsave(&pmm_lock, &flags);
  usize frame_count = (usize)(pmm.max_address / PAGE_SIZE);
  for (usize idx = 0; idx < frame_count; idx++) {
    if (!bitmap_get(idx)) {
      freelist_push(frame_from_index(idx));
    }
  }
  pmm.free_list_ready = 1;
  spin_unlock_irqrestore(&pmm_lock, flags);
}
