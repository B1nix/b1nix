ARCH ?= x86
BUILD_DIR := build/$(ARCH)
KERNEL_ELF := $(BUILD_DIR)/kernel.elf
RUN_ISO := /tmp/b1nix-run.iso
INITRAMFS_NATIVE_SMOKE_INC := $(BUILD_DIR)/initramfs_native_smoke.inc
INITRAMFS_M12_SMOKE_INC := $(BUILD_DIR)/initramfs_m12_smoke.inc
INITRAMFS_M13_SMOKE_INC := $(BUILD_DIR)/initramfs_m13_smoke.inc
INITRAMFS_M13_JOB_CONTROL_INC := $(BUILD_DIR)/initramfs_m13_job_control.inc
INITRAMFS_M8_AIO_TEST_INC := $(BUILD_DIR)/initramfs_m8_aio_test.inc
INITRAMFS_M17_SMOKE_INC := $(BUILD_DIR)/initramfs_m17_smoke.inc
INITRAMFS_M14_SMOKE_INC := $(BUILD_DIR)/initramfs_m14_smoke.inc
INITRAMFS_M15_SMOKE_INC := $(BUILD_DIR)/initramfs_m15_smoke.inc
INITRAMFS_TCC_FILES_INC := $(BUILD_DIR)/initramfs_tcc_files.inc
INITRAMFS_M25_SMOKE_INC := $(BUILD_DIR)/initramfs_m25_smoke.inc
AP_TRAMPOLINE_INC := $(BUILD_DIR)/ap_trampoline.inc

CC := clang
LD := $(shell command -v ld.lld 2>/dev/null || printf '%s' /opt/homebrew/opt/lld/bin/ld.lld)
MKE2FS := $(shell command -v mke2fs 2>/dev/null || command -v /sbin/mke2fs 2>/dev/null || printf '%s' /opt/homebrew/opt/e2fsprogs/sbin/mke2fs)
GRUB_MKRESCUE := $(shell command -v grub-mkrescue 2>/dev/null || command -v i686-elf-grub-mkrescue 2>/dev/null || echo /opt/homebrew/bin/i686-elf-grub-mkrescue)
QEMU_X86_64 := qemu-system-x86_64
KERNEL_CMDLINE ?=

COMMON_CFLAGS := \
	-std=c11 \
	-ffreestanding \
	-fno-builtin \
	-fno-stack-protector \
	-fno-pic \
	-mno-red-zone \
	-Wall \
	-Wextra \
	-I kernel/include

ifeq ($(ARCH),x86)
TARGET := x86_64-elf
ARCH_CFLAGS := --target=$(TARGET) -mcmodel=kernel -mno-sse -mno-mmx -mno-sse2 -mno-3dnow
ARCH_LDFLAGS := -m elf_x86_64 -z max-page-size=0x1000
LINKER_SCRIPT := kernel/arch/x86/linker.ld
ASM_SOURCES := kernel/arch/x86/boot.S kernel/arch/x86/context_switch.S kernel/arch/x86/isr.S kernel/arch/x86/user_jump.S kernel/arch/x86/syscall_entry.S
ARCH_SOURCES := kernel/arch/x86/arch.c kernel/arch/x86/console.c kernel/arch/x86/fb_console.c kernel/arch/x86/interrupts.c kernel/arch/x86/io.c kernel/arch/x86/paging.c kernel/arch/x86/serial.c kernel/arch/x86/rtc.c kernel/arch/x86/signal.c kernel/arch/x86/lapic.c
else
$(error Unsupported ARCH=$(ARCH). AArch64 is archived; use ARCH=x86)
endif

KERNEL_SOURCES := \
	kernel/main.c \
	kernel/lib/string.c \
	kernel/lib/klog.c \
	kernel/lib/stdio.c \
	kernel/lib/stdlib.c \
	kernel/lib/unistd.c \
	kernel/mm/kheap.c \
	kernel/mm/pmm.c \
	kernel/mm/page_cache.c \
	kernel/mm/swap.c \
	kernel/mm/eviction.c \
	kernel/sched/scheduler.c \
	kernel/syscall/syscall.c \
	kernel/fs/initramfs.c \
	kernel/fs/vfs.c \
	kernel/fs/aio.c \
	kernel/fs/vfs_slab.c \
	kernel/fs/pipe.c \
	kernel/fs/fat32.c \
	kernel/fs/ext2.c \
	kernel/fs/ext1.c \
	kernel/fs/ext3.c \
	kernel/fs/ext4.c \
	kernel/fs/btrfs.c \
	kernel/fs/journal.c \
	kernel/fs/filelock.c \
	kernel/dev/blk.c \
	kernel/dev/video.c \
	kernel/ipc/mqueue.c \
	kernel/ipc/shm.c \
	kernel/sched/uidgid.c \
	kernel/sched/runqueue.c \
	kernel/user/process.c \
	kernel/user/programs.c \
	kernel/user/busybox.c \
	kernel/user/tui_common.c \
	kernel/user/mc.c \
	kernel/user/editor.c \
	kernel/user/nmake.c \
	$(ARCH_SOURCES)

ifeq ($(ARCH),x86)
KERNEL_SOURCES += \
	kernel/bootinfo/multiboot2.c \
	kernel/dev/pci.c \
	kernel/dev/virtio.c \
	kernel/dev/virtio_blk.c \
	kernel/dev/ahci.c \
	kernel/dev/nvme.c \
	kernel/dev/ps2_kbd.c \
	kernel/dev/ps2_mouse.c \
	kernel/dev/compositor.c \
	kernel/dev/virtio_gpu.c \
	kernel/net/net.c \
	kernel/net/socket.c \
	kernel/net/unix.c \
	kernel/net/ethernet.c \
	kernel/net/arp.c \
	kernel/net/ipv4.c \
	kernel/net/icmp.c \
	kernel/net/udp.c \
	kernel/net/tcp.c \
	kernel/net/dhcp.c \
	kernel/net/dns.c
endif


OBJECTS := \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(KERNEL_SOURCES)) \
	$(patsubst %.S,$(BUILD_DIR)/%.o,$(ASM_SOURCES))

ANALYZE_DIR := $(BUILD_DIR)/analyze

# ── Static Analysis ──
analyze: $(KERNEL_SOURCES) $(ASM_SOURCES)
	@mkdir -p $(ANALYZE_DIR)
	@echo "Running clang static analyzer..."
	@for src in $(KERNEL_SOURCES); do \
		rel=$${src#kernel/}; \
		out="$(ANALYZE_DIR)/$${rel%.c}.plist"; \
		mkdir -p "$$(dirname "$$out")"; \
		$(CC) $(COMMON_CFLAGS) $(ARCH_CFLAGS) --analyze -Xclang -analyzer-output=plist -o "$$out" -c "$$src" 2>&1 | \
			grep -v "^$$" | grep -v "analyz" | head -5 || true; \
	done
	@echo "Analysis results in $(ANALYZE_DIR)"
	@find $(ANALYZE_DIR) -name '*.plist' -exec echo "  {}" \;

.PHONY: all clean run-x86 run-root smoke-m18 root-image iso check-tools objects graphics-smoke analyze

all: $(KERNEL_ELF)

objects: $(OBJECTS)

$(KERNEL_ELF): $(OBJECTS) $(LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD) $(ARCH_LDFLAGS) -T $(LINKER_SCRIPT) -o $@ $(OBJECTS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) $(ARCH_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel/arch/x86/lapic.o: $(AP_TRAMPOLINE_INC)
$(BUILD_DIR)/kernel/fs/initramfs.o: $(INITRAMFS_NATIVE_SMOKE_INC) $(INITRAMFS_M12_SMOKE_INC) $(INITRAMFS_M13_SMOKE_INC) $(INITRAMFS_M13_JOB_CONTROL_INC) $(INITRAMFS_M8_AIO_TEST_INC) $(INITRAMFS_M17_SMOKE_INC) $(INITRAMFS_M14_SMOKE_INC) $(INITRAMFS_M15_SMOKE_INC) $(INITRAMFS_TCC_FILES_INC) $(INITRAMFS_M25_SMOKE_INC)

# Anything in userspace libc/includes/crt that affects every embedded ELF.
# Listed as prereqs of each *.inc so changes to libc force an xxd re-bundle —
# otherwise initramfs ships with stale userspace and the kernel sees old libc.
USERSPACE_DEPS := \
	$(wildcard userspace/libc/*.c) \
	$(wildcard userspace/include/*.h) \
	$(wildcard userspace/include/sys/*.h) \
	$(wildcard userspace/crt/*.S) \
	userspace/Makefile \
	userspace/linker.ld

$(INITRAMFS_NATIVE_SMOKE_INC): userspace/bin/native_smoke.S $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/bin/native_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_native_smoke_elf userspace/build/bin/native_smoke > $@

$(INITRAMFS_M12_SMOKE_INC): userspace/bin/m12_smoke.c $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/bin/m12_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m12_smoke_elf userspace/build/bin/m12_smoke > $@

$(INITRAMFS_M13_SMOKE_INC): userspace/bin/m13_smoke.c $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/bin/m13_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m13_smoke_elf userspace/build/bin/m13_smoke > $@

$(INITRAMFS_M13_JOB_CONTROL_INC): userspace/bin/m13_job_control.c $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/bin/m13_job_control
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m13_job_control_elf userspace/build/bin/m13_job_control > $@

$(INITRAMFS_M8_AIO_TEST_INC): userspace/bin/m8_aio_test.c $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/bin/m8_aio_test
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m8_aio_test_elf userspace/build/bin/m8_aio_test > $@

$(INITRAMFS_M17_SMOKE_INC): userspace/bin/m17_smoke.c $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/bin/m17_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m17_smoke_elf userspace/build/bin/m17_smoke > $@

$(INITRAMFS_M14_SMOKE_INC): userspace/bin/m14_smoke.c $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/bin/m14_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m14_smoke_elf userspace/build/bin/m14_smoke > $@

$(INITRAMFS_M15_SMOKE_INC): userspace/bin/m15_smoke.c $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/bin/m15_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m15_smoke_elf userspace/build/bin/m15_smoke > $@

$(INITRAMFS_TCC_FILES_INC): $(USERSPACE_DEPS) tools/gen_tcc_initramfs.sh $(wildcard userspace/tcc/*.c) $(wildcard userspace/tcc/include/*.h)
	@$(MAKE) -C userspace build/bin/tcc
	@mkdir -p $(dir $@)
	sh tools/gen_tcc_initramfs.sh $@

$(INITRAMFS_M25_SMOKE_INC): userspace/bin/m25_smoke.c $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/bin/m25_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m25_smoke_elf userspace/build/bin/m25_smoke > $@

# ── AP Trampoline (flat binary linked at 0x8000) ──
AP_TRAMP_OBJ := $(BUILD_DIR)/kernel/arch/x86/ap_trampoline_tmp.o
AP_TRAMP_BIN := $(BUILD_DIR)/ap_trampoline.bin

$(AP_TRAMP_OBJ): kernel/arch/x86/ap_trampoline.S
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) $(ARCH_CFLAGS) -c $< -o $@

$(AP_TRAMP_BIN): $(AP_TRAMP_OBJ)
	$(LD) -m elf_x86_64 -z max-page-size=0x1000 --image-base 0 -Ttext 0x8000 -o $@ --oformat binary $<

$(AP_TRAMPOLINE_INC): $(AP_TRAMP_BIN)
	@mkdir -p $(dir $@)
	xxd -i -n ap_trampoline_bin $< > $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) $(ARCH_CFLAGS) -c $< -o $@

iso: $(KERNEL_ELF)
	@test -n "$(GRUB_MKRESCUE)" || (echo "missing grub-mkrescue or i686-elf-grub-mkrescue"; exit 1)
	@mkdir -p $(BUILD_DIR)/iso/boot/grub
	cp $(KERNEL_ELF) $(BUILD_DIR)/iso/boot/kernel.elf
	@printf 'set timeout=0\nset default=0\n\nmenuentry "b1nix" {\n    multiboot2 /boot/kernel.elf $(KERNEL_CMDLINE)\n    boot\n}\n' > $(BUILD_DIR)/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(BUILD_DIR)/b1nix.iso $(BUILD_DIR)/iso

# ── M25 Userspace ──
userspace:
	@$(MAKE) -C userspace

userspace-install: userspace
	@$(MAKE) -C userspace install

install-native-toolchain:
	@if [ -d /root/b1nix-toolchain/native_root ]; then \
		echo "Installing native toolchain to rootfs..."; \
		mkdir -p build/x86/rootfs/lib/gcc/x86_64-b1nix/13.2.0; \
		cp -r /root/b1nix-toolchain/native_root/* build/x86/rootfs/; \
		cp /root/b1nix-toolchain/cross/lib/gcc/x86_64-b1nix/13.2.0/libgcc.a build/x86/rootfs/lib/gcc/x86_64-b1nix/13.2.0/; \
	fi

iso-full: userspace-install install-native-toolchain iso

run-x86: iso userspace-install root-image
	@command -v $(QEMU_X86_64) >/dev/null || (echo "missing qemu-system-x86_64"; exit 1)
	cp $(BUILD_DIR)/b1nix.iso $(RUN_ISO)
	$(QEMU_X86_64) -cdrom $(RUN_ISO) -serial stdio -no-reboot -boot d \
		-drive file=$(BUILD_DIR)/root.ext4,format=raw,if=virtio \
		-netdev user,id=n0 -device virtio-net-pci,netdev=n0

run-root: run-x86

root-image: userspace-install install-native-toolchain
	@mkdir -p $(BUILD_DIR)/rootfs/bin $(BUILD_DIR)/rootfs/etc $(BUILD_DIR)/rootfs/dev $(BUILD_DIR)/rootfs/home $(BUILD_DIR)/rootfs/tmp $(BUILD_DIR)/rootfs/var
	@echo "b1nix persistent root" > $(BUILD_DIR)/rootfs/etc/motd
	@# Copy userspace binaries into rootfs
	@cp $(BUILD_DIR)/rootfs/bin/* /dev/null 2>/dev/null; true
	@cp -r $(BUILD_DIR)/rootfs/* $(BUILD_DIR)/rootfs/ 2>/dev/null; true
	@dd if=/dev/zero of=$(BUILD_DIR)/root.ext4 bs=1048576 count=32 2>/dev/null
	@$(MKE2FS) -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q -d $(BUILD_DIR)/rootfs $(BUILD_DIR)/root.ext4 2>/dev/null || \
	 $(MKE2FS) -t ext4 -q -d $(BUILD_DIR)/rootfs $(BUILD_DIR)/root.ext4
	@echo "created $(BUILD_DIR)/root.ext4 ($(shell du -sh $(BUILD_DIR)/root.ext4 | cut -f1))"

check-tools:
	@command -v $(CC) >/dev/null || (echo "missing $(CC)"; exit 1)
	@command -v $(LD) >/dev/null || (echo "missing $(LD)"; exit 1)
	@test -n "$(GRUB_MKRESCUE)" || echo "optional: missing grub-mkrescue/i686-elf-grub-mkrescue for ISO creation"
	@command -v $(QEMU_X86_64) >/dev/null || echo "optional: missing qemu-system-x86_64 for running"

clean:
	rm -rf build
	@$(MAKE) -C userspace clean

# ── Smoke Tests ──
smoke:
	@echo "Running smoke tests..."
	sh tests/smoke.sh $(ARCH)

smoke-x86: ARCH=x86
smoke-x86: smoke

graphics-smoke:
	sh tests/graphics-smoke.sh

.PHONY: all clean run-x86 run-root root-image iso userspace userspace-install iso-full smoke smoke-x86 check-tools run-persistent graphics-smoke
