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
INITRAMFS_M26_SMOKE_INC := $(BUILD_DIR)/initramfs_m26_smoke.inc
INITRAMFS_M24B_SMOKE_INC := $(BUILD_DIR)/initramfs_m24b_smoke.inc
INITRAMFS_M27_SMOKE_INC := $(BUILD_DIR)/initramfs_m27_smoke.inc
INITRAMFS_M29_SMOKE_INC := $(BUILD_DIR)/initramfs_m29_smoke.inc
INITRAMFS_M31_SMOKE_INC := $(BUILD_DIR)/initramfs_m31_smoke.inc
INITRAMFS_M31_SETUID_INC := $(BUILD_DIR)/initramfs_m31_setuid.inc
INITRAMFS_M32_SMOKE_INC := $(BUILD_DIR)/initramfs_m32_smoke.inc
INITRAMFS_M32_NETTOOL_INC := $(BUILD_DIR)/initramfs_m32_nettool.inc
INITRAMFS_CURL_INC := $(BUILD_DIR)/initramfs_curl.inc
INITRAMFS_WGET_INC := $(BUILD_DIR)/initramfs_wget.inc
INITRAMFS_CACERT_INC := $(BUILD_DIR)/initramfs_cacert.inc
INITRAMFS_TLSTEST_INC := $(BUILD_DIR)/initramfs_tlstest.inc
INITRAMFS_M32_PCRE2_SMOKE_INC := $(BUILD_DIR)/initramfs_m32_pcre2_smoke.inc
INITRAMFS_M30_PIE_INC := $(BUILD_DIR)/initramfs_m30_pie.inc
INITRAMFS_M34_SMOKE_INC := $(BUILD_DIR)/initramfs_m34_smoke.inc
INITRAMFS_M35_SMOKE_INC := $(BUILD_DIR)/initramfs_m35_smoke.inc
INITRAMFS_DROPBEAR_INC := $(BUILD_DIR)/initramfs_dropbear.inc
AP_TRAMPOLINE_INC := $(BUILD_DIR)/ap_trampoline.inc
CURL_ELF := build/curl-b1nix/src/curl
WGET_ELF := build/wget-b1nix/src/wget
DROPBEAR_VERSION := 2022.83
DROPBEAR_ELF := build/dropbear-src/dropbear-$(DROPBEAR_VERSION)/dropbearmulti
B1NIX_TLS ?= mbedtls

# Kernel build toolchain selector. Default is clang; `make TOOLCHAIN=gcc ...`
# builds the kernel with the ported cross x86_64-b1nix-gcc/ld (toward M26
# self-host). CC/LD are assigned below, after CROSS_TOOLCHAIN_ROOT is known.
TOOLCHAIN ?= clang
MKE2FS := $(shell command -v mke2fs 2>/dev/null || command -v /sbin/mke2fs 2>/dev/null || printf '%s' /opt/homebrew/opt/e2fsprogs/sbin/mke2fs)
GRUB_MKRESCUE := $(shell command -v grub-mkrescue 2>/dev/null || command -v grub2-mkrescue 2>/dev/null || command -v i686-elf-grub-mkrescue 2>/dev/null || echo /opt/homebrew/bin/i686-elf-grub-mkrescue)
QEMU_X86_64 := qemu-system-x86_64
KERNEL_CMDLINE ?=
# GRUB menu timeout in seconds. 0 = boot the default entry immediately (used by
# the smoke harness so QEMU never stalls). Set e.g. GRUB_TIMEOUT=5 for an
# interactive build where you want to see/select the boot menu.
GRUB_TIMEOUT ?= 0

# Persistent root image size in MB. 512MB fits native gcc + binutils + kernel
# source for self-host (M26). Override with: make ROOT_IMAGE_SIZE=256 root-image
ROOT_IMAGE_SIZE ?= 512

# Locate the native toolchain that tools/build-native-toolchain.sh produced.
# The script writes to build/toolchain_build/native_root by default, or to
# ~/b1nix-toolchain/native_root when the project path contains spaces (WSL).
# /root/b1nix-toolchain is the legacy Docker-builder location kept as fallback.
NATIVE_TOOLCHAIN_ROOT := $(shell \
	for p in build/toolchain_build/native_root $$HOME/b1nix-toolchain/native_root /root/b1nix-toolchain/native_root; do \
		if [ -d "$$p" ]; then echo "$$p"; break; fi; \
	done)
CROSS_TOOLCHAIN_ROOT := $(shell \
	for p in build/toolchain_build/cross $$HOME/b1nix-toolchain/cross /root/b1nix-toolchain/cross; do \
		if [ -d "$$p" ]; then echo "$$p"; break; fi; \
	done)

# Lockdep-light (M28 #2): debug-only lock-order validator. Off by default
# (zero cost — the LOCKDEP_* macros compile to no-ops). Enable with
# `make ... LOCKDEP=1` to panic on a lock-order inversion / out-of-order
# release against the DAG in docs/m28-locking.md. Never ship with it on.
ifeq ($(LOCKDEP),1)
CFLAGS_EXTRA += -DKERNEL_LOCKDEP=1
endif

# CC/LD for the clang (default) toolchain. The kernel links with LLVM's ld.lld
# because the ELF linker script uses GNU-ld options (-z, -T) that Apple's system
# `ld` rejects. Only override when LD is still make's built-in default ("ld");
# an explicit `make LD=...` (e.g. the in-guest gcc/binutils build) is respected.
ifeq ($(TOOLCHAIN),clang)
ifeq ($(origin LD),default)
LD := $(shell command -v ld.lld 2>/dev/null || echo /opt/homebrew/opt/lld/bin/ld.lld)
endif
# Default CC is clang for the clang toolchain. macOS aliases `cc`->clang, but on
# Linux make's built-in `cc` is gcc, which rejects clang-only flags like
# --target=x86_64-elf. Only override make's built-in default; an explicit
# `make CC=...` is respected.
ifeq ($(origin CC),default)
CC := clang
endif
endif

COMMON_CFLAGS := \
	-std=c11 \
	-g \
	-ffreestanding \
	-fno-builtin \
	-fno-stack-protector \
	-fno-pic \
	-mno-red-zone \
	-Wall \
	-Wextra \
	-I kernel/include \
	$(CFLAGS_EXTRA)


ifeq ($(ARCH),x86)
TARGET := x86_64-elf
# clang needs --target to cross-compile; the b1nix-gcc cross compiler already
# targets x86_64 natively, so it must NOT receive --target (it is clang-only).
ifeq ($(TOOLCHAIN),gcc)
ARCH_CFLAGS := -mcmodel=kernel -mno-sse -mno-mmx -mno-sse2 -mno-3dnow
else
ARCH_CFLAGS := --target=$(TARGET) -mcmodel=kernel -mno-sse -mno-mmx -mno-sse2 -mno-3dnow
endif
ARCH_LDFLAGS := -m elf_x86_64 -z max-page-size=0x1000
LINKER_SCRIPT := kernel/arch/x86/linker.ld
ASM_SOURCES := kernel/arch/x86/boot.S kernel/arch/x86/context_switch.S kernel/arch/x86/isr.S kernel/arch/x86/user_jump.S kernel/arch/x86/syscall_entry.S kernel/arch/x86/fpu.S
ARCH_SOURCES := kernel/arch/x86/arch.c kernel/arch/x86/console.c kernel/arch/x86/fb_console.c kernel/arch/x86/interrupts.c kernel/arch/x86/io.c kernel/arch/x86/paging.c kernel/arch/x86/serial.c kernel/arch/x86/rtc.c kernel/arch/x86/signal.c kernel/arch/x86/lapic.c kernel/arch/x86/tlb.c kernel/arch/x86/coredump.c kernel/arch/x86/gdbstub.c
else
$(error Unsupported ARCH=$(ARCH). AArch64 is archived; use ARCH=x86)
endif

KERNEL_SOURCES := \
	kernel/main.c \
	kernel/lib/string.c \
	kernel/lib/klog.c \
	kernel/lib/stdio.c \
	kernel/lib/m35_diag.c \
	kernel/lib/m36_diag.c \
	kernel/lib/ftrace.c \
	kernel/lib/ftrace_demo.c \
	kernel/lib/stdlib.c \
	kernel/lib/unistd.c \
	kernel/lib/sha512.c \
	kernel/lib/crypt.c \
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
	kernel/fs/procfs.c \
	kernel/fs/sysfs.c \
	kernel/fs/journal.c \
	kernel/fs/filelock.c \
	kernel/dev/acpi.c \
	kernel/dev/ioapic.c \
	kernel/dev/blk.c \
	kernel/dev/video.c \
	kernel/ipc/mqueue.c \
	kernel/ipc/shm.c \
	kernel/sched/uidgid.c \
	kernel/sched/runqueue.c \
	kernel/sched/lockdep.c \
	kernel/sched/smp_test.c \
	kernel/sched/m28_ctxbench.c \
	kernel/sched/m28_heapbench.c \
	kernel/sched/futex.c \
	kernel/user/process.c \
	kernel/user/programs.c \
	kernel/user/busybox.c \
	kernel/user/tui_common.c \
	kernel/user/mc.c \
	kernel/user/editor.c \
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
	kernel/dev/pty.c \
	kernel/dev/compositor.c \
	kernel/dev/virtio_gpu.c \
	kernel/net/net.c \
	kernel/net/socket.c \
	kernel/net/unix.c \
	kernel/net/ethernet.c \
	kernel/net/arp.c \
	kernel/net/ipv4.c \
	kernel/net/ipv6.c \
	kernel/net/ndp.c \
	kernel/net/icmp.c \
	kernel/net/udp.c \
	kernel/net/tcp.c \
	kernel/net/dhcp.c \
	kernel/net/dns.c \
	kernel/net/ntp.c
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

# Symbol-table tooling for kallsyms (M35). nm reads the linked ELF; Apple's
# /usr/bin/nm handles our ELF output fine, llvm-nm is preferred when present.
NM ?= $(shell command -v llvm-nm 2>/dev/null || command -v nm 2>/dev/null || echo nm)
KALLSYMS_S := $(BUILD_DIR)/kallsyms.S
KALLSYMS_O := $(BUILD_DIR)/kallsyms.o

# Two-pass link for kallsyms:
#   pass 1 → kernel.elf.stage1 with empty .kallsyms (final .text addresses)
#   generate the symbol blob from stage1, assemble it
#   pass 2 → final kernel.elf with the blob appended into .kallsyms
# The blob lands after .text/.rodata/.data (frozen by 512K padding), so the
# pass-1 addresses it records remain correct in the final image.
$(KERNEL_ELF): $(OBJECTS) $(LINKER_SCRIPT) tools/gen_kallsyms.sh
	@mkdir -p $(dir $@)
	$(LD) $(ARCH_LDFLAGS) -T $(LINKER_SCRIPT) -o $@.stage1 $(OBJECTS)
	NM='$(NM)' sh tools/gen_kallsyms.sh $@.stage1 > $(KALLSYMS_S)
	$(CC) $(COMMON_CFLAGS) $(ARCH_CFLAGS) -c $(KALLSYMS_S) -o $(KALLSYMS_O)
	$(LD) $(ARCH_LDFLAGS) -T $(LINKER_SCRIPT) -o $@ $(OBJECTS) $(KALLSYMS_O)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) $(ARCH_CFLAGS) $(INSTRUMENT_FLAGS) -c $< -o $@

# M36: only the ftrace demo TU is instrumented, so __cyg_profile hooks fire
# there and nowhere else (global instrumentation would recurse / slow the
# whole kernel).
$(BUILD_DIR)/kernel/lib/ftrace_demo.o: INSTRUMENT_FLAGS := -finstrument-functions

$(BUILD_DIR)/kernel/arch/x86/lapic.o: $(AP_TRAMPOLINE_INC)
$(BUILD_DIR)/kernel/fs/initramfs.o: $(INITRAMFS_NATIVE_SMOKE_INC) $(INITRAMFS_M12_SMOKE_INC) $(INITRAMFS_M13_SMOKE_INC) $(INITRAMFS_M13_JOB_CONTROL_INC) $(INITRAMFS_M8_AIO_TEST_INC) $(INITRAMFS_M17_SMOKE_INC) $(INITRAMFS_M14_SMOKE_INC) $(INITRAMFS_M15_SMOKE_INC) $(INITRAMFS_TCC_FILES_INC) $(INITRAMFS_M25_SMOKE_INC) $(INITRAMFS_M26_SMOKE_INC) $(INITRAMFS_M24B_SMOKE_INC) $(INITRAMFS_M27_SMOKE_INC) $(INITRAMFS_M29_SMOKE_INC) $(INITRAMFS_M31_SMOKE_INC) $(INITRAMFS_M31_SETUID_INC) $(INITRAMFS_M32_SMOKE_INC) $(INITRAMFS_M32_NETTOOL_INC) $(INITRAMFS_M32_PCRE2_SMOKE_INC) $(INITRAMFS_CURL_INC) $(INITRAMFS_WGET_INC) $(INITRAMFS_CACERT_INC) $(INITRAMFS_TLSTEST_INC) $(INITRAMFS_M30_PIE_INC) $(INITRAMFS_M34_SMOKE_INC) $(INITRAMFS_M35_SMOKE_INC) $(INITRAMFS_DROPBEAR_INC)

# Anything in userspace libc/includes/crt that affects every embedded ELF.
# Listed as prereqs of each *.inc so changes to libc force an xxd re-bundle —
# otherwise initramfs ships with stale userspace and the kernel sees old libc.
USERSPACE_DEPS := \
	$(wildcard userspace/libc/*.c) \
	$(wildcard userspace/include/*.h) \
	$(wildcard userspace/include/arpa/*.h) \
	$(wildcard userspace/include/netinet/*.h) \
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

$(INITRAMFS_M26_SMOKE_INC): userspace/bin/m26_smoke.c $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/bin/m26_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m26_smoke_elf userspace/build/bin/m26_smoke > $@

$(INITRAMFS_M24B_SMOKE_INC): userspace/bin/m24b_smoke.c $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/bin/m24b_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m24b_smoke_elf userspace/build/bin/m24b_smoke > $@

$(INITRAMFS_M27_SMOKE_INC): userspace/bin/m27_smoke.c $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/bin/m27_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m27_smoke_elf userspace/build/bin/m27_smoke > $@

$(INITRAMFS_M29_SMOKE_INC): userspace/bin/m29_smoke.c $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/bin/m29_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m29_smoke_elf userspace/build/bin/m29_smoke > $@

$(INITRAMFS_M31_SMOKE_INC): userspace/bin/m31_smoke.c $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/bin/m31_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m31_smoke_elf userspace/build/bin/m31_smoke > $@

$(INITRAMFS_M31_SETUID_INC): userspace/bin/m31_setuid.c $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/bin/m31_setuid
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m31_setuid_elf userspace/build/bin/m31_setuid > $@

$(INITRAMFS_M32_SMOKE_INC): userspace/bin/m32_smoke.c $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/bin/m32_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m32_smoke_elf userspace/build/bin/m32_smoke > $@

# Depends on $(CURL_ELF): building curl (with B1NIX_TLS=mbedtls) produces the
# static mbedTLS archives that m32_nettool's tls-server links against, so curl
# must build first to guarantee the libs exist.
$(INITRAMFS_M32_NETTOOL_INC): userspace/bin/m32_nettool.c $(USERSPACE_DEPS) $(CURL_ELF)
	@$(MAKE) -C userspace build/bin/m32_nettool
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m32_nettool_elf userspace/build/bin/m32_nettool > $@

# PCRE2: cross-build the static 8-bit library, then link the smoke against it.
PCRE2_LIB := build/pcre2-b1nix/install/lib/libpcre2-8.a
$(PCRE2_LIB): tools/build-pcre2.sh tools/b1nix-autotools-cc $(USERSPACE_DEPS)
	tools/build-pcre2.sh >/dev/null

$(INITRAMFS_M32_PCRE2_SMOKE_INC): userspace/bin/m32_pcre2_smoke.c $(USERSPACE_DEPS) $(PCRE2_LIB)
	@$(MAKE) -C userspace build/bin/m32_pcre2_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m32_pcre2_smoke_elf userspace/build/bin/m32_pcre2_smoke > $@

$(CURL_ELF): tools/build-curl.sh tools/b1nix-autotools-cc $(USERSPACE_DEPS)
	B1NIX_TLS="$(B1NIX_TLS)" tools/build-curl.sh

$(INITRAMFS_CURL_INC): $(CURL_ELF)
	@mkdir -p $(dir $@)
	xxd -i -n vfs_curl_elf $(CURL_ELF) > $@

# Dropbear SSH server (dropbearmulti: server + dropbearkey + dropbearconvert,
# dispatched by argv[0]). Built static against the b1nix userspace libc.
$(DROPBEAR_ELF): tools/build-dropbear.sh tools/b1nix-autotools-cc $(USERSPACE_DEPS)
	tools/build-dropbear.sh all >/dev/null

$(INITRAMFS_DROPBEAR_INC): $(DROPBEAR_ELF)
	@mkdir -p $(dir $@)
	xxd -i -n vfs_dropbear_elf $(DROPBEAR_ELF) > $@

OPENSSL_LIB := build/openssl-b1nix/install/lib/libssl.a
$(OPENSSL_LIB): tools/build-openssl.sh tools/b1nix-autotools-cc $(USERSPACE_DEPS)
	tools/build-openssl.sh >/dev/null

LIBIDN2_LIB := build/libidn2-b1nix/install/lib/libidn2.a
$(LIBIDN2_LIB): tools/build-libidn2.sh tools/build-libunistring.sh tools/b1nix-autotools-cc $(USERSPACE_DEPS)
	tools/build-libidn2.sh >/dev/null

$(WGET_ELF): tools/build-wget.sh tools/b1nix-autotools-cc $(USERSPACE_DEPS) $(OPENSSL_LIB) $(LIBIDN2_LIB)
	B1NIX_TLS="$(B1NIX_TLS)" tools/build-wget.sh

$(INITRAMFS_WGET_INC): $(WGET_ELF)
	@mkdir -p $(dir $@)
	xxd -i -n vfs_wget_elf $(WGET_ELF) > $@

CACERT_PEM := build/cacert.pem
$(CACERT_PEM): tools/fetch-cacert.sh
	tools/fetch-cacert.sh $(CACERT_PEM)

$(INITRAMFS_CACERT_INC): $(CACERT_PEM)
	@mkdir -p $(dir $@)
	xxd -i -n vfs_cacert_pem $(CACERT_PEM) > $@

# Self-contained TLS test PKI (CA + server cert/key) embedded under
# /etc/tls-test for the M32 loopback HTTPS smoke. No network dependency.
TLS_TEST_DIR := build/tls-test
$(TLS_TEST_DIR)/ca.pem $(TLS_TEST_DIR)/server-cert.pem $(TLS_TEST_DIR)/server-key.pem: tools/gen-tls-test-certs.sh
	sh tools/gen-tls-test-certs.sh $(TLS_TEST_DIR) >/dev/null

$(INITRAMFS_TLSTEST_INC): $(TLS_TEST_DIR)/ca.pem $(TLS_TEST_DIR)/server-cert.pem $(TLS_TEST_DIR)/server-key.pem
	@mkdir -p $(dir $@)
	xxd -i -n vfs_tls_ca_pem $(TLS_TEST_DIR)/ca.pem > $@
	xxd -i -n vfs_tls_server_cert_pem $(TLS_TEST_DIR)/server-cert.pem >> $@
	xxd -i -n vfs_tls_server_key_pem $(TLS_TEST_DIR)/server-key.pem >> $@

$(INITRAMFS_M30_PIE_INC): userspace/bin/m30_pie.c $(USERSPACE_DEPS) userspace/linker_pie.ld
	@$(MAKE) -C userspace build/bin/m30_pie
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m30_pie_elf userspace/build/bin/m30_pie > $@

$(INITRAMFS_M34_SMOKE_INC): userspace/bin/m34_smoke.c $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/bin/m34_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m34_smoke_elf userspace/build/bin/m34_smoke > $@

$(INITRAMFS_M35_SMOKE_INC): userspace/bin/m35_smoke.c $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/bin/m35_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m35_smoke_elf userspace/build/bin/m35_smoke > $@


# ── AP Trampoline (flat binary linked at 0x8000) ──
AP_TRAMP_OBJ := $(BUILD_DIR)/kernel/arch/x86/ap_trampoline_tmp.o
AP_TRAMP_BIN := $(BUILD_DIR)/ap_trampoline.bin
# ld.lld defaults its image base to 0x200000 and rejects -Ttext 0x8000 unless
# we pin the image base to 0. GNU ld doesn't recognise --image-base; pass it
# only when LD is lld.
ifneq (,$(findstring lld,$(LD)))
AP_IMAGE_BASE := --image-base=0
endif

$(AP_TRAMP_OBJ): kernel/arch/x86/ap_trampoline.S
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) $(ARCH_CFLAGS) -c $< -o $@

$(AP_TRAMP_BIN): $(AP_TRAMP_OBJ)
	$(LD) -m elf_x86_64 -z max-page-size=0x1000 $(AP_IMAGE_BASE) -Ttext 0x8000 -o $@ --oformat binary $<

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
	@sed -e 's|@TIMEOUT@|$(GRUB_TIMEOUT)|g' -e 's|@CMDLINE@|$(KERNEL_CMDLINE)|g' boot/grub/grub.cfg > $(BUILD_DIR)/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(BUILD_DIR)/b1nix.iso $(BUILD_DIR)/iso

# ── M25 Userspace ──
userspace:
	@$(MAKE) -C userspace

userspace-install: userspace
	@$(MAKE) -C userspace install

install-native-toolchain:
	@if [ -n "$(NATIVE_TOOLCHAIN_ROOT)" ]; then \
		echo "Installing native toolchain from $(NATIVE_TOOLCHAIN_ROOT) to rootfs..."; \
		mkdir -p $(BUILD_DIR)/rootfs/lib/gcc/x86_64-b1nix/13.2.0; \
		cp -R $(NATIVE_TOOLCHAIN_ROOT)/. $(BUILD_DIR)/rootfs/; \
		if [ -f "$(CROSS_TOOLCHAIN_ROOT)/lib/gcc/x86_64-b1nix/13.2.0/libgcc.a" ]; then \
			cp "$(CROSS_TOOLCHAIN_ROOT)/lib/gcc/x86_64-b1nix/13.2.0/libgcc.a" $(BUILD_DIR)/rootfs/lib/gcc/x86_64-b1nix/13.2.0/; \
		fi; \
	else \
		echo "Note: native toolchain not built (looked in build/toolchain_build/native_root and ~/b1nix-toolchain/native_root)."; \
		echo "      Run tools/build-toolchain.sh && tools/build-native-toolchain.sh to enable self-host workflow."; \
	fi

# Stage kernel + userspace + build harness source into the rootfs so the
# in-guest toolchain can rebuild b1nix from inside b1nix (M26 self-host).
# Excludes generated artifacts (build/, *.o, *.a, *.elf, .git).
install-kernel-source:
	@echo "Staging b1nix source tree into $(BUILD_DIR)/rootfs/usr/src/b1nix..."
	@mkdir -p $(BUILD_DIR)/rootfs/usr/src/b1nix
	@for d in kernel userspace tools tests docs; do \
		if [ -d "$$d" ]; then \
			rsync -a --delete \
				--exclude='build/' \
				--exclude='*.o' \
				--exclude='*.a' \
				--exclude='*.elf' \
				--exclude='*.bin' \
				--exclude='*.iso' \
				--exclude='.git/' \
				"$$d" $(BUILD_DIR)/rootfs/usr/src/b1nix/ ; \
		fi; \
	done
	@cp Makefile $(BUILD_DIR)/rootfs/usr/src/b1nix/
	@if [ -f README.md ]; then cp README.md $(BUILD_DIR)/rootfs/usr/src/b1nix/; fi
	@# The in-guest kernel build (self-host) compiles lapic.c and initramfs.c,
	@# which #include generated artifacts from build/x86 (ap_trampoline.inc and
	@# the initramfs_*.inc byte arrays). build/ is rsync-excluded above as it is
	@# host output, so stage just these generated *.inc inputs the compile needs.
	@mkdir -p $(BUILD_DIR)/rootfs/usr/src/b1nix/build/x86
	@cp $(BUILD_DIR)/*.inc $(BUILD_DIR)/rootfs/usr/src/b1nix/build/x86/ 2>/dev/null || true
	@du -sh $(BUILD_DIR)/rootfs/usr/src/b1nix | sed 's/^/source tree size: /'

iso-full: userspace-install install-native-toolchain install-kernel-source iso

run-x86: iso userspace-install root-image
	@command -v $(QEMU_X86_64) >/dev/null || (echo "missing qemu-system-x86_64"; exit 1)
	cp $(BUILD_DIR)/b1nix.iso $(RUN_ISO)
	$(QEMU_X86_64) -cdrom $(RUN_ISO) -serial stdio -no-reboot -boot d \
		-drive file=$(BUILD_DIR)/root.ext4,format=raw,if=virtio \
		-netdev user,id=n0 -device virtio-net-pci,netdev=n0

run-root: run-x86

root-image: userspace-install install-native-toolchain install-kernel-source
	@mkdir -p $(BUILD_DIR)/rootfs/bin $(BUILD_DIR)/rootfs/etc $(BUILD_DIR)/rootfs/dev $(BUILD_DIR)/rootfs/home $(BUILD_DIR)/rootfs/tmp $(BUILD_DIR)/rootfs/var
	@echo "b1nix persistent root" > $(BUILD_DIR)/rootfs/etc/motd
	@dd if=/dev/zero of=$(BUILD_DIR)/root.ext4 bs=1048576 count=$(ROOT_IMAGE_SIZE) 2>/dev/null
	@$(MKE2FS) -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q -d $(BUILD_DIR)/rootfs $(BUILD_DIR)/root.ext4 2>/dev/null || \
	 $(MKE2FS) -t ext4 -q -d $(BUILD_DIR)/rootfs $(BUILD_DIR)/root.ext4
	@printf 'created %s (%s)\n' "$(BUILD_DIR)/root.ext4" "$$(du -sh $(BUILD_DIR)/root.ext4 | cut -f1)"

check-tools:
	@command -v $(CC) >/dev/null || (echo "missing $(CC)"; exit 1)
	@command -v $(LD) >/dev/null || (echo "missing $(LD)"; exit 1)
	@test -n "$(GRUB_MKRESCUE)" || echo "optional: missing grub-mkrescue/i686-elf-grub-mkrescue for ISO creation"
	@command -v $(QEMU_X86_64) >/dev/null || echo "optional: missing qemu-system-x86_64 for running"

clean:
	@# Preserve build/toolchain_build — cross + native GCC/Binutils take an hour
	@# to rebuild. Use `make distclean` to wipe everything including the toolchain.
	@if [ -d build ]; then \
		find build -mindepth 1 -maxdepth 1 ! -name toolchain_build -exec rm -rf {} +; \
	fi
	@$(MAKE) -C userspace clean

distclean: clean
	rm -rf build

# ── Smoke Tests ──
smoke:
	@echo "Running smoke tests..."
	sh tests/smoke.sh $(ARCH)

smoke-x86: ARCH=x86
smoke-x86: smoke

graphics-smoke:
	sh tests/graphics-smoke.sh

.PHONY: all clean distclean run-x86 run-root root-image iso userspace userspace-install iso-full smoke smoke-x86 check-tools run-persistent graphics-smoke install-native-toolchain install-kernel-source
