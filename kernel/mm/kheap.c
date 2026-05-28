#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/panic.h>
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

static struct kheap_state heap;
static struct kheap_block *free_list;
static spinlock_t heap_lock = SPINLOCK_INIT;

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
  usize pages = (minimum_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

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
  free_list = 0;
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


static void *kmalloc_internal(usize size, u64 caller) {
  if (size == 0) {
    return 0;
  }

  u64 flags;
  spin_lock_irqsave(&heap_lock, &flags);

  kheap_validate("kmalloc_start");

  size = align_up_u64(size, 16);
  struct kheap_block *block = 0;
  /* Free-list reuse is gated by a tunable minimum. At KHEAP_REUSE_MIN_SIZE == 0
   * (reuse everything) the guard is unconditional; the #if keeps it meaningful
   * for a non-zero threshold without a tautological always-true comparison. */
#if KHEAP_REUSE_MIN_SIZE > 0
  if (size >= KHEAP_REUSE_MIN_SIZE)
#endif
  {
    struct kheap_block **prev = &free_list;
    block = free_list;
    while (block) {
      u64 bp = (u64)(usize)block;
      /* Detect free-list corruption (UAF / buffer overflow into a freed
       * neighbour). On corruption, sever the list here so we fall through to
       * bump allocation instead of crashing in a #GP/#PF. */
      if (!is_canonical_addr(bp) ||
          (bp & 0xF) != 0 ||
          bp < heap.base + KHEAP_HEADER_SIZE ||
          bp + KHEAP_HEADER_SIZE > heap.end ||
          block->magic != KHEAP_FREED_MAGIC) {
        *prev = 0;
        block = 0;
        break;
      }
      if (block->size >= size) {
        /* Split off the remainder as its own free block when it is large enough
         * to hold a header plus a minimal allocation; this stops internal
         * fragmentation (a small alloc otherwise wastes the whole big block). */
        if (block->size >= size + KHEAP_HEADER_SIZE + 16) {
          struct kheap_block *rem =
              (struct kheap_block *)((u8 *)block + KHEAP_HEADER_SIZE + size);
          rem->size = block->size - size - KHEAP_HEADER_SIZE;
          rem->next = block->next;
          rem->magic = KHEAP_FREED_MAGIC;
          *prev = rem;
          block->size = size;
        } else {
          *prev = block->next;
        }
        block->next = 0;
        block->magic = KHEAP_MAGIC;
        void *ptr = (void *)((u8 *)block + KHEAP_HEADER_SIZE);
        track_alloc((u64)(usize)ptr, size, caller);
        spin_unlock_irqrestore(&heap_lock, flags);
        return ptr;
      }
      prev = &block->next;
      block = block->next;
    }
  }

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
  block->next = free_list;
  free_list = block;
  
  kheap_validate("kfree_end");
  spin_unlock_irqrestore(&heap_lock, flags);
}

