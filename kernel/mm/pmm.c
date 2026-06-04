#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/page_cache.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/arch.h>
#include <b1nix/lapic.h>   /* struct percpu + MAX_CPUS for the per-CPU PCP cache */
#include <string.h>

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

/* Bulk version of mark_frame_free for pmm_init's "mark whole region free"
 * walk. The naive per-frame loop dominated boot under TCG with large RAM
 * (~2M iterations for 8 GB ⇒ tens of seconds before the shell appeared).
 * Pre-condition: every bitmap bit in [start_idx, end_idx) is currently SET
 * (the bitmap was just memset(0xff)'d in pmm_init); the bulk clear is the
 * inverse — memset(0) for the byte-aligned interior, bit-clear at the edges.
 * Caller may not depend on free_list_ready here (it is false during init). */
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
  pmm.kernel_start = (u64)(usize)__kernel_start;
  pmm.kernel_end = align_up_u64((u64)(usize)__kernel_end, PAGE_SIZE);
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

  for (u64 frame = (u64)(usize)pmm.free_list_bitmap;
       frame < (u64)(usize)pmm.free_list_bitmap + pmm.bitmap_bytes;
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
struct pmm_pcp {
  u64 head;       /* intrusive stack head, same on-frame link layout as global */
  u32 count;      /* number of frames currently parked */
  u32 _pad;
} __attribute__((aligned(64)));

#define PMM_PCP_LIMIT  128   /* drain to global once cache exceeds */
#define PMM_PCP_REFILL  32   /* pull this many on miss */
#define PMM_PCP_DRAIN   64   /* push this many back on overflow */

static struct pmm_pcp pmm_pcp[MAX_CPUS];

static inline int pmm_pcp_ready(void) {
  if (!pmm.free_list_ready) return 0;
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
    u64 frame = freelist_pop();
    if (frame == 0) break;
    usize idx = frame_index(frame);
    if (bitmap_get(idx)) continue; /* stale entry, skip */
    bitmap_set(idx);
    pmm.free_frames--;
    *(u64 *)(usize)(frame + DIRECT_MAP_BASE) = pcp->head;
    pcp->head = frame;
    pcp->count++;
  }
  pmm_release(flags);
}

static void pmm_pcp_drain(struct pmm_pcp *pcp) {
  u64 flags;
  pmm_acquire(&flags);
  for (int i = 0; i < PMM_PCP_DRAIN && pcp->count > 0; i++) {
    u64 frame = pcp->head;
    pcp->head = *(u64 *)(usize)(frame + DIRECT_MAP_BASE);
    pcp->count--;
    usize idx = frame_index(frame);
    if (bitmap_get(idx)) {
      bitmap_clear(idx);
      pmm.free_frames++;
    }
    freelist_push(frame);
  }
  pmm_release(flags);
}

static void pmm_pcp_drain_all(void) {
  for (int c = 0; c < MAX_CPUS; c++) {
    struct pmm_pcp *pcp = &pmm_pcp[c];
    if (pcp->count == 0) continue;
    u64 flags;
    pmm_acquire(&flags);
    while (pcp->count > 0) {
      u64 frame = pcp->head;
      pcp->head = *(u64 *)(usize)(frame + DIRECT_MAP_BASE);
      pcp->count--;
      usize idx = frame_index(frame);
      if (bitmap_get(idx)) {
        bitmap_clear(idx);
        pmm.free_frames++;
      }
      freelist_push(frame);
    }
    pmm_release(flags);
  }
}

void pmm_free_frame(u64 frame) {
  if ((frame & (PAGE_SIZE - 1)) != 0 || frame >= pmm.max_address) {
    klog_warn("pmm_free_frame: invalid frame");
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
    if (pcp->count >= PMM_PCP_LIMIT) {
      /* Overflowing — drain half back to global. pmm_pcp_drain takes the
       * spinlock internally (which save/restores the flag we already
       * cleared); the inner cli is a no-op. */
      pmm_pcp_drain(pcp);
    }
    /* Frame stays bitmap-USED while it sits in the cache; the link goes
     * in the frame's own first 8 bytes via the direct map. */
    *(u64 *)(usize)(frame + DIRECT_MAP_BASE) = pcp->head;
    pcp->head = frame;
    pcp->count++;
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

    /* Single-frame fast path: pop the free-list stack in O(1). */
    if (count == 1 && pmm.free_list_ready) {
      u64 frame = freelist_pop();
      if (frame != 0) {
        claim_frame(frame_index(frame));
        zero_frames(frame, 1);
        pmm_release(flags);
        if (reclaim_attempts == 0) {
          /* Healthy fast-path success. After a stretch of these, the
           * pressure episode is over — re-arm the warning. */
          if (++pmm_clean_alloc_streak >= 128) {
            pmm_warned_pressure = 0;
            pmm_total_reclaims = 0;
            pmm_clean_alloc_streak = 0;
          }
        }
        return frame;
      }
      /* List empty. If the bitmap still shows free frames, the list missed
       * some (it must not): fall back to the authoritative scan. Skip the scan
       * when nothing is free so the OOM path stays O(1) instead of O(N). */
      if (pmm.free_frames > 0) {
        frame = bitmap_scan_alloc(1);
        if (frame != 0) {
          pmm_release(flags);
          if (reclaim_attempts == 0) {
          /* Healthy fast-path success. After a stretch of these, the
           * pressure episode is over — re-arm the warning. */
          if (++pmm_clean_alloc_streak >= 128) {
            pmm_warned_pressure = 0;
            pmm_total_reclaims = 0;
            pmm_clean_alloc_streak = 0;
          }
        }
          return frame;
        }
      }
    } else {
      u64 frame = bitmap_scan_alloc(count);
      if (frame != 0) {
        pmm_release(flags);
        if (reclaim_attempts == 0) {
          /* Healthy fast-path success. After a stretch of these, the
           * pressure episode is over — re-arm the warning. */
          if (++pmm_clean_alloc_streak >= 128) {
            pmm_warned_pressure = 0;
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
    /* This call needed reclaim — break any clean-streak. */
    pmm_clean_alloc_streak = 0;
    pmm_total_reclaims++;
    enum { PMM_PRESSURE_WARN = 32 };
    if (pmm_total_reclaims == PMM_PRESSURE_WARN && !pmm_warned_pressure) {
      pmm_warned_pressure = 1;
      klog_warn("pmm: memory pressure — 32 cumulative reclaim cycles "
                "(too many parallel tasks for available RAM; suggest "
                "fewer make -j N jobs, more memory, or enable swap)");
    }

    usize reclaim_target = pmm_reclaim_target_frames(count, free_snapshot);
    usize pc_evicted = page_cache_evict(reclaim_target);
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

    // Nothing left to reclaim — genuinely out of memory. Tell the user
    // exactly what we tried and what would make this work, then return 0 so
    // the caller can panic with a meaningful upstream message. This is the
    // closest analogue to Linux's "Out of memory: Killed process X" dmesg
    // line — observable, actionable, not a silent hang.
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
   * walk); from here on alloc/free maintain it incrementally. Skip whole
   * all-used 64-bit bitmap words — for typical layouts a chunk of low memory,
   * the kernel, and the bitmap/refcount pages are all used, so word-stride
   * skipping cuts the seed cost a lot at large RAM under TCG. */
  u64 flags;
  pmm_acquire(&flags);
  usize frame_count = (usize)(pmm.max_address / PAGE_SIZE);
  const u64 *words = (const u64 *)pmm.bitmap;
  for (usize idx = 0; idx < frame_count;) {
    if ((idx & 63) == 0 && idx + 64 <= frame_count && words[idx / 64] == ~0ULL) {
      idx += 64;
      continue;
    }
    if (!bitmap_get(idx)) {
      freelist_push(frame_from_index(idx));
    }
    idx++;
  }
  pmm.free_list_ready = 1;
  pmm_release(flags);
}
