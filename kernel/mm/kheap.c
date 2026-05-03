#include <string.h>
#include <tinyunix/console.h>
#include <tinyunix/mm.h>
#include <tinyunix/panic.h>

struct kheap_state {
	u64 current;
	u64 end;
};

static struct kheap_state heap;

static u64 align_up_u64(u64 value, u64 alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

static void heap_grow(usize minimum_bytes)
{
	usize pages = (minimum_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
	u64 span_start = 0;
	u64 span_end = 0;

	for (usize i = 0; i < pages; i++) {
		u64 frame = pmm_alloc_frame();

		if (i == 0) {
			span_start = frame;
			span_end = frame + PAGE_SIZE;
			continue;
		}

		if (frame != span_end) {
			panic("early heap expected contiguous frames within one allocation");
		}

		span_end = frame + PAGE_SIZE;
	}

	heap.current = span_start;
	heap.end = span_end;
}

void kheap_init(void)
{
	heap.current = 0;
	heap.end = 0;
	heap_grow(PAGE_SIZE);

	console_write("kheap: start 0x");
	console_write_hex64(heap.current);
	console_write(" end 0x");
	console_write_hex64(heap.end);
	console_write("\n");
}

void *kmalloc(usize size)
{
	if (size == 0) {
		return 0;
	}

	u64 aligned_current = align_up_u64(heap.current, 16);
	u64 next = aligned_current + size;

	if (next > heap.end) {
		heap_grow(next - heap.end);
	}

	heap.current = next;
	return (void *)(usize)aligned_current;
}

void *kzalloc(usize size)
{
	void *ptr = kmalloc(size);

	if (ptr != 0) {
		memset(ptr, 0, size);
	}

	return ptr;
}
