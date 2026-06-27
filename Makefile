.DEFAULT_GOAL := all
ARCH ?= x86_64
export B1NIX_ARCH := $(ARCH)
# BUILD_ROOT isolates the per-task kernel/initramfs/ISO output. Override it to
# build a second task (e.g. an M40 agent) without touching the main build:
#   make BUILD_ROOT=build-m40 ARCH=x86_64 iso
# The expensive, task-independent trees (build/toolchain_build, build/rust-native,
# the port build dirs) keep their literal `build/...` paths below, so they stay
# SHARED and read-only across isolated builds — never rebuilt per task.
BUILD_ROOT ?= build
BUILD_DIR := $(BUILD_ROOT)/$(ARCH)
# Host triplet for the ported userspace toolchain + programs. Their build trees
# live under per-triplet directories (build/toolchain_build/<triplet>,
# build/<prog>-{src,b1nix}/<triplet>) so x86 and x86_64 never share objects.
# Keep this mapping in sync with tools/toolchain/env.sh.
ifeq ($(ARCH),x86)
B1NIX_TRIPLET := i686-b1nix
else
B1NIX_TRIPLET := x86_64-b1nix
endif
KERNEL_ELF := $(BUILD_DIR)/kernel.elf
INITRAMFS_NATIVE_SMOKE_INC := $(BUILD_DIR)/initramfs_native_smoke.inc
INITRAMFS_TCC_FILES_INC := $(BUILD_DIR)/initramfs_tcc_files.inc
INITRAMFS_CURL_INC := $(BUILD_DIR)/initramfs_curl.inc
INITRAMFS_WGET_INC := $(BUILD_DIR)/initramfs_wget.inc
INITRAMFS_CACERT_INC := $(BUILD_DIR)/initramfs_cacert.inc
INITRAMFS_TLSTEST_INC := $(BUILD_DIR)/initramfs_tlstest.inc
INITRAMFS_DROPBEAR_INC := $(BUILD_DIR)/initramfs_dropbear.inc
INITRAMFS_BUSYBOX_INC := $(BUILD_DIR)/initramfs_busybox.inc
INITRAMFS_BASH_INC := $(BUILD_DIR)/initramfs_bash.inc
INITRAMFS_TESTWAV_INC := $(BUILD_DIR)/initramfs_testwav.inc
INITRAMFS_TESTFONT_INC := $(BUILD_DIR)/initramfs_testfont.inc
# M40: a committed static Linux x86_64 ELF blob (tools/m40/linux_hello.bin)
# embedded as /bin/m40-linux-hello to validate the Linux ABI compat layer.
INITRAMFS_M40_LINUX_INC := $(BUILD_DIR)/initramfs_m40_linux.inc
# M67: a prebuilt static Rust (x86_64-unknown-b1nix) ELF blob
# (tools/m67/hello_b1nix.elf, regen via tools/m67/build-hello.sh) embedded as
# /bin/m67-rust to validate the Rust std cross-toolchain at runtime. x86_64-only.
INITRAMFS_M67_RUST_INC := $(BUILD_DIR)/initramfs_m67_rust.inc
# M53: NetSurf framebuffer browser + resources + test page.
INITRAMFS_NETSURF_INC := $(BUILD_DIR)/initramfs_netsurf_files.inc
NSFB_ELF := build/netsurf-fb-b1nix/$(B1NIX_TRIPLET)/nsfb

# Applet manifest for /bin replacement (M42 items 3 and 4).
APPLET_MANIFEST := tools/configs/applet-manifest.conf
APPLET_SYMLINKS_INC := $(BUILD_DIR)/initramfs_applet_symlinks.inc
APPLET_REGISTRATION_INC := $(BUILD_DIR)/initramfs_applet_registration.inc

EMBEDDED_USER_PROGRAMS := \
	m8_aio_test \
	m12_smoke \
	m13_smoke \
	m13_job_control \
	m14_smoke \
	m15_smoke \
	m56_smoke \
	m17_smoke \
	m24b_smoke \
	m25_smoke \
	m26_smoke \
	m27_smoke \
	m29_smoke \
	m30_pie \
	m31_smoke \
	m31_setuid \
	m32_smoke \
	m32_nettool \
	m32_pcre2_smoke \
	m53_zlib_smoke \
	m53_libpng_smoke \
	m53_libjpeg_smoke \
	m53_libwebp_smoke \
	m53_libvpx_smoke \
	m53_wapcaplet_smoke \
	m53_parserutils_smoke \
	m53_hubbub_smoke \
	m53_libcss_smoke \
	m53_libdom_smoke \
	m53_nslibs_smoke \
	m53_httpd \
	m53_httpsd \
	m53_virgl_smoke \
	m53_mesa_virgl \
	m34_smoke \
	m35_smoke \
	m38_sound \
	m42_w5pre_smoke \
	m46_smoke \
	m57_smoke \
	m47_smoke \
	m48_smoke \
	m49_smoke \
	m49_libwayland \
	m49_libwayland_server \
	m_posixmm_smoke \
	m50_smoke \
	m51_smoke \
	m51_pixman_smoke \
	m51_freetype_smoke \
	m51_cairo_smoke \
	m51_cairo_wayland \
	m51_xkb_smoke \
	m51_clipboard_smoke \
	m51_harfbuzz_smoke \
	m51_fontconfig_smoke \
	m52_gl_smoke \
	m52_osmesa \
	m52_glsl \
	m59_smoke \
	cxx_smoke \
	m55_iostream \
	m55_litehtml \
	js \
	m58_smoke \
	displayd \
	gclock \
	gterm \
	gpaint \
	gdesktop \
	gabout \
	su passwd groups useradd userdel groupadd halt setfattr telinit

ifeq ($(ARCH),x86_64)
# m64_clang_smoke is x86_64-only: the clang frontend links the GCC-built
# libstdc++, but clang and that libstdc++ disagree on size_t mangling for
# i686-b1nix (unsigned int vs unsigned long), so the i686 clang link fails.
EMBEDDED_USER_PROGRAMS += m30_dynamic m64_clang_smoke
INITRAMFS_SHARED_LIBC_INC := $(BUILD_DIR)/initramfs_shared_libc.inc
INITRAMFS_M69_PLUGIN_INC := $(BUILD_DIR)/initramfs_m69_plugin.inc
# The --enable-shared x86_64 cross GCC links libgcc dynamically (DT_NEEDED
# libgcc_s.so, for the _Unwind_* exception symbols), so the C/C++ port binaries
# (NetSurf, the m53 servers, ...) need /lib/libgcc_s.so at exec — the M69 kernel
# linker resolves it. i686 GCC is static-libgcc, so this is x86_64-only.
INITRAMFS_LIBGCC_S_INC := $(BUILD_DIR)/initramfs_libgcc_s.inc
endif

INITRAMFS_USER_PROGRAM_INCS := \
	$(addprefix $(BUILD_DIR)/initramfs_,$(addsuffix .inc,$(EMBEDDED_USER_PROGRAMS)))
AP_TRAMPOLINE_INC := $(BUILD_DIR)/ap_trampoline.inc
# Upstream BusyBox is always embedded (M42 full integration).
ifeq ($(MINIMAL_INITRAMFS),1)
INITRAMFS_INCS := \
	$(INITRAMFS_NATIVE_SMOKE_INC) \
	$(BUILD_DIR)/initramfs_return_42.inc \
	$(BUILD_DIR)/initramfs_b1cc_hello.inc \
	$(BUILD_DIR)/initramfs_b1cc_argv.inc \
	$(BUILD_DIR)/initramfs_b1cc_file_write.inc \
	$(BUILD_DIR)/initramfs_b1cc_stderr_exit.inc
else
INITRAMFS_INCS := \
	$(INITRAMFS_NATIVE_SMOKE_INC) \
	$(INITRAMFS_TCC_FILES_INC) \
	$(INITRAMFS_USER_PROGRAM_INCS) \
	$(INITRAMFS_SHARED_LIBC_INC) \
	$(INITRAMFS_M69_PLUGIN_INC) \
	$(INITRAMFS_LIBGCC_S_INC) \
	$(INITRAMFS_CURL_INC) \
	$(INITRAMFS_WGET_INC) \
	$(INITRAMFS_CACERT_INC) \
	$(INITRAMFS_TLSTEST_INC) \
	$(INITRAMFS_DROPBEAR_INC) \
	$(INITRAMFS_BUSYBOX_INC) \
	$(INITRAMFS_BASH_INC) \
	$(INITRAMFS_TESTWAV_INC) \
	$(INITRAMFS_TESTFONT_INC) \
	$(INITRAMFS_M40_LINUX_INC) \
	$(INITRAMFS_M67_RUST_INC) \
	$(INITRAMFS_NETSURF_INC)
endif
GENERATED_INCS := $(AP_TRAMPOLINE_INC) $(INITRAMFS_INCS) $(APPLET_SYMLINKS_INC) $(APPLET_REGISTRATION_INC)
CURL_ELF := build/curl-b1nix/$(B1NIX_TRIPLET)/src/curl
WGET_ELF := build/wget-b1nix/$(B1NIX_TRIPLET)/src/wget
DROPBEAR_VERSION := 2022.83
DROPBEAR_ELF := build/dropbear-src/$(B1NIX_TRIPLET)/dropbear-$(DROPBEAR_VERSION)/dropbearmulti
BASH_VERSION_NUM := 5.2.37
BASH_ELF := build/bash-src/$(B1NIX_TRIPLET)/bash-$(BASH_VERSION_NUM)/bash
B1NIX_TLS ?= mbedtls
PORTS_SOURCE ?= download
PACKAGE_INDEX_URL ?= https://cdn.jsdelivr.net/gh/B1nix/b1nix-pkgs@main/pkgs/index

# Kernel build toolchain selector. Default is clang; `make TOOLCHAIN=gcc ...`
# builds the kernel with the ported cross x86_64-b1nix-gcc/ld (toward M26
# self-host). CC/LD are assigned below, after CROSS_TOOLCHAIN_ROOT is known.
TOOLCHAIN ?= clang
MKE2FS := $(shell command -v mke2fs 2>/dev/null || command -v /sbin/mke2fs 2>/dev/null || printf '%s' /opt/homebrew/opt/e2fsprogs/sbin/mke2fs)
GRUB_MKRESCUE := $(shell command -v grub-mkrescue 2>/dev/null || command -v grub2-mkrescue 2>/dev/null || command -v i686-elf-grub-mkrescue 2>/dev/null || echo /opt/homebrew/bin/i686-elf-grub-mkrescue)
QEMU_X86_64 := qemu-system-x86_64
# Default RAM for `make run`. QEMU's own default is only 128 MB, which OOMs on
# heavy pages (a real browser tab with JavaScript needs far more). Override with
# `make run RUN_MEM=2048`.
RUN_MEM ?= 1024
KERNEL_CMDLINE ?=
# GRUB menu timeout in seconds. 0 = boot the default entry immediately (used by
# the smoke harness so QEMU never stalls). Set e.g. GRUB_TIMEOUT=5 for an
# interactive build where you want to see/select the boot menu.
GRUB_TIMEOUT ?= 0

# Persistent root image size in MB. 512MB fits native gcc + binutils + kernel
# source for self-host (M26). Override with: make ROOT_IMAGE_SIZE=256 root-image
ROOT_IMAGE_SIZE ?= 512

# Locate the native toolchain that tools/toolchain/build-native-toolchain.sh produced.
# Per-triplet: build/toolchain_build/<triplet>/native_root by default, or
# ~/b1nix-toolchain/<triplet>/native_root when the project path has spaces (WSL).
# /root/b1nix-toolchain is the legacy Docker-builder location kept as fallback.
NATIVE_TOOLCHAIN_ROOT := $(shell \
	for p in build/toolchain_build/$(B1NIX_TRIPLET)/native_root $$HOME/b1nix-toolchain/$(B1NIX_TRIPLET)/native_root /root/b1nix-toolchain/$(B1NIX_TRIPLET)/native_root; do \
		if [ -d "$$p" ]; then echo "$$p"; break; fi; \
	done)
CROSS_TOOLCHAIN_ROOT := $(shell \
	for p in build/toolchain_build/$(B1NIX_TRIPLET)/cross $$HOME/b1nix-toolchain/$(B1NIX_TRIPLET)/cross /root/b1nix-toolchain/$(B1NIX_TRIPLET)/cross; do \
		if [ -d "$$p" ]; then echo "$$p"; break; fi; \
	done)

# Lockdep-light (M28 #2): debug-only lock-order validator. Off by default
# (zero cost — the LOCKDEP_* macros compile to no-ops). Enable with
# `make ... LOCKDEP=1` to panic on a lock-order inversion / out-of-order
# release against the lock-order DAG. Never ship with it on.
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
# `make CC=...` is respected. Prefix ccache when present so a `make clean`
# rebuild (the kernel has no incremental header-dep cache across cleans) hits the
# compiler cache instead of recompiling every .o — near-instant on the 2nd build.
ifeq ($(origin CC),default)
CCACHE := $(shell command -v ccache 2>/dev/null)
CC := $(if $(CCACHE),$(CCACHE) clang,clang)
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
	-MMD \
	-MP \
	-Wall \
	-Wextra \
	-I kernel/include \
	-I $(BUILD_DIR) \
	$(CFLAGS_EXTRA)


ifeq ($(ARCH),x86_64)
TARGET := x86_64-elf
# clang needs --target to cross-compile; the b1nix-gcc cross compiler already
# targets x86_64 natively, so it must NOT receive --target (it is clang-only).
ifeq ($(TOOLCHAIN),gcc)
ARCH_CFLAGS := -mcmodel=kernel -mno-sse -mno-mmx -mno-sse2 -mno-3dnow
else
ARCH_CFLAGS := --target=$(TARGET) -mcmodel=kernel -mno-sse -mno-mmx -mno-sse2 -mno-3dnow
endif
ARCH_LDFLAGS := -m elf_x86_64 -z max-page-size=0x1000
LINKER_SCRIPT := kernel/arch/x86_64/linker.ld
ASM_SOURCES := kernel/arch/x86_64/boot.S kernel/arch/x86_64/context_switch.S kernel/arch/x86_64/isr.S kernel/arch/x86_64/user_jump.S kernel/arch/x86_64/syscall_entry.S kernel/arch/x86_64/fpu.S
ARCH_SOURCES := kernel/arch/x86_64/arch.c kernel/arch/x86_64/console.c kernel/arch/x86_64/fb_console.c kernel/arch/x86_64/interrupts.c kernel/arch/x86_64/io.c kernel/arch/x86_64/paging.c kernel/arch/x86_64/serial.c kernel/arch/x86_64/rtc.c kernel/arch/x86_64/signal.c kernel/arch/x86_64/lapic.c kernel/arch/x86_64/tlb.c kernel/arch/x86_64/coredump.c kernel/arch/x86_64/gdbstub.c
else ifeq ($(ARCH),x86)
TARGET := i686-elf
ifeq ($(TOOLCHAIN),gcc)
ARCH_CFLAGS := -m32 -mno-sse -mno-mmx -mno-sse2 -mno-3dnow
else
ARCH_CFLAGS := --target=$(TARGET) -m32 -mno-sse -mno-mmx -mno-sse2 -mno-3dnow
endif
ARCH_LDFLAGS := -m elf_i386 -z max-page-size=0x1000
LINKER_SCRIPT := kernel/arch/x86/linker.ld
ASM_SOURCES := kernel/arch/x86/boot.S kernel/arch/x86/context_switch.S kernel/arch/x86/isr.S kernel/arch/x86/user_jump.S kernel/arch/x86/syscall_entry.S kernel/arch/x86/fpu.S
ARCH_SOURCES := kernel/arch/x86/arch.c kernel/arch/x86/console.c kernel/arch/x86/fb_console.c kernel/arch/x86/interrupts.c kernel/arch/x86/io.c kernel/arch/x86/paging.c kernel/arch/x86/serial.c kernel/arch/x86/rtc.c kernel/arch/x86/signal.c kernel/arch/x86/lapic.c kernel/arch/x86/tlb.c kernel/arch/x86/coredump.c kernel/arch/x86/gdbstub.c
else
$(error Unsupported ARCH=$(ARCH). AArch64 is archived; use ARCH=x86_64 or ARCH=x86)
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
	kernel/syscall/linux_abi.c \
	kernel/fs/initramfs.c \
	kernel/fs/vfs.c \
	kernel/fs/aio.c \
	kernel/fs/vfs_slab.c \
	kernel/fs/pipe.c \
	kernel/fs/eventpoll.c \
	kernel/fs/fat32.c \
	kernel/fs/isofs.c \
	kernel/fs/exfat.c \
	kernel/fs/ntfs.c \
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
	kernel/dev/loop.c \
	kernel/dev/ramdisk.c \
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
	kernel/user/tui_common.c \
	kernel/user/mc.c \
	kernel/user/editor.c \
	$(ARCH_SOURCES)

ifneq ($(filter $(ARCH),x86_64 x86),)
KERNEL_SOURCES += \
	kernel/bootinfo/multiboot2.c \
	kernel/dev/pci.c \
	kernel/dev/virtio.c \
	kernel/dev/virtio_blk.c \
	kernel/dev/virtio_net.c \
	kernel/dev/e1000.c \
	kernel/dev/r8169.c \
	kernel/dev/ahci.c \
	kernel/dev/nvme.c \
	kernel/dev/ps2_kbd.c \
	kernel/dev/ps2_mouse.c \
	kernel/dev/usb_xhci.c \
	kernel/dev/hda.c \
	kernel/dev/pty.c \
	kernel/dev/serial_tty.c \
	kernel/dev/compositor.c \
	kernel/dev/virtio_gpu.c \
	kernel/dev/virtio_input.c \
	kernel/dev/drm.c \
	kernel/dev/fb.c \
	kernel/dev/input.c \
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
KERNEL_DEPS := $(OBJECTS:.o=.d)

-include $(KERNEL_DEPS)

ANALYZE_DIR := $(BUILD_DIR)/analyze

# ── Static Analysis ──
analyze: $(GENERATED_INCS) $(KERNEL_SOURCES) $(ASM_SOURCES)
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

.PHONY: all analyze objects FORCE iso iso-core iso-graphics iso-shell iso-live iso-test iso-full \
	userspace userspace-install busybox-package busybox-iso \
	install-native-toolchain install-kernel-source install-ports root-image disk-image \
	run run-graphics run-x86_64 run-x86 run-root check-tools clean distclean \
	smoke smoke-quick graphics-smoke memory-smoke

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
$(KERNEL_ELF): $(OBJECTS) $(LINKER_SCRIPT) tools/kernel/gen_kallsyms.sh
	@mkdir -p $(dir $@)
	$(LD) $(ARCH_LDFLAGS) -T $(LINKER_SCRIPT) -o $@.stage1 $(OBJECTS)
	NM='$(NM)' sh tools/kernel/gen_kallsyms.sh $@.stage1 > $(KALLSYMS_S)
	$(CC) $(COMMON_CFLAGS) $(ARCH_CFLAGS) -c $(KALLSYMS_S) -o $(KALLSYMS_O)
	$(LD) $(ARCH_LDFLAGS) -T $(LINKER_SCRIPT) -o $@ $(OBJECTS) $(KALLSYMS_O)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) $(ARCH_CFLAGS) $(INSTRUMENT_FLAGS) -c $< -o $@

# M36: only the ftrace demo TU is instrumented, so __cyg_profile hooks fire
# there and nowhere else (global instrumentation would recurse / slow the
# whole kernel).
$(BUILD_DIR)/kernel/lib/ftrace_demo.o: INSTRUMENT_FLAGS := -finstrument-functions

$(BUILD_DIR)/kernel/arch/$(ARCH)/lapic.o: $(AP_TRAMPOLINE_INC)
$(BUILD_DIR)/kernel/fs/initramfs.o: $(INITRAMFS_INCS) $(APPLET_SYMLINKS_INC)

# programs.c includes the generated applet registration .inc
$(BUILD_DIR)/kernel/user/programs.o: $(APPLET_REGISTRATION_INC)

# ── Applet manifest generation (M42 items 3 & 4) ──
# Reads tools/configs/applet-manifest.conf and generates:
#   (a) initramfs_applet_symlinks.inc — symlink entries for upstream applets
#   (b) initramfs_applet_registration.inc — conditional user_register_program calls
#
# The manifest controls per-command selection; upstream commands get a VFS
# symlink to the embedded upstream BusyBox ELF, and their native registration
# is skipped.  Native-only applets are always registered.

$(APPLET_SYMLINKS_INC): $(APPLET_MANIFEST)
	@mkdir -p $(dir $@)
	@awk -F'=' '/^[[:space:]]*[^#]/ { gsub(/^[[:space:]]+|[[:space:]]+$$/, "", $$1); gsub(/^[[:space:]]+|[[:space:]]+$$/, "", $$2); if ($$2 == "upstream") { cmd = $$1; if (cmd == "[") printf "  {\"/bin/[\", \"/opt/busybox/bin/busybox\", 24, INITRAMFS_SYMLINK},\n"; else printf "  {\"/bin/%s\", \"/opt/busybox/bin/busybox\", 24, INITRAMFS_SYMLINK},\n", cmd; } }' $< > $@

$(APPLET_REGISTRATION_INC): $(APPLET_MANIFEST)
	@mkdir -p $(dir $@)
	@printf '/* Generated from %s — native-only applets (upstream handled by VFS symlinks) */\n' '$<' > $@
	@printf '\n' >> $@
	@awk -F'=' '/^[[:space:]]*[^#]/ { gsub(/^[[:space:]]+|[[:space:]]+$$/, "", $$1); gsub(/^[[:space:]]+|[[:space:]]+$$/, "", $$2); if ($$2 == "native") printf "  user_register_program(\"/bin/%s\", busybox_main);\n", $$1; }' $< >> $@

# Arch guard for the SHARED userspace build dir.
#
# userspace/build/ is not arch-qualified — both ARCH=x86 and ARCH=x86_64 compile
# into the same tree. Switching ARCH must invalidate every embedded binary, but
# most binaries only rebuild because their per-arch prereqs ($(LIB)/$(CRT0)) get
# rebuilt; native_smoke/tcc/m30_pie link neither, so a leftover binary from the
# other arch survives an arch switch and gets xxd-bundled into the wrong-arch
# initramfs. A 64-bit native_smoke embedded in the 32-bit kernel is accepted by
# the ELF64 loader and SIGILLs the moment it runs in ring3 (the "0x2000000
# collision" — it was never a PMM/layout bug, just a stale .inc).
#
# The stamp records the arch the shared tree was last built for. When ARCH
# changes it wipes the tree so every output relinks for the new arch, then
# rewrites itself; its mtime advances only on a real switch, so same-arch builds
# don't churn. It is part of USERSPACE_DEPS so all *.inc re-bundle after a wipe.
# Anything in userspace libc/includes/crt that affects every embedded ELF.
# Listed as prereqs of each *.inc so changes to libc force an xxd re-bundle —
# otherwise initramfs ships with stale userspace and the kernel sees old libc.
USERSPACE_DEPS := \
	$(wildcard userspace/libc/*.c) \
	$(wildcard userspace/libgui/*.c) \
	$(wildcard userspace/include/*.h) \
	$(wildcard userspace/include/b1nix/*.h) \
	$(wildcard userspace/include/arpa/*.h) \
	$(wildcard userspace/include/netinet/*.h) \
	$(wildcard userspace/include/sys/*.h) \
	$(wildcard userspace/crt/*.S) \
	userspace/Makefile \
	userspace/linker.ld

$(INITRAMFS_NATIVE_SMOKE_INC): userspace/bin/native_smoke.S $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/$(ARCH)/bin/native_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_native_smoke_elf userspace/build/$(ARCH)/bin/native_smoke > $@

$(INITRAMFS_TCC_FILES_INC): $(USERSPACE_DEPS) tools/images/gen_tcc_initramfs.sh $(wildcard userspace/tcc/*.c) $(wildcard userspace/tcc/include/*.h)
	@$(MAKE) -C userspace build/$(ARCH)/bin/tcc
	@mkdir -p $(dir $@)
	sh tools/images/gen_tcc_initramfs.sh $@

$(BUILD_DIR)/initramfs_%.inc: userspace/bin/%.c $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/$(ARCH)/bin/$*
	@mkdir -p $(dir $@)
	xxd -i -n vfs_$*_elf userspace/build/$(ARCH)/bin/$* > $@

# Depends on $(CURL_ELF): building curl (with B1NIX_TLS=mbedtls) produces the
# static mbedTLS archives that m32_nettool's tls-server links against, so curl
# must build first to guarantee the libs exist.
$(BUILD_DIR)/initramfs_m32_nettool.inc: userspace/bin/m32_nettool.c $(USERSPACE_DEPS) $(CURL_ELF)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m32_nettool
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m32_nettool_elf userspace/build/$(ARCH)/bin/m32_nettool > $@

# PCRE2: cross-build the static 8-bit library, then link the smoke against it.
PCRE2_LIB := build/pcre2-b1nix/$(B1NIX_TRIPLET)/install/lib/libpcre2-8.a
$(PCRE2_LIB): tools/ports/build-pcre2.sh tools/toolchain/bin/b1nix-autotools-cc
	tools/ports/build-pcre2.sh >/dev/null

# M51: libm (openlibm), cross-built static, linked into m51_smoke.
LIBM_LIB := build/openlibm-b1nix/$(B1NIX_TRIPLET)/install/lib/libm.a
$(LIBM_LIB): tools/ports/build-openlibm.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-openlibm.sh >/dev/null

# M58: /bin/js embeds Duktape and links the ported openlibm — so libm must be
# built before js. js.c links duktape.c (a vendored amalgamation under
# userspace/duktape/), both compiled by the userspace Makefile's custom rule.
$(BUILD_DIR)/initramfs_js.inc: userspace/bin/js.c userspace/duktape/duktape.c \
		userspace/duktape/duktape.h userspace/duktape/duk_config.h \
		$(USERSPACE_DEPS) $(LIBM_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/js
	@mkdir -p $(dir $@)
	xxd -i -n vfs_js_elf userspace/build/$(ARCH)/bin/js > $@

$(BUILD_DIR)/initramfs_m51_smoke.inc: userspace/bin/m51_smoke.c $(USERSPACE_DEPS) $(LIBM_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m51_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m51_smoke_elf userspace/build/$(ARCH)/bin/m51_smoke > $@

# M51: pixman (generic C), cross-built static, linked into m51_pixman_smoke.
PIXMAN_LIB := build/pixman-b1nix/$(B1NIX_TRIPLET)/install/lib/libpixman-1.a
$(PIXMAN_LIB): tools/ports/build-pixman.sh tools/ports/build-openlibm.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-pixman.sh >/dev/null

$(BUILD_DIR)/initramfs_m51_pixman_smoke.inc: userspace/bin/m51_pixman_smoke.c $(USERSPACE_DEPS) $(PIXMAN_LIB) $(LIBM_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m51_pixman_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m51_pixman_smoke_elf userspace/build/$(ARCH)/bin/m51_pixman_smoke > $@

# M51: FreeType (TrueType + smooth rasterizer), cross-built static.
FREETYPE_LIB := build/freetype-b1nix/$(B1NIX_TRIPLET)/install/lib/libfreetype.a
$(FREETYPE_LIB): tools/ports/build-freetype.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-freetype.sh >/dev/null

$(BUILD_DIR)/initramfs_m51_freetype_smoke.inc: userspace/bin/m51_freetype_smoke.c $(USERSPACE_DEPS) $(FREETYPE_LIB) $(LIBM_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m51_freetype_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m51_freetype_smoke_elf userspace/build/$(ARCH)/bin/m51_freetype_smoke > $@

# M51: Cairo (image surface + FreeType backend), cross-built static.
CAIRO_LIB := build/cairo-b1nix/$(B1NIX_TRIPLET)/install/lib/libcairo.a
$(CAIRO_LIB): tools/ports/build-cairo.sh tools/ports/build-pixman.sh tools/ports/build-freetype.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-cairo.sh >/dev/null

$(BUILD_DIR)/initramfs_m51_cairo_smoke.inc: userspace/bin/m51_cairo_smoke.c $(USERSPACE_DEPS) $(CAIRO_LIB) $(FREETYPE_LIB) $(PIXMAN_LIB) $(LIBM_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m51_cairo_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m51_cairo_smoke_elf userspace/build/$(ARCH)/bin/m51_cairo_smoke > $@

$(BUILD_DIR)/initramfs_m51_cairo_wayland.inc: userspace/bin/m51_cairo_wayland.c $(USERSPACE_DEPS) $(CAIRO_LIB) $(FREETYPE_LIB) $(PIXMAN_LIB) $(LIBM_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m51_cairo_wayland
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m51_cairo_wayland_elf userspace/build/$(ARCH)/bin/m51_cairo_wayland > $@

# M51: xkbcommon (keymap compile + keysym translation), cross-built static.
XKB_LIB := build/xkbcommon-b1nix/$(B1NIX_TRIPLET)/install/lib/libxkbcommon.a
$(XKB_LIB): tools/ports/build-xkbcommon.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-xkbcommon.sh >/dev/null

$(BUILD_DIR)/initramfs_m51_xkb_smoke.inc: userspace/bin/m51_xkb_smoke.c $(USERSPACE_DEPS) $(XKB_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m51_xkb_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m51_xkb_smoke_elf userspace/build/$(ARCH)/bin/m51_xkb_smoke > $@

# M49: libwayland-client/server share one generated source/build tree. Build it
# once at the top level so parallel initramfs packaging cannot race two nested
# userspace make invocations through tools/ports/build-wayland.sh.
WAYLAND_CLIENT_LIB := build/wayland-b1nix/$(B1NIX_TRIPLET)/install/lib/libwayland-client.a
WAYLAND_SERVER_LIB := build/wayland-b1nix/$(B1NIX_TRIPLET)/install/lib/libwayland-server.a
$(WAYLAND_CLIENT_LIB): tools/ports/build-wayland.sh tools/ports/build-libffi.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-wayland.sh >/dev/null
$(WAYLAND_SERVER_LIB): $(WAYLAND_CLIENT_LIB)

$(BUILD_DIR)/initramfs_m49_libwayland.inc: $(WAYLAND_CLIENT_LIB)
$(BUILD_DIR)/initramfs_m49_libwayland_server.inc: $(WAYLAND_SERVER_LIB)

# M51: HarfBuzz (HB_TINY, unified C++ build via cross g++), cross-built static.
HB_LIB := build/harfbuzz-b1nix/$(B1NIX_TRIPLET)/install/lib/libharfbuzz.a
$(HB_LIB): tools/ports/build-harfbuzz.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-harfbuzz.sh >/dev/null

$(BUILD_DIR)/initramfs_m51_harfbuzz_smoke.inc: userspace/bin/m51_harfbuzz_smoke.c $(USERSPACE_DEPS) $(HB_LIB) $(LIBM_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m51_harfbuzz_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m51_harfbuzz_smoke_elf userspace/build/$(ARCH)/bin/m51_harfbuzz_smoke > $@

# M51: expat (XML) + Fontconfig (font discovery), cross-built static.
EXPAT_LIB := build/expat-b1nix/$(B1NIX_TRIPLET)/install/lib/libexpat.a
$(EXPAT_LIB): tools/ports/build-expat.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-expat.sh >/dev/null
FONTCONFIG_LIB := build/fontconfig-b1nix/$(B1NIX_TRIPLET)/install/lib/libfontconfig.a
$(FONTCONFIG_LIB): tools/ports/build-fontconfig.sh tools/ports/build-expat.sh tools/ports/build-freetype.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-fontconfig.sh >/dev/null

$(BUILD_DIR)/initramfs_m51_fontconfig_smoke.inc: userspace/bin/m51_fontconfig_smoke.c $(USERSPACE_DEPS) $(FONTCONFIG_LIB) $(EXPAT_LIB) $(FREETYPE_LIB) $(LIBM_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m51_fontconfig_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m51_fontconfig_smoke_elf userspace/build/$(ARCH)/bin/m51_fontconfig_smoke > $@

# M52: TinyGL (software OpenGL) + b1nix EGL shim, cross-built static.
TINYGL_LIB := build/tinygl-b1nix/$(B1NIX_TRIPLET)/install/lib/libEGL.a
$(TINYGL_LIB): tools/ports/build-tinygl.sh userspace/libegl/b1egl.c userspace/include/EGL/egl.h
	B1NIX_ARCH=$(ARCH) tools/ports/build-tinygl.sh >/dev/null

$(BUILD_DIR)/initramfs_m52_gl_smoke.inc: userspace/bin/m52_gl_smoke.c $(USERSPACE_DEPS) $(TINYGL_LIB) $(LIBM_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m52_gl_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m52_gl_smoke_elf userspace/build/$(ARCH)/bin/m52_gl_smoke > $@

# Hosted C++ runtime smoke. Enable libstdc++ against the b1nix libc first
# (idempotent: stages headers + fixes mbstate_t config), then build via the
# cross GCC C++ wrapper.
$(BUILD_DIR)/initramfs_cxx_smoke.inc: userspace/bin/cxx_smoke.cpp $(USERSPACE_DEPS) tools/toolchain/bin/b1nix-c++ tools/toolchain/enable-cxx-toolchain.sh
	@tools/toolchain/enable-cxx-toolchain.sh $(B1NIX_TRIPLET) >/dev/null 2>&1 || true
	@$(MAKE) -C userspace build/$(ARCH)/bin/cxx_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_cxx_smoke_elf userspace/build/$(ARCH)/bin/cxx_smoke > $@

$(BUILD_DIR)/initramfs_m64_clang_smoke.inc: userspace/bin/m64_clang_smoke.cpp $(USERSPACE_DEPS) tools/toolchain/bin/b1nix-clang++ tools/toolchain/bin/b1nix-c++
	@tools/toolchain/enable-cxx-toolchain.sh $(B1NIX_TRIPLET) >/dev/null 2>&1 || true
	@$(MAKE) -C userspace build/$(ARCH)/bin/m64_clang_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m64_clang_smoke_elf userspace/build/$(ARCH)/bin/m64_clang_smoke > $@

# M55: std::iostream + std::filesystem acceptance test (hosted libstdc++).
$(BUILD_DIR)/initramfs_m55_iostream.inc: userspace/bin/m55_iostream.cpp $(USERSPACE_DEPS) tools/toolchain/bin/b1nix-c++ tools/toolchain/enable-cxx-toolchain.sh
	@tools/toolchain/enable-cxx-toolchain.sh $(B1NIX_TRIPLET) >/dev/null 2>&1 || true
	@$(MAKE) -C userspace build/$(ARCH)/bin/m55_iostream
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m55_iostream_elf userspace/build/$(ARCH)/bin/m55_iostream > $@

# Serialize the Mesa build to prevent race conditions during parallel builds.
$(BUILD_DIR)/.mesa-built: tools/ports/build-mesa.sh $(USERSPACE_DEPS)
	@mkdir -p $(dir $@)
	B1NIX_ARCH=$(ARCH) tools/ports/build-mesa.sh >/dev/null
	@touch $@

# M52: real Mesa OSMesa (software OpenGL) demo. The shared demo builder builds
# the whole Mesa stack (build-mesa.sh) and links the demo against it.
$(BUILD_DIR)/initramfs_m52_osmesa.inc: userspace/bin/m52_osmesa.c tools/demos/build-m52-mesa-demo.sh $(BUILD_DIR)/.mesa-built $(USERSPACE_DEPS)
	B1NIX_ARCH=$(ARCH) tools/demos/build-m52-mesa-demo.sh m52_osmesa userspace/build/$(ARCH)/bin/m52_osmesa
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m52_osmesa_elf userspace/build/$(ARCH)/bin/m52_osmesa > $@

# M53 variant B: Mesa's gallium virgl driver renders on the host GPU through the
# b1nix /dev/virtio-gpu winsys. tools/demos/build-m53-mesa-virgl.sh builds the Mesa
# stack (with the virgl driver) and links the pipe-API render test against it.
$(BUILD_DIR)/initramfs_m53_mesa_virgl.inc: userspace/bin/m53_mesa_virgl.c tools/demos/build-m53-mesa-virgl.sh $(BUILD_DIR)/.mesa-built $(wildcard tools/patches/mesa/files/src/gallium/winsys/virgl/b1nix/*) $(USERSPACE_DEPS)
	B1NIX_ARCH=$(ARCH) tools/demos/build-m53-mesa-virgl.sh userspace/build/$(ARCH)/bin/m53_mesa_virgl
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m53_mesa_virgl_elf userspace/build/$(ARCH)/bin/m53_mesa_virgl > $@

# M52: programmable GLSL shader demo, sharing the same Mesa build as the OSMesa
# demo. Exercises the GL 2.x programmable pipeline (shaders, VBOs, varyings).
$(BUILD_DIR)/initramfs_m52_glsl.inc: userspace/bin/m52_glsl.c tools/demos/build-m52-mesa-demo.sh $(BUILD_DIR)/.mesa-built $(USERSPACE_DEPS)
	B1NIX_ARCH=$(ARCH) tools/demos/build-m52-mesa-demo.sh m52_glsl userspace/build/$(ARCH)/bin/m52_glsl
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m52_glsl_elf userspace/build/$(ARCH)/bin/m52_glsl > $@

# M59: EGL over the real Mesa OSMesa softpipe. tools/demos/build-m59-egl.sh compiles
# the OSMesa-backed EGL implementation (userspace/libegl/b1egl_mesa.c) together
# with the off-screen pbuffer smoke and links them against the same Mesa stack
# as the M52 OSMesa demo. The smoke renders entirely off-screen (no displayd).
$(BUILD_DIR)/initramfs_m59_smoke.inc: userspace/bin/m59_smoke.c userspace/libegl/b1egl_mesa.c userspace/include/EGL/egl.h tools/demos/build-m59-egl.sh $(BUILD_DIR)/.mesa-built $(USERSPACE_DEPS)
	B1NIX_ARCH=$(ARCH) tools/demos/build-m59-egl.sh userspace/build/$(ARCH)/bin/m59_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m59_smoke_elf userspace/build/$(ARCH)/bin/m59_smoke > $@

# M55: validate the C++ runtime with litehtml (real HTML/CSS layout engine).
# tools/demos/build-m55-litehtml.sh builds litehtml+gumbo (build-litehtml.sh) and
# links the parse/layout/draw acceptance test against them + libstdc++/libm.
$(BUILD_DIR)/initramfs_m55_litehtml.inc: userspace/bin/m55_litehtml.cpp tools/demos/build-m55-litehtml.sh tools/ports/build-litehtml.sh $(USERSPACE_DEPS)
	B1NIX_ARCH=$(ARCH) tools/demos/build-m55-litehtml.sh userspace/build/$(ARCH)/bin/m55_litehtml
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m55_litehtml_elf userspace/build/$(ARCH)/bin/m55_litehtml > $@

$(BUILD_DIR)/initramfs_m32_pcre2_smoke.inc: userspace/bin/m32_pcre2_smoke.c $(USERSPACE_DEPS) $(PCRE2_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m32_pcre2_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m32_pcre2_smoke_elf userspace/build/$(ARCH)/bin/m32_pcre2_smoke > $@

# M53: zlib (image-codec dependency for the NetSurf browser platform).
ZLIB_LIB := build/zlib-b1nix/$(B1NIX_TRIPLET)/install/lib/libz.a
$(ZLIB_LIB): tools/ports/build-zlib.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-zlib.sh >/dev/null

$(BUILD_DIR)/initramfs_m53_zlib_smoke.inc: userspace/bin/m53_zlib_smoke.c $(USERSPACE_DEPS) $(ZLIB_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_zlib_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m53_zlib_smoke_elf userspace/build/$(ARCH)/bin/m53_zlib_smoke > $@

# M53: libpng (over zlib + libm) — NetSurf image-codec dependency.
LIBPNG_LIB := build/libpng-b1nix/$(B1NIX_TRIPLET)/install/lib/libpng16.a
$(LIBPNG_LIB): tools/ports/build-libpng.sh tools/ports/build-zlib.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libpng.sh >/dev/null

$(BUILD_DIR)/initramfs_m53_libpng_smoke.inc: userspace/bin/m53_libpng_smoke.c $(USERSPACE_DEPS) $(LIBPNG_LIB) $(ZLIB_LIB) $(LIBM_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_libpng_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m53_libpng_smoke_elf userspace/build/$(ARCH)/bin/m53_libpng_smoke > $@

# M53: libjpeg (IJG) — NetSurf image-codec dependency.
LIBJPEG_LIB := build/libjpeg-b1nix/$(B1NIX_TRIPLET)/install/lib/libjpeg.a
$(LIBJPEG_LIB): tools/ports/build-libjpeg.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libjpeg.sh >/dev/null

$(BUILD_DIR)/initramfs_m53_libjpeg_smoke.inc: userspace/bin/m53_libjpeg_smoke.c $(USERSPACE_DEPS) $(LIBJPEG_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_libjpeg_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m53_libjpeg_smoke_elf userspace/build/$(ARCH)/bin/m53_libjpeg_smoke > $@

# M53: libwebp (image + VP8 video-keyframe codec) — NetSurf codec dependency.
LIBWEBP_LIB := build/libwebp-b1nix/$(B1NIX_TRIPLET)/install/lib/libwebp.a
$(LIBWEBP_LIB): tools/ports/build-libwebp.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libwebp.sh >/dev/null

$(BUILD_DIR)/initramfs_m53_libwebp_smoke.inc: userspace/bin/m53_libwebp_smoke.c $(USERSPACE_DEPS) $(LIBWEBP_LIB) $(LIBM_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_libwebp_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m53_libwebp_smoke_elf userspace/build/$(ARCH)/bin/m53_libwebp_smoke > $@

# M53: libvpx (VP8 full-motion video decode) — NetSurf/WebM video codec.
LIBVPX_LIB := build/libvpx-b1nix/$(B1NIX_TRIPLET)/install/lib/libvpx.a
$(LIBVPX_LIB): tools/ports/build-libvpx.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libvpx.sh >/dev/null

$(BUILD_DIR)/initramfs_m53_libvpx_smoke.inc: userspace/bin/m53_libvpx_smoke.c $(USERSPACE_DEPS) $(LIBVPX_LIB) $(LIBWEBP_LIB) $(LIBM_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_libvpx_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m53_libvpx_smoke_elf userspace/build/$(ARCH)/bin/m53_libvpx_smoke > $@

# M53: libwapcaplet (string internment) — first NetSurf browser-library dep.
LWC_LIB := build/libwapcaplet-b1nix/$(B1NIX_TRIPLET)/install/lib/liblwc.a
$(LWC_LIB): tools/ports/build-libwapcaplet.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libwapcaplet.sh >/dev/null

$(BUILD_DIR)/initramfs_m53_wapcaplet_smoke.inc: userspace/bin/m53_wapcaplet_smoke.c $(USERSPACE_DEPS) $(LWC_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_wapcaplet_smoke
	xxd -i -n vfs_m53_wapcaplet_smoke_elf userspace/build/$(ARCH)/bin/m53_wapcaplet_smoke > $@

# M53: libparserutils (input streams + bundled charset codecs) — NetSurf dep.
PU_LIB := build/libparserutils-b1nix/$(B1NIX_TRIPLET)/install/lib/libparserutils.a
$(PU_LIB): tools/ports/build-libparserutils.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libparserutils.sh >/dev/null

$(BUILD_DIR)/initramfs_m53_parserutils_smoke.inc: userspace/bin/m53_parserutils_smoke.c $(USERSPACE_DEPS) $(PU_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_parserutils_smoke
	xxd -i -n vfs_m53_parserutils_smoke_elf userspace/build/$(ARCH)/bin/m53_parserutils_smoke > $@

# M53: libhubbub (HTML5 tokeniser + tree builder) over libparserutils — NetSurf.
HUBBUB_LIB := build/libhubbub-b1nix/$(B1NIX_TRIPLET)/install/lib/libhubbub.a
$(HUBBUB_LIB): tools/ports/build-libhubbub.sh tools/ports/build-libparserutils.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libhubbub.sh >/dev/null

$(BUILD_DIR)/initramfs_m53_hubbub_smoke.inc: userspace/bin/m53_hubbub_smoke.c $(USERSPACE_DEPS) $(HUBBUB_LIB) $(PU_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_hubbub_smoke
	xxd -i -n vfs_m53_hubbub_smoke_elf userspace/build/$(ARCH)/bin/m53_hubbub_smoke > $@

# M53: libcss (CSS parser + selection) over libwapcaplet + libparserutils.
LIBCSS_LIB := build/libcss-b1nix/$(B1NIX_TRIPLET)/install/lib/libcss.a
$(LIBCSS_LIB): tools/ports/build-libcss.sh tools/ports/build-libwapcaplet.sh tools/ports/build-libparserutils.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libcss.sh >/dev/null

$(BUILD_DIR)/initramfs_m53_libcss_smoke.inc: userspace/bin/m53_libcss_smoke.c $(USERSPACE_DEPS) $(LIBCSS_LIB) $(LWC_LIB) $(PU_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_libcss_smoke
	xxd -i -n vfs_m53_libcss_smoke_elf userspace/build/$(ARCH)/bin/m53_libcss_smoke > $@

# M53: libdom (DOM) + hubbub binding over libhubbub + libparserutils + lwc.
LIBDOM_LIB := build/libdom-b1nix/$(B1NIX_TRIPLET)/install/lib/libdom.a
$(LIBDOM_LIB): tools/ports/build-libdom.sh tools/ports/build-libhubbub.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libdom.sh >/dev/null

$(BUILD_DIR)/initramfs_m53_libdom_smoke.inc: userspace/bin/m53_libdom_smoke.c $(USERSPACE_DEPS) $(LIBDOM_LIB) $(HUBBUB_LIB) $(PU_LIB) $(LWC_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_libdom_smoke
	xxd -i -n vfs_m53_libdom_smoke_elf userspace/build/$(ARCH)/bin/m53_libdom_smoke > $@

# M53: NetSurf helper/decoder libs — libnsutils, libnsgif, libnsbmp, libnslog.
NSUTILS_LIB := build/libnsutils-b1nix/$(B1NIX_TRIPLET)/install/lib/libnsutils.a
NSGIF_LIB := build/libnsgif-b1nix/$(B1NIX_TRIPLET)/install/lib/libnsgif.a
NSBMP_LIB := build/libnsbmp-b1nix/$(B1NIX_TRIPLET)/install/lib/libnsbmp.a
NSLOG_LIB := build/libnslog-b1nix/$(B1NIX_TRIPLET)/install/lib/libnslog.a
$(NSUTILS_LIB): tools/ports/build-libnsutils.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libnsutils.sh >/dev/null
$(NSGIF_LIB): tools/ports/build-libnsgif.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libnsgif.sh >/dev/null
$(NSBMP_LIB): tools/ports/build-libnsbmp.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libnsbmp.sh >/dev/null
$(NSLOG_LIB): tools/ports/build-libnslog.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libnslog.sh >/dev/null

$(BUILD_DIR)/initramfs_m53_nslibs_smoke.inc: userspace/bin/m53_nslibs_smoke.c $(USERSPACE_DEPS) $(NSUTILS_LIB) $(NSGIF_LIB) $(NSBMP_LIB) $(NSLOG_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_nslibs_smoke
	xxd -i -n vfs_m53_nslibs_smoke_elf userspace/build/$(ARCH)/bin/m53_nslibs_smoke > $@

# M53: userspace VirGL smoke — drives /dev/virtio-gpu (host-GPU-accelerated 3D).
$(BUILD_DIR)/initramfs_m53_virgl_smoke.inc: userspace/bin/m53_virgl_smoke.c $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_virgl_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m53_virgl_smoke_elf userspace/build/$(ARCH)/bin/m53_virgl_smoke > $@

$(CURL_ELF): tools/ports/build-curl.sh tools/toolchain/bin/b1nix-autotools-cc $(USERSPACE_DEPS)
	B1NIX_TLS="$(B1NIX_TLS)" tools/ports/build-curl.sh

$(INITRAMFS_CURL_INC): $(CURL_ELF)
	@mkdir -p $(dir $@)
	xxd -i -n vfs_curl_elf $(CURL_ELF) > $@

# Dropbear SSH server (dropbearmulti: server + dropbearkey + dropbearconvert,
# dispatched by argv[0]). Built static against the b1nix userspace libc.
$(DROPBEAR_ELF): tools/ports/build-dropbear.sh tools/toolchain/bin/b1nix-autotools-cc $(USERSPACE_DEPS)
	tools/ports/build-dropbear.sh all >/dev/null

$(INITRAMFS_DROPBEAR_INC): $(DROPBEAR_ELF)
	@mkdir -p $(dir $@)
	xxd -i -n vfs_dropbear_elf $(DROPBEAR_ELF) > $@

# GNU bash 5.2 — the default interactive shell (and /bin/sh). Built static
# against the b1nix userspace libc by tools/ports/build-bash.sh (autotools cross
# build with a preseeded config.cache).
$(BASH_ELF): tools/ports/build-bash.sh tools/toolchain/bin/b1nix-autotools-cc $(USERSPACE_DEPS)
	B1NIX_ARCH=$(ARCH) tools/ports/build-bash.sh >/dev/null

$(INITRAMFS_BASH_INC): $(BASH_ELF)
	@mkdir -p $(dir $@)
	xxd -i -n vfs_bash_elf $(BASH_ELF) > $@

OPENSSL_LIB := build/openssl-b1nix/$(B1NIX_TRIPLET)/install/lib/libssl.a
$(OPENSSL_LIB): tools/ports/build-openssl.sh tools/toolchain/bin/b1nix-autotools-cc
	tools/ports/build-openssl.sh >/dev/null

LIBIDN2_LIB := build/libidn2-b1nix/$(B1NIX_TRIPLET)/install/lib/libidn2.a
$(LIBIDN2_LIB): tools/ports/build-libidn2.sh tools/ports/build-libunistring.sh tools/toolchain/bin/b1nix-autotools-cc
	tools/ports/build-libidn2.sh >/dev/null

LIBPSL_LIB := build/libpsl-b1nix/$(B1NIX_TRIPLET)/install/lib/libpsl.a
$(LIBPSL_LIB): tools/ports/build-libpsl.sh tools/toolchain/bin/b1nix-autotools-cc
	tools/ports/build-libpsl.sh >/dev/null

$(WGET_ELF): tools/ports/build-wget.sh tools/toolchain/bin/b1nix-autotools-cc $(OPENSSL_LIB) $(LIBIDN2_LIB) $(LIBPSL_LIB)
	B1NIX_TLS="$(B1NIX_TLS)" tools/ports/build-wget.sh

$(INITRAMFS_WGET_INC): $(WGET_ELF)
	@mkdir -p $(dir $@)
	xxd -i -n vfs_wget_elf $(WGET_ELF) > $@

CACERT_PEM := build/cacert.pem
$(CACERT_PEM): tools/images/fetch-cacert.sh
	tools/images/fetch-cacert.sh $(CACERT_PEM)

$(INITRAMFS_CACERT_INC): $(CACERT_PEM)
	@mkdir -p $(dir $@)
	xxd -i -n vfs_cacert_pem $(CACERT_PEM) > $@

$(INITRAMFS_TESTWAV_INC): tools/images/gen_test_wav.py
	@mkdir -p $(dir $@)
	python3 tools/images/gen_test_wav.py $(BUILD_DIR)/test.wav
	xxd -i -n vfs_testwav $(BUILD_DIR)/test.wav > $@

# M51: the project's own scalable font (B1nix Mono) used by the
# FreeType/Cairo/HarfBuzz smokes. Mounted at /share/fonts/B1nixMono-Regular.ttf.
$(INITRAMFS_TESTFONT_INC): userspace/share/fonts/B1nixMono-Regular.ttf
	@mkdir -p $(dir $@)
	xxd -i -n vfs_testfont userspace/share/fonts/B1nixMono-Regular.ttf > $@

# M40: embed the committed static Linux x86_64 ELF blob. The blob is checked in
# (regenerated by hand via tools/m40/build-linux-hello.sh) so the kernel build
# does not require a Linux assembler on the build host.
$(INITRAMFS_M40_LINUX_INC): tools/m40/linux_hello.bin
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m40_linux_hello tools/m40/linux_hello.bin > $@

# M67: embed the committed prebuilt static Rust ELF blob. Checked in (regenerated
# by hand via tools/m67/build-hello.sh) so the kernel build needs no Rust
# toolchain. Same pattern as the M40 Linux blob above.
$(INITRAMFS_M67_RUST_INC): tools/m67/hello_b1nix.elf
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m67_rust_elf tools/m67/hello_b1nix.elf > $@

# M53: the loopback HTTPS server links mbedTLS; depend on NSFB_ELF so curl ->
# mbedTLS is built (its archives present) before this binary is embedded.
$(BUILD_DIR)/initramfs_m53_httpsd.inc: userspace/bin/m53_httpsd.c $(USERSPACE_DEPS) $(NSFB_ELF)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_httpsd
	xxd -i -n vfs_m53_httpsd_elf userspace/build/$(ARCH)/bin/m53_httpsd > $@

# M53: build the NetSurf framebuffer browser (the real nsfb binary) and package
# it + its runtime resources + a test page into the initramfs.
$(NSFB_ELF): tools/ports/build-netsurf-fb.sh tools/ports/build-libnsfb.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-netsurf-fb.sh >/dev/null

$(INITRAMFS_NETSURF_INC): $(NSFB_ELF) tools/images/gen_netsurf_initramfs.sh tools/netsurf-assets/test.html tools/netsurf-assets/test.png tools/netsurf-assets/test.svg tools/netsurf-assets/test.jxl
	@mkdir -p $(dir $@)
	B1NIX_ARCH=$(ARCH) tools/images/gen_netsurf_initramfs.sh $@

# Self-contained TLS test PKI (CA + server cert/key) embedded under
# /etc/tls-test for the M32 loopback HTTPS smoke. No network dependency.
TLS_TEST_DIR := build/tls-test
# Grouped target (&:): one script run produces all three PEMs. Without &: a
# parallel `make -j` runs this recipe once per output, racing on ca-key.pem.
$(TLS_TEST_DIR)/ca.pem $(TLS_TEST_DIR)/server-cert.pem $(TLS_TEST_DIR)/server-key.pem &: tools/images/gen-tls-test-certs.sh
	sh tools/images/gen-tls-test-certs.sh $(TLS_TEST_DIR) >/dev/null

$(INITRAMFS_TLSTEST_INC): $(TLS_TEST_DIR)/ca.pem $(TLS_TEST_DIR)/server-cert.pem $(TLS_TEST_DIR)/server-key.pem
	@mkdir -p $(dir $@)
	xxd -i -n vfs_tls_ca_pem $(TLS_TEST_DIR)/ca.pem > $@
	xxd -i -n vfs_tls_server_cert_pem $(TLS_TEST_DIR)/server-cert.pem >> $@
	xxd -i -n vfs_tls_server_key_pem $(TLS_TEST_DIR)/server-key.pem >> $@

$(BUILD_DIR)/initramfs_m30_pie.inc: userspace/bin/m30_pie.c $(USERSPACE_DEPS) userspace/linker_pie.ld
	@$(MAKE) -C userspace build/$(ARCH)/bin/m30_pie
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m30_pie_elf userspace/build/$(ARCH)/bin/m30_pie > $@

$(BUILD_DIR)/initramfs_m30_dynamic.inc: userspace/bin/m30_dynamic.c $(USERSPACE_DEPS) userspace/linker_pie.ld userspace/linker_shared.ld
	@$(MAKE) -C userspace build/$(ARCH)/bin/m30_dynamic
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m30_dynamic_elf userspace/build/$(ARCH)/bin/m30_dynamic > $@

ifeq ($(ARCH),x86_64)
$(INITRAMFS_SHARED_LIBC_INC): $(USERSPACE_DEPS) userspace/linker_shared.ld
	@$(MAKE) -C userspace build/$(ARCH)/libc.so.1
	@mkdir -p $(dir $@)
	xxd -i -n vfs_shared_libc_elf userspace/build/$(ARCH)/libc.so.1 > $@

# M69: a shared object the m30-dynamic smoke dlopen's at runtime.
$(INITRAMFS_M69_PLUGIN_INC): userspace/bin/m69_plugin.c $(USERSPACE_DEPS) userspace/linker_shared.ld
	@$(MAKE) -C userspace build/$(ARCH)/bin/m69_plugin.so
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m69_plugin_elf userspace/build/$(ARCH)/bin/m69_plugin.so > $@

# /lib/libgcc_s.so for the dynamically-linked C/C++ port binaries (see the
# INITRAMFS_LIBGCC_S_INC comment above). Sourced from the cross GCC's own
# libgcc_s.so.1 (a leaf ET_DYN — no further DT_NEEDED).
$(INITRAMFS_LIBGCC_S_INC): $(CROSS_TOOLCHAIN_ROOT)/bin/$(B1NIX_TRIPLET)-gcc
	@mkdir -p $(dir $@)
	@LIB="$$($< -print-file-name=libgcc_s.so.1 2>/dev/null)"; \
	[ -f "$$LIB" ] || { echo "libgcc_s.so.1 not found via $(B1NIX_TRIPLET)-gcc ($$LIB)"; exit 1; }; \
	xxd -i -n vfs_libgcc_s_elf "$$LIB" > $@
endif

$(INITRAMFS_BUSYBOX_INC): tools/ports/build-busybox.sh tools/configs/busybox-1.38.0.config $(USERSPACE_DEPS)
	B1NIX_ARCH=$(ARCH) tools/ports/build-busybox.sh
	@mkdir -p $(dir $@)
	xxd -i -n vfs_upstream_busybox_elf build/busybox-b1nix/$(B1NIX_TRIPLET)/busybox > $@




# ── AP Trampoline (flat binary linked at 0x8000) ──
AP_TRAMP_OBJ := $(BUILD_DIR)/kernel/arch/$(ARCH)/ap_trampoline_tmp.o
AP_TRAMP_BIN := $(BUILD_DIR)/ap_trampoline.bin
# ld.lld defaults its image base to 0x200000 and rejects -Ttext 0x8000 unless
# we pin the image base to 0. GNU ld doesn't recognise --image-base; pass it
# only when LD is lld.
ifneq (,$(findstring lld,$(LD)))
AP_IMAGE_BASE := --image-base=0
endif

$(AP_TRAMP_OBJ): kernel/arch/$(ARCH)/ap_trampoline.S
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) $(ARCH_CFLAGS) -c $< -o $@

$(AP_TRAMP_BIN): $(AP_TRAMP_OBJ)
	$(LD) $(ARCH_LDFLAGS) $(AP_IMAGE_BASE) -Ttext 0x8000 -o $@ --oformat binary $<

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
	@sed -e 's|@TIMEOUT@|$(GRUB_TIMEOUT)|g' \
	     -e 's|@ARCH@|$(ARCH)|g' \
	     -e 's|@CMDLINE@|$(KERNEL_CMDLINE)|g' \
	     -e 's|@MODULE_CMD@||g' \
	     boot/grub/grub.cfg > $(BUILD_DIR)/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(BUILD_DIR)/b1nix.iso $(BUILD_DIR)/iso

iso-core: $(KERNEL_ELF)
	@test -n "$(GRUB_MKRESCUE)" || (echo "missing grub-mkrescue or i686-elf-grub-mkrescue"; exit 1)
	@mkdir -p $(BUILD_DIR)/iso-core/boot/grub
	cp $(KERNEL_ELF) $(BUILD_DIR)/iso-core/boot/kernel.elf
	@sed -e 's|@TIMEOUT@|$(GRUB_TIMEOUT)|g' \
	     -e 's|@ARCH@|$(ARCH)|g' \
	     -e 's|@CMDLINE@|b1nix.test=1 b1nix.kvtest=abc123 b1nix.ssh-loopback=1 b1nix.smoke=core|g' \
	     -e 's|@MODULE_CMD@||g' \
	     boot/grub/grub.cfg > $(BUILD_DIR)/iso-core/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(BUILD_DIR)/b1nix-core.iso $(BUILD_DIR)/iso-core

iso-graphics: $(KERNEL_ELF)
	@test -n "$(GRUB_MKRESCUE)" || (echo "missing grub-mkrescue or i686-elf-grub-mkrescue"; exit 1)
	@mkdir -p $(BUILD_DIR)/iso-graphics/boot/grub
	cp $(KERNEL_ELF) $(BUILD_DIR)/iso-graphics/boot/kernel.elf
	@sed -e 's|@TIMEOUT@|$(GRUB_TIMEOUT)|g' \
	     -e 's|@ARCH@|$(ARCH)|g' \
	     -e 's|@CMDLINE@|b1nix.test=1 b1nix.kvtest=abc123 b1nix.ssh-loopback=1 b1nix.smoke=graphics|g' \
	     -e 's|@MODULE_CMD@||g' \
	     boot/grub/grub.cfg > $(BUILD_DIR)/iso-graphics/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(BUILD_DIR)/b1nix-graphics.iso $(BUILD_DIR)/iso-graphics

iso-shell: $(KERNEL_ELF)
	@test -n "$(GRUB_MKRESCUE)" || (echo "missing grub-mkrescue or i686-elf-grub-mkrescue"; exit 1)
	@mkdir -p $(BUILD_DIR)/iso-shell/boot/grub
	cp $(KERNEL_ELF) $(BUILD_DIR)/iso-shell/boot/kernel.elf
	@sed -e 's|@TIMEOUT@|$(GRUB_TIMEOUT)|g' \
	     -e 's|@ARCH@|$(ARCH)|g' \
	     -e 's|@CMDLINE@|b1nix.test=1 b1nix.kvtest=abc123 b1nix.ssh-loopback=1 b1nix.smoke=shell|g' \
	     -e 's|@MODULE_CMD@||g' \
	     boot/grub/grub.cfg > $(BUILD_DIR)/iso-shell/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(BUILD_DIR)/b1nix-shell.iso $(BUILD_DIR)/iso-shell

# V8 run instance: the kernel hook (gated by b1nix.v8run) mounts sata0 -> /mnt/v8
# and runs d8 on m58.js. It boots in test mode (b1nix.test=1) on purpose: the hook
# loads d8 off the disk early — before the rc's M14 test touches sata0 — and the
# active rc keeps the scheduler busy so d8's thread runs. (Without test mode, init
# drops to an interactive getty/bash that starves d8 on a single CPU.) Reuses the
# shared kernel.elf — no recompile, just a different grub cmdline. The d8 binary +
# m58.js ride on build/v8-out/v8-ext4.img, attached as sata0 by tests/smoke.sh.
iso-v8: $(KERNEL_ELF)
	@test -n "$(GRUB_MKRESCUE)" || (echo "missing grub-mkrescue or i686-elf-grub-mkrescue"; exit 1)
	@mkdir -p $(BUILD_DIR)/iso-v8/boot/grub
	cp $(KERNEL_ELF) $(BUILD_DIR)/iso-v8/boot/kernel.elf
	@sed -e 's|@TIMEOUT@|$(GRUB_TIMEOUT)|g' \
	     -e 's|@ARCH@|$(ARCH)|g' \
	     -e 's|@CMDLINE@|b1nix.test=1 b1nix.v8run b1nix.smoke=v8|g' \
	     -e 's|@MODULE_CMD@||g' \
	     boot/grub/grub.cfg > $(BUILD_DIR)/iso-v8/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(BUILD_DIR)/b1nix-v8.iso $(BUILD_DIR)/iso-v8
# NOTE: the smoke v8 instance runs JITLESS (no b1nix.v8jit) — that is the proven
# config and matches the jitless d8 on v8-ext4.img. The JIT d8 is exercised
# manually (build the default ISO with b1nix.v8jit + the v8-jit-ext4.img disk).

iso-live: root-image $(KERNEL_ELF)
	@test -n "$(GRUB_MKRESCUE)" || (echo "missing grub-mkrescue or i686-elf-grub-mkrescue"; exit 1)
	@mkdir -p $(BUILD_DIR)/iso-live/boot/grub
	cp $(KERNEL_ELF) $(BUILD_DIR)/iso-live/boot/kernel.elf
	cp $(BUILD_DIR)/root.ext4 $(BUILD_DIR)/iso-live/boot/rootfs.img
	@sed -e 's|@TIMEOUT@|$(GRUB_TIMEOUT)|g' \
	     -e 's|@ARCH@|$(ARCH)|g' \
	     -e 's|@CMDLINE@|$(KERNEL_CMDLINE)|g' \
	     -e 's|@MODULE_CMD@|module2 /boot/rootfs.img rootfs.img|g' \
	     boot/grub/grub.cfg > $(BUILD_DIR)/iso-live/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(BUILD_DIR)/b1nix-live.iso $(BUILD_DIR)/iso-live

# Installer ISO: a live ISO that ALSO carries b1nix-disk.img so that, after
# booting it, `b1nix_install /dev/<disk>` installs b1nix to that disk (the
# installer auto-finds /mnt/iso/boot/b1nix-disk.img). Needs disk-image (root for
# losetup/grub-install — passwordless sudo or run as root). Bigger ISO since it
# ships both the live rootfs.img and the disk image.
disk-iso: disk-image iso-live
	cp $(BUILD_DIR)/b1nix-disk.img $(BUILD_DIR)/iso-live/boot/b1nix-disk.img
	$(GRUB_MKRESCUE) -o $(BUILD_DIR)/b1nix-installer.iso $(BUILD_DIR)/iso-live
	@printf 'created %s\n  boot it, then run:  b1nix_install /dev/<target-disk>\n' "$(BUILD_DIR)/b1nix-installer.iso"

iso-test: root-image $(KERNEL_ELF)
	@test -n "$(GRUB_MKRESCUE)" || (echo "missing grub-mkrescue or i686-elf-grub-mkrescue"; exit 1)
	@mkdir -p $(BUILD_DIR)/iso-test/boot/grub
	cp $(KERNEL_ELF) $(BUILD_DIR)/iso-test/boot/kernel.elf
	cp $(BUILD_DIR)/root.ext4 $(BUILD_DIR)/iso-test/boot/rootfs.img
	@sed -e 's|@TIMEOUT@|$(GRUB_TIMEOUT)|g' \
	     -e 's|@ARCH@|$(ARCH)|g' \
	     -e 's|@CMDLINE@|$(KERNEL_CMDLINE) b1nix.test=1|g' \
	     -e 's|@MODULE_CMD@|module2 /boot/rootfs.img rootfs.img|g' \
	     boot/grub/grub.cfg > $(BUILD_DIR)/iso-test/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(BUILD_DIR)/b1nix-test.iso $(BUILD_DIR)/iso-test

userspace:
	@$(MAKE) -C userspace B1NIX_ARCH=$(ARCH)

userspace-install: userspace
	@$(MAKE) -C userspace B1NIX_ARCH=$(ARCH) install

busybox-package:
	B1NIX_ARCH=$(ARCH) tools/ports/build-busybox.sh

# Native toolchain for b1nix self-host: prefer Clang/LLVM, fallback to GCC.
NATIVE_CLANG_ROOT := $(shell \
	for p in build/native-clang/installed; do \
		if [ -d "$$p/bin" ]; then echo "$$p"; break; fi; \
	done)
install-native-toolchain:
	@if [ -n "$(NATIVE_CLANG_ROOT)" ]; then \
		echo "Installing native Clang toolchain from $(NATIVE_CLANG_ROOT) to rootfs..."; \
		mkdir -p $(BUILD_DIR)/rootfs/usr/bin $(BUILD_DIR)/rootfs/usr/lib; \
		cp -R $(NATIVE_CLANG_ROOT)/bin/. $(BUILD_DIR)/rootfs/usr/bin/ 2>/dev/null || true; \
		cp -R $(NATIVE_CLANG_ROOT)/lib/. $(BUILD_DIR)/rootfs/usr/lib/ 2>/dev/null || true; \
		echo "Native Clang toolchain installed to rootfs/usr/"; \
	elif [ -n "$(NATIVE_TOOLCHAIN_ROOT)" ]; then \
		echo "Installing native GCC toolchain (fallback) from $(NATIVE_TOOLCHAIN_ROOT) to rootfs..."; \
		mkdir -p $(BUILD_DIR)/rootfs/lib/gcc/$(B1NIX_TRIPLET)/13.2.0; \
		cp -R $(NATIVE_TOOLCHAIN_ROOT)/. $(BUILD_DIR)/rootfs/; \
		if [ -f "$(CROSS_TOOLCHAIN_ROOT)/lib/gcc/$(B1NIX_TRIPLET)/13.2.0/libgcc.a" ]; then \
			cp "$(CROSS_TOOLCHAIN_ROOT)/lib/gcc/$(B1NIX_TRIPLET)/13.2.0/libgcc.a" $(BUILD_DIR)/rootfs/lib/gcc/$(B1NIX_TRIPLET)/13.2.0/; \
		fi; \
		for o in crtbegin.o crtend.o crtbeginT.o crtbeginS.o crtendS.o; do \
			if [ -f "$(CROSS_TOOLCHAIN_ROOT)/lib/gcc/$(B1NIX_TRIPLET)/13.2.0/$$o" ]; then \
				cp "$(CROSS_TOOLCHAIN_ROOT)/lib/gcc/$(B1NIX_TRIPLET)/13.2.0/$$o" $(BUILD_DIR)/rootfs/lib/gcc/$(B1NIX_TRIPLET)/13.2.0/; \
			fi; \
		done; \
	else \
		echo "Note: native toolchain not built."; \
		echo "      Run tools/build-native-clang.sh (preferred) or tools/toolchain/build-native-toolchain.sh (fallback)."; \
	fi

install-ports: userspace-install install-native-toolchain
	tools/packages/install-ports.sh $(BUILD_DIR)/rootfs $(ARCH) $(PORTS_SOURCE) $(PACKAGE_INDEX_URL)

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
	@# which #include generated artifacts from build/$(ARCH) (ap_trampoline.inc and
	@# the initramfs_*.inc byte arrays). build/ is rsync-excluded above as it is
	@# host output, so stage just these generated *.inc inputs the compile needs.
	@mkdir -p $(BUILD_DIR)/rootfs/usr/src/b1nix/build/$(ARCH)
	@cp $(BUILD_DIR)/*.inc $(BUILD_DIR)/rootfs/usr/src/b1nix/build/$(ARCH)/ 2>/dev/null || true
	@du -sh $(BUILD_DIR)/rootfs/usr/src/b1nix | sed 's/^/source tree size: /'

# A full ISO must carry the staged rootfs. The old dependency on plain `iso`
# built the toolchain and then omitted it from the resulting image.
iso-full: iso-live

# Standalone-bootable disk image (MBR + real GRUB + ext4 root), excluding
# V8/Chromium. The in-guest installer (/bin/b1nix_install) copies this onto a
# target disk. Needs root for losetup/grub-install (the script re-execs sudo).
disk-image: root-image $(KERNEL_ELF)
	sh tools/images/mk-disk-image.sh $(ARCH) $(BUILD_DIR)/b1nix-disk.img

run: iso
	@command -v $(QEMU_X86_64) >/dev/null || (echo "missing qemu-system-x86_64"; exit 1)
	$(QEMU_X86_64) -m $(RUN_MEM) -cdrom $(BUILD_DIR)/b1nix.iso -serial stdio -no-reboot -boot d \
		-netdev user,id=n0 -device virtio-net-pci,netdev=n0

run-graphics: KERNEL_CMDLINE += b1nix.runlevel=5
run-graphics: iso
	@command -v $(QEMU_X86_64) >/dev/null || (echo "missing qemu-system-x86_64"; exit 1)
	$(QEMU_X86_64) -m $(RUN_MEM) -cdrom $(BUILD_DIR)/b1nix.iso -serial stdio -no-reboot -boot d \
		-netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
		-vga virtio -device virtio-tablet-pci

run-x86_64: run

run-x86: run

run-root: iso userspace-install root-image
	@command -v $(QEMU_X86_64) >/dev/null || (echo "missing qemu-system-x86_64"; exit 1)
	$(QEMU_X86_64) -cdrom $(BUILD_DIR)/b1nix.iso -serial stdio -no-reboot -boot d \
		-drive file=$(BUILD_DIR)/root.ext4,format=raw,if=virtio \
		-netdev user,id=n0 -device virtio-net-pci,netdev=n0

root-image: install-ports install-kernel-source
	@mkdir -p $(BUILD_DIR)/rootfs/bin $(BUILD_DIR)/rootfs/etc $(BUILD_DIR)/rootfs/dev $(BUILD_DIR)/rootfs/home $(BUILD_DIR)/rootfs/tmp $(BUILD_DIR)/rootfs/var
	@mkdir -p $(BUILD_DIR)/rootfs/proc $(BUILD_DIR)/rootfs/sys $(BUILD_DIR)/rootfs/mnt
	@mkdir -p $(BUILD_DIR)/rootfs/mnt/ext1 $(BUILD_DIR)/rootfs/mnt/ext2 $(BUILD_DIR)/rootfs/mnt/ext3 $(BUILD_DIR)/rootfs/mnt/ext4 $(BUILD_DIR)/rootfs/mnt/ext4nvme
	@ln -sfn . $(BUILD_DIR)/rootfs/persist
	@echo "b1nix persistent root" > $(BUILD_DIR)/rootfs/etc/motd
	@dd if=/dev/zero of=$(BUILD_DIR)/root.ext4 bs=1048576 count=$(ROOT_IMAGE_SIZE) 2>/dev/null
	@$(MKE2FS) -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q -L b1nix-root -d $(BUILD_DIR)/rootfs $(BUILD_DIR)/root.ext4 2>/dev/null || \
	 $(MKE2FS) -t ext4 -q -L b1nix-root -d $(BUILD_DIR)/rootfs $(BUILD_DIR)/root.ext4
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
	@echo "Running parallel full smoke tests for $(ARCH)..."
	sh tests/smoke.sh $(ARCH)

smoke-quick:
	@echo "Running quick smoke tests for $(ARCH)..."
	SMOKE_QUICK=1 sh tests/smoke.sh $(ARCH)

graphics-smoke:
	sh tests/graphics-smoke.sh $(ARCH)

memory-smoke:
	@sh tests/memory-smoke.sh
