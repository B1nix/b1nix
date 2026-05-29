#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <string.h>

struct kheap_state {
  u64 base;
  u64 current;
  u64 end;
};

struct kheap_block {
  usize size;
  struct kheap_block *next;
  u32 magic;
};

#define KHEAP_MAGIC 0xB1A110C
#define KHEAP_FREED_MAGIC 0xDEAD110C
#define KHEAP_HEADER_SIZE 32
#define KHEAP_REUSE_MIN_SIZE 0

/* -----------------------------------------------------------------------
 * Large-allocation arena (returns whole pages to the pmm on free)
 *
 * The general bump heap above never hands freed pages back to the pmm — its
 * mapped high-water only grows. During the in-guest self-host build this lets
 * the kheap climb to ~2 GB even though live use is ~57 MB, because cc1/as/ld
 * allocate big transient ELF-staging buffers (tens of MB) that are freed but
 * whose pages stay mapped. Those large, short-lived allocations are exactly
 * the ones that blow the high-water, so we route every allocation >=
 * KLARGE_THRESHOLD to a separate page-granular arena that maps frames on
 * alloc and unmaps + frees them to the pmm on free. The general heap below is
 * left byte-for-byte identical to the known-good baseline (no coalescing, no
 * boundary tags), so its behaviour — and the host smoke result — is unchanged.
 *
 * The arena lives in the upper part of the kheap's PML4[384] slot (which spans
 * 512 GB), well above where the general bump heap ever reaches, so it shares
 * the same globally-mapped kernel page tables and needs no extra early paging
 * setup: a PD/PT installed for an arena page lands in the shared PDPT and is
 * visible in every address space, exactly like a general-heap growth.
 * -----------------------------------------------------------------------*/
#define KLARGE_START     (KHEAP_START + 0x1000000000ULL) /* +64 GB, same PML4[384] */
#define KLARGE_END       (KHEAP_START + 0x8000000000ULL) /* +512 GB (PML4[384] top) */
#define KLARGE_THRESHOLD (256u * 1024u)
#define KLARGE_MAGIC     0xB1A11A6EULL
#define KLARGE_HEADER_SIZE 32

struct klarge_header {
  u64 magic;
  usize size;   /* requested payload bytes */
  usize npages; /* total mapped pages (header + payload, rounded up) */
  u64 pad;
};

/* Free vaddr spans available for reuse; physical frames are always returned to
 * the pmm on free, so an entry here is purely an address-space range. A fixed
 * pool avoids allocating during free; on overflow we simply drop the span
 * (its vaddr leaks, but the physical frames were already freed — safe). */
struct klarge_span {
  u64 vaddr;
  usize npages;
};
#define KLARGE_MAX_SPANS 1024
static struct klarge_span klarge_free_spans[KLARGE_MAX_SPANS];
static int klarge_free_count;
static u64 klarge_bump = KLARGE_START;

/* -----------------------------------------------------------------------
 * Segregated free lists
 *
 * Instead of a single free_list (O(N) first-fit scan over ALL free blocks),
 * we keep NBUCKETS separate singly-linked lists partitioned by payload size.
 * kfree() places a block into the right bucket; kmalloc() first searches the
 * smallest bucket that can satisfy the request, then falls through to larger
 * buckets.  Each bucket's list is short → search stays O(1) amortised even
 * under pressure with thousands of free blocks.
 *
 * The internal block structure, split-on-reuse logic, magic canaries, and
 * bump allocator are unchanged from the known-good first-fit baseline.
 * No coalescing, no page return — both are intentionally excluded to avoid
 * exposing the latent VFS UAF bug documented in docs/m26-selfhost.md.
 *
 * Bucket i holds blocks with payload in (bucket_min[i-1], bucket_min[i]].
 * Bucket NBUCKETS-1 is the catch-all for the largest blocks.
 * -----------------------------------------------------------------------*/
#define NBUCKETS 16
/* Upper bound (inclusive) of each bucket's size class. */
static const usize bucket_max[NBUCKETS] = {
  32, 64, 128, 256, 512, 1024, 2048, 4096,
  8192, 16384, 32768, 65536, 131072, 262144, 1048576, (usize)-1
};

static struct kheap_state heap;
static struct kheap_block *free_lists[NBUCKETS];
static spinlock_t heap_lock = SPINLOCK_INIT;

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

/* Return the bucket index for a block with the given payload size. */
static int size_to_bucket(usize size) {
  for (int i = 0; i < NBUCKETS - 1; i++) {
    if (size <= bucket_max[i]) return i;
  }
  return NBUCKETS - 1;
}

struct tracked_alloc {
  u64 addr;
  usize size;
  u64 caller;
};

#define MAX_TRACKED_BLOCKS 1024
static struct tracked_alloc tracked_blocks[MAX_TRACKED_BLOCKS];

static void track_alloc(u64 addr, usize size, u64 caller) {
  if (size < 1024 * 1024) return;
  for (int i = 0; i < MAX_TRACKED_BLOCKS; i++) {
    if (tracked_blocks[i].addr == 0) {
      tracked_blocks[i].addr = addr;
      tracked_blocks[i].size = size;
      tracked_blocks[i].caller = caller;
      break;
    }
  }
}

static void track_free(u64 addr) {
  for (int i = 0; i < MAX_TRACKED_BLOCKS; i++) {
    if (tracked_blocks[i].addr == addr) {
      tracked_blocks[i].addr = 0;
      tracked_blocks[i].size = 0;
      tracked_blocks[i].caller = 0;
      break;
    }
  }
}

void kheap_dump_large_allocs(void) {
  u64 flags;
  if (spin_is_locked(&heap_lock)) {
    console_write("[OOMDIAG] Large allocation dump skipped: heap_lock held\n");
    return;
  }

  spin_lock_irqsave(&heap_lock, &flags);
  console_write("[OOMDIAG] Large allocations (>= 1MB):\n");
  for (int i = 0; i < MAX_TRACKED_BLOCKS; i++) {
    if (tracked_blocks[i].addr != 0) {
      console_write("  Address: 0x");
      console_write_hex64(tracked_blocks[i].addr);
      console_write(" Size: ");
      console_write_dec(tracked_blocks[i].size);
      console_write(" Caller: 0x");
      console_write_hex64(tracked_blocks[i].caller);
      console_write("\n");
    }
  }
  spin_unlock_irqrestore(&heap_lock, flags);
}

static u64 align_up_u64(u64 value, u64 alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

static int is_canonical_addr(u64 addr) {
  return ((isize)addr >> 47) == 0 || ((isize)addr >> 47) == -1;
}

static void heap_grow(usize minimum_bytes) {
  static u64 grow_calls;
  usize pages = (minimum_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
  grow_calls++;

  if (minimum_bytes >= 1024 * 1024 || is_power_of_two_u64(grow_calls)) {
    console_write("[M26DIAG] kheap_grow call=");
    console_write_dec(grow_calls);
    console_write(" bytes=");
    console_write_dec(minimum_bytes);
    console_write(" pages=");
    console_write_dec(pages);
    console_write(" heap_end=0x");
    console_write_hex64(heap.end);
    console_write(" free_frames=");
    console_write_dec(pmm_free_frame_count());
    m26_diag_task();
    console_write("\n");
  }

  for (usize i = 0; i < pages; i++) {
    u64 frame = pmm_alloc_frame();
    if (!frame) {
      panic("kheap: OOM during heap growth");
    }

    u64 vaddr = heap.end;
    vmm_map_page(vaddr, frame, VMM_PRESENT | VMM_WRITABLE);

    heap.end += PAGE_SIZE;
  }
}

void kheap_init(void) {
  heap.base = KHEAP_START;
  heap.current = KHEAP_START;
  heap.end = KHEAP_START;
  for (int i = 0; i < NBUCKETS; i++) free_lists[i] = 0;
  heap_grow(PAGE_SIZE);

  console_write("kheap: start 0x");
  console_write_hex64(heap.current);
  console_write(" end 0x");
  console_write_hex64(heap.end);
  console_write("\n");
}

void kheap_use_direct_map(void) {
  /* kheap has been transitioned to KHEAP_START in the higher half.
   * Direct-map is ready, and page tables for the heap are shared globally. */
  return;
}

void kheap_validate(const char *func) {
  (void)func;
}


/* Allocate from the large arena: map npages fresh frames and return a 16-byte
 * aligned payload pointer. Reuses a freed vaddr span when one is big enough
 * (split on reuse), else bumps. Holds heap_lock (same lock as the general
 * heap; large allocations are infrequent so contention is a non-issue). */
static void *klarge_alloc(usize size, u64 caller) {
  usize total = KLARGE_HEADER_SIZE + size;
  usize npages = (total + PAGE_SIZE - 1) / PAGE_SIZE;

  /* Phase 1 — reserve a vaddr span under the lock (fast, non-blocking). */
  u64 flags;
  spin_lock_irqsave(&heap_lock, &flags);

  u64 base = 0;
  for (int i = 0; i < klarge_free_count; i++) {
    if (klarge_free_spans[i].npages >= npages) {
      base = klarge_free_spans[i].vaddr;
      usize extra = klarge_free_spans[i].npages - npages;
      if (extra > 0) {
        /* Keep the remainder span (its vaddr is unmapped, no physical cost). */
        klarge_free_spans[i].vaddr = base + npages * PAGE_SIZE;
        klarge_free_spans[i].npages = extra;
      } else {
        klarge_free_spans[i] = klarge_free_spans[--klarge_free_count];
      }
      break;
    }
  }
  if (!base) {
    base = klarge_bump;
    if (base + npages * PAGE_SIZE > KLARGE_END) {
      panic("klarge: arena address space exhausted");
    }
    klarge_bump = base + npages * PAGE_SIZE;
  }
  void *ptr = (void *)(usize)(base + KLARGE_HEADER_SIZE);
  track_alloc((u64)(usize)ptr, size, caller);
  spin_unlock_irqrestore(&heap_lock, flags);

  /* Phase 2 — map fresh frames with the lock released, i.e. at the CALLER's
   * interrupt state. This is deliberate and load-bearing: `pmm_alloc_frame`'s
   * OOM path can only swap user pages out to relieve pressure when interrupts
   * are enabled (swap I/O yields), so doing this under heap_lock (IRQs off)
   * gates swap reclaim off entirely — which is exactly why a 128MB + swap
   * in-guest build OOM'd at `KBUILD 1` without ever calling swap_evict_page.
   * The span is already reserved exclusively here, so no other allocator can
   * touch it. b1nix is cooperatively scheduled (no preemption mid-map), so on
   * the uniprocessor build/smoke path vmm_map_page is atomic w.r.t. other
   * tasks; on true SMP this shares the single-writer assumption already made
   * by general-heap growth. */
  for (usize i = 0; i < npages; i++) {
    u64 frame = pmm_alloc_frame();
    if (!frame) {
      panic("klarge: OOM mapping large allocation");
    }
    vmm_map_page(base + i * PAGE_SIZE, frame, VMM_PRESENT | VMM_WRITABLE);
  }

  struct klarge_header *h = (struct klarge_header *)(usize)base;
  h->magic = KLARGE_MAGIC;
  h->size = size;
  h->npages = npages;
  h->pad = 0;
  return ptr;
}

/* Free a large-arena allocation: unmap every page and return its frame to the
 * pmm (this is the whole point — the pages go back, unlike the general heap),
 * then record the vaddr span for later reuse. */
static void klarge_free(void *ptr) {
  u64 p = (u64)(usize)ptr;
  struct klarge_header *h =
      (struct klarge_header *)(usize)(p - KLARGE_HEADER_SIZE);

  u64 flags;
  spin_lock_irqsave(&heap_lock, &flags);

  if (h->magic != KLARGE_MAGIC) {
    spin_unlock_irqrestore(&heap_lock, flags);
    return;
  }
  u64 base = (u64)(usize)h;
  usize npages = h->npages;
  h->magic = 0;
  track_free(p);

  for (usize i = 0; i < npages; i++) {
    u64 vaddr = base + i * PAGE_SIZE;
    u64 frame = vmm_virt_to_phys((void *)(usize)vaddr);
    vmm_unmap_page(vaddr);
    if (frame) {
      pmm_free_frame(frame);
    }
  }

  if (klarge_free_count < KLARGE_MAX_SPANS) {
    klarge_free_spans[klarge_free_count].vaddr = base;
    klarge_free_spans[klarge_free_count].npages = npages;
    klarge_free_count++;
  }
  spin_unlock_irqrestore(&heap_lock, flags);
}

static void *kmalloc_internal(usize size, u64 caller) {
  if (size == 0) {
    return 0;
  }

  if (size >= KLARGE_THRESHOLD) {
    return klarge_alloc(size, caller);
  }

  u64 flags;
  spin_lock_irqsave(&heap_lock, &flags);

  kheap_validate("kmalloc_start");

  size = align_up_u64(size, 16);
  struct kheap_block *block = 0;

  /* Search segregated buckets from the smallest that could fit `size`
   * upward.  Each bucket list is short, so the scan stays fast even
   * under pressure.  The logic inside each bucket is identical to the
   * original first-fit + split: we walk the list, validate entries, and
   * split large blocks to avoid internal fragmentation. */
#if KHEAP_REUSE_MIN_SIZE > 0
  if (size >= KHEAP_REUSE_MIN_SIZE)
#endif
  {
    int start_bucket = size_to_bucket(size);
    for (int bi = start_bucket; bi < NBUCKETS && !block; bi++) {
      struct kheap_block **prev = &free_lists[bi];
      struct kheap_block *b = free_lists[bi];
      while (b) {
        u64 bp = (u64)(usize)b;
        /* Detect free-list corruption: sever the list on a bad entry. */
        if (!is_canonical_addr(bp) ||
            (bp & 0xF) != 0 ||
            bp < heap.base + KHEAP_HEADER_SIZE ||
            bp + KHEAP_HEADER_SIZE > heap.end ||
            b->magic != KHEAP_FREED_MAGIC) {
          *prev = 0;
          b = 0;
          break;
        }
        if (b->size >= size) {
          /* Split off the remainder as its own free block when it is large
           * enough to hold a header plus a minimal allocation; this stops
           * internal fragmentation (a small alloc otherwise wastes the
           * whole big block).  The remainder is inserted into its own
           * bucket, not back into bi — ensures large remainders are
           * findable by later large requests without a full-list walk. */
          if (b->size >= size + KHEAP_HEADER_SIZE + 16) {
            struct kheap_block *rem =
                (struct kheap_block *)((u8 *)b + KHEAP_HEADER_SIZE + size);
            rem->size = b->size - size - KHEAP_HEADER_SIZE;
            rem->magic = KHEAP_FREED_MAGIC;
            /* Link remainder into the correct bucket for its size. */
            int rem_bkt = size_to_bucket(rem->size);
            rem->next = free_lists[rem_bkt];
            free_lists[rem_bkt] = rem;
            b->size = size;
          }
          /* Unlink b from bucket bi. */
          *prev = b->next;
          b->next = 0;
          b->magic = KHEAP_MAGIC;
          block = b;
          break;
        }
        prev = &b->next;
        b = b->next;
      }
    }
  }

  if (block) {
    void *ptr = (void *)((u8 *)block + KHEAP_HEADER_SIZE);
    track_alloc((u64)(usize)ptr, block->size, caller);
    spin_unlock_irqrestore(&heap_lock, flags);
    return ptr;
  }

  /* No suitable free block found — bump allocate. */
  u64 aligned_current = align_up_u64(heap.current, 16);
  u64 next = aligned_current + KHEAP_HEADER_SIZE + size;

  if (next > heap.end) {
    heap_grow(KHEAP_HEADER_SIZE + size);
    aligned_current = align_up_u64(heap.current, 16);
    next = aligned_current + KHEAP_HEADER_SIZE + size;
  }

  heap.current = next;
  block = (struct kheap_block *)(usize)aligned_current;
  block->size = size;
  block->next = 0;
  block->magic = KHEAP_MAGIC;

  void *ptr = (void *)((u8 *)block + KHEAP_HEADER_SIZE);
  track_alloc((u64)(usize)ptr, size, caller);
  spin_unlock_irqrestore(&heap_lock, flags);
  return ptr;
}

void *kmalloc(usize size) {
  return kmalloc_internal(size, (u64)__builtin_return_address(0));
}

static void *kzalloc_internal(usize size, u64 caller) {
  void *ptr = kmalloc_internal(size, caller);

  if (ptr != 0) {
    memset(ptr, 0, size);
  }

  return ptr;
}

void *kzalloc(usize size) {
  return kzalloc_internal(size, (u64)__builtin_return_address(0));
}

void kfree(void *ptr) {
  if (!ptr)
    return;
  u64 p = (u64)(usize)ptr;
  if (!is_canonical_addr(p))
    return;
  if (p >= KLARGE_START && p < KLARGE_END) {
    klarge_free(ptr);
    return;
  }
  if (heap.base != 0) {
    if (p < heap.base + KHEAP_HEADER_SIZE || p >= heap.end)
      return;
  }

  u64 flags;
  spin_lock_irqsave(&heap_lock, &flags);

  kheap_validate("kfree_start");

  struct kheap_block *block =
      (struct kheap_block *)((u8 *)ptr - KHEAP_HEADER_SIZE);
  if (block->magic != KHEAP_MAGIC) {
    spin_unlock_irqrestore(&heap_lock, flags);
    return;
  }
#if KHEAP_REUSE_MIN_SIZE > 0
  if (block->size < KHEAP_REUSE_MIN_SIZE) {
    spin_unlock_irqrestore(&heap_lock, flags);
    return;
  }
#endif
  track_free((u64)(usize)ptr);
  block->magic = KHEAP_FREED_MAGIC;
  /* Insert into the appropriate size-class bucket instead of a global list.
   * This prevents large freed blocks from being buried behind many small
   * blocks, eliminating the O(N) scan that caused the 2048 MB timeout. */
  int bkt = size_to_bucket(block->size);
  block->next = free_lists[bkt];
  free_lists[bkt] = block;

  kheap_validate("kfree_end");
  spin_unlock_irqrestore(&heap_lock, flags);
}
