#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/panic.h>
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
};

static struct pmm_state pmm;

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

static void mark_frame_free(u64 frame) {
  usize index = frame_index(frame);

  if (bitmap_get(index)) {
    bitmap_clear(index);
    pmm.free_frames++;
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

static u64 find_bitmap_address(const struct boot_info *boot_info,
                               usize bitmap_bytes) {
  u64 bitmap_end = align_up_u64((u64)(usize)__kernel_end, PAGE_SIZE);

  for (usize i = 0; i < boot_info->memory_region_count; i++) {
    const struct boot_memory_region *region = &boot_info->memory_regions[i];

    if (region->type != BOOT_MEMORY_AVAILABLE) {
      continue;
    }

    u64 start = align_up_u64(region->base, PAGE_SIZE);
    u64 end = align_down_u64(region->base + region->length, PAGE_SIZE);

    if (bitmap_end < start) {
      bitmap_end = start;
    }

    if (bitmap_end + bitmap_bytes > 0x100000000ULL) {
      continue;
    }

    if (region_contains(start, end - start, bitmap_end,
                        align_up_u64(bitmap_bytes, PAGE_SIZE))) {
      console_write("pmm: found bitmap location at 0x");
      console_write_hex64(bitmap_end);
      console_write("\n");
      return bitmap_end;
    }
  }

  panic("no space for physical memory bitmap");
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

    if (end > pmm.max_address) {
      pmm.max_address = end;
    }
  }

  usize frame_count = (usize)(pmm.max_address / PAGE_SIZE);
  pmm.bitmap_bytes = align_up_u64((frame_count + 7) / 8, PAGE_SIZE);
  pmm.bitmap = (u8 *)(usize)find_bitmap_address(boot_info, pmm.bitmap_bytes);
  memset(pmm.bitmap, 0xff, pmm.bitmap_bytes);

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

  console_write("pmm: bitmap 0x");
  console_write_hex64((u64)(usize)pmm.bitmap);
  console_write("-0x");
  console_write_hex64((u64)(usize)pmm.bitmap + pmm.bitmap_bytes);
  console_write("\n");

  console_write("pmm: total usable bytes 0x");
  console_write_hex64(pmm.total_usable);
  console_write("\n");
}

u64 pmm_alloc_frame(void) { return pmm_alloc_frames(1); }

u64 pmm_alloc_frames(usize count) {
  usize frame_count = (usize)(pmm.max_address / PAGE_SIZE);

  for (usize i = 0; i <= frame_count - count; i++) {
    int free = 1;
    for (usize j = 0; j < count; j++) {
      if (bitmap_get(i + j)) {
        free = 0;
        i += j; // Skip past the used frame
        break;
      }
    }
    if (free) {
      u64 frame = frame_from_index(i);
      for (usize j = 0; j < count; j++) {
        mark_frame_used(frame_from_index(i + j));
      }
      memset((void *)(usize)frame, 0, count * PAGE_SIZE);
      return frame;
    }
  }

  klog_warn("pmm: out of contiguous physical memory");
  return 0;
}

void pmm_free_frame(u64 frame) {
  if ((frame & (PAGE_SIZE - 1)) != 0 || frame >= pmm.max_address) {
    klog_warn("pmm_free_frame: invalid frame");
    return;
  }

  mark_frame_free(frame);
}

u64 pmm_total_usable_memory(void) { return pmm.total_usable; }

u64 pmm_free_memory_estimate(void) { return pmm.free_frames * PAGE_SIZE; }

usize pmm_free_frame_count(void) { return (usize)pmm.free_frames; }
