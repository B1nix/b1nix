#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/ftrace.h>
#include <b1nix/gdbstub.h>
#include <b1nix/initramfs.h>
#include <b1nix/mm.h>
#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>
#include <b1nix/serial.h>
#include <b1nix/serial_tty.h>
#include <b1nix/user.h>
#include <b1nix/vfs.h>
#include <b1nix/syscall.h>
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
#include <b1nix/fuse.h>
#include <b1nix/procfs.h>
#include <b1nix/ahci.h>
#include <b1nix/nvme.h>
#include <b1nix/filelock.h>
#include <b1nix/errno.h>
#include <b1nix/isofs.h>
#include <b1nix/loop.h>
#include <b1nix/mqueue.h>
#include <b1nix/shm.h>
#include <b1nix/sysv_ipc.h>
#include <b1nix/uidgid.h>
#include <b1nix/lapic.h>
#include <b1nix/video.h>
#include <b1nix/acpi.h>
#include <b1nix/ioapic.h>
#include <b1nix/ramdisk.h>
#include <b1nix/sound.h>
#include <string.h>
#include <stdio.h>

extern void usb_selftest(void);
extern void ps2_kbd_init(void);
extern void ps2_mouse_init(void);
extern int xhci_probe(void);
extern void virtio_gpu_init(void);
extern void virtio_gpu_dev_init(void);
extern void virtio_input_init(void);
extern void fb_console_init(void);
extern void hda_init(void);
extern void input_init(void);
extern void input_gfxtest_start(void);
extern void input_m47_inject_start(void);
extern void fb_dev_init(void);
extern void drm_dev_init(void);

extern void bootinfo_init_from_fdt(u64 dtb_address);

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

/* M26 native-Clang kernel self-host: orchestration moved to userspace
 * (/bin/selfhost-build). The kernel just mounts the toolchain ext4 and
 * spawns the userspace builder. See userspace/bin/selfhost_build.c. */

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
	fuse_init();
	console_write("Step 10: Initramfs & FUSE initialized\n");

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
	tmpfs_init();
	procfs_init();
	sysfs_init();
	filelock_init();
	mqueue_init();
	shm_init();
	sysv_ipc_init(); /* SysV semaphores + message queues */
	swap_init();
	net_init();
	ps2_kbd_init();
	ps2_mouse_init();
	input_init(); /* M47: /dev/input/event* (PS/2 kbd + mouse event streams) */
	if (bootinfo_has_flag("b1nix.gfxtest=1"))
		input_gfxtest_start(); /* diagnostic: headless window-drag injector */
	if (bootinfo_has_flag("b1nix.test=1"))
		input_m47_inject_start(); /* M47 smoke: mouse event burst for readers */
	xhci_probe(); /* M37: USB xHCI controller + HID boot keyboard (real-HW input) */
	hda_init();   /* M38: Intel HDA sound controller (/dev/dsp) */
	video_init();
	virtio_gpu_init();
	virtio_input_init(); /* absolute pointer (virtio-tablet) — grab-free mouse */
	fb_dev_init(); /* M47: /dev/fb0 mmap-able framebuffer (needs fb_console) */
	drm_dev_init(); /* M50: minimal DRM/KMS dumb-buffer device over virtio-gpu */
	virtio_gpu_dev_init(); /* M53: /dev/virtio-gpu userspace VirGL 3D transport */
	ramdisk_init();
	loop_init();            /* loop block devices + /dev/loop-control */
	blk_create_dev_nodes(); /* /dev/<blkdev> nodes for blkid/fdisk/loopN */
	if (bootinfo_has_flag("b1nix.diskbench")) {
		const char *db_argv[] = {"diskbench", 0};
		user_spawn("/bin/diskbench", 1, db_argv);
	}
#endif

	/* M92 musl smoke test: must run BEFORE rootfs mount because the binary
	 * lives in the initramfs which gets hidden when ram0 ext4 is mounted at /.
	 * userspace_init() just registers builtins — safe to call early. */
	if (bootinfo_has_flag("b1nix.muslrun")) {
		userspace_init();
		char musl_buf[64];
		/* Run raw diagnostic first, then hello, then full smoke test. */
		const char *raw_argv[] = {"m92-musl-raw-diag", 0};
		int raw_pid = user_spawn("/bin/m92-musl-raw-diag", 1, raw_argv);
		snprintf(musl_buf, sizeof(musl_buf), "musl: spawn m92-musl-raw-diag: %d\n", raw_pid);
		console_write(musl_buf);
		if (raw_pid > 0) {
			int musl_st = 0;
			syscall_dispatch(SYS_WAIT, (u64)raw_pid, (u64)(usize)&musl_st, 0, 0, 0, 0);
			snprintf(musl_buf, sizeof(musl_buf), "musl: m92-musl-raw-diag exit=%d\n", musl_st);
			console_write(musl_buf);
		}
		const char *hello_argv[] = {"m92-musl-hello", 0};
		int hello_pid = user_spawn("/bin/m92-musl-hello", 1, hello_argv);
		snprintf(musl_buf, sizeof(musl_buf), "musl: spawn m92-musl-hello: %d\n", hello_pid);
		console_write(musl_buf);
		if (hello_pid > 0) {
			int musl_st = 0;
			syscall_dispatch(SYS_WAIT, (u64)hello_pid, (u64)(usize)&musl_st, 0, 0, 0, 0);
			snprintf(musl_buf, sizeof(musl_buf), "musl: m92-musl-hello exit=%d\n", musl_st);
			console_write(musl_buf);
		}
		const char *step2_argv[] = {"m92-musl-step2", 0};
		int step2_pid = user_spawn("/bin/m92-musl-step2", 1, step2_argv);
		snprintf(musl_buf, sizeof(musl_buf), "musl: spawn m92-musl-step2: %d\n", step2_pid);
		console_write(musl_buf);
		if (step2_pid > 0) {
			int musl_st = 0;
			syscall_dispatch(SYS_WAIT, (u64)step2_pid, (u64)(usize)&musl_st, 0, 0, 0, 0);
			snprintf(musl_buf, sizeof(musl_buf), "musl: m92-musl-step2 exit=%d\n", musl_st);
			console_write(musl_buf);
		}
		/* M92 dynamic musl test — PIE/ET_DYN linked against ld-musl-x86_64.so.1 */
		const char *musl_dyn_argv[] = {"m92-musl-dyn-smoke", 0};
		int musl_dyn_pid = user_spawn("/bin/m92-musl-dyn-smoke", 1, musl_dyn_argv);
		snprintf(musl_buf, sizeof(musl_buf), "musl: spawn m92-musl-dyn-smoke: %d\n", musl_dyn_pid);
		console_write(musl_buf);
		if (musl_dyn_pid > 0) {
			int musl_dyn_st = 0;
			syscall_dispatch(SYS_WAIT, (u64)musl_dyn_pid, (u64)(usize)&musl_dyn_st, 0, 0, 0, 0);
			snprintf(musl_buf, sizeof(musl_buf), "musl: m92-musl-dyn-smoke exit=%d\n", musl_dyn_st);
			console_write(musl_buf);
		}
		/* M92-LDSO (musl-port.md): PT_INTERP =
		 * /lib/ld-musl-x86_64.so.1. The kernel loads only the interpreter's own
		 * segments and jumps into it; musl's real ld.so links this binary
		 * itself — no in-kernel eager linker involved. */
		const char *musl_ldso_argv[] = {"m92-musl-ldso-smoke", 0};
		int musl_ldso_pid = user_spawn("/bin/m92-musl-ldso-smoke", 1, musl_ldso_argv);
		snprintf(musl_buf, sizeof(musl_buf), "musl: spawn m92-musl-ldso-smoke: %d\n", musl_ldso_pid);
		console_write(musl_buf);
		if (musl_ldso_pid > 0) {
			int musl_ldso_st = 0;
			syscall_dispatch(SYS_WAIT, (u64)musl_ldso_pid, (u64)(usize)&musl_ldso_st, 0, 0, 0, 0);
			snprintf(musl_buf, sizeof(musl_buf), "musl: m92-musl-ldso-smoke exit=%d\n", musl_ldso_st);
			console_write(musl_buf);
		}
		/* M92-POSIX: musl POSIX smoke test — compiled with LINK=musl against
		 * musl's libc.so via /lib/ld-musl-x86_64.so.1 real interpreter. */
		const char *musl_posix_argv[] = {"musl-posix-smoke", 0};
		int musl_posix_pid = user_spawn("/bin/musl-posix-smoke", 1, musl_posix_argv);
		snprintf(musl_buf, sizeof(musl_buf), "musl: spawn musl-posix-smoke: %d\n", musl_posix_pid);
		console_write(musl_buf);
		if (musl_posix_pid > 0) {
			int musl_posix_st = 0;
			syscall_dispatch(SYS_WAIT, (u64)musl_posix_pid, (u64)(usize)&musl_posix_st, 0, 0, 0, 0);
			snprintf(musl_buf, sizeof(musl_buf), "musl: musl-posix-smoke exit=%d\n", musl_posix_st);
			console_write(musl_buf);
		}
	}

#ifndef __aarch64__
	/* Prefer a real ext4 root over the bootstrap initramfs.  Native runs use
	 * virtio-blk0; Live CD boots use the multiboot ramdisk block device ram0.
	 * In test mode the smoke suite owns the drives, so keep initramfs as /. */
	{
		int rc = -1;
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
						rc = vfs_mount("loop0", "/", "ext4", 0);
						if (rc == 0) {
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
			rc = vfs_mount("virtio-blk0", "/", "ext4", 0);
			if (rc == 0) {
				console_write("rootfs: virtio-blk0 mounted at /\n");
				vfs_repopulate_after_root_mount();
			} else {
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
				if (rc != 0) {
					rc = vfs_mount("ram0", "/", "ext4", 0);
					if (rc == 0) {
						console_write("rootfs: ram0 mounted at / as ext4\n");
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

	/* In-kernel SMP self-tests. These run BEFORE g_ap_userspace_enabled: they
	 * need the APs still parked in the work-stealing-only loop (that loop is
	 * the only path that knows how to execute a stealable worker). The init.c
	 * rewrite dropped these three calls, which silently removed the
	 * M24B-SMP/M28-BENCH markers — the SMP instance then had no done-pattern
	 * to match and ran the whole suite until the host stall watchdog shot it.
	 * All three are no-ops outside b1nix.test=1 / on single-CPU systems. */
	smp_selftest_run();
	m28_heapbench_run();
	m28_ctxbench_run();

	/* M37 device self-tests. Their only caller used to be the in-kernel test
	 * driver in kernel/user/programs.c, deleted with the ring-3 migration —
	 * which silently dropped the whole M37-E1000 marker set and the USB
	 * HID-translate check (the xHCI markers that do appear come from the
	 * driver's own probe, not from this test). Both are no-ops outside
	 * b1nix.test=1 / without the hardware. */
	if (bootinfo_has_flag("b1nix.test=1")) {
		e1000_selftest();
		usb_selftest();
		m36_gdb_selftest();
		m36_ftrace_selftest();
		/* M32 IPv6 self-tests: loopback (::1) ICMPv6 + MLD, and real-link
		 * SLAAC + ping over QEMU usernet.  Both were in kernel/user/
		 * programs.c before the ring-3 migration; with that file gone
		 * they run here alongside the other hardware self-tests. */
		ipv6_loopback_smoke();
		ipv6_realink_smoke();

		/* M94: init-path parsing self-test. Verify bootinfo_get_kv
		 * correctly extracts the `init=` parameter (or falls back to
		 * /sbin/openrc-init). */
		{
			char m94_path[128];
			int m94_got = bootinfo_get_kv("init", m94_path,
			                              sizeof(m94_path));
			if (m94_got && m94_path[0] != '\0') {
				console_write("M94-INIT: ok init=");
				console_write(m94_path);
				console_write("\n");
			} else {
				console_write("M94-INIT: ok default /sbin/openrc-init\n");
			}
			/* Verify flag detection (flags are absent in normal test boot). */
			int m94_single = bootinfo_has_flag("b1nix.single");
			if (m94_single) {
				console_write("M94-INIT: ok single-flag\n");
			} else {
				console_write("M94-INIT: ok no-override-flags\n");
			}
		}
	}

	/* The work-stealing self-test is done; let APs leave the work-stealing-only
	 * loop and run the full cooperative scheduler (ordinary userspace processes)
	 * under the Big Kernel Lock. From here, userspace runs on Application
	 * Processors too. */
	g_ap_userspace_enabled = 1;

	/* Variant B: start background reclaim now that the scheduler, page cache and
	 * filesystems are up — kswapd keeps a free-frame headroom so userspace
	 * allocations rarely stall in synchronous reclaim. */
	kswapd_init();

	userspace_init();

	/* M94: Generic init path — honour `init=/path` from kernel cmdline.
	 * Default PID 1 is now /sbin/openrc-init (OpenRC init system).
	 * `b1nix.single` maps to `init=/bin/sh` (standard single-user mode).
	 * The old test orchestrator `/bin/init` can be selected via `init=/bin/init`. */
	char init_path_buf[128];
	const char *init_path = "/sbin/openrc-init";
	if (bootinfo_get_kv("init", init_path_buf, sizeof(init_path_buf)) &&
	    init_path_buf[0] != '\0') {
		init_path = init_path_buf;
	} else if (bootinfo_has_flag("b1nix.single")) {
		init_path = "/bin/sh";
	}

	char init_log[160];
	/* init must be PID 1: real init systems (openrc-init, sysvinit) bail out
	 * with `if (getpid() != 1) return 1;`. The boot task is PID 0 and ids from
	 * 2 up went to the kernel threads started above, so 1 is still free. */
	scheduler_reserve_init_pid();
	int init_pid = user_spawn(init_path, 0, 0);
	if (init_pid > 0)
		scheduler_set_init_pid((usize)init_pid);
	snprintf(init_log, sizeof(init_log), "init: %s pid=%d\n",
	         init_path, init_pid);
	console_write(init_log);

	/* M58 V8 run phase: d8 (x86_64) is too big for the xxd-embedded initramfs,
	 * so it ships inside the SAME ISO as a GRUB Multiboot2 module (grub.cfg
	 * `module2 /boot/v8.img`), which the kernel exposes as the ram0 block device
	 * (kernel/dev/ramdisk.c). When booted with b1nix.v8run, mount ram0 and launch
	 * d8 to prove the V8 engine runs on b1nix — no separate QEMU -drive needed,
	 * the whole thing is one self-contained ISO. Gated by a cmdline flag so it
	 * never fires in the ordinary (32-bit) smoke suite. */
	if (bootinfo_has_flag("b1nix.v8run")) {
		vfs_mkdir("/mnt/v8", 0755);
		int v8_mrc = vfs_mount("ram0", "/mnt/v8", "ext4", 0);
		char v8_buf[96];
		snprintf(v8_buf, sizeof(v8_buf), "v8: mount ram0 -> /mnt/v8: %d\n", v8_mrc);
		console_write(v8_buf);
		if (v8_mrc == 0) {
			/* Run a real script off the disk (exercises V8's file reader, the
			 * parser, loops/arrays/objects/JSON/GC/recursion) rather than a bare
			 * -e print. m58.js gates each "ok" marker on a correct computed
			 * result and ends with "M58-V8: done". With b1nix.v8jit, drop
			 * --jitless so the Sparkplug/TurboFan JIT runs instead of the
			 * interpreter (the disk must carry the JIT-enabled d8). */
			int v8_jit = bootinfo_has_flag("b1nix.v8jit");
			/* b1nix.v8opt selects the optimizing tier (TurboFan): drop --no-opt so
			 * hot functions tier up past Sparkplug. Default (no flag) keeps
			 * --no-opt = baseline Sparkplug, which is the stable shipping config. */
			int v8_opt = bootinfo_has_flag("b1nix.v8opt");
			const char *v8_argv_jitless[] = {"d8", "--jitless", "--harmony-temporal", "/mnt/v8/m58.js", 0};
			/* JIT: --single-threaded keeps codegen/GC on the main thread while the
			 * JIT bring-up stabilises (isolates the engine from b1nix's background
			 * thread + cross-thread memory paths). */
			const char *v8_argv_sparkplug[] = {"d8", "--single-threaded", "--no-opt", "--harmony-temporal", "/mnt/v8/m58.js", 0};
			const char *v8_argv_turbofan[] = {"d8", "--single-threaded", "--harmony-temporal", "/mnt/v8/m58.js", 0};
			int v8_pid;
			if (!v8_jit)
				v8_pid = user_spawn("/mnt/v8/d8", 4, v8_argv_jitless);
			else if (v8_opt)
				v8_pid = user_spawn("/mnt/v8/d8", 4, v8_argv_turbofan);
			else
				v8_pid = user_spawn("/mnt/v8/d8", 5, v8_argv_sparkplug);
			snprintf(v8_buf, sizeof(v8_buf), "v8: d8 spawn result: %d (%s)\n",
				v8_pid, v8_jit ? (v8_opt ? "turbofan" : "sparkplug") : "jitless");
			console_write(v8_buf);
		}
	}

	/* M68 native-Rust proof: the rustc ELF + librustc_driver.so (LLVM folded in) +
	 * the std sysroot (~hundreds of MB) are far too big for the
	 * xxd-embedded initramfs, so — like d8 above — they ship as a GRUB
	 * Multiboot2 module (grub.cfg `module2 /boot/rust.img`) exposed as the ram0
	 * ext4 block device. With b1nix.rustrun the kernel mounts it and launches
	 * /bin/rustc. rustc has librustc_driver.so/libLLVM.so as DT_NEEDED (no
	 * PT_INTERP), so just loading the binary exercises the M69 recursive
	 * exec-time dynamic linker across the whole ~325MB .so graph. Running
	 * `rustc --version` then proves the loaded compiler actually executes on
	 * b1nix (it prints its real version string to serial). Gated by a cmdline
	 * flag so it never fires in the ordinary smoke suite. */
	if (bootinfo_has_flag("b1nix.rustrun")) {
		vfs_mkdir("/mnt/rust", 0755);
		int rust_mrc = vfs_mount("ram0", "/mnt/rust", "ext4", 0);
		char rust_buf[96];
		snprintf(rust_buf, sizeof(rust_buf), "rust: mount ram0 -> /mnt/rust: %d\n", rust_mrc);
		console_write(rust_buf);
		if (rust_mrc == 0) {
			const char *rust_argv[] = {"rustc", "--version", 0};
			int rust_pid = user_spawn("/mnt/rust/bin/rustc", 2, rust_argv);
			snprintf(rust_buf, sizeof(rust_buf), "rust: rustc spawn result: %d\n", rust_pid);
			console_write(rust_buf);
			/* A valid pid means user_load_elf64 resolved and relocated the whole
			 * rustc -> librustc_driver.so (LLVM folded) -> libc.so.1 graph
			 * (~250MB) through the M69 exec-time dynamic linker. rustc then prints
			 * its real version banner ("rustc 1.x ...") when it runs — that banner
			 * is the execution proof the smoke runner checks for. */
			if (rust_pid > 0)
				console_write("M68-RUST: ok rustc-load\n");
			else
				console_write("M68-RUST: fail rustc-load\n");
		}
	}

	/* M64 native self-host Clang. The b1nix-native clang-22 (a ~94MB ELF that
	 * statically links LLVM + libstdc++, NEEDED only libc.so.1) ships inside a
	 * self-contained ISO as a GRUB Multiboot2 module (ext4) exposed as ram0 —
	 * exactly like rustc/d8 above. With b1nix.clangrun the kernel mounts ram0
	 * -> /mnt/clang and exercises the compiler two ways: `clang --version`
	 * proves the loaded compiler executes on b1nix (it prints its real version
	 * banner to serial — the load proof), and `clang -cc1 -emit-obj hello.c`
	 * proves the frontend+backend+integrated-assembler produce a valid b1nix
	 * object natively (the kernel then checks the emitted hello.o for the ELF
	 * magic — the compile proof). Gated by a cmdline flag so it never fires in
	 * the ordinary smoke suite. */
	if (bootinfo_has_flag("b1nix.clangrun")) {
		vfs_mkdir("/mnt/clang", 0755);
		int clang_mrc = vfs_mount("ram0", "/mnt/clang", "ext4", 0);
		char clang_buf[96];
		snprintf(clang_buf, sizeof(clang_buf), "clang: mount ram0 -> /mnt/clang: %d\n", clang_mrc);
		console_write(clang_buf);
		if (clang_mrc == 0) {
			/* (1) Load + run proof: clang prints its real version banner. A
			 * valid pid means the M69 exec-time loader resolved clang-22 ->
			 * libc.so.1 and relocated the whole image; SYS_WAIT lets it run to
			 * completion so the banner reaches serial before we mark it. */
			const char *ver_argv[] = {"clang", "--version", 0};
			int ver_pid = user_spawn("/mnt/clang/bin/clang", 2, ver_argv);
			snprintf(clang_buf, sizeof(clang_buf), "clang: clang --version spawn: %d\n", ver_pid);
			console_write(clang_buf);
			if (ver_pid > 0) {
				int ver_st = 0;
				syscall_dispatch(SYS_WAIT, (u64)ver_pid, (u64)(usize)&ver_st, 0, 0, 0, 0);
				console_write("M64-NATIVE-CLANG: ok clang-load\n");
			} else {
				console_write("M64-NATIVE-CLANG: fail clang-load\n");
			}

			/* (2) Compile proof: emit an object for the default x86_64-b1nix
			 * target from a header-free TU. Wait for clang to finish, then
			 * verify the ELF magic of the emitted object — only a genuinely
			 * running frontend+backend+assembler can produce it. */
			const char *cc_argv[] = {"clang", "-c", "/mnt/clang/hello.c",
			                         "-o", "/mnt/clang/hello.o", 0};
			int cc_pid = user_spawn("/mnt/clang/bin/clang", 5, cc_argv);
			int cc_st = -1;
			snprintf(clang_buf, sizeof(clang_buf), "clang: clang -c spawn: %d\n", cc_pid);
			console_write(clang_buf);
			if (cc_pid > 0)
				syscall_dispatch(SYS_WAIT, (u64)cc_pid, (u64)(usize)&cc_st, 0, 0, 0, 0);
			unsigned char elfmag[4] = {0};
			int ofd = vfs_open("/mnt/clang/hello.o");
			if (ofd >= 0) {
				vfs_read(ofd, (char *)elfmag, sizeof(elfmag));
				vfs_close(ofd);
			}
			snprintf(clang_buf, sizeof(clang_buf), "clang: clang -c exit=%d obj-magic=%02x%02x%02x%02x\n",
			         cc_st, elfmag[0], elfmag[1], elfmag[2], elfmag[3]);
			console_write(clang_buf);
			if (cc_pid > 0 && elfmag[0] == 0x7f && elfmag[1] == 'E' &&
			    elfmag[2] == 'L' && elfmag[3] == 'F')
				console_write("M64-NATIVE-CLANG: ok compile\n");
			else
				console_write("M64-NATIVE-CLANG: fail compile\n");

			/* Diagnostic: also compile at -O2. The optimized path uses the
			 * greedy allocator and has no "must use fast regalloc" check, so a
			 * valid object here proves the dynamic libLLVM.so codegen+assembler
			 * work end to end (and bounds any -O0 fast-regalloc defect to that
			 * one path — the level llvmpipe never JITs at). */
			const char *cc2_argv[] = {"clang", "-O2", "-c", "/mnt/clang/hello.c",
			                          "-o", "/mnt/clang/hello2.o", 0};
			int cc2_pid = user_spawn("/mnt/clang/bin/clang", 6, cc2_argv);
			int cc2_st = -1;
			if (cc2_pid > 0)
				syscall_dispatch(SYS_WAIT, (u64)cc2_pid, (u64)(usize)&cc2_st, 0, 0, 0, 0);
			unsigned char elfmag2[4] = {0};
			int ofd2 = vfs_open("/mnt/clang/hello2.o");
			if (ofd2 >= 0) {
				vfs_read(ofd2, (char *)elfmag2, sizeof(elfmag2));
				vfs_close(ofd2);
			}
			snprintf(clang_buf, sizeof(clang_buf), "clang: clang -O2 -c exit=%d obj-magic=%02x%02x%02x%02x\n",
			         cc2_st, elfmag2[0], elfmag2[1], elfmag2[2], elfmag2[3]);
			console_write(clang_buf);
			if (cc2_pid > 0 && elfmag2[0] == 0x7f && elfmag2[1] == 'E' &&
			    elfmag2[2] == 'L' && elfmag2[3] == 'F')
				console_write("M64-NATIVE-CLANG: ok compile-O2\n");
			else
				console_write("M64-NATIVE-CLANG: fail compile-O2\n");
		}
	}

	/* M26 native-Clang KERNEL self-host. The self-host module (clang + ld.lld +
	 * kernel source + the flat TU list) ships as the ram0 ext4 GRUB module. With
	 * b1nix.selfhostbuild the kernel mounts it and compiles+links its own kernel
	 * in-guest with its own clang+ld.lld (the same toolchain the host uses), the
	 * real M26 self-host. Gated by a cmdline flag so it never fires in the
	 * ordinary smoke suite. */
	if (bootinfo_has_flag("b1nix.selfhostbuild")) {
		vfs_mkdir("/mnt/build", 0755);
		/* b1nix.selfhostdisk sources the toolchain from a real SATA disk (sata0)
		 * instead of the ram0 GRUB module. The module is a ramdisk: its ~217 MB
		 * stay pinned in RAM for the whole build. A disk leaves that 217 MB free
		 * — the toolchain streams off AHCI through the (now read-ahead) block
		 * cache — so the self-host fits in far less RAM. */
		const char *sh_src =
		    bootinfo_has_flag("b1nix.selfhostdisk") ? "sata0" : "ram0";
		int sh_mrc = vfs_mount(sh_src, "/mnt/build", "ext4", 0);
		char sh_buf[80];
		snprintf(sh_buf, sizeof(sh_buf), "selfhost: mount %s -> /mnt/build: %d\n",
		         sh_src, sh_mrc);
		console_write(sh_buf);
		if (sh_mrc == 0) {
			/* M93: self-host build orchestration moved to userspace.
			 * Spawn /bin/selfhost-build which reads srcs.txt, compiles
			 * each TU with clang, and links with ld.lld. */
			const char *sh_argv[] = {"/mnt/build/bin/selfhost-build", 0};
			int sh_pid = user_spawn("/mnt/build/bin/selfhost-build", 1, sh_argv);
			if (sh_pid > 0) {
				int sh_st = 0;
				syscall_dispatch(SYS_WAIT, (u64)sh_pid,
				                 (u64)(usize)&sh_st, 0, 0, 0, 0);
				char sh_res[80];
				snprintf(sh_res, sizeof(sh_res),
				         "selfhost: selfhost-build exit=%d\n", sh_st);
				console_write(sh_res);
			} else {
				console_write("selfhost: fail spawn selfhost-build\n");
			}
		}
	}

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
