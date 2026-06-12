#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/initramfs.h>
#include <b1nix/mm.h>
#include <b1nix/net.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>
#include <b1nix/serial.h>
#include <b1nix/serial_tty.h>
#include <b1nix/user.h>
#include <b1nix/vfs.h>
#include <b1nix/blk.h>
#include <b1nix/page_cache.h>
#include <b1nix/tlb.h>
#include <b1nix/ext2.h>
#include <b1nix/ext1.h>
#include <b1nix/fat32.h>
#include <b1nix/exfat.h>
#include <b1nix/ntfs.h>
#include <b1nix/ext3.h>
#include <b1nix/ext4.h>
#include <b1nix/btrfs.h>
#include <b1nix/procfs.h>
#include <b1nix/ahci.h>
#include <b1nix/nvme.h>
#include <b1nix/filelock.h>
#include <b1nix/errno.h>
#include <b1nix/isofs.h>
#include <b1nix/loop.h>
#include <b1nix/mqueue.h>
#include <b1nix/shm.h>
#include <b1nix/uidgid.h>
#include <b1nix/lapic.h>
#include <b1nix/video.h>
#include <b1nix/acpi.h>
#include <b1nix/ioapic.h>
#include <b1nix/ramdisk.h>
#include <b1nix/sound.h>
#include <string.h>
#include <stdio.h>

extern void ps2_kbd_init(void);
extern void ps2_mouse_init(void);
extern int xhci_probe(void);
extern void compositor_init(void);
extern void virtio_gpu_init(void);
extern void fb_console_init(void);
extern void hda_init(void);
extern void input_init(void);
extern void input_gfxtest_start(void);
extern void fb_dev_init(void);

extern void bootinfo_init_from_fdt(u64 dtb_address);
extern void m35_diag_run(void);
extern void m36_diag_run(void);

static int parse_hex_digit(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
	return -1;
}

static int parse_uuid(const char *str, u8 *uuid_out) {
	int out_idx = 0;
	for (int i = 0; str[i] != '\0' && out_idx < 16; i++) {
		if (str[i] == '-') {
			continue;
		}
		int high = parse_hex_digit(str[i]);
		if (high < 0) return 0;
		i++;
		if (str[i] == '\0') return 0;
		int low = parse_hex_digit(str[i]);
		if (low < 0) return 0;
		uuid_out[out_idx++] = (u8)((high << 4) | low);
	}
	return (out_idx == 16);
}

static int match_label(const char *expected, const char *s_volume_name) {
	int len = 0;
	while (expected[len] != '\0') {
		if (len >= 16) return 0;
		if (expected[len] != s_volume_name[len]) return 0;
		len++;
	}
	for (int i = len; i < 16; i++) {
		if (s_volume_name[i] != '\0' && s_volume_name[i] != ' ' &&
		    s_volume_name[i] != '\r' && s_volume_name[i] != '\n') {
			return 0;
		}
	}
	return 1;
}

static struct block_device *find_device_by_uuid(const u8 *expected_uuid) {
	u8 *sb_buf = kmalloc(1024);
	if (!sb_buf) return 0;

	struct block_device *found = 0;
	usize count = blk_count();
	for (usize i = 0; i < count; i++) {
		struct block_device *dev = blk_at(i);
		if (!dev || dev->block_size != 512 || dev->block_count < 4) {
			continue;
		}
		if (blk_read_cached(dev, 2, 2, sb_buf) == 0) {
			struct ext2_superblock *sb = (struct ext2_superblock *)sb_buf;
			if (sb->s_magic == EXT2_SUPER_MAGIC) {
				if (memcmp(sb->s_uuid, expected_uuid, 16) == 0) {
					found = dev;
					break;
				}
			}
		}
	}

	kfree(sb_buf);
	return found;
}

static struct block_device *find_device_by_label(const char *expected_label) {
	u8 *sb_buf = kmalloc(1024);
	if (!sb_buf) return 0;

	struct block_device *found = 0;
	usize count = blk_count();
	for (usize i = 0; i < count; i++) {
		struct block_device *dev = blk_at(i);
		if (!dev || dev->block_size != 512 || dev->block_count < 4) {
			continue;
		}
		if (blk_read_cached(dev, 2, 2, sb_buf) == 0) {
			struct ext2_superblock *sb = (struct ext2_superblock *)sb_buf;
			if (sb->s_magic == EXT2_SUPER_MAGIC) {
				if (match_label(expected_label, sb->s_volume_name)) {
					found = dev;
					break;
				}
			}
		}
	}

	kfree(sb_buf);
	return found;
}

static struct block_device *find_root_device(const char *root_val) {
	if (strncmp(root_val, "UUID=", 5) == 0) {
		const char *uuid_str = root_val + 5;
		u8 expected_uuid[16];
		if (!parse_uuid(uuid_str, expected_uuid)) {
			return 0;
		}
		return find_device_by_uuid(expected_uuid);
	} else if (strncmp(root_val, "LABEL=", 6) == 0) {
		const char *label_str = root_val + 6;
		return find_device_by_label(label_str);
	} else {
		const char *dev_name = root_val;
		if (strncmp(dev_name, "/dev/", 5) == 0) {
			dev_name += 5;
		}
		return blk_get(dev_name);
	}
}

void kernel_main(usize arg0, usize arg1)
{
	serial_init();
	serial_tty_init();
	console_init();
	console_write("b1nix kernel starting...\n");

	console_write("arg0 (magic): 0x");
	console_write_hex32((u32)arg0);
	console_write("\n");
	console_write("arg1 (info):  0x");
	console_write_hex32((u32)arg1);
	console_write("\n");

	if (arg0 != 0x36d76289) {
		console_write("Warning: Multiboot2 magic mismatch\n");
	}

#ifdef __aarch64__
	bootinfo_init_from_fdt(arg0);
#else
	bootinfo_init_from_multiboot2((u32)arg0, (u32)arg1);
#endif

	console_write("Step 1: Bootinfo parsed\n");

	/* Initialize the BSP per-CPU area (sets the GS base) FIRST: current_task is
	 * now a per-CPU accessor (get_percpu()->cur_task), and pmm/kheap diagnostics
	 * read current_task, so GS must be valid before any of that runs. */
	percpu_init();

	pmm_init(bootinfo_get());
	console_write("Step 2: PMM initialized\n");

	u64 frame = pmm_alloc_frame();
	console_write("Step 3: PMM probe frame: 0x");
	console_write_hex64(frame);
	console_write("\n");

	vmm_init();
	console_write("Step 3.5: VMM initialized\n");

	kheap_init();
	console_write("Step 4: KHeap initialized\n");

	void *heap_probe = kzalloc(64);
	console_write("Step 5: KHeap probe allocation: 0x");
	console_write_hex64((u64)(usize)heap_probe);
	console_write("\n");


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
	/* Discover CPU + interrupt topology from ACPI (RSDP -> RSDT/XSDT -> MADT)
	 * before LAPIC bring-up: smp_boot_aps prefers the MADT CPU list to the
	 * CPUID guess. Silently no-ops on platforms without ACPI. */
	acpi_init();
	lapic_init();
	/* Switch IRQ routing from the 8259 PIC to the IOAPIC discovered via
	 * ACPI. No-op (PIC stays in charge) when no IOAPIC was reported. */
	ioapic_init();

	/* M28-A: switch the BSP scheduler tick from PIT IRQ0 (vector 32) to the
	 * per-CPU LAPIC timer (vector 64) at 100 Hz. APs arm the same timer when
	 * they enter the cooperative phase in ap_main, so every core now ticks
	 * itself instead of relying on the BSP-only PIT route. */
	if (lapic_timer_start_periodic_ms(10)) {
		console_write("timer: LAPIC periodic timer armed at 100 Hz; masking PIT IRQ0\n");
		ioapic_mask_irq(0);
	} else {
		console_write("timer: LAPIC calibration unavailable, keeping PIT IRQ0 active\n");
	}
	blk_cache_init();

	initramfs_init();
	console_write("Step 10: Initramfs initialized\n");

#ifndef __aarch64__
	vfs_init();
	page_cache_init();
	ext2_init();
	ext1_init();
	ext3_init();
	fat32_init();
	ext4_init();
	ahci_init();
	nvme_init();
	btrfs_init();
	isofs_init();
	exfat_init();
	ntfs_init();
	procfs_init();
	sysfs_init();
	filelock_init();
	mqueue_init();
	shm_init();
	swap_init();
	net_init();
	ps2_kbd_init();
	ps2_mouse_init();
	input_init(); /* M47: /dev/input/event* (PS/2 kbd + mouse event streams) */
	if (bootinfo_has_flag("b1nix.gfxtest=1"))
		input_gfxtest_start(); /* diagnostic: headless window-drag injector */
	xhci_probe(); /* M37: USB xHCI controller + HID boot keyboard (real-HW input) */
	hda_init();   /* M38: Intel HDA sound controller (/dev/dsp) */
	video_init();
	compositor_init();
	virtio_gpu_init();
	fb_dev_init(); /* M47: /dev/fb0 mmap-able framebuffer (needs fb_console) */
	ramdisk_init();
	loop_init();            /* loop block devices + /dev/loop-control */
	blk_create_dev_nodes(); /* /dev/<blkdev> nodes for blkid/fdisk/loopN */
#endif
#ifndef __aarch64__
	/* Prefer a real ext4 root over the bootstrap initramfs.  Native runs use
	 * virtio-blk0; Live CD boots use the multiboot ramdisk block device ram0.
	 * In test mode the smoke suite owns the drives, so keep initramfs as /. */
	{
		int rc = -1;
		int test_mode = bootinfo_has_flag("b1nix.test=1");
		char root_val[64];
		if (bootinfo_get_kv("root", root_val, sizeof(root_val))) {
			if (strcmp(root_val, "liveiso") == 0) {
				console_write("rootfs: liveiso mount requested, searching for USB storage...\n");
				struct block_device *iso_dev = NULL;
				char mounted_iso_name[64];
				mounted_iso_name[0] = '\0';
				int retries = 0;
				while (retries < 50) {
					for (usize i = 0; i < blk_count(); i++) {
						struct block_device *dev = blk_at(i);
						if (dev && strncmp(dev->name, "usb", 3) == 0) {
							iso_dev = dev;
							break;
						}
					}
					if (iso_dev) break;
					scheduler_sleep_ticks(10);
					retries++;
				}

				int is_exfat = 0;
				if (iso_dev) {
					/* Try each USB block device (whole disk + partitions) in order with iso9660 */
					for (usize i = 0; i < blk_count(); i++) {
						struct block_device *dev = blk_at(i);
						if (dev && strncmp(dev->name, "usb", 3) == 0) {
							rc = vfs_mount(dev->name, "/mnt/iso", "iso9660", 0);
							if (rc == 0) {
								struct vfs_node *check = vfs_find_node("/mnt/iso/boot/rootfs.img");
								if (!IS_ERR(check)) {
									vfs_node_put(check);
									snprintf(mounted_iso_name, sizeof(mounted_iso_name), "%s", dev->name);
									console_write("isofs: mounted ");
									console_write(dev->name);
									console_write(" at /mnt/iso\n");
									break;
								}
								vfs_umount("/mnt/iso");
								rc = -1;
							}
						}
					}

					/* If not found on iso9660, try exfat */
					if (rc != 0) {
						for (usize i = 0; i < blk_count(); i++) {
							struct block_device *dev = blk_at(i);
							if (dev && strncmp(dev->name, "usb", 3) == 0) {
								rc = vfs_mount(dev->name, "/mnt/iso", "exfat", 0);
								if (rc == 0) {
									struct vfs_node *check = vfs_find_node("/mnt/iso/boot/rootfs.img");
									if (!IS_ERR(check)) {
										vfs_node_put(check);
										is_exfat = 1;
										snprintf(mounted_iso_name, sizeof(mounted_iso_name), "%s", dev->name);
										console_write("exfat: mounted ");
										console_write(dev->name);
										console_write(" at /mnt/iso\n");
										break;
									}
									vfs_umount("/mnt/iso");
									rc = -1;
								}
							}
						}
					}
				}

				if (rc == 0) {
					struct block_device *loop_dev = loop_register_file("/mnt/iso/boot/rootfs.img", "loop0");
					if (loop_dev && !IS_ERR(loop_dev)) {
						if (is_exfat) {
							console_write("livefile: loop0 backing /boot/rootfs.img\n");
						} else {
							console_write("loop: loop0 backing /boot/rootfs.img\n");
						}
						/* If we are running in test/smoke mode, we mount at /mnt/root
						 * to keep the bootstrap initramfs as active root.
						 * Otherwise (on real hardware or normal boots), we mount
						 * loop0 as the primary rootfs at /. */
						const char *target = test_mode ? "/mnt/root" : "/";
						rc = vfs_mount("loop0", target, "ext4", 0);
						if (rc == 0) {
							if (test_mode) {
								console_write("rootfs: loop0 mounted at /mnt/root as ext4\n");
							} else {
								console_write("rootfs: loop0 mounted at / as ext4\n");
								vfs_repopulate_after_root_mount();
								if (mounted_iso_name[0] != '\0') {
									int remount_rc = vfs_mount(mounted_iso_name, "/mnt/iso", is_exfat ? "exfat" : "iso9660", 0);
									if (remount_rc == 0) {
										if (is_exfat) {
											console_write("exfat: remounted at /mnt/iso after root switch\n");
										} else {
											console_write("isofs: remounted at /mnt/iso after root switch\n");
										}
									} else {
										char err_buf[80];
										snprintf(err_buf, sizeof(err_buf), "%s: remount after root switch failed: %d\n", is_exfat ? "exfat" : "isofs", remount_rc);
										console_write(err_buf);
									}
								}
							}
							if (is_exfat) {
								console_write("M37-LIVEFILE: ok exfat-loop-root\n");
							} else {
								console_write("M37-LIVEISO: ok isofs-loop-root\n");
							}
						}
					} else {
						rc = -1;
					}
				}

				if (rc != 0) {
					console_write("rootfs: liveiso mount failed, falling back to ram0...\n");
					rc = vfs_mount("ram0", "/", "ext4", 0);
					if (rc == 0) {
						console_write("rootfs: ram0 mounted at / (Live CD fallback)\n");
						vfs_repopulate_after_root_mount();
					} else {
						char mount_err_buf[64];
						snprintf(mount_err_buf, sizeof(mount_err_buf), "rootfs: fallback mount failed: %d\n", rc);
						console_write(mount_err_buf);
						vfs_repopulate_after_root_mount();
					}
				}
			} else {
				struct block_device *root_dev = find_root_device(root_val);
				if (!root_dev) {
					console_write("rootfs: waiting for root device ");
					console_write(root_val);
					console_write("...\n");
					int retries = 0;
					while (retries < 50) {
						scheduler_sleep_ticks(10);
						root_dev = find_root_device(root_val);
						if (root_dev) {
							break;
						}
						retries++;
					}
				}
				if (root_dev) {
					const char *fs_types[] = {"ext4", "ext3", "ext2"};
					for (int i = 0; i < 3; i++) {
						rc = vfs_mount(root_dev->name, "/", fs_types[i], 0);
						if (rc == 0) {
							char mounted_buf[96];
							snprintf(mounted_buf, sizeof(mounted_buf), "rootfs: %s mounted at / as %s\n", root_dev->name, fs_types[i]);
							console_write(mounted_buf);
							vfs_repopulate_after_root_mount();
							break;
						}
					}
				}
				if (rc != 0) {
					char mount_err_buf[96];
					snprintf(mount_err_buf, sizeof(mount_err_buf), "rootfs: staying on initramfs, failed to mount root: %s\n", root_val);
					console_write(mount_err_buf);
					vfs_repopulate_after_root_mount();
				}
			}
		} else {
			if (!test_mode) {
				rc = vfs_mount("virtio-blk0", "/", "ext4", 0);
			}
			if (rc == 0) {
				console_write("rootfs: virtio-blk0 mounted at /\n");
				vfs_repopulate_after_root_mount();
			} else {
				if (!test_mode) {
					/* Try finding a block device by default label 'b1nix-root' (e.g. USB flash drive) */
					struct block_device *root_dev = find_device_by_label("b1nix-root");
					if (root_dev) {
						const char *fs_types[] = {"ext4", "ext3", "ext2"};
						for (int i = 0; i < 3; i++) {
							rc = vfs_mount(root_dev->name, "/", fs_types[i], 0);
							if (rc == 0) {
								char mounted_buf[96];
								snprintf(mounted_buf, sizeof(mounted_buf), "rootfs: %s (label b1nix-root) mounted at / as %s\n", root_dev->name, fs_types[i]);
								console_write(mounted_buf);
								vfs_repopulate_after_root_mount();
								break;
							}
						}
					}
				}
				if (rc != 0) {
					if (!test_mode)
						rc = vfs_mount("ram0", "/", "ext4", 0);
					if (rc == 0) {
						console_write("rootfs: ram0 mounted at / (Live CD)\n");
						vfs_repopulate_after_root_mount();
					} else {
						char mount_err_buf[64];
						snprintf(mount_err_buf, sizeof(mount_err_buf), "rootfs: staying on initramfs, mount failed: %d\n", rc);
						console_write(mount_err_buf);
						vfs_repopulate_after_root_mount();
					}
				}
			}
		}
	}

	/* M34: mount synthetic filesystems after the final root is selected, so
	 * they sit on top of /proc and /sys in the real root image. */
	vfs_mkdir("/proc", 0555);
	if (vfs_mount("proc", "/proc", "procfs", 0) == 0)
		console_write("procfs: mounted at /proc\n");
	vfs_mkdir("/sys", 0555);
	if (vfs_mount("sys", "/sys", "sysfs", 0) == 0)
		console_write("sysfs: mounted at /sys\n");
	/* BusyBox sysctl chdirs to /proc/sys; point at /sys where the actual
	 * files live (kernel.osrelease → /sys/kernel/osrelease). */
	vfs_symlink("/sys", "/proc/sys");
#endif
	console_write("Step 11: Drivers initialized\n");

	/* Bring up Application Processors */
	smp_boot_aps();

	/* M28 T4: enable cross-CPU TLB shootdown. With BKL out of syscall_entry.S
	 * (T4), two CPUs can simultaneously execute vmm_unmap_page on different
	 * pml4s; without shootdown, the stale TLB entry on another CPU lets a
	 * write hit the freed-and-reused physical frame. Safe to enable here
	 * because all APs have come up and have functional LAPICs to ACK IPIs. */
	tlb_shootdown_set_enabled(1);

	/* M24b: verify cross-CPU work-stealing (no-op outside test mode / single CPU) */
	smp_selftest_run();

	/* M28 #4: measure heap_lock contention across cores (decides whether a
	 * per-CPU kmalloc magazine is worth its fragmentation cost). Same
	 * stealable-worker window as the self-test; no-op outside test mode / single CPU. */
	m28_heapbench_run();

	/* M28 #9: ctx-switch + light-syscall rdtsc baseline (single-CPU, test mode). */
	m28_ctxbench_run();

	/* M35: verify kallsyms symbolication resolves kernel addresses. */
	m35_diag_run();

	/* M36: verify the GDB serial-stub protocol engine and ftrace tracer. */
	m36_diag_run();

	/* The work-stealing self-test is done; let APs leave the work-stealing-only
	 * loop and run the full cooperative scheduler (ordinary userspace processes)
	 * under the Big Kernel Lock. From here, userspace runs on Application
	 * Processors too. */
	g_ap_userspace_enabled = 1;

	userspace_init();
	int init_pid = user_spawn("/bin/init", 0, 0);
	char init_spawn_buf[64];
	snprintf(init_spawn_buf, sizeof(init_spawn_buf), "init spawn result: %d\n", init_pid);
	console_write(init_spawn_buf);

	/* BSP idle loop. The BKL is fully retired (M28 #7): kernel entry runs
	 * BKL-free and scheduler_yield no longer hands a lock across the context
	 * switch, so the idle loop just yields and parks when there is no work. */
	while (scheduler_task_count() > 1) {
		int switched = scheduler_yield();
		if (!switched) {
			__asm__ volatile("sti; hlt" : : : "memory");
		}
	}

	console_write("\nM6 network layer demo complete\n");
	arch_halt();
}
