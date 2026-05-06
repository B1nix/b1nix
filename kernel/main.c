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
#include <b1nix/blk.h>
#include <b1nix/ext2.h>
#include <b1nix/ext1.h>
#include <b1nix/ext3.h>
#include <b1nix/ext4.h>
#include <b1nix/btrfs.h>
#include <b1nix/ahci.h>
#include <b1nix/nvme.h>
#include <b1nix/filelock.h>
#include <b1nix/mqueue.h>
#include <b1nix/shm.h>
#include <b1nix/uidgid.h>
#include <b1nix/video.h>

extern void ps2_kbd_init(void);
extern void compositor_init(void);
extern void virtio_gpu_init(void);
extern void fb_console_init(void);

extern void bootinfo_init_from_fdt(u64 dtb_address);

void kernel_main(u64 arg0, u64 arg1)
{
	serial_init();
	console_init();
	console_write("b1nix kernel starting...\n");

	if (arg0 != 0x36d76289) {
		/* Not multiboot2 magic, but maybe we can still try to parse? 
		   Actually, better to just log it. */
		console_write("Warning: Multiboot2 magic mismatch: 0x");
		console_write_hex32((u32)arg0);
		console_write("\n");
	}

#ifdef __aarch64__
	bootinfo_init_from_fdt(arg0);
#else
	bootinfo_init_from_multiboot2((u32)arg0, (u32)arg1);
#endif

	console_write("Step 1: Bootinfo parsed\n");
	
	pmm_init(bootinfo_get());
	console_write("Step 2: PMM initialized\n");

	u64 frame = pmm_alloc_frame();
	console_write("Step 3: PMM probe frame: 0x");
	console_write_hex64(frame);
	console_write("\n");

	kheap_init();
	console_write("Step 4: KHeap initialized\n");

	void *heap_probe = kzalloc(64);
	console_write("Step 5: KHeap probe allocation: 0x");
	console_write_hex64((u64)(usize)heap_probe);
	console_write("\n");

	vmm_init();
	console_write("Step 6: VMM initialized\n");

	kheap_use_direct_map();
	console_write("Step 7: KHeap switched to direct map\n");

#ifndef __aarch64__
	if (bootinfo_get()->has_framebuffer) {
		fb_console_init();
		console_write("Step 8: FB Console initialized\n");
	}
#endif

	scheduler_init();
	console_write("Step 9: Scheduler initialized\n");

	uidgid_init();
	arch_init();
	blk_cache_init();

	initramfs_init();
	console_write("Step 10: Initramfs initialized\n");

#ifndef __aarch64__
	vfs_init();
	ext2_init();
	ext1_init();
	ext3_init();
	ext4_init();
	ahci_init();
	nvme_init();
	btrfs_init();
	filelock_init();
	mqueue_init();
	shm_init();
	swap_init();
	net_init();
	ps2_kbd_init();
	video_init();
	compositor_init();
	virtio_gpu_init();
#endif
	console_write("Step 11: Drivers initialized\n");

	userspace_init();
	user_spawn("/bin/init", 0, 0);

	while (scheduler_task_count() > 1) {
		scheduler_yield();
	}

	console_write("\nM6 network layer demo complete\n");
	arch_halt();
}
