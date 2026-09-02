#include <b1nix/kprintf.h>
#include <b1nix/console.h>
#include <b1nix/vfs.h>
#include <b1nix/klog.h>
#include <b1nix/mm.h>

/* Defined in arch/x86_64/tlb.c; a no-op on a single-CPU boot. */
void tlb_shootdown_all(void);
#include <b1nix/panic.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/arch.h>
#include <string.h>

struct kheap_block {
  usize size;       /* payload bytes of this block (16-aligned) */
  usize prev_size;  /* payload bytes of the physically-preceding block, 0 if none */
  u32 magic;
  u32 padding;      /* keeps `next` 8-byte aligned; header stride is KHEAP_HEADER_SIZE */
  struct kheap_block *next; /* free-list link, valid only while freed */
};

struct kheap_state {
  u64 base;
  u64 current;
  u64 end;
  struct kheap_block *last_block; /* physically-topmost block, 0 if heap empty */
};

#define KHEAP_MAGIC 0xB1A110C
#define KHEAP_FREED_MAGIC 0xDEAD110C
/* A block parked in a per-CPU magazine. Distinct from both of the above on
 * purpose: coalescing must not claim it (it only merges KHEAP_FREED_MAGIC), and
 * kfree() must not cache it a second time. It used to keep KHEAP_MAGIC, so a
 * double kfree() of a small block pushed the SAME block into the magazine
 * twice and the allocator then handed one piece of memory to two owners —
 * whichever of them zeroed it destroyed the other's data. */
#define KHEAP_MAG_MAGIC 0xB1A6A21E
#define KHEAP_HEADER_SIZE 32
#define KHEAP_REUSE_MIN_SIZE 0

/* A freed tail block at least this large is dropped from the general heap and
 * its whole pages returned to the pmm (high-water shrinks). It is set above
 * KLARGE_THRESHOLD on purpose: single general-heap allocations are always
 * smaller than that (bigger ones go to the klarge arena, which already returns
 * pages), so this only fires when coalescing has merged many small frees into a
 * large contiguous tail. Keeping it large avoids thrashing unmap/remap on the
 * normal small-allocation churn. */
#define KHEAP_SHRINK_MIN (512u * 1024u)

/* Free-block coalescing (boundary-tag bidirectional merge) and tail page-return
 * for the general bump heap. Both maintain the per-block prev_size boundary tag
 * and heap.last_block; those invariants are checked by kheap_validate when
 * KHEAP_VALIDATE is enabled. Flip either to 0 to bisect against the no-coalesce
 * baseline. */
#define KHEAP_ENABLE_COALESCE 1
#if defined(__aarch64__)
/* Returning a freed tail to the pmm is the only thing that ever UNMAPS heap
 * memory, and kernel stacks (128 KiB) are ordinary heap blocks. Runs on this
 * arch kept dying with a write fault at exception-entry to an unmapped heap
 * page — i.e. a live task's kernel stack inside a range the shrink had handed
 * back. Until that is understood, keep the pages mapped here: the cost is
 * heap high-water only, which is what the optimisation exists to reduce. */
#define KHEAP_ENABLE_PAGE_RETURN 0
#else
#define KHEAP_ENABLE_PAGE_RETURN 1
#endif

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
 * alloc and unmaps + frees them to the pmm on free. The general heap below
 * additionally coalesces adjacent free blocks (boundary-tag prev_size) and
 * returns large freed tails to the pmm; the arena still owns every single
 * allocation >= KLARGE_THRESHOLD.
 *
 * The arena lives in the upper part of the kheap's PML4[384] slot (which spans
 * 512 GB), well above where the general bump heap ever reaches, so it shares
 * the same globally-mapped kernel page tables and needs no extra early paging
 * setup: a PD/PT installed for an arena page lands in the shared PDPT and is
 * visible in every address space, exactly like a general-heap growth.
 * -----------------------------------------------------------------------*/
#ifdef __x86_64__
#define KLARGE_START     (KHEAP_START + 0x1000000000ULL) /* +64 GB, same PML4[384] */
#define KLARGE_END       (KHEAP_START + 0x8000000000ULL) /* +512 GB (PML4[384] top) */
#elif defined(__aarch64__)
/* Same reasoning as KHEAP_START (see b1nix/mm.h): keep the arena out of the
 * identity-mapped RAM window, and inside L0[0] so its page tables stay shared
 * with every process. 128 GiB .. 320 GiB, both well under the 512 GiB that
 * L0[0] spans. */
#define KLARGE_START     (KHEAP_START + 0x1000000000ULL)
#define KLARGE_END       (KHEAP_START + 0x4000000000ULL)
#else
/* 32-bit: the +64 GB offset above overflows the 32-bit virtual address space and
 * aliases straight back onto the general kheap
 * (0xc0000000 + 0x1000000000 == 0x10c0000000, truncated to 0xc0000000 by the
 * 2-level pager), so a single >=256 KB allocation (e.g. the boot block cache)
 * mapped and zeroed frames on top of live kheap objects — including the boot
 * task struct, whose corrupted state field let find_unused_task recycle slot 0.
 * The 32-bit map leaves 0xE0000000-0xFFFFFFFF free above the kheap (0xC0000000)
 * and the MMIO window (0xD0000000-0xE0000000), so site the arena there. PD
 * index 0xE0000000>>22 == 896 falls in the cloned kernel half (>=512), so arena
 * page tables stay globally visible just like the 64-bit case. */
#define KLARGE_START     0xE0000000ULL
#define KLARGE_END       0xFF000000ULL   /* leave the top 16 MB unmapped */
#endif
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
 * On top of the segregated lists, kfree() coalesces a freed block with its
 * physical neighbours when they are also free (boundary-tag prev_size locates
 * the predecessor in O(1)), and returns a large freed tail's pages to the pmm.
 * See KHEAP_ENABLE_COALESCE / KHEAP_ENABLE_PAGE_RETURN above.
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

/* ----------------------------------------------------------------------------
 * Per-CPU small-allocation magazine (M28 #4)
 *
 * The single heap_lock serialises every kmalloc/kfree across cores; the SMP
 * heap benchmark measured ~2.58x per-op cost at -smp 4 under allocation-heavy
 * load. A per-CPU magazine caches fully-formed freed blocks (header intact,
 * still carrying KHEAP_MAGIC + size + prev_size) so the common small-alloc
 * traffic — task structs, vfs nodes, fd tables, path buffers — hits a
 * lock-free per-CPU stack instead of the global lock.
 *
 * Bounded and small-classes-only ON PURPOSE: the kheap already has a known
 * fragmentation blocker at large RAM (cached blocks sit outside the coalescing
 * pool), so only payload classes <= MAG_MAX_CLASS are cached, each magazine is
 * capped at MAG_DEPTH, and on overflow the block goes to the shared free list
 * normally (so coalescing still reclaims it). Large allocations bypass the
 * magazine entirely and are unaffected.
 *
 * Access discipline mirrors the PMM per-CPU cache: IRQs off across the
 * per-CPU touch (prevents migration mid-pop/push), no lock — the magazine
 * belongs only to this CPU. A block cached on one CPU and reused on another is
 * fine: it is an ordinary heap block, independent of which core frees/allocs.
 * ------------------------------------------------------------------------- */
#define MAG_NCLASS    6         /* size classes 16,32,64,128,256,512 */
#define MAG_MAX_CLASS 512u      /* payloads above this never enter the magazine */
#define MAG_DEPTH     32        /* max cached blocks per class per CPU
                                 * (worst case ~30 KiB/CPU: 512*32 + ... ) */

struct kheap_magazine {
  struct kheap_block *slot[MAG_NCLASS][MAG_DEPTH];
  u16 count[MAG_NCLASS];
};
static struct kheap_magazine kheap_mag[MAX_CPUS];

/* Map an exact 16-aligned payload size to a magazine class index, or -1 if the
 * size is not one of the cached classes (sizes are aligned to 16 first). */
static inline int mag_class(usize size) {
  switch (size) {
    case 16:  return 0;
    case 32:  return 1;
    case 64:  return 2;
    case 128: return 3;
    case 256: return 4;
    case 512: return 5;
    default:  return -1;
  }
}

static inline int kheap_mag_ready(void) {
  struct percpu *p = get_percpu();
  return p && p->cpu_id < MAX_CPUS;
}

static inline u64 kheap_irq_save_cli(void) {
  return interrupts_save();
}
static inline void kheap_irq_restore(u64 f) {
  interrupts_restore(f);
}

/* Try to satisfy a small alloc from this CPU's magazine. Returns a ready-to-use
 * payload pointer, or 0 on miss (caller falls through to the locked path).
 * `size` must already be 16-aligned. */
static void *kheap_mag_alloc(usize size) {
  int cls = mag_class(size);
  if (cls < 0 || !kheap_mag_ready())
    return 0;
  u64 f = kheap_irq_save_cli();
  struct percpu *p = get_percpu();
  struct kheap_magazine *m = &kheap_mag[p->cpu_id];
  void *ptr = 0;
  if (m->count[cls] > 0) {
    struct kheap_block *block = m->slot[cls][--m->count[cls]];
    block->magic = KHEAP_MAGIC;
    block->next = 0;
    ptr = (void *)((u8 *)block + KHEAP_HEADER_SIZE);
  }
  kheap_irq_restore(f);
  return ptr;
}

/* Try to cache a freed small block in this CPU's magazine. Returns 1 if cached
 * (caller is done), 0 if declined (wrong class / full / not ready) and the
 * block must take the normal locked free path. The block keeps KHEAP_MAGIC
 * while cached so a concurrent coalesce never merges it. */
static int kheap_mag_free(struct kheap_block *block) {
  int cls = mag_class(block->size);
  if (cls < 0 || !kheap_mag_ready())
    return 0;
  u64 f = kheap_irq_save_cli();
  struct percpu *p = get_percpu();
  struct kheap_magazine *m = &kheap_mag[p->cpu_id];
  int cached = 0;
  if (m->count[cls] < MAG_DEPTH) {
    block->next = 0;
    block->magic = KHEAP_MAG_MAGIC;
    m->slot[cls][m->count[cls]++] = block;
    cached = 1;
  }
  kheap_irq_restore(f);
  return cached;
}

/* Drop every cached block that lives in [lo, hi) from all magazines. The tail
 * page-return is about to unmap that range, and a magazine entry is a free
 * block that coalescing never saw (cached blocks keep KHEAP_MAGIC on purpose so
 * a concurrent merge cannot claim them). Without this the allocator happily
 * hands out a pointer into address space the heap no longer maps, and the
 * caller faults on its first write with nothing in the backtrace to explain
 * it. The blocks are free, so forgetting them loses nothing — the memory is
 * going back to the pmm either way. */
/* Physically topmost block, found by walking the boundary tags from the base.
 * Returns 0 if the chain does not check out (a corrupted header means nothing
 * here can be trusted, and the caller must not shrink on that basis). */
static struct kheap_block *kheap_walk_topmost(void) {
  u64 p = heap.base;
  struct kheap_block *last = 0;
  while (p + KHEAP_HEADER_SIZE <= heap.current) {
    struct kheap_block *b = (struct kheap_block *)(usize)p;
    if ((b->magic != KHEAP_MAGIC && b->magic != KHEAP_FREED_MAGIC &&
         b->magic != KHEAP_MAG_MAGIC) ||
        b->size == 0)
      return 0;
    last = b;
    p += KHEAP_HEADER_SIZE + b->size;
  }
  return (p == heap.current) ? last : 0;
}

static void kheap_mag_evict_range(u64 lo, u64 hi) {
  for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
    struct kheap_magazine *m = &kheap_mag[cpu];
    for (int cls = 0; cls < MAG_NCLASS; cls++) {
      u16 keep = 0;
      for (u16 i = 0; i < m->count[cls]; i++) {
        struct kheap_block *b = m->slot[cls][i];
        u64 start = (u64)(usize)b;
        u64 end = start + KHEAP_HEADER_SIZE + b->size;
        if (end > lo && start < hi)
          continue; /* overlaps the range being unmapped — forget it */
        m->slot[cls][keep++] = b;
      }
      m->count[cls] = keep;
    }
  }
}

#include <b1nix/lockdep.h>
static inline void heap_acquire(u64 *flags) {
  spin_lock_irqsave(&heap_lock, flags);
  LOCKDEP_ACQUIRE(LOCKDEP_LVL_HEAP);
}
static inline void heap_release(u64 flags) {
  LOCKDEP_RELEASE(LOCKDEP_LVL_HEAP);
  spin_unlock_irqrestore(&heap_lock, flags);
}

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

/* Remove a freed block from its size-class bucket. Coalescing merges a block
 * with a physical neighbour that is currently sitting on a free list, so that
 * neighbour must first be unlinked from wherever it lives. Panics if the block
 * is not found — that would mean the free lists and the physical heap layout
 * have diverged, i.e. corruption we want to catch immediately. */
static void bucket_unlink(struct kheap_block *blk) {
  int bkt = size_to_bucket(blk->size);
  struct kheap_block **pp = &free_lists[bkt];
  while (*pp) {
    if (*pp == blk) {
      *pp = blk->next;
      blk->next = 0;
      return;
    }
    pp = &(*pp)->next;
  }
  console_write("bucket_unlink: block 0x");
  console_write_hex64((u64)(usize)blk);
  console_write(" size ");
  console_write_dec(blk->size);
  console_write(" magic 0x");
  console_write_hex64(blk->magic);
  console_write(" not in bucket ");
  console_write_dec(bkt);
  console_write("\n");
  panic("bucket_unlink: block not found in bucket");
}

struct tracked_alloc {
  u64 addr;
  usize size;
  u64 caller;
};

#define MAX_TRACKED_BLOCKS 8192
/* 64 KB, not 1 MB.
 *
 * The gate decides which allocations can ever be attributed to a caller, and
 * at 1 MB it answered nothing about a heap that grew by ~5 MB a second in
 * blocks of 64 KB to 512 KB -- every one of them was below the gate, so the
 * table stayed empty while a gigabyte leaked. These are still rare enough
 * (single figures per second) that the linear scan below costs nothing on the
 * paths that matter. */
#define TRACK_MIN_SIZE (64u * 1024u)
static struct tracked_alloc tracked_blocks[MAX_TRACKED_BLOCKS];

static void track_alloc(u64 addr, usize size, u64 caller) {
  if (size < TRACK_MIN_SIZE) return;
  for (int i = 0; i < MAX_TRACKED_BLOCKS; i++) {
    if (tracked_blocks[i].addr == 0) {
      tracked_blocks[i].addr = addr;
      tracked_blocks[i].size = size;
      tracked_blocks[i].caller = caller;
      break;
    }
  }
}

/* Mirror track_alloc's size gate: only >=1MB blocks are ever inserted, so a
 * free below that can never be in the table — skip the O(MAX_TRACKED_BLOCKS)
 * scan entirely. Before this, EVERY kfree (the overwhelmingly common small
 * free) walked all 1024 slots to the end (the matching `break` never fires for
 * an untracked address), a fixed ~1024-iteration cost inside heap_lock on the
 * hottest kernel path — which also lengthened the critical section and worsened
 * cross-core heap_lock contention. */
static void track_free(u64 addr, usize size) {
  if (size < TRACK_MIN_SIZE) return;
  for (int i = 0; i < MAX_TRACKED_BLOCKS; i++) {
    if (tracked_blocks[i].addr == addr) {
      tracked_blocks[i].addr = 0;
      tracked_blocks[i].size = 0;
      tracked_blocks[i].caller = 0;
      break;
    }
  }
}

/*
 * What the heap is actually holding, by size class.
 *
 * kheap_mapped_bytes says how far the heap has grown; it does not say whether
 * that is live data or a high-water mark left by fragmentation, and it never
 * says WHAT. When the heap climbs by megabytes a second on an idle desktop,
 * the shape of the live blocks is the first real evidence: a million objects
 * of one size names the structure, while a mapped span far above the live
 * total names fragmentation instead.
 *
 * Walks the whole heap once, so it is on demand only (reading
 * /proc/b1nix-kheap), never on a timer.
 */
/*
 * The allocator gives back what a request does not use.
 *
 * This is a regression test for a bug that was invisible from userspace and
 * cost a desktop its machine: reuse handed out whole free blocks, so a small
 * allocation landing on a large free block consumed all of it forever, and the
 * heap climbed by megabytes a second at a constant object count. Nothing
 * failed, nothing was corrupted, and the only symptom was running out of
 * memory much later -- which is why it has a check of its own rather than
 * being left to the suites that merely allocate.
 *
 * Freeing a large block and then asking for a small one must NOT consume the
 * large block: the remainder has to come back as free space, which is measured
 * here as the heap's mapped high-water not moving across the second request.
 */
/* The size the allocator recorded for a live allocation, which is what the
 * split test has to look at: the request is 64 bytes either way, and the
 * question is whether the BLOCK behind it is still half a megabyte. */
usize kmalloc_reuse_probe_size(void *ptr) {
  if (!ptr)
    return 0;
  u64 p = (u64)(usize)ptr;
  if (p >= KLARGE_START && p < KLARGE_END)
    return 0;
  struct kheap_block *b =
      (struct kheap_block *)((u8 *)ptr - KHEAP_HEADER_SIZE);
  return b->magic == KHEAP_MAGIC ? b->size : 0;
}

void kheap_selftest(void) {
  const usize big = 512u * 1024u;
  void *a = kmalloc(big);

  if (!a) {
    console_write("KHEAP-SELFTEST: FAIL big-alloc\n");
    return;
  }
  kfree(a);

  /* The small request should be carved out of the block just freed, and the
   * rest of it left free -- so the heap must not have to grow to serve it. */
  u64 before = kheap_mapped_bytes();
  void *small = kmalloc(64);
  if (!small) {
    console_write("KHEAP-SELFTEST: FAIL small-alloc\n");
    return;
  }
  u64 after = kmalloc_reuse_probe_size(small);

  if (after > 4096) {
    console_write("KHEAP-SELFTEST: FAIL split block size ");
    console_write_dec(after);
    console_write("\n");
    kfree(small);
    return;
  }
  console_write("KHEAP-SELFTEST: ok split-on-reuse\n");

  /* And the remainder is usable: several more small requests must still fit
   * without the heap growing, which is what "given back" has to mean. */
  void *more[8];
  for (int i = 0; i < 8; i++)
    more[i] = kmalloc(1024);
  u64 grew = kheap_mapped_bytes();

  for (int i = 0; i < 8; i++)
    kfree(more[i]);
  kfree(small);

  if (grew > before) {
    console_write("KHEAP-SELFTEST: FAIL remainder-unusable grew ");
    console_write_dec((grew - before) / 1024);
    console_write(" kB\n");
    return;
  }
  console_write("KHEAP-SELFTEST: ok remainder-reusable\n");
}

void kheap_dump_histogram(void) {
  /* Size classes by power of two, 16 bytes to 1 MB and a tail. */
  u64 live_count[24] = {0};
  u64 live_bytes[24] = {0};
  u64 free_count[24] = {0};
  u64 free_bytes[24] = {0};
  u64 total_live = 0, total_free = 0, blocks = 0;
  u64 flags;

  heap_acquire(&flags);
  u64 addr = heap.base;
  while (addr + KHEAP_HEADER_SIZE <= heap.current) {
    struct kheap_block *b = (struct kheap_block *)(usize)addr;

    if (b->magic != KHEAP_MAGIC && b->magic != KHEAP_FREED_MAGIC)
      break; /* the walk has lost the chain; report what was counted */
    if (b->size == 0 || b->size > (heap.current - addr))
      break;
    int cls = 0;
    usize sz = b->size;
    while (sz > 16 && cls < 23) {
      sz >>= 1;
      cls++;
    }
    if (b->magic == KHEAP_MAGIC) {
      live_count[cls]++;
      live_bytes[cls] += b->size;
      total_live += b->size;
    } else {
      free_count[cls]++;
      free_bytes[cls] += b->size;
      total_free += b->size;
    }
    blocks++;
    addr += KHEAP_HEADER_SIZE + b->size;
  }
  u64 mapped = heap.current > heap.base ? heap.current - heap.base : 0;
  heap_release(flags);

  console_write("kheap: mapped ");
  console_write_dec(mapped / 1024);
  console_write(" kB, live ");
  console_write_dec(total_live / 1024);
  console_write(" kB, free ");
  console_write_dec(total_free / 1024);
  console_write(" kB, blocks ");
  console_write_dec(blocks);
  console_write("\n");
  for (int i = 0; i < 24; i++) {
    if (!live_count[i] && !free_count[i])
      continue;
    console_write("kheap:  <=");
    console_write_dec(16ull << i);
    console_write("B live ");
    console_write_dec(live_count[i]);
    console_write(" (");
    console_write_dec(live_bytes[i] / 1024);
    console_write(" kB) free ");
    console_write_dec(free_count[i]);
    console_write(" (");
    console_write_dec(free_bytes[i] / 1024);
    console_write(" kB)\n");
  }
}

/*
 * Live tracked allocations, aggregated by the code that made them.
 *
 * Listing every block names nothing when there are thousands of them; the
 * question is which call site is holding the memory, and that is a total per
 * caller. Sizes are the reason a caller matters, so both the count and the
 * bytes are reported and the list is scanned in place rather than sorted --
 * the caller with megabytes against it is obvious in the output.
 */
void kheap_dump_tracked_by_caller(void) {
  struct {
    u64 caller;
    u64 count;
    u64 bytes;
  } agg[64];
  int naggs = 0;
  u64 flags;
  u64 untracked = 0, total = 0;

  if (spin_is_locked(&heap_lock)) {
    k_info(NULL, "kheap: caller dump skipped, heap_lock held");
    return;
  }
  heap_acquire(&flags);
  for (int i = 0; i < MAX_TRACKED_BLOCKS; i++) {
    if (!tracked_blocks[i].addr)
      continue;
    total += tracked_blocks[i].size;
    int j = 0;
    for (; j < naggs; j++)
      if (agg[j].caller == tracked_blocks[i].caller)
        break;
    if (j == naggs) {
      if (naggs == 64) {
        untracked += tracked_blocks[i].size;
        continue;
      }
      agg[naggs].caller = tracked_blocks[i].caller;
      agg[naggs].count = 0;
      agg[naggs].bytes = 0;
      naggs++;
    }
    agg[j].count++;
    agg[j].bytes += tracked_blocks[i].size;
  }
  heap_release(flags);

  console_write("kheap: live blocks >=64kB total ");
  console_write_dec(total / 1024);
  console_write(" kB across ");
  console_write_dec((u64)naggs);
  console_write(" callers\n");
  for (int i = 0; i < naggs; i++) {
    console_write("kheap:  caller 0x");
    console_write_hex64(agg[i].caller);
    console_write(" count ");
    console_write_dec(agg[i].count);
    console_write(" bytes ");
    console_write_dec(agg[i].bytes / 1024);
    console_write(" kB\n");
  }
  if (untracked) {
    console_write("kheap:  (callers beyond the 64 listed: ");
    console_write_dec(untracked / 1024);
    console_write(" kB)\n");
  }
}

void kheap_dump_large_allocs(void) {
  u64 flags;
  if (spin_is_locked(&heap_lock)) {
    k_info(NULL, "[OOMDIAG] Large allocation dump skipped: heap_lock held");
    return;
  }

  heap_acquire(&flags);
  k_info(NULL, "[OOMDIAG] Large allocations (>= 1MB):");
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
  heap_release(flags);
}

static u64 align_up_u64(u64 value, u64 alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

static int is_canonical_addr(u64 addr) {
#if defined(__x86_64__)
  return ((isize)addr >> 47) == 0 || ((isize)addr >> 47) == -1;
#elif defined(__aarch64__)
  /* TCR_EL1.T0SZ=16 (kernel/arch/aarch64/boot.S) — 48-bit VAs through TTBR0,
   * bit 47 upwards clear. The 32-bit test below would reject every kernel
   * pointer above 4GB, which silently turns kfree() of such a pointer into a
   * no-op leak. */
  return (addr >> 48) == 0;
#else
  return (addr >> 32) == 0;
#endif
}

#include <b1nix/bootinfo.h>

static void heap_grow(usize minimum_bytes) {
  static u64 grow_calls;
  usize pages = (minimum_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
  grow_calls++;

  if (bootinfo_has_flag("b1nix.debug.heap") && (minimum_bytes >= 1024 * 1024 || is_power_of_two_u64(grow_calls))) {
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
    /* Growing must only ever map fresh address space. If this VA already
     * translates to a frame, heap.end has moved backwards over live heap and
     * the fresh (zeroed) frame we are about to install would replace the one
     * holding real objects — which reads exactly like "my data turned into
     * zeros" much later, with nothing pointing back here. */
    /* "Already translates" has two spellings: x86_64's vmm_virt_to_phys reports
     * an absent page as 0, aarch64's reports it as the address itself (it has
     * nothing better to say about an identity-mapped arch). Accept both, or
     * this fires on every single growth on one arch and never on the other —
     * as it did on x86_64, which panicked at the first kheap growth of boot. */
    u64 existing = vmm_virt_to_phys((void *)(usize)vaddr);
    if (existing != 0 && existing != vaddr) {
      console_write("kheap: growth would remap live page 0x");
      console_write_hex64(vaddr);
      console_write(" (already at 0x");
      console_write_hex64(existing);
      console_write("), heap.current=0x");
      console_write_hex64(heap.current);
      console_write("\n");
      panic("kheap: heap.end moved backwards over live memory");
    }
    vmm_map_page(vaddr, frame, VMM_PRESENT | VMM_WRITABLE);

    heap.end += PAGE_SIZE;
  }
}

void kheap_init(void) {
  heap.base = KHEAP_START;
  heap.current = KHEAP_START;
  heap.end = KHEAP_START;
  heap.last_block = 0;
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

/* Strict heap validator (recovered from the m26-coalesce-wip experiment).
 * When enabled it walks the entire general-heap bump region on EVERY
 * kmalloc/kfree and panics on the first corrupt block header. This is the tool
 * that originally surfaced the M16/M25 "heap corruption" — now believed to have
 * been the 16KB kernel-stack overflow (the per-task stack is a kheap block, so
 * a frame overrun smashes the adjacent header). Re-run it on the 32KB stack to
 * confirm the heap stays clean end-to-end. O(N) per op — debug only.
 * Verified 2026-05-29 on the 32KB stack: host smoke reaches B1NIX-TEST: done,
 * 178 ok markers, zero validator trips — the old M16/M25 corruption was the
 * 16KB kernel-stack overflow, not a heap/DMA UAF. Set to 1 to re-run. */
#define KHEAP_VALIDATE 0

#if KHEAP_VALIDATE
static void kheap_dump(void) {
  if (heap.base == 0) return;
  k_info(NULL, "=== KHEAP DUMP ===");
  console_write("heap.base: 0x"); console_write_hex64(heap.base);
  console_write(" heap.current: 0x"); console_write_hex64(heap.current);
  console_write(" heap.end: 0x"); console_write_hex64(heap.end);
  console_write(" heap.last_block: 0x"); console_write_hex64((u64)(usize)heap.last_block);
  console_write("\n");
  u64 addr = heap.base;
  int idx = 0;
  while (addr < heap.current) {
    struct kheap_block *block = (struct kheap_block *)(usize)addr;
    console_write("  ["); console_write_dec(idx++); console_write("] addr=0x");
    console_write_hex64(addr);
    console_write(" size="); console_write_dec(block->size);
    console_write(" prev_size="); console_write_dec(block->prev_size);
    console_write(" magic=0x"); console_write_hex64(block->magic);
    console_write("\n");
    if (block->size == 0 || block->size > 100u * 1024u * 1024u) {
      k_info(NULL, "  (aborting dump: insane size)");
      break;
    }
    addr += KHEAP_HEADER_SIZE + block->size;
  }
  k_info(NULL, "==================");
}
#endif /* KHEAP_VALIDATE */

/* Diagnostic: identify the general-heap block containing `addr` (base, payload
 * size, live/freed) and print one line. Used by the fatal-fault dump to tell a
 * wild kernel rip/cr2 inside the heap apart from a legitimate pointer — a
 * 64 KiB live block is a task kernel stack, a freed block means a use-after-
 * free. Read-only, bounded walk; safe to call from the exception path. */
/* How much address space the general heap has mapped.
 *
 * This is the kernel's own footprint as far as /proc/meminfo is concerned --
 * the closest honest analogue of Slab. It is a high-water figure, not live
 * use: the bump heap returns pages to the pmm only when coalescing leaves a
 * large free tail (KHEAP_SHRINK_MIN), so what it reports is what the kernel is
 * still holding, which is exactly the question asked of it. */
u64 kheap_mapped_bytes(void) {
  return heap.current > heap.base ? heap.current - heap.base : 0;
}

void kheap_bounds(u64 *base, u64 *current, u64 *end) {
  if (base) *base = heap.base;
  if (current) *current = heap.current;
  if (end) *end = heap.end;
}

void kheap_describe(u64 addr, const char *label) {
  console_write(label);
  console_write(" 0x");
  console_write_hex64(addr);
  if (heap.base == 0 || addr < heap.base || addr >= heap.current) {
    console_write(" not-in-general-heap\n");
    return;
  }
  u64 cur = heap.base;
  int idx = 0;
  while (cur < heap.current) {
    struct kheap_block *block = (struct kheap_block *)(usize)cur;
    if (block->size == 0 || block->size > 512u * 1024u * 1024u) {
      console_write(" walk-aborted-at 0x");
      console_write_hex64(cur);
      console_write("\n");
      return;
    }
    u64 payload = cur + KHEAP_HEADER_SIZE;
    u64 next = payload + block->size;
    if (addr >= cur && addr < next) {
      console_write(" heap block #");
      console_write_dec(idx);
      console_write(" base=0x");
      console_write_hex64(payload);
      console_write(" size=");
      console_write_dec(block->size);
      console_write(" off=+0x");
      console_write_hex64(addr - payload);
      console_write(block->magic == KHEAP_MAGIC     ? " LIVE"
                    : block->magic == KHEAP_FREED_MAGIC ? " FREED"
                                                        : " BAD-MAGIC");
      console_write("\n");
      if (block->magic == KHEAP_MAGIC)
        vfs_inode_chan_report(addr, payload, block->size);
      return;
    }
    cur = next;
    idx++;
  }
  k_info(NULL, " past-heap-walk");
}

void kheap_validate(const char *func) {
#if KHEAP_VALIDATE
  if (heap.base == 0) return;
  u64 addr = heap.base;
  struct kheap_block *prev_block = 0;
  while (addr < heap.current) {
    struct kheap_block *block = (struct kheap_block *)(usize)addr;
    if (block->magic != KHEAP_MAGIC && block->magic != KHEAP_FREED_MAGIC &&
        block->magic != KHEAP_MAG_MAGIC) {
      console_write("kheap_validate (from ");
      console_write(func);
      console_write("): block at 0x");
      console_write_hex64(addr);
      console_write(" invalid magic 0x");
      console_write_hex64(block->magic);
      console_write("\n  raw: ");
      u64 *raw = (u64 *)(usize)addr;
      console_write_hex64(raw[0]); console_write(" ");
      console_write_hex64(raw[1]); console_write(" ");
      console_write_hex64(raw[2]); console_write("\n");
      kheap_dump();
      panic("kheap magic corrupt");
    }
    if (block->size == 0 || block->size > heap.current - addr - KHEAP_HEADER_SIZE) {
      console_write("kheap_validate (from ");
      console_write(func);
      console_write("): block at 0x");
      console_write_hex64(addr);
      console_write(" insane size ");
      console_write_dec(block->size);
      console_write("\n");
      kheap_dump();
      panic("kheap size corrupt");
    }
    usize expect_prev = prev_block ? prev_block->size : 0;
    if (block->prev_size != expect_prev) {
      console_write("kheap_validate (from ");
      console_write(func);
      console_write("): block at 0x");
      console_write_hex64(addr);
      console_write(" prev_size ");
      console_write_dec(block->prev_size);
      console_write(" != expected ");
      console_write_dec(expect_prev);
      console_write("\n");
      kheap_dump();
      panic("kheap prev_size corrupt");
    }
    prev_block = block;
    addr += KHEAP_HEADER_SIZE + block->size;
  }
  if (addr != heap.current) {
    console_write("kheap_validate (from ");
    console_write(func);
    console_write("): walk overran to 0x");
    console_write_hex64(addr);
    console_write(" != heap.current 0x");
    console_write_hex64(heap.current);
    console_write("\n");
    kheap_dump();
    panic("kheap walk mismatch");
  }
  if (heap.last_block != prev_block) {
    console_write("kheap_validate (from ");
    console_write(func);
    console_write("): last_block 0x");
    console_write_hex64((u64)(usize)heap.last_block);
    console_write(" != topmost 0x");
    console_write_hex64((u64)(usize)prev_block);
    console_write("\n");
    kheap_dump();
    panic("kheap last_block mismatch");
  }
#else
  (void)func;
#endif
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
  heap_acquire(&flags);

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
  heap_release(flags);

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
  enum { KLARGE_MAP_BATCH = 256 };
  u64 map_batch[KLARGE_MAP_BATCH];
  usize batch_n = 0;

  for (usize i = 0; i < npages; i++) {
    u64 frame = pmm_alloc_frame();
    if (!frame) {
      for (usize j = 0; j < i; j++) {
        u64 vaddr = base + j * PAGE_SIZE;
        u64 fr = vmm_virt_to_phys((void *)(usize)vaddr);
        vmm_unmap_page(vaddr);
        if (fr) pmm_free_frame(fr);
      }
      heap_acquire(&flags);
      track_free((u64)(usize)ptr, size);
      if (klarge_free_count < KLARGE_MAX_SPANS) {
        klarge_free_spans[klarge_free_count].vaddr = base;
        klarge_free_spans[klarge_free_count].npages = npages;
        klarge_free_count++;
      }
      heap_release(flags);
      klog_warn("klarge: large allocation denied (low memory) — returning NULL");
      return 0;
    }
    /* Batch the mapping: one page-table lock acquisition per run of frames
     * rather than one per page. A tens-of-megabytes allocation was tens of
     * thousands of irqsave round trips against CPUs taking faults meanwhile. */
    map_batch[batch_n++] = frame;
    if (batch_n == KLARGE_MAP_BATCH) {
      vmm_map_range(base + (i + 1 - batch_n) * PAGE_SIZE, map_batch, batch_n,
                    VMM_PRESENT | VMM_WRITABLE);
      batch_n = 0;
    }
  }
  if (batch_n)
    vmm_map_range(base + (npages - batch_n) * PAGE_SIZE, map_batch, batch_n,
                  VMM_PRESENT | VMM_WRITABLE);

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
  heap_acquire(&flags);

  if (h->magic != KLARGE_MAGIC) {
    heap_release(flags);
    return;
  }
  u64 base = (u64)(usize)h;
  usize npages = h->npages;
  h->magic = 0;
  track_free(p, npages * PAGE_SIZE);
  /* Phase 2 runs with the lock RELEASED, for the reason klarge_alloc's rollback
   * path spells out: vmm_unmap_page issues a cross-CPU TLB shootdown and spins
   * with IRQs off until every other CPU ACKs the IPI, and a CPU spinning for
   * heap_lock with IRQs off can never send that ACK. Clearing the magic above
   * already retired the block, and its address span is not published in
   * klarge_free_spans until phase 3, so no other allocator can reach these
   * pages while they are unmapped. */
  heap_release(flags);

  /* Retire the pages in batches, with ONE cross-CPU flush per batch.
   *
   * vmm_unmap_page tells every other core about a single page: a global lock,
   * an inter-processor interrupt, and a spin until all of them answer. A
   * browser process image is a quarter of a gigabyte, so freeing it one page at
   * a time is tens of thousands of those rounds — and when two cores reap a
   * child at the same moment they starve each other on that lock until the
   * watchdog declares a deadlock and panics (seen as "spinlock lockup" with
   * both backtraces in tlb_shootdown_page).
   *
   * The frames cannot be released before the flush: another core could still
   * hold the old translation and write into a frame that has been handed out
   * again. So each batch is unmapped, then flushed once, then freed — which is
   * why the batch is a fixed on-stack array rather than the whole span. */
  enum { KLARGE_UNMAP_BATCH = 64 };
  u64 frames[KLARGE_UNMAP_BATCH];

  for (usize i = 0; i < npages;) {
    usize n = npages - i;

    if (n > KLARGE_UNMAP_BATCH)
      n = KLARGE_UNMAP_BATCH;
    vmm_unmap_range_nosync(base + i * PAGE_SIZE, n, frames);
    tlb_shootdown_all();
    for (usize k = 0; k < n; k++) {
      if (frames[k])
        pmm_free_frame(frames[k]);
    }
    i += n;
  }

  heap_acquire(&flags);
  if (klarge_free_count < KLARGE_MAX_SPANS) {
    klarge_free_spans[klarge_free_count].vaddr = base;
    klarge_free_spans[klarge_free_count].npages = npages;
    klarge_free_count++;
  }
  heap_release(flags);
}

/*
 * Carve the unused tail of a reused free block back into a free block.
 *
 * Without this the allocator hands out whole blocks: the search below takes
 * the first free block whose size is >= the request and returns it unchanged,
 * so a 64-byte allocation that lands on a 512 KB free block consumes all 512
 * KB, and `block->size` stays 512 KB forever. Coalescing makes it a ratchet
 * rather than a one-off -- adjacent frees merge into ever larger blocks, each
 * of which is then swallowed whole by whatever small allocation reaches it
 * next -- so the heap's live bytes climb without bound at a constant object
 * count. On an idle Plasma desktop that was ~5 MB a second, and it is why a
 * 4 GB guest ran out of memory: live 1483 MB across 301387 blocks, an average
 * of 4.9 kB per object in a kernel whose objects are mostly tens of bytes.
 *
 * The boundary tags have to stay true across the split: the tail records the
 * head's new size as its prev_size, the block physically after the tail
 * records the tail's, and if there is no such block then the tail is the new
 * topmost block. Those are exactly the invariants kheap_validate checks and
 * that coalescing reads.
 *
 * Called with heap_lock held and `want` already 16-aligned.
 */
#define KHEAP_MIN_SPLIT_PAYLOAD 32u

static void kheap_split_block(struct kheap_block *blk, usize want) {
  if (blk->size <= want)
    return;
  usize rest = blk->size - want;

  /* A remainder too small to hold a header plus a usable payload cannot become
   * a block; leave it as slack inside the allocation, which is what any
   * allocator does with the last few bytes. */
  if (rest < KHEAP_HEADER_SIZE + KHEAP_MIN_SPLIT_PAYLOAD)
    return;

  usize tail_payload = rest - KHEAP_HEADER_SIZE;
  struct kheap_block *tail =
      (struct kheap_block *)((u8 *)blk + KHEAP_HEADER_SIZE + want);

  tail->size = tail_payload;
  tail->prev_size = want;
  tail->magic = KHEAP_FREED_MAGIC;
  tail->padding = 0;
  tail->next = 0;

  struct kheap_block *after =
      (struct kheap_block *)((u8 *)tail + KHEAP_HEADER_SIZE + tail->size);
  if ((u64)(usize)after < heap.current)
    after->prev_size = tail->size;
  else
    heap.last_block = tail;

  blk->size = want;

  int bkt = size_to_bucket(tail->size);
  tail->next = free_lists[bkt];
  free_lists[bkt] = tail;
}

static void *kmalloc_internal(usize size, u64 caller) {
  if (size == 0) {
    return 0;
  }

  if (size >= KLARGE_THRESHOLD) {
    return klarge_alloc(size, caller);
  }

  size = align_up_u64(size, 16);

  /* Fast path: per-CPU magazine for small classes, lock-free. track_alloc only
   * acts on >=1MB blocks, so a magazine hit (<=256b) needs no accounting. */
  void *mag = kheap_mag_alloc(size);
  if (mag)
    return mag;

  u64 flags;
  heap_acquire(&flags);

  kheap_validate("kmalloc_start");

  struct kheap_block *block = 0;
  if (size >= KHEAP_REUSE_MIN_SIZE) {
    /* Search the smallest size-class bucket that can satisfy the request, then
     * larger buckets (segregated free lists, matching the kfree() push). */
    for (int bkt = size_to_bucket(size); bkt < NBUCKETS && !block; bkt++) {
      struct kheap_block **prev = &free_lists[bkt];
      struct kheap_block *cur = free_lists[bkt];
      while (cur) {
        u64 bp = (u64)(usize)cur;
        /* Detect free-list corruption (UAF / buffer overflow into a freed
         * neighbour). On corruption, sever this bucket here so we fall through
         * to bump allocation instead of crashing in a #GP/#PF. */
        if (!is_canonical_addr(bp) ||
            (bp & 0xF) != 0 ||
            bp < heap.base + KHEAP_HEADER_SIZE ||
            bp + KHEAP_HEADER_SIZE + cur->size > heap.end ||
            cur->magic != KHEAP_FREED_MAGIC) {
          *prev = 0;
          break;
        }
        if (cur->size >= size) {
          *prev = cur->next;
          cur->next = 0;
          cur->magic = KHEAP_MAGIC;
          block = cur;
          break;
        }
        prev = &cur->next;
        cur = cur->next;
      }
    }
  }

  if (block) {
    /* Give back whatever of the block this request does not need, before it is
     * accounted for or handed out. */
    kheap_split_block(block, size);
    void *ptr = (void *)((u8 *)block + KHEAP_HEADER_SIZE);
    track_alloc((u64)(usize)ptr, block->size, caller);
    kheap_validate("kmalloc_end_reuse");
    heap_release(flags);
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
  /* The block must lie entirely inside mapped heap. Handing out its tail past
   * heap.end reads back as a fault at exactly heap.end much later, from
   * whatever code happens to touch the end of that object. */
  if (next > heap.end) {
    panic("kheap: bump allocation past mapped end");
  }

  heap.current = next;
  block = (struct kheap_block *)(usize)aligned_current;
  block->size = size;
  block->prev_size = heap.last_block ? heap.last_block->size : 0;
  block->next = 0;
  block->magic = KHEAP_MAGIC;
  heap.last_block = block;

  void *ptr = (void *)((u8 *)block + KHEAP_HEADER_SIZE);
  track_alloc((u64)(usize)ptr, size, caller);
  kheap_validate("kmalloc_end_bump");
  heap_release(flags);
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

  /* Fast path: cache small blocks in this CPU's magazine, lock-free. Validate
   * the magic here (same double-free guard the locked path applies) before
   * handing the block to the per-CPU cache. A cached block keeps KHEAP_MAGIC,
   * so it stays invisible to coalescing (which only merges KHEAP_FREED_MAGIC
   * neighbours) and passes kheap_validate as a live block. track_free only
   * matters for >=1MB blocks, so a small magazine free needs no accounting. */
  {
    struct kheap_block *mb =
        (struct kheap_block *)((u8 *)ptr - KHEAP_HEADER_SIZE);
    if (mb->magic == KHEAP_MAGIC && kheap_mag_free(mb))
      return;
  }

  u64 flags;
  heap_acquire(&flags);

  kheap_validate("kfree_start");

  struct kheap_block *block =
      (struct kheap_block *)((u8 *)ptr - KHEAP_HEADER_SIZE);
  if (block->magic == KHEAP_FREED_MAGIC) {
    heap_release(flags);
    panic("kheap: double-free detected on block");
  }
  if (block->magic != KHEAP_MAGIC) {
    heap_release(flags);
    panic("kheap: freeing unallocated or corrupted block");
  }

#if KHEAP_REUSE_MIN_SIZE > 0
  if (block->size < KHEAP_REUSE_MIN_SIZE) {
    heap_release(flags);
    return;
  }
#endif
  track_free((u64)(usize)ptr, block->size);
  block->magic = KHEAP_FREED_MAGIC;

  /* Coalesce with the physically-preceding block when it is also free. The
   * boundary tag prev_size locates the predecessor in O(1); merging undoes the
   * fragmentation that split-on-reuse otherwise leaves behind. */
  if (KHEAP_ENABLE_COALESCE && block->prev_size > 0) {
    struct kheap_block *prev_phys =
        (struct kheap_block *)((u8 *)block - KHEAP_HEADER_SIZE - block->prev_size);
    if ((u64)(usize)prev_phys >= heap.base &&
        prev_phys->magic == KHEAP_FREED_MAGIC) {
      bucket_unlink(prev_phys);
      prev_phys->size += KHEAP_HEADER_SIZE + block->size;
      struct kheap_block *after =
          (struct kheap_block *)((u8 *)prev_phys + KHEAP_HEADER_SIZE + prev_phys->size);
      if ((u64)(usize)after < heap.current) {
        after->prev_size = prev_phys->size;
      } else {
        heap.last_block = prev_phys;
      }
      block = prev_phys;
    }
  }

  /* Coalesce with the physically-succeeding block when it is also free. */
  if (KHEAP_ENABLE_COALESCE) {
    struct kheap_block *next_phys =
        (struct kheap_block *)((u8 *)block + KHEAP_HEADER_SIZE + block->size);
    if ((u64)(usize)next_phys < heap.current &&
        next_phys->magic == KHEAP_FREED_MAGIC) {
      bucket_unlink(next_phys);
      block->size += KHEAP_HEADER_SIZE + next_phys->size;
      struct kheap_block *after =
          (struct kheap_block *)((u8 *)next_phys + KHEAP_HEADER_SIZE + next_phys->size);
      if ((u64)(usize)after < heap.current) {
        after->prev_size = block->size;
      } else {
        heap.last_block = block;
      }
    }
  }

  /* Tail page-return: when the merged block is the topmost block and big
   * enough, hand its whole pages back to the pmm so the heap high-water
   * shrinks. Only full pages strictly above the (shrunken) payload are
   * unmapped; the header page always stays mapped. KHEAP_SHRINK_MIN sits above
   * KLARGE_THRESHOLD, so this only triggers when coalescing has merged many
   * small frees into a large contiguous tail. */
  /* heap.last_block must really be the physically topmost block: the shrink
   * below unmaps everything from the end of this block up to heap.end and
   * rewinds the bump pointer there, so a stale last_block hands live memory
   * back to the pmm and then re-allocates it — which surfaces much later as an
   * unrelated live object full of zeros. Walk the boundary tags and check,
   * rather than trusting the cached pointer; the walk only runs on the rare
   * >=512 KB tail free. */
  if (KHEAP_ENABLE_PAGE_RETURN && heap.last_block == block &&
      block->size >= KHEAP_SHRINK_MIN) {
    struct kheap_block *top = kheap_walk_topmost();
    if (top != block) {
      static unsigned lb_reported;
      if (lb_reported < 8) {
        console_write("kheap: last_block 0x");
        console_write_hex64((u64)(usize)block);
        console_write(" is not the topmost block (walk says 0x");
        console_write_hex64((u64)(usize)top);
        console_write(") — skipping tail return\n");
        lb_reported++;
      }
    }
  }
  if (KHEAP_ENABLE_PAGE_RETURN && heap.last_block == block &&
      block->size >= KHEAP_SHRINK_MIN &&
      kheap_walk_topmost() == block) {
    u64 free_start =
        align_up_u64((u64)(usize)block + KHEAP_HEADER_SIZE + 16, PAGE_SIZE);
    if (free_start < heap.end) {
      kheap_mag_evict_range(free_start, heap.end);
      for (u64 vaddr = free_start; vaddr < heap.end; vaddr += PAGE_SIZE) {
        u64 frame = vmm_virt_to_phys((void *)(usize)vaddr);
        vmm_unmap_page(vaddr);
        if (frame) {
          pmm_free_frame(frame);
        }
      }
      heap.end = free_start;
      heap.current = free_start;
      block->size = free_start - (u64)(usize)block - KHEAP_HEADER_SIZE;
    }
  }

  /* Insert the (possibly merged/shrunken) block into its size-class bucket
   * instead of a global list. This keeps large freed blocks out from behind
   * many small ones, eliminating the O(N) scan that caused the 2048 MB
   * timeout. */
  int bkt = size_to_bucket(block->size);
  block->next = free_lists[bkt];
  free_lists[bkt] = block;

  kheap_validate("kfree_end");
  heap_release(flags);
}
