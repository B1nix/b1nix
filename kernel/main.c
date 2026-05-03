#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/initramfs.h>
#include <b1nix/mm.h>
#include <b1nix/net.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>
#include <b1nix/serial.h>
#include <b1nix/user.h>
#include <b1nix/vfs.h>

extern void ps2_kbd_init(void);
extern void compositor_init(void);
extern void virtio_gpu_init(void);
extern void fb_console_init(void);

void kernel_main(u32 multiboot_magic, u32 multiboot_info)
{
	serial_init();
	console_init();

	console_write("b1nix kernel\n");
	serial_write("b1nix kernel booted\n");

	console_write("multiboot magic: 0x");
	console_write_hex32(multiboot_magic);
	console_write("\n");

	console_write("multiboot info:  0x");
	console_write_hex32(multiboot_info);
	console_write("\n");

	bootinfo_init_from_multiboot2(multiboot_magic, multiboot_info);
	pmm_init(bootinfo_get());

	u64 frame = pmm_alloc_frame();
	console_write("pmm: first allocated frame 0x");
	console_write_hex64(frame);
	console_write("\n");

	kheap_init();
	void *heap_probe = kzalloc(64);
	console_write("kheap: probe allocation 0x");
	console_write_hex64((u64)(usize)heap_probe);
	console_write("\n");

	vmm_init();
	u64 mapped_frame = pmm_alloc_frame();
	u64 mapped_virtual = 0x40000000ULL;
	vmm_map_page(mapped_virtual, mapped_frame, VMM_WRITABLE);
	volatile u64 *mapped_probe = (volatile u64 *)(usize)mapped_virtual;
	*mapped_probe = 0x54494e59554e4958ULL;
	console_write("vmm: mapped 0x");
	console_write_hex64(mapped_virtual);
	console_write(" -> 0x");
	console_write_hex64(mapped_frame);
	console_write("\n");

	if (bootinfo_get()->has_framebuffer) {
		fb_console_init();
		console_write("fb_console: initialized\n");
	}

	scheduler_init();
	arch_init();

	initramfs_init();
	vfs_init();
	net_init();
	ps2_kbd_init();
	compositor_init();
	virtio_gpu_init();
	userspace_init();
	user_spawn("/bin/init", 0, 0);

	while (scheduler_task_count() > 1) {
		scheduler_yield();
	}

	console_write("\nM6 network layer demo complete\n");
	arch_halt();
}
