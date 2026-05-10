#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/panic.h>
#include <string.h>

struct kheap_state {
  u64 current;
  u64 end;
};

struct kheap_block {
  usize size;
  struct kheap_block *next;
  u32 magic;
};

#define KHEAP_MAGIC 0xB1A110C
#define KHEAP_HEADER_SIZE 32
#define KHEAP_REUSE_MIN_SIZE 0

static struct kheap_state heap;
static struct kheap_block *free_list;
static int use_direct_map;

static u64 align_up_u64(u64 value, u64 alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
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

    if (heap.current == 0) {
      heap.current = vaddr;
    }

    heap.end += PAGE_SIZE;
  }
}

void kheap_init(void) {
  heap.current = 0;
  heap.end = 0;
  free_list = 0;
  use_direct_map = 0;
  heap_grow(PAGE_SIZE);

  console_write("kheap: start 0x");
  console_write_hex64(heap.current);
  console_write(" end 0x");
  console_write_hex64(heap.end);
  console_write("\n");
}

void kheap_use_direct_map(void) { use_direct_map = 1; }

void *kmalloc(usize size) {
  if (size == 0) {
    return 0;
  }

  size = align_up_u64(size, 16);
  struct kheap_block *block = 0;
  if (size >= KHEAP_REUSE_MIN_SIZE) {
    struct kheap_block **prev = &free_list;
    block = free_list;
    while (block) {
      if (block->size >= size) {
        *prev = block->next;
        block->next = 0;
        block->magic = KHEAP_MAGIC;
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
  struct kheap_block *block =
      (struct kheap_block *)((u8 *)ptr - KHEAP_HEADER_SIZE);
  if (block->magic != KHEAP_MAGIC)
    return;
  if (block->size < KHEAP_REUSE_MIN_SIZE)
    return;
  block->next = free_list;
  free_list = block;
}
