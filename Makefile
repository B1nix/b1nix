ARCH ?= x86
BUILD_DIR := build/$(ARCH)
KERNEL_ELF := $(BUILD_DIR)/kernel.elf
RUN_ISO := /private/tmp/b1nix-run.iso

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
ASM_SOURCES := kernel/arch/x86/boot.S kernel/arch/x86/context_switch.S kernel/arch/x86/isr.S
ARCH_SOURCES := kernel/arch/x86/arch.c kernel/arch/x86/console.c kernel/arch/x86/fb_console.c kernel/arch/x86/interrupts.c kernel/arch/x86/io.c kernel/arch/x86/paging.c kernel/arch/x86/serial.c
else ifeq ($(ARCH),aarch64)
TARGET := aarch64-elf
ARCH_CFLAGS := --target=$(TARGET)
ARCH_LDFLAGS := -m aarch64elf
LINKER_SCRIPT := kernel/arch/aarch64/linker.ld
ASM_SOURCES := kernel/arch/aarch64/boot.S kernel/arch/aarch64/context_switch.S kernel/arch/aarch64/isr.S
ARCH_SOURCES := kernel/arch/aarch64/arch.c kernel/arch/aarch64/console.c kernel/arch/aarch64/interrupts.c kernel/arch/aarch64/paging.c kernel/arch/aarch64/serial.c kernel/arch/aarch64/bootinfo.c
else
$(error Unsupported ARCH=$(ARCH). Try ARCH=x86 or ARCH=aarch64)
endif

KERNEL_SOURCES := \
	kernel/main.c \
	kernel/lib/string.c \
	kernel/lib/panic.c \
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
	kernel/fs/journal.c \
	kernel/fs/filelock.c \
	kernel/user/process.c \
	kernel/user/programs.c \
	$(ARCH_SOURCES)

ifeq ($(ARCH),x86)
KERNEL_SOURCES += \
	kernel/bootinfo/multiboot2.c \
	kernel/dev/pci.c \
	kernel/dev/blk.c \
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
	kernel/net/dhcp.c \
	kernel/net/dns.c
endif


OBJECTS := \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(KERNEL_SOURCES)) \
	$(patsubst %.S,$(BUILD_DIR)/%.o,$(ASM_SOURCES))

.PHONY: all clean run-x86 run-aarch64 iso check-tools objects

all: $(KERNEL_ELF)

objects: $(OBJECTS)

$(KERNEL_ELF): $(OBJECTS) $(LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD) $(ARCH_LDFLAGS) -T $(LINKER_SCRIPT) -o $@ $(OBJECTS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) $(ARCH_CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) $(ARCH_CFLAGS) -c $< -o $@

iso: $(KERNEL_ELF)
	@test -n "$(GRUB_MKRESCUE)" || (echo "missing grub-mkrescue or i686-elf-grub-mkrescue"; exit 1)
	@mkdir -p $(BUILD_DIR)/iso/boot/grub
	cp $(KERNEL_ELF) $(BUILD_DIR)/iso/boot/kernel.elf
	cp boot/grub/grub.cfg $(BUILD_DIR)/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(BUILD_DIR)/b1nix.iso $(BUILD_DIR)/iso

run-x86: iso
	@command -v $(QEMU_X86_64) >/dev/null || (echo "missing qemu-system-x86_64"; exit 1)
	@mkdir -p $(BUILD_DIR)/fat_dir
	@echo "Hello from FAT32" > $(BUILD_DIR)/fat_dir/test.txt
	cp $(BUILD_DIR)/b1nix.iso $(RUN_ISO)
	$(QEMU_X86_64) -cdrom $(RUN_ISO) -serial stdio -no-reboot -boot d \
		-drive file=fat:32:rw:$(BUILD_DIR)/fat_dir,format=raw,if=virtio \
		-netdev user,id=n0 -device virtio-net-pci,netdev=n0

run-ext2: iso
	@command -v $(QEMU_X86_64) >/dev/null || (echo "missing qemu-system-x86_64"; exit 1)
	@echo "Generating ext2 image..."
	@mkdir -p $(BUILD_DIR)/ext2_root
	@echo "Hello from Ext2!" > $(BUILD_DIR)/ext2_root/hello_ext2.txt
	@echo "Another file" > $(BUILD_DIR)/ext2_root/test.txt
	@dd if=/dev/zero of=$(BUILD_DIR)/disk.ext2 bs=1048576 count=8 2>/dev/null
	@/opt/homebrew/opt/e2fsprogs/sbin/mke2fs -t ext2 -q -d $(BUILD_DIR)/ext2_root $(BUILD_DIR)/disk.ext2
	cp $(BUILD_DIR)/b1nix.iso $(RUN_ISO)
	$(QEMU_X86_64) -cdrom $(RUN_ISO) -serial stdio -no-reboot -boot d \
		-drive file=$(BUILD_DIR)/disk.ext2,format=raw,if=virtio \
		-netdev user,id=n0 -device virtio-net-pci,netdev=n0

run-aarch64: ARCH=aarch64
run-aarch64: $(KERNEL_ELF)
	@command -v qemu-system-aarch64 >/dev/null || (echo "missing qemu-system-aarch64"; exit 1)
	qemu-system-aarch64 -machine virt -cpu cortex-a57 -nographic -kernel $(KERNEL_ELF)

check-tools:
	@command -v $(CC) >/dev/null || (echo "missing $(CC)"; exit 1)
	@command -v $(LD) >/dev/null || (echo "missing $(LD)"; exit 1)
	@test -n "$(GRUB_MKRESCUE)" || echo "optional: missing grub-mkrescue/i686-elf-grub-mkrescue for ISO creation"
	@command -v $(QEMU_X86_64) >/dev/null || echo "optional: missing qemu-system-x86_64 for running"

clean:
	rm -rf build
