ARCH ?= x86
BUILD_DIR := build/$(ARCH)
KERNEL_ELF := $(BUILD_DIR)/kernel.elf
RUN_ISO := /private/tmp/b1nix-run.iso
INITRAMFS_NATIVE_SMOKE_INC := $(BUILD_DIR)/initramfs_native_smoke.inc

CC := clang
LD := $(shell command -v ld.lld 2>/dev/null || printf '%s' /opt/homebrew/opt/lld/bin/ld.lld)
GRUB_MKRESCUE := $(shell command -v grub-mkrescue 2>/dev/null || command -v i686-elf-grub-mkrescue 2>/dev/null)
QEMU_X86_64 := qemu-system-x86_64

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
ARCH_SOURCES := kernel/arch/x86/arch.c kernel/arch/x86/console.c kernel/arch/x86/fb_console.c kernel/arch/x86/interrupts.c kernel/arch/x86/io.c kernel/arch/x86/paging.c kernel/arch/x86/serial.c
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
	kernel/mm/swap.c \
	kernel/sched/scheduler.c \
	kernel/syscall/syscall.c \
	kernel/fs/initramfs.c \
	kernel/fs/vfs.c \
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
	kernel/dev/compositor.c \
	kernel/dev/virtio_gpu.c \
	kernel/net/net.c \
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

.PHONY: all clean run-x86 run-root smoke-m18 root-image iso check-tools objects

all: $(KERNEL_ELF)

objects: $(OBJECTS)

$(KERNEL_ELF): $(OBJECTS) $(LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD) $(ARCH_LDFLAGS) -T $(LINKER_SCRIPT) -o $@ $(OBJECTS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) $(ARCH_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel/fs/initramfs.o: $(INITRAMFS_NATIVE_SMOKE_INC)

$(INITRAMFS_NATIVE_SMOKE_INC): userspace/bin/native_smoke.S userspace/linker.ld
	@$(MAKE) -C userspace build/bin/native_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_native_smoke_elf userspace/build/bin/native_smoke > $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) $(ARCH_CFLAGS) -c $< -o $@

iso: $(KERNEL_ELF)
	@test -n "$(GRUB_MKRESCUE)" || (echo "missing grub-mkrescue or i686-elf-grub-mkrescue"; exit 1)
	@mkdir -p $(BUILD_DIR)/iso/boot/grub
	cp $(KERNEL_ELF) $(BUILD_DIR)/iso/boot/kernel.elf
	cp boot/grub/grub.cfg $(BUILD_DIR)/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(BUILD_DIR)/b1nix.iso $(BUILD_DIR)/iso

# ── M25 Userspace ──
userspace:
	@$(MAKE) -C userspace

userspace-install: userspace
	@$(MAKE) -C userspace install

iso-full: userspace-install iso

run-x86: iso userspace-install root-image
	@command -v $(QEMU_X86_64) >/dev/null || (echo "missing qemu-system-x86_64"; exit 1)
	cp $(BUILD_DIR)/b1nix.iso $(RUN_ISO)
	$(QEMU_X86_64) -cdrom $(RUN_ISO) -serial stdio -no-reboot -boot d \
		-drive file=$(BUILD_DIR)/root.ext4,format=raw,if=virtio \
		-netdev user,id=n0 -device virtio-net-pci,netdev=n0

run-root: run-x86

root-image:
	@mkdir -p $(BUILD_DIR)/rootfs/bin $(BUILD_DIR)/rootfs/etc $(BUILD_DIR)/rootfs/dev $(BUILD_DIR)/rootfs/home $(BUILD_DIR)/rootfs/tmp $(BUILD_DIR)/rootfs/var
	@echo "b1nix persistent root" > $(BUILD_DIR)/rootfs/etc/motd
	@dd if=/dev/zero of=$(BUILD_DIR)/root.ext4 bs=1048576 count=16 2>/dev/null
	@/opt/homebrew/opt/e2fsprogs/sbin/mke2fs -t ext4 -q -d $(BUILD_DIR)/rootfs $(BUILD_DIR)/root.ext4
	@echo "created $(BUILD_DIR)/root.ext4"

check-tools:
	@command -v $(CC) >/dev/null || (echo "missing $(CC)"; exit 1)
	@command -v $(LD) >/dev/null || (echo "missing $(LD)"; exit 1)
	@test -n "$(GRUB_MKRESCUE)" || echo "optional: missing grub-mkrescue/i686-elf-grub-mkrescue for ISO creation"
	@command -v $(QEMU_X86_64) >/dev/null || echo "optional: missing qemu-system-x86_64 for running"

clean:
	rm -rf build
	@$(MAKE) -C userspace clean

# ── Smoke Tests ──
smoke: iso
	@echo "Running smoke tests..."
	sh tests/smoke.sh $(ARCH)

smoke-x86: ARCH=x86
smoke-x86: smoke

.PHONY: all clean run-x86 run-root root-image iso userspace userspace-install iso-full smoke smoke-x86 check-tools
