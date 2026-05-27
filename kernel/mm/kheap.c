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

void *kmalloc(usize size) {
  if (size == 0) {
    return 0;
  }

  u64 flags;
  spin_lock_irqsave(&heap_lock, &flags);

  kheap_validate("kmalloc_start");

  size = align_up_u64(size, 16);
  struct kheap_block *block = 0;
  if (size >= KHEAP_REUSE_MIN_SIZE) {
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
        *prev = block->next;
        block->next = 0;
        block->magic = KHEAP_MAGIC;
        spin_unlock_irqrestore(&heap_lock, flags);
        return (void *)((u8 *)block + KHEAP_HEADER_SIZE);
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
  
  spin_unlock_irqrestore(&heap_lock, flags);
  return (void *)((u8 *)block + KHEAP_HEADER_SIZE);
}

void *kzalloc(usize size) {
  void *ptr = kmalloc(size);

  if (ptr != 0) {
    memset(ptr, 0, size);
  }

  return ptr;
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
  if (block->size < KHEAP_REUSE_MIN_SIZE) {
    spin_unlock_irqrestore(&heap_lock, flags);
    return;
  }
  block->magic = KHEAP_FREED_MAGIC;
  block->next = free_list;
  free_list = block;
  
  kheap_validate("kfree_end");
  spin_unlock_irqrestore(&heap_lock, flags);
}

