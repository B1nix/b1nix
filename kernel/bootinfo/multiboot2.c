#include <tinyunix/bootinfo.h>
#include <tinyunix/console.h>
#include <tinyunix/panic.h>

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289
#define MULTIBOOT2_TAG_TYPE_END 0
#define MULTIBOOT2_TAG_TYPE_MMAP 6
#define MULTIBOOT2_TAG_TYPE_FRAMEBUFFER 8

struct multiboot2_info {
	u32 total_size;
	u32 reserved;
} __attribute__((packed));

struct multiboot2_tag {
	u32 type;
	u32 size;
} __attribute__((packed));

struct multiboot2_mmap_tag {
	u32 type;
	u32 size;
	u32 entry_size;
	u32 entry_version;
} __attribute__((packed));

struct multiboot2_mmap_entry {
	u64 addr;
	u64 len;
	u32 type;
	u32 zero;
} __attribute__((packed));

struct multiboot2_framebuffer_tag {
	u32 type;
	u32 size;
	u64 addr;
	u32 pitch;
	u32 width;
	u32 height;
	u8 bpp;
	u8 framebuffer_type;
	u16 reserved;
} __attribute__((packed));

static struct boot_info current_boot_info;

static usize align_up(usize value, usize alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

static void add_memory_region(u64 base, u64 length, u32 type)
{
	if (current_boot_info.memory_region_count >= BOOTINFO_MAX_MEMORY_REGIONS) {
		return;
	}

	usize index = current_boot_info.memory_region_count++;
	current_boot_info.memory_regions[index].base = base;
	current_boot_info.memory_regions[index].length = length;
	current_boot_info.memory_regions[index].type = type;
}

static void parse_mmap_tag(const struct multiboot2_mmap_tag *tag)
{
	usize entries_start = (usize)tag + sizeof(*tag);
	usize entries_end = (usize)tag + tag->size;

	for (usize cursor = entries_start; cursor < entries_end; cursor += tag->entry_size) {
		const struct multiboot2_mmap_entry *entry = (const struct multiboot2_mmap_entry *)cursor;
		add_memory_region(entry->addr, entry->len, entry->type);
	}
}

void bootinfo_init_from_multiboot2(u32 magic, u32 info_address)
{
	if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
		panic("bootloader did not provide multiboot2 info");
	}

	const struct multiboot2_info *info = (const struct multiboot2_info *)(usize)info_address;
	usize cursor = (usize)info + sizeof(*info);
	usize end = (usize)info + info->total_size;

	current_boot_info.memory_region_count = 0;

	while (cursor < end) {
		const struct multiboot2_tag *tag = (const struct multiboot2_tag *)cursor;

		if (tag->type == MULTIBOOT2_TAG_TYPE_END) {
			break;
		}

		if (tag->type == MULTIBOOT2_TAG_TYPE_MMAP) {
			parse_mmap_tag((const struct multiboot2_mmap_tag *)tag);
		}

		if (tag->type == MULTIBOOT2_TAG_TYPE_FRAMEBUFFER) {
			const struct multiboot2_framebuffer_tag *fb = (const struct multiboot2_framebuffer_tag *)tag;
			current_boot_info.framebuffer.address = fb->addr;
			current_boot_info.framebuffer.pitch = fb->pitch;
			current_boot_info.framebuffer.width = fb->width;
			current_boot_info.framebuffer.height = fb->height;
			current_boot_info.framebuffer.bpp = fb->bpp;
			current_boot_info.framebuffer.type = fb->framebuffer_type;
			current_boot_info.has_framebuffer = 1;
		}

		cursor = align_up(cursor + tag->size, 8);
	}

	console_write("bootinfo: memory regions: ");
	console_write_hex64(current_boot_info.memory_region_count);
	console_write("\n");
}

const struct boot_info *bootinfo_get(void)
{
	return &current_boot_info;
}
