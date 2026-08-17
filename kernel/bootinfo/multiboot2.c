#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/panic.h>
#include <string.h>

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289
#define MULTIBOOT2_TAG_TYPE_END 0
#define MULTIBOOT2_TAG_TYPE_CMDLINE 1
#define MULTIBOOT2_TAG_TYPE_MODULE 3
#define MULTIBOOT2_TAG_TYPE_MMAP 6
#define MULTIBOOT2_TAG_TYPE_FRAMEBUFFER 8

struct multiboot2_module_tag {
	u32 type;
	u32 size;
	u32 mod_start;
	u32 mod_end;
	char cmdline[1];
} __attribute__((packed));

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

static void copy_cmdline(const struct multiboot2_tag *tag)
{
	const char *src = (const char *)tag + sizeof(*tag);
	usize max = tag->size > sizeof(*tag) ? tag->size - sizeof(*tag) : 0;
	usize i = 0;

	while (i + 1 < sizeof(current_boot_info.command_line) && i < max &&
	       src[i] != '\0') {
		current_boot_info.command_line[i] = src[i];
		i++;
	}
	current_boot_info.command_line[i] = '\0';

	/* Say so when the line did not fit. A truncated boot line makes the kernel
	 * ignore whatever was asked for last, and it looks exactly like a flag
	 * that was never implemented. */
	if (i < max && src[i] != '\0')
		console_write("bootinfo: command line truncated — options past the "
		              "buffer were dropped\n");
}

static void parse_mmap_tag(const struct multiboot2_mmap_tag *tag)
{
	usize entries_start = (usize)tag + sizeof(*tag);
	usize entries_end = (usize)tag + tag->size;

	/* Reject a bootloader-supplied entry_size that would not advance the cursor
	 * (entry_size == 0 → infinite loop) or that is too small to hold an entry
	 * (misaligned/overlapping reads). Use cursor+entry_size <= end so the last
	 * entry cannot straddle past entries_end. */
	if (tag->entry_size < sizeof(struct multiboot2_mmap_entry))
		return;

	for (usize cursor = entries_start;
	     cursor + tag->entry_size <= entries_end;
	     cursor += tag->entry_size) {
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
	current_boot_info.has_framebuffer = 0;
	current_boot_info.command_line[0] = '\0';
	current_boot_info.ramdisk_addr = 0;
	current_boot_info.ramdisk_size = 0;
	current_boot_info.has_ramdisk = 0;

	while (cursor < end) {
		const struct multiboot2_tag *tag = (const struct multiboot2_tag *)cursor;

		/* A tag whose size is 0 (or smaller than the header, or that runs past
		 * the info block) cannot be trusted: align_up(cursor + size, 8) would
		 * not advance and the walk would spin forever before serial init can
		 * report. Stop the walk instead. */
		if (tag->size < sizeof(*tag) || cursor + tag->size > end)
			break;

		if (tag->type == MULTIBOOT2_TAG_TYPE_END) {
			break;
		}

		if (tag->type == MULTIBOOT2_TAG_TYPE_MMAP) {
			parse_mmap_tag((const struct multiboot2_mmap_tag *)tag);
		}

		if (tag->type == MULTIBOOT2_TAG_TYPE_MODULE) {
			const struct multiboot2_module_tag *mod = (const struct multiboot2_module_tag *)tag;
			/* Guard against mod_end < mod_start underflowing ramdisk_size. */
			if (mod->mod_end >= mod->mod_start) {
				current_boot_info.ramdisk_addr = mod->mod_start;
				current_boot_info.ramdisk_size = mod->mod_end - mod->mod_start;
				current_boot_info.has_ramdisk = 1;
			}
		}

		if (tag->type == MULTIBOOT2_TAG_TYPE_CMDLINE) {
			copy_cmdline(tag);
		}

		if (tag->type == MULTIBOOT2_TAG_TYPE_FRAMEBUFFER) {
			const struct multiboot2_framebuffer_tag *fb = (const struct multiboot2_framebuffer_tag *)tag;
			if (fb->width > 0 && fb->height > 0 && fb->pitch > 0 &&
			    (fb->bpp == 24 || fb->bpp == 32)) {
				current_boot_info.framebuffer.address = fb->addr;
				current_boot_info.framebuffer.pitch = fb->pitch;
				current_boot_info.framebuffer.width = fb->width;
				current_boot_info.framebuffer.height = fb->height;
				current_boot_info.framebuffer.bpp = fb->bpp;
				current_boot_info.framebuffer.type = fb->framebuffer_type;
				current_boot_info.has_framebuffer = 1;
			}
		}

		cursor = align_up(cursor + tag->size, 8);
	}
}

const struct boot_info *bootinfo_get(void)
{
	return &current_boot_info;
}

const char *bootinfo_cmdline(void)
{
	return current_boot_info.command_line;
}

int bootinfo_has_flag(const char *flag)
{
	const char *cmd = current_boot_info.command_line;
	usize flag_len = 0;

	if (!flag || !flag[0]) {
		return 0;
	}
	while (flag[flag_len]) {
		flag_len++;
	}

	for (usize i = 0; cmd[i];) {
		while (cmd[i] == ' ') {
			i++;
		}
		if (!cmd[i]) {
			break;
		}

		usize start = i;
		while (cmd[i] && cmd[i] != ' ') {
			i++;
		}
		if (i - start == flag_len && memcmp(cmd + start, flag, flag_len) == 0) {
			return 1;
		}
	}

	return 0;
}

int bootinfo_get_kv(const char *key, char *out, usize out_size)
{
	const char *cmd = current_boot_info.command_line;
	usize key_len = 0;

	if (!key || !key[0]) {
		return 0;
	}
	while (key[key_len]) {
		key_len++;
	}

	for (usize i = 0; cmd[i];) {
		while (cmd[i] == ' ') {
			i++;
		}
		if (!cmd[i]) {
			break;
		}

		usize start = i;
		while (cmd[i] && cmd[i] != ' ') {
			i++;
		}

		usize tok_len = i - start;
		if (tok_len > key_len && cmd[start + key_len] == '=' &&
		    memcmp(cmd + start, key, key_len) == 0) {
			const char *val = cmd + start + key_len + 1;
			usize val_len = tok_len - key_len - 1;

			if (out && out_size > 0) {
				usize n = val_len < out_size - 1 ? val_len
							         : out_size - 1;
				memcpy(out, val, n);
				out[n] = '\0';
			}
			return 1;
		}
	}

	return 0;
}
