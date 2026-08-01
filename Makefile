.DEFAULT_GOAL := all
ARCH ?= x86_64
export B1NIX_ARCH := $(ARCH)
export B1NIX_HEADERS_INSTALLED := 1
# BUILD_ROOT isolates the per-task kernel/initramfs/ISO output. Override it to
# build a second task (e.g. an M40 agent) without touching the main build:
#   make BUILD_ROOT=build-m40 ARCH=x86_64 iso
# The expensive, task-independent trees (build/$(ARCH)/toolchain, build/rust-native,
# the port build dirs) keep their literal `build/...` paths below, so they stay
# SHARED and read-only across isolated builds — never rebuilt per task.
BUILD_ROOT ?= build
BUILD_DIR := $(BUILD_ROOT)/$(ARCH)
INC_DIR := $(BUILD_DIR)/inc
USERSPACE_HDR_DEPS := $(BUILD_DIR)/.userspace-headers-installed
USERSPACE_DEPS := $(BUILD_DIR)/.userspace-bins-built
# Host triplet for the ported userspace toolchain + programs.
# Keep this mapping in sync with tools/toolchain/env.sh.
B1NIX_TRIPLET := x86_64-b1nix

# Port library targets. Defined here at the top so they can be referenced in
# dependency lists of targets further down (e.g. .userspace-bins-built).
# Layout: build/$(ARCH)/ports/<port>/install/lib/lib<name>.a
PCRE2_LIB := build/$(ARCH)/ports/pcre2/install/lib/libpcre2-8.a
LIBM_LIB := build/$(ARCH)/ports/openlibm/install/lib/libm.a
PIXMAN_LIB := build/$(ARCH)/ports/pixman/install/lib/libpixman-1.a
FREETYPE_LIB := build/$(ARCH)/ports/freetype/install/lib/libfreetype.a
CAIRO_LIB := build/$(ARCH)/ports/cairo/install/lib/libcairo.a
XKB_LIB := build/$(ARCH)/ports/xkbcommon/install/lib/libxkbcommon.a
FFI_LIB := build/$(ARCH)/ports/libffi/install/lib/libffi.a
WAYLAND_CLIENT_LIB := build/$(ARCH)/ports/wayland/install/lib/libwayland-client.a
WAYLAND_SERVER_LIB := build/$(ARCH)/ports/wayland/install/lib/libwayland-server.a
HB_LIB := build/$(ARCH)/ports/harfbuzz/install/lib/libharfbuzz.a
EXPAT_LIB := build/$(ARCH)/ports/expat/install/lib/libexpat.a
FONTCONFIG_LIB := build/$(ARCH)/ports/fontconfig/install/lib/libfontconfig.a
TINYGL_LIB := build/$(ARCH)/ports/tinygl/install/lib/libEGL.a
ZLIB_LIB := build/$(ARCH)/ports/zlib/install/lib/libz.a
LIBPNG_LIB := build/$(ARCH)/ports/libpng/install/lib/libpng16.a
LIBJPEG_LIB := build/$(ARCH)/ports/libjpeg/install/lib/libjpeg.a
LIBWEBP_LIB := build/$(ARCH)/ports/libwebp/install/lib/libwebp.a
LIBVPX_LIB := build/$(ARCH)/ports/libvpx/install/lib/libvpx.a
LWC_LIB := build/$(ARCH)/ports/libwapcaplet/install/lib/liblwc.a
PU_LIB := build/$(ARCH)/ports/libparserutils/install/lib/libparserutils.a
HUBBUB_LIB := build/$(ARCH)/ports/libhubbub/install/lib/libhubbub.a
LIBCSS_LIB := build/$(ARCH)/ports/libcss/install/lib/libcss.a
LIBDOM_LIB := build/$(ARCH)/ports/libdom/install/lib/libdom.a
NSUTILS_LIB := build/$(ARCH)/ports/libnsutils/install/lib/libnsutils.a
NSGIF_LIB := build/$(ARCH)/ports/libnsgif/install/lib/libnsgif.a
NSBMP_LIB := build/$(ARCH)/ports/libnsbmp/install/lib/libnsbmp.a
NSLOG_LIB := build/$(ARCH)/ports/libnslog/install/lib/libnslog.a
OPENSSL_LIB := build/$(ARCH)/ports/openssl/install/lib/libssl.a
IDN2_LIB := build/$(ARCH)/ports/libidn2/install/lib/libidn2.a
LIBPSL_LIB := build/$(ARCH)/ports/libpsl/install/lib/libpsl.a
LITEHTML_LIB := build/$(ARCH)/ports/litehtml/install/lib/liblitehtml.a
MBEDTLS_LIB := build/$(ARCH)/ports/mbedtls/install/lib/libmbedtls.a
UNISTRING_LIB := build/$(ARCH)/ports/libunistring/install/lib/libunistring.a

# Ensure all port libraries depend on headers stamp so that they are compiled
# only after libc.so.1 and headers are fully built and installed.
$(LIBM_LIB) $(PCRE2_LIB) $(PIXMAN_LIB) $(FREETYPE_LIB) $(CAIRO_LIB) $(XKB_LIB) \
$(WAYLAND_CLIENT_LIB) $(HB_LIB) $(EXPAT_LIB) $(FONTCONFIG_LIB) $(TINYGL_LIB) \
$(ZLIB_LIB) $(LIBPNG_LIB) $(LIBJPEG_LIB) $(LIBWEBP_LIB) $(LIBVPX_LIB) \
$(LWC_LIB) $(PU_LIB) $(HUBBUB_LIB) $(LIBCSS_LIB) $(LIBDOM_LIB) \
$(NSUTILS_LIB) $(NSGIF_LIB) $(NSBMP_LIB) $(NSLOG_LIB) $(IDN2_LIB) \
$(OPENSSL_LIB) $(LIBPSL_LIB) $(FFI_LIB) \
$(LITEHTML_LIB) $(MBEDTLS_LIB) $(UNISTRING_LIB): $(USERSPACE_HDR_DEPS)

KERNEL_ELF := $(BUILD_DIR)/kernel.elf
INITRAMFS_NATIVE_SMOKE_INC := $(INC_DIR)/initramfs_native_smoke.inc
# b1cc (in-tree C compiler + its M5/M32-M34 smoke corpus)
INITRAMFS_B1CC_M34_INC := $(INC_DIR)/initramfs_b1cc_m34.inc
INITRAMFS_CURL_INC := $(INC_DIR)/initramfs_curl.inc
INITRAMFS_CACERT_INC := $(INC_DIR)/initramfs_cacert.inc
INITRAMFS_TLSTEST_INC := $(INC_DIR)/initramfs_tlstest.inc
INITRAMFS_DROPBEAR_INC := $(INC_DIR)/initramfs_dropbear.inc
INITRAMFS_BUSYBOX_INC := $(INC_DIR)/initramfs_busybox.inc
INITRAMFS_TESTWAV_INC := $(INC_DIR)/initramfs_testwav.inc
INITRAMFS_TESTFONT_INC := $(INC_DIR)/initramfs_testfont.inc
# M40: a committed static Linux x86_64 ELF blob (tools/m40/linux_hello.bin)
# embedded as /bin/m40-linux-hello to validate the Linux ABI compat layer.
INITRAMFS_M40_LINUX_INC := $(INC_DIR)/initramfs_m40_linux.inc
# M67: a prebuilt static Rust (x86_64-unknown-b1nix) ELF blob
# (tools/m67/hello_b1nix.elf, regen via tools/m67/build-hello.sh) embedded as
# /bin/m67-rust to validate the Rust std cross-toolchain at runtime. x86_64-only.
INITRAMFS_M67_RUST_INC := $(INC_DIR)/initramfs_m67_rust.inc
# M53: NetSurf framebuffer browser + resources + test page.
INITRAMFS_NETSURF_INC := $(INC_DIR)/initramfs_netsurf_files.inc
# Canonical output of tools/ports/build-netsurf-fb.sh. Must NOT be a
# $(wildcard ...) lookup: on a clean build no candidate exists yet at
# Makefile-parse time, wildcard would evaluate empty, and every rule keyed off
# NSFB_ELF ($(NSFB_ELF): ..., initramfs_m53_httpsd.inc, INITRAMFS_NETSURF_INC,
# install-ports) would silently drop nsfb as a prerequisite — nsfb then never
# gets built and the M53 NetSurf smoke tests never start.
NSFB_ELF := build/$(ARCH)/ports/netsurf-fb/install/bin/nsfb

# Applet manifest for /bin replacement (M42 items 3 and 4).
APPLET_MANIFEST := tools/configs/applet-manifest.conf
APPLET_SYMLINKS_INC := $(INC_DIR)/initramfs_applet_symlinks.inc
APPLET_REGISTRATION_INC := $(INC_DIR)/initramfs_applet_registration.inc

EMBEDDED_USER_PROGRAMS := \
	hello \
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
	m36_smoke \
	diskbench \
	m38_sound \
	m39_smoke \
	m42_w5pre_smoke \
	m46_smoke \
	m57_smoke \
	m73_smoke \
	m63_smoke \
	m71_aslr \
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
	m91_skia_smoke \
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
# m64_clang_smoke is x86_64-only: the clang frontend links against LLVM libc++,
# but libc++ and i686-b1nix disagree on size_t mangling (unsigned int vs
# unsigned long), so the i686 clang link fails.
EMBEDDED_USER_PROGRAMS += m30_dynamic m64_clang_smoke
# Under musl (musl libc.so installed), drop all C++ binaries and raw-b1nix
# programs (they use b1nix syscalls/headers, not POSIX, so can't compile
# with musl headers). Re-enabled once libc++ and the POSIX rewrites land.
# ── Userspace C library ──
# LIBC_FLAVOR selects which implementation supplies the userspace runtime. Each
# flavor defines the same four things and nothing else: where its tree lives,
# the runtime blob, the loader name its binaries name in PT_INTERP, and the
# symbol its initramfs blob is emitted under. Consumers below read only those
# variables, so adding an implementation stays a self-contained block here.
LIBC_FLAVOR ?= musl
ifeq ($(LIBC_FLAVOR),musl)
LIBC_ROOT := build/$(ARCH)/ports/musl/install
# musl's libc.so is one file that is BOTH the C library and the dynamic loader
# (entry _dlstart). It also carries math, threads, timers, dlopen, crypt and the
# resolver, so -lm/-lpthread/-lrt/-ldl resolve against empty archives at link
# time and bind here at run time — one blob on the image, not one per facility.
LIBC_SO := $(LIBC_ROOT)/lib/libc.so
LIBC_LDSO_NAME := ld-musl-x86_64.so.1
LIBC_INC_SYM := vfs_ld_musl_x86_64_so_1
LIBC_INC_NAME := initramfs_ld_musl_x86_64_so_1.inc
endif

ifeq ($(LIBC_FLAVOR),musl)
CXX_RUNTIME_LIB := $(LIBC_ROOT)/lib
else
CXX_RUNTIME_LIB := build/$(ARCH)/toolchain/$(B1NIX_TRIPLET)/cross/$(B1NIX_TRIPLET)/lib
endif

MUSL_INSTALLED := $(LIBC_SO)
ifdef MUSL_INSTALLED
ifneq ($(MUSL_INSTALLED),)
CXX_RUNTIME_READY := $(BUILD_DIR)/.libcxx-musl-built
MUSL_LIBCXX_STAMP := $(BUILD_DIR)/.libcxx-musl-built
CFLAGS_EXTRA += -DB1NIX_MUSL
endif
endif
ifdef LIBC_SO
# The selected libc replaces the retired b1nix shared libc: /lib/$(LIBC_LDSO_NAME)
# is the blob and /lib/libc.so is a symlink onto it (see kernel/fs/initramfs.c).
INITRAMFS_SHARED_LIBC_INC :=
INITRAMFS_LD_MUSL_INC := $(INC_DIR)/$(LIBC_INC_NAME)
else
INITRAMFS_SHARED_LIBC_INC := $(INC_DIR)/initramfs_shared_libc.inc
INITRAMFS_LD_MUSL_INC :=
endif
ifndef MUSL_INSTALLED
INITRAMFS_M69_PLUGIN_INC := $(INC_DIR)/initramfs_m69_plugin.inc
# /lib/libc++.so.1 + /lib/libc++abi.so.1 — shared LLVM C++ stdlib (M89), linked
# from the PIC libc++.a/libc++abi.a by build-libcxx-shared.sh. The hosted C++
# smoke binaries (cxx_smoke/m55_iostream/m55_litehtml/m64_clang) link these via
# the libc++-default b1nix-c++; libc++abi.so.1 folds the libunwind DWARF unwinder.
INITRAMFS_LIBCXX_INC := $(INC_DIR)/initramfs_libcxx.inc
INITRAMFS_LIBCXXABI_INC := $(INC_DIR)/initramfs_libcxxabi.inc
# M91: Mesa shared libraries are stored in rootfs.img (Multiboot2 module), not in
# the kernel initramfs (too large for clang source location limit).
# See root-image target which copies .so files into $(BUILD_DIR)/rootfs/lib/.
INITRAMFS_M91_SO_INCS := \
	$(INC_DIR)/initramfs_m91_skia_dm.inc \
	$(INC_DIR)/initramfs_libskia.inc \
	$(INC_DIR)/initramfs_libraw_ptr.inc \
	$(INC_DIR)/initramfs_libfontconfig.inc \
	$(INC_DIR)/initramfs_libGLESv2.inc \
	$(INC_DIR)/initramfs_libEGL.inc \
	$(INC_DIR)/initramfs_libb1gui.inc
endif
endif

INITRAMFS_USER_PROGRAM_INCS := \
	$(addprefix $(INC_DIR)/initramfs_,$(addsuffix .inc,$(EMBEDDED_USER_PROGRAMS)))
AP_TRAMPOLINE_INC := $(INC_DIR)/ap_trampoline.inc
INITRAMFS_B1CC_INCS := $(INITRAMFS_B1CC_M34_INC)
INITRAMFS_B1CC_SELFHOST_INC := $(INC_DIR)/initramfs_b1cc_selfhost.inc

ifdef MUSL_INSTALLED
# Under musl the M69 dlopen plugin and the old b1nix-sysroot libc++ are replaced
# by the musl-linked shared objects in $(LIBC_ROOT)/lib/ (built by
# tools/ports/build-libcxx-musl.sh). Point the .inc rules at those.
INITRAMFS_M69_PLUGIN_INC :=
INITRAMFS_LIBCXX_INC := $(INC_DIR)/initramfs_libcxx.inc
INITRAMFS_LIBCXXABI_INC := $(INC_DIR)/initramfs_libcxxabi.inc
endif
# Bootstrap initramfs: init, sh, and essential boot loader binaries
# are embedded; all other apps, libraries, and tests live on the ext4 rootfs.
INITRAMFS_INCS := \
	$(INITRAMFS_NATIVE_SMOKE_INC) \
	$(INITRAMFS_LD_MUSL_INC)
GENERATED_INCS := $(AP_TRAMPOLINE_INC) $(INITRAMFS_INCS) $(APPLET_SYMLINKS_INC) $(APPLET_REGISTRATION_INC)
CURL_ELF := build/$(ARCH)/ports/curl/install/bin/curl
DROPBEAR_VERSION := 2022.83
DROPBEAR_ELF := build/$(ARCH)/ports/dropbear/dropbearmulti
ZSH_ELF := build/$(ARCH)/ports/zsh/install/bin/zsh
BMAKE_ELF := build/$(ARCH)/ports/bmake/install/bin/bmake
SAMU_ELF := build/$(ARCH)/ports/samurai/install/bin/samu
B1NIX_TLS ?= mbedtls
PORTS_SOURCE ?= download
PACKAGE_INDEX_URL ?= https://cdn.jsdelivr.net/gh/B1nix/b1nix-pkgs@main/pkgs/index

# Kernel build toolchain selector (Clang/LLVM).
TOOLCHAIN ?= clang
MKE2FS := $(shell command -v mke2fs 2>/dev/null || command -v /sbin/mke2fs 2>/dev/null || printf '%s' /opt/homebrew/opt/e2fsprogs/sbin/mke2fs)
# ISO builder. Limine (BSD-2-Clause) + xorriso replaced GRUB's grub-mkrescue
# (GPLv3) — see tools/mkiso.sh and boot/limine/limine.conf.in.
MKISO := tools/mkiso.sh
LIMINE := $(shell command -v limine 2>/dev/null)
QEMU_X86_64 := qemu-system-x86_64
# Default RAM for `make run`. QEMU's own default is only 128 MB, which OOMs on
# heavy pages (a real browser tab with JavaScript needs far more). Override with
# `make run RUN_MEM=2048`.
RUN_MEM ?= 1024
KERNEL_CMDLINE ?=
# Boot-menu timeout in seconds. 0 = boot the default entry immediately (used by
# the smoke harness so QEMU never stalls). Set e.g. BOOT_TIMEOUT=5 for an
# interactive build where you want to see/select the boot menu. GRUB_TIMEOUT is
# kept as an alias for scripts written before the Limine migration.
GRUB_TIMEOUT ?= 0
BOOT_TIMEOUT ?= $(GRUB_TIMEOUT)

# Persistent root image size in MB. 512MB fits native gcc + binutils + kernel
# source for self-host (M26). Override with: make ROOT_IMAGE_SIZE=256 root-image
ROOT_IMAGE_SIZE ?= 256

# Locate the native toolchain that tools/toolchain/build-native-toolchain.sh produced.
# Per-triplet: build/<arch>/toolchain/<triplet>/native_root by default, or
# ~/b1nix-toolchain/<triplet>/native_root when the project path has spaces (WSL).
# /root/b1nix-toolchain is the legacy Docker-builder location kept as fallback.
NATIVE_TOOLCHAIN_ROOT := $(shell \
	for p in build/$(ARCH)/toolchain/native_root build/$(ARCH)/toolchain/$(B1NIX_TRIPLET)/native_root $$HOME/b1nix-toolchain/$(B1NIX_ARCH)/native_root /root/b1nix-toolchain/$(B1NIX_TRIPLET)/native_root; do \
		if [ -d "$$p" ]; then echo "$$p"; break; fi; \
	done)
CROSS_TOOLCHAIN_ROOT := $(shell \
	for p in build/$(ARCH)/toolchain/cross build/$(ARCH)/toolchain/$(B1NIX_TRIPLET)/cross $$HOME/b1nix-toolchain/$(B1NIX_ARCH)/cross /root/b1nix-toolchain/$(B1NIX_TRIPLET)/cross; do \
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
	-I $(INC_DIR) \
	-I $(BUILD_DIR) \
	$(CFLAGS_EXTRA)


ifeq ($(ARCH),x86_64)
TARGET := x86_64-elf
ARCH_CFLAGS := --target=$(TARGET) -mcmodel=kernel -mno-sse -mno-mmx -mno-sse2 -mno-3dnow
ARCH_LDFLAGS := -m elf_x86_64 -z max-page-size=0x1000
LINKER_SCRIPT := kernel/arch/x86_64/linker.ld
ASM_SOURCES := kernel/arch/x86_64/boot.S kernel/arch/x86_64/context_switch.S kernel/arch/x86_64/isr.S kernel/arch/x86_64/user_jump.S kernel/arch/x86_64/syscall_entry.S kernel/arch/x86_64/fpu.S
ARCH_SOURCES := kernel/arch/x86_64/arch.c kernel/arch/x86_64/console.c kernel/arch/x86_64/fb_console.c kernel/arch/x86_64/interrupts.c kernel/arch/x86_64/io.c kernel/arch/x86_64/paging.c kernel/arch/x86_64/serial.c kernel/arch/x86_64/rtc.c kernel/arch/x86_64/signal.c kernel/arch/x86_64/lapic.c kernel/arch/x86_64/tlb.c kernel/arch/x86_64/coredump.c kernel/arch/x86_64/gdbstub.c
else
$(error Unsupported ARCH=$(ARCH). Active builds support ARCH=x86_64 only; i686 and AArch64 are archived)
endif

KERNEL_SOURCES := \
	kernel/main.c \
	kernel/lib/string.c \
	kernel/lib/klog.c \
	kernel/lib/stdio.c \
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
	kernel/syscall/resource_caps.c \
	kernel/fs/initramfs.c \
	kernel/fs/vfs.c \
	kernel/fs/aio.c \
	kernel/fs/vfs_slab.c \
	kernel/fs/pipe.c \
	kernel/fs/eventpoll.c \
	kernel/fs/inotify.c \
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
	kernel/fs/tmpfs.c \
	kernel/fs/sysfs.c \
	kernel/fs/journal.c \
	kernel/fs/filelock.c \
	kernel/fs/fuse.c \
	kernel/dev/acpi.c \
	kernel/dev/ioapic.c \
	kernel/dev/blk.c \
	kernel/dev/loop.c \
	kernel/dev/ramdisk.c \
	kernel/dev/video.c \
	kernel/ipc/mqueue.c \
	kernel/ipc/shm.c \
	kernel/ipc/sysv_sem.c \
	kernel/ipc/sysv_msg.c \
	kernel/sched/uidgid.c \
	kernel/sched/runqueue.c \
	kernel/sched/lockdep.c \
	kernel/sched/smp_test.c \
	kernel/sched/m28_ctxbench.c \
	kernel/sched/m28_heapbench.c \
	kernel/sched/futex.c \
	kernel/sched/rseq.c \
	kernel/sched/ptrace.c \
	kernel/sched/seccomp.c \
	kernel/user/process.c \
	$(ARCH_SOURCES)

ifneq ($(filter $(ARCH),x86_64),)
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
	kernel/dev/ac97.c \
	kernel/dev/mixer.c \
	kernel/dev/pty.c \
 	kernel/dev/serial_tty.c \
 	kernel/dev/virtio_gpu.c \
 	kernel/dev/virtio_input.c \
	kernel/dev/drm.c \
	kernel/dev/fb.c \
	kernel/dev/input.c \
	kernel/net/net.c \
	kernel/net/socket.c \
	kernel/net/unix.c \
	kernel/net/arp.c \
	kernel/net/ethernet.c \
	kernel/net/ipv4.c \
	kernel/net/ipv6.c \
	kernel/net/icmp.c \
	kernel/net/ndp.c \
	kernel/net/tcp.c \
	kernel/net/udp.c \
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

.PHONY: all analyze objects FORCE iso iso-sys iso-gfx iso-posix iso-blk iso-openrc iso-live iso-test iso-full check-dynamic \
	check-ports \
	userspace userspace-install busybox-package busybox-iso \
	install-native-toolchain install-kernel-source install-ports root-image disk-image \
	run run-graphics run-x86_64 run-root check-tools clean distclean \
	smoke smoke-quick graphics-smoke memory-smoke build-all test-b1cc

all: check-b1cc-sync $(KERNEL_ELF)

# build-all — one orchestrator that builds the whole working system in dependency
# order by reusing the existing build scripts (see tools/build-all.sh). Forwards
# ARCH; pass extra flags via BUILD_ALL_ARGS, e.g.:
#   make build-all                                  # OS + ISO (default)
#   make build-all BUILD_ALL_ARGS=--all             # + every opt-in component
#   make build-all BUILD_ALL_ARGS=--with-dynamic-clang
build-all:
	ARCH=$(ARCH) sh tools/build-all.sh $(BUILD_ALL_ARGS)

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

# Anything in userspace libc/includes/crt that affects every embedded ELF.
# Listed as prereqs of each *.inc so changes to libc force an xxd re-bundle —
# otherwise initramfs ships with stale userspace and the kernel sees old libc.
$(BUILD_DIR)/.userspace-headers-installed: \
	$(wildcard userspace/libc/*.c) \
	$(wildcard userspace/libgui/*.c) \
	$(wildcard userspace/include/*.h) \
	$(wildcard userspace/include/b1nix/*.h) \
	$(wildcard userspace/include/arpa/*.h) \
	$(wildcard userspace/include/netinet/*.h) \
	$(wildcard userspace/include/sys/*.h) \
	$(wildcard userspace/crt/*.S) \
	userspace/Makefile
	# Build libs + headers into sysroot in one serialized pass, then mark done.
	# Port scripts check B1NIX_HEADERS_INSTALLED=1 and skip their own sub-makes.
	@$(MAKE) -C userspace B1NIX_ARCH=$(ARCH) install-headers-libs
	@touch $@

# Second stamp: build ALL userspace binaries after libraries are ready.
# .inc rules depend on this so they can safely xxd pre-built outputs without
# spawning competing $(MAKE) -C userspace processes in parallel.
#
# IMPORTANT: This stamp MUST list every port library that userspace/Makefile
# rules link against. Without these deps, the root-level parallel build can
# invoke port build scripts at the same time as $(MAKE) -C userspace does
# (which triggers the same port scripts internally) → corrupt concurrent builds.
# After all ports are listed here, $(MAKE) -C userspace just compiles ELF
# binaries and finds all libraries already present — no port script is invoked.
#
# Port libraries that are only ever referenced by .inc rules that already have
# them as explicit dependencies (and NOT used directly by userspace/Makefile
# targets) do NOT need to be listed here. Only list libs that userspace/Makefile
# pulls in transitively (openlibm, wayland, libffi, pcre2, zlib, pixman,
# freetype, cairo, xkbcommon, harfbuzz, fontconfig, expat, libjpeg, libpng,
# libwebp, libvpx, all NetSurf platform libs, tinygl, libidn2).
$(BUILD_DIR)/.userspace-bins-built: $(BUILD_DIR)/.userspace-headers-installed \
	$(wildcard userspace/bin/*.c) $(wildcard userspace/bin/*.S) \
	$(wildcard userspace/src/*.c) \
	$(wildcard userspace/b1cc/src/*.c) \
	$(wildcard userspace/displayd/*.c) \
	$(wildcard userspace/duktape/duktape.c) \
	$(LIBM_LIB) \
	$(PCRE2_LIB) \
	$(PIXMAN_LIB) \
	$(FREETYPE_LIB) \
	$(CAIRO_LIB) \
	$(XKB_LIB) \
	$(WAYLAND_CLIENT_LIB) \
	$(HB_LIB) \
	$(EXPAT_LIB) \
	$(FONTCONFIG_LIB) \
	$(TINYGL_LIB) \
	$(ZLIB_LIB) \
	$(LIBPNG_LIB) \
	$(LIBJPEG_LIB) \
	$(LIBWEBP_LIB) \
	$(LIBVPX_LIB) \
	$(LWC_LIB) \
	$(PU_LIB) \
	$(HUBBUB_LIB) \
	$(LIBCSS_LIB) \
	$(LIBDOM_LIB) \
	$(NSUTILS_LIB) $(NSGIF_LIB) $(NSBMP_LIB) $(NSLOG_LIB) \
	$(LIBIDN2_LIB) \
	$(MBEDTLS_LIB)
	@$(MAKE) -C userspace B1NIX_ARCH=$(ARCH) install
	@touch $@




$(INITRAMFS_NATIVE_SMOKE_INC): userspace/bin/native_smoke.S $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/$(ARCH)/bin/native_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_native_smoke_elf userspace/build/$(ARCH)/bin/native_smoke > $@

$(INITRAMFS_B1CC_M34_INC): tools/images/gen_b1cc_m34_initramfs.sh userspace/bin/b1cc_m34_corpus.c userspace/Makefile $(wildcard userspace/b1cc/tests/*.c) $(USERSPACE_DEPS)
	@mkdir -p $(dir $@)
	B1NIX_ARCH=$(ARCH) sh tools/images/gen_b1cc_m34_initramfs.sh $@

# displayd is multi-source, not a single bin/*.c
DISPLAYD_SRCS := $(wildcard userspace/displayd/*.c) $(wildcard userspace/displayd/*.h)
$(INC_DIR)/initramfs_displayd.inc: $(DISPLAYD_SRCS) $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/$(ARCH)/bin/displayd
	@mkdir -p $(dir $@)
	xxd -i -n vfs_displayd_elf userspace/build/$(ARCH)/bin/displayd > $@

$(INC_DIR)/initramfs_%.inc: userspace/bin/%.c $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/$(ARCH)/bin/$*
	@mkdir -p $(dir $@)
	xxd -i -n vfs_$*_elf userspace/build/$(ARCH)/bin/$* > $@

# M32/M33: bundle the on-device b1cc + its static-link inputs into one .inc.
# b1cc is a multi-source binary built from the separate b1cc repo via b1nix-cc
# (see userspace/Makefile /bin/b1cc rule); crt0.o and libb1nix.a are the same
# artifacts b1cc's internal linker consumes, shipped to /lib/b1cc on device.
# Depend on the b1cc *sources* (same glob as userspace/Makefile's B1CC_SRCDIR) so
# editing b1cc re-embeds it — otherwise the .inc looks up-to-date vs
# USERSPACE_DEPS and the stale compiler binary gets shipped.
B1CC_SELFHOST_SRCS := $(wildcard $(or $(B1CC_SRCDIR),userspace/b1cc/src)/*.c)
$(INC_DIR)/initramfs_b1cc_selfhost.inc: $(USERSPACE_DEPS) $(B1CC_SELFHOST_SRCS)
	@$(MAKE) -C userspace build/$(ARCH)/bin/b1cc
	@mkdir -p $(dir $@)
	xxd -i -n vfs_b1cc_elf userspace/build/$(ARCH)/bin/b1cc > $@
	xxd -i -n vfs_b1cc_crt0 userspace/build/$(ARCH)/crt/crt0.o >> $@
	xxd -i -n vfs_b1cc_libc userspace/build/$(ARCH)/libb1nix.a >> $@
	xxd -i -n vfs_b1cc_crt0dyn userspace/build/$(ARCH)/crt/crt0-dynamic.o >> $@

# Depends on $(CURL_ELF): building curl (with B1NIX_TLS=mbedtls) produces the
# static mbedTLS archives that m32_nettool's tls-server links against, so curl
# must build first to guarantee the libs exist.
$(INC_DIR)/initramfs_m32_nettool.inc: userspace/bin/m32_nettool.c $(USERSPACE_DEPS) $(CURL_ELF)
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m32_nettool_elf userspace/build/$(ARCH)/bin/m32_nettool > $@

# PCRE2: cross-build the static 8-bit library, then link the smoke against it.
PCRE2_LIB := build/$(ARCH)/ports/pcre2/install/lib/libpcre2-8.a
$(PCRE2_LIB): tools/ports/build-pcre2.sh tools/toolchain/bin/b1nix-autotools-cc
	tools/ports/build-pcre2.sh >/dev/null

# M51: libm (openlibm), cross-built static, linked into m51_smoke.
LIBM_LIB := build/$(ARCH)/ports/openlibm/install/lib/libm.a
$(LIBM_LIB): tools/ports/build-openlibm.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-openlibm.sh >/dev/null

# M58: /bin/js embeds Duktape and links the ported openlibm — so libm must be
# built before js. js.c links duktape.c (a vendored amalgamation under
# userspace/duktape/), both compiled by the userspace Makefile's custom rule.
$(INC_DIR)/initramfs_js.inc: userspace/bin/js.c userspace/duktape/duktape.c \
		userspace/duktape/duktape.h userspace/duktape/duk_config.h \
		$(USERSPACE_DEPS) $(LIBM_LIB)
	@mkdir -p $(dir $@)
	@$(MAKE) -C userspace build/$(ARCH)/bin/js
	xxd -i -n vfs_js_elf userspace/build/$(ARCH)/bin/js > $@

$(INC_DIR)/initramfs_m51_smoke.inc: userspace/bin/m51_smoke.c $(USERSPACE_DEPS) $(LIBM_LIB)
	@mkdir -p $(dir $@)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m51_smoke
	xxd -i -n vfs_m51_smoke_elf userspace/build/$(ARCH)/bin/m51_smoke > $@

# M51: pixman (generic C), cross-built static, linked into m51_pixman_smoke.
PIXMAN_LIB := build/$(ARCH)/ports/pixman/install/lib/libpixman-1.a
$(PIXMAN_LIB): tools/ports/build-pixman.sh tools/ports/build-openlibm.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-pixman.sh >/dev/null

$(INC_DIR)/initramfs_m51_pixman_smoke.inc: userspace/bin/m51_pixman_smoke.c $(USERSPACE_DEPS) $(PIXMAN_LIB) $(LIBM_LIB)
	@mkdir -p $(dir $@)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m51_pixman_smoke
	xxd -i -n vfs_m51_pixman_smoke_elf userspace/build/$(ARCH)/bin/m51_pixman_smoke > $@

# M51: FreeType (TrueType + smooth rasterizer), cross-built static.
FREETYPE_LIB := build/$(ARCH)/ports/freetype/install/lib/libfreetype.a
$(FREETYPE_LIB): tools/ports/build-freetype.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-freetype.sh >/dev/null

$(INC_DIR)/initramfs_m51_freetype_smoke.inc: userspace/bin/m51_freetype_smoke.c $(USERSPACE_DEPS) $(FREETYPE_LIB) $(LIBM_LIB)
	@mkdir -p $(dir $@)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m51_freetype_smoke
	xxd -i -n vfs_m51_freetype_smoke_elf userspace/build/$(ARCH)/bin/m51_freetype_smoke > $@

# M51: Cairo (image surface + FreeType backend), cross-built static.
CAIRO_LIB := build/$(ARCH)/ports/cairo/install/lib/libcairo.a
$(CAIRO_LIB): tools/ports/build-cairo.sh $(PIXMAN_LIB) $(FREETYPE_LIB)
	B1NIX_ARCH=$(ARCH) tools/ports/build-cairo.sh >/dev/null

$(INC_DIR)/initramfs_m51_cairo_smoke.inc: userspace/bin/m51_cairo_smoke.c $(USERSPACE_DEPS) $(CAIRO_LIB) $(FREETYPE_LIB) $(PIXMAN_LIB) $(LIBM_LIB)
	@mkdir -p $(dir $@)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m51_cairo_smoke
	xxd -i -n vfs_m51_cairo_smoke_elf userspace/build/$(ARCH)/bin/m51_cairo_smoke > $@

$(INC_DIR)/initramfs_m51_cairo_wayland.inc: userspace/bin/m51_cairo_wayland.c $(USERSPACE_DEPS) $(CAIRO_LIB) $(FREETYPE_LIB) $(PIXMAN_LIB) $(LIBM_LIB)
	@mkdir -p $(dir $@)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m51_cairo_wayland
	xxd -i -n vfs_m51_cairo_wayland_elf userspace/build/$(ARCH)/bin/m51_cairo_wayland > $@

# M51: xkbcommon (keymap compile + keysym translation), cross-built static.
XKB_LIB := build/$(ARCH)/ports/xkbcommon/install/lib/libxkbcommon.a
$(XKB_LIB): tools/ports/build-xkbcommon.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-xkbcommon.sh >/dev/null

$(INC_DIR)/initramfs_m51_xkb_smoke.inc: userspace/bin/m51_xkb_smoke.c $(USERSPACE_DEPS) $(XKB_LIB)
	@mkdir -p $(dir $@)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m51_xkb_smoke
	xxd -i -n vfs_m51_xkb_smoke_elf userspace/build/$(ARCH)/bin/m51_xkb_smoke > $@

# M49: libwayland-client/server share one generated source/build tree. Build it
# once at the top level so parallel initramfs packaging cannot race two nested
# userspace make invocations through tools/ports/build-wayland.sh.
WAYLAND_CLIENT_LIB := build/$(ARCH)/ports/wayland/install/lib/libwayland-client.a
WAYLAND_SERVER_LIB := build/$(ARCH)/ports/wayland/install/lib/libwayland-server.a
$(WAYLAND_CLIENT_LIB): tools/ports/build-wayland.sh $(LIBFFI_LIB)
	B1NIX_ARCH=$(ARCH) tools/ports/build-wayland.sh >/dev/null
$(WAYLAND_SERVER_LIB): $(WAYLAND_CLIENT_LIB)

$(INC_DIR)/initramfs_m49_libwayland.inc: $(WAYLAND_CLIENT_LIB)
$(INC_DIR)/initramfs_m49_libwayland_server.inc: $(WAYLAND_SERVER_LIB)

# M51: HarfBuzz (HB_TINY, unified C++ build via cross g++), cross-built static.
HB_LIB := build/$(ARCH)/ports/harfbuzz/install/lib/libharfbuzz.a
$(HB_LIB): tools/ports/build-harfbuzz.sh $(MUSL_LIBCXX_STAMP)
	B1NIX_ARCH=$(ARCH) tools/ports/build-harfbuzz.sh >/dev/null

$(INC_DIR)/initramfs_m51_harfbuzz_smoke.inc: userspace/bin/m51_harfbuzz_smoke.c $(USERSPACE_DEPS) $(HB_LIB) $(LIBM_LIB)
	@mkdir -p $(dir $@)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m51_harfbuzz_smoke
	xxd -i -n vfs_m51_harfbuzz_smoke_elf userspace/build/$(ARCH)/bin/m51_harfbuzz_smoke > $@

# M51: expat (XML) + Fontconfig (font discovery), cross-built static.
EXPAT_LIB := build/$(ARCH)/ports/expat/install/lib/libexpat.a
$(EXPAT_LIB): tools/ports/build-expat.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-expat.sh >/dev/null
FONTCONFIG_LIB := build/$(ARCH)/ports/fontconfig/install/lib/libfontconfig.a
$(FONTCONFIG_LIB): tools/ports/build-fontconfig.sh $(EXPAT_LIB) $(FREETYPE_LIB)
	B1NIX_ARCH=$(ARCH) tools/ports/build-fontconfig.sh >/dev/null

$(INC_DIR)/initramfs_m51_fontconfig_smoke.inc: userspace/bin/m51_fontconfig_smoke.c $(USERSPACE_DEPS) $(FONTCONFIG_LIB) $(EXPAT_LIB) $(FREETYPE_LIB) $(LIBM_LIB)
	@mkdir -p $(dir $@)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m51_fontconfig_smoke
	xxd -i -n vfs_m51_fontconfig_smoke_elf userspace/build/$(ARCH)/bin/m51_fontconfig_smoke > $@

# M52: TinyGL (software OpenGL) + b1nix EGL shim, cross-built static.
TINYGL_LIB := build/$(ARCH)/ports/tinygl/install/lib/libEGL.a
$(TINYGL_LIB): tools/ports/build-tinygl.sh userspace/libegl/b1egl.c userspace/include/EGL/egl.h
	B1NIX_ARCH=$(ARCH) tools/ports/build-tinygl.sh >/dev/null

# ── C++ / Mesa / Skia / Mesa-VirGL .inc rules ──
# Skipped under musl: these need libc++/libstdc++ which has not been built
# against musl yet. Placeholder .inc files are pre-created in the build dir.
$(INC_DIR)/initramfs_m52_gl_smoke.inc: userspace/bin/m52_gl_smoke.c $(USERSPACE_DEPS) $(TINYGL_LIB) $(LIBM_LIB)
	@mkdir -p $(dir $@)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m52_gl_smoke
	xxd -i -n vfs_m52_gl_smoke_elf userspace/build/$(ARCH)/bin/m52_gl_smoke > $@
# Hosted C++ runtime smoke. Enable LLVM libc++ against the b1nix libc first
# (idempotent: stages headers + fixes mbstate_t config), then build via the
# cross clang C++ wrapper.
$(INC_DIR)/initramfs_cxx_smoke.inc: userspace/bin/cxx_smoke.cpp $(USERSPACE_DEPS) $(CXX_RUNTIME_READY) tools/toolchain/bin/b1nix-c++ tools/toolchain/enable-cxx-toolchain.sh
	@tools/toolchain/enable-cxx-toolchain.sh $(B1NIX_TRIPLET) >/dev/null 2>&1 || true
	@mkdir -p $(dir $@)
	@$(MAKE) -C userspace build/$(ARCH)/bin/cxx_smoke
	xxd -i -n vfs_cxx_smoke_elf userspace/build/$(ARCH)/bin/cxx_smoke > $@

$(INC_DIR)/initramfs_m64_clang_smoke.inc: userspace/bin/m64_clang_smoke.cpp $(USERSPACE_DEPS) $(CXX_RUNTIME_READY) tools/toolchain/bin/b1nix-clang++ tools/toolchain/bin/b1nix-c++
	@tools/toolchain/enable-cxx-toolchain.sh $(B1NIX_TRIPLET) >/dev/null 2>&1 || true
	@mkdir -p $(dir $@)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m64_clang_smoke
	xxd -i -n vfs_m64_clang_smoke_elf userspace/build/$(ARCH)/bin/m64_clang_smoke > $@

# M55: std::iostream + std::filesystem acceptance test (hosted libc++).
$(INC_DIR)/initramfs_m55_iostream.inc: userspace/bin/m55_iostream.cpp $(USERSPACE_DEPS) $(CXX_RUNTIME_READY) tools/toolchain/bin/b1nix-c++ tools/toolchain/enable-cxx-toolchain.sh
	@tools/toolchain/enable-cxx-toolchain.sh $(B1NIX_TRIPLET) >/dev/null 2>&1 || true
	@mkdir -p $(dir $@)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m55_iostream
	xxd -i -n vfs_m55_iostream_elf userspace/build/$(ARCH)/bin/m55_iostream > $@

# Serialize the Mesa build to prevent race conditions during parallel builds.
# M89: Mesa C++ is built against the shared LLVM libc++ (the static Mesa archives
# fold libc++/libc++abi; the demos link them — see build-m52-mesa-demo.sh).
# Mesa links against musl + the shared libc++ (toolchain), needing only the musl
# headers — NOT the userspace binaries. Depending on $(USERSPACE_DEPS) re-ran the
# whole Mesa build on every userspace-bin change and raced the demos' `-lOSMesa`
# link against a momentarily-absent libOSMesa.so. Depend on the headers stamp.
$(BUILD_DIR)/.mesa-built: tools/ports/build-mesa.sh $(BUILD_DIR)/.userspace-headers-installed
	@mkdir -p $(dir $@)
	B1NIX_CXX_STDLIB=libc++ B1NIX_ARCH=$(ARCH) tools/ports/build-mesa.sh >/dev/null
	@touch $@

# M52: real Mesa OSMesa (software OpenGL) demo. The shared demo builder builds
# the whole Mesa stack (build-mesa.sh) and links the demo against it.
$(INC_DIR)/initramfs_m52_osmesa.inc: userspace/bin/m52_osmesa.c tools/demos/build-m52-mesa-demo.sh $(BUILD_DIR)/.mesa-built $(USERSPACE_DEPS)
	B1NIX_CXX_STDLIB=libc++ B1NIX_ARCH=$(ARCH) tools/demos/build-m52-mesa-demo.sh m52_osmesa userspace/build/$(ARCH)/bin/m52_osmesa
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m52_osmesa_elf userspace/build/$(ARCH)/bin/m52_osmesa > $@

# M53 variant B: Mesa's gallium virgl driver renders on the host GPU through the
# b1nix /dev/virtio-gpu winsys. tools/demos/build-m53-mesa-virgl.sh builds the Mesa
# stack (with the virgl driver) and links the pipe-API render test against it.
$(INC_DIR)/initramfs_m53_mesa_virgl.inc: userspace/bin/m53_mesa_virgl.c tools/demos/build-m53-mesa-virgl.sh $(BUILD_DIR)/.mesa-built $(wildcard tools/patches/mesa/files/src/gallium/winsys/virgl/b1nix/*) $(USERSPACE_DEPS)
	B1NIX_CXX_STDLIB=libc++ B1NIX_ARCH=$(ARCH) tools/demos/build-m53-mesa-virgl.sh userspace/build/$(ARCH)/bin/m53_mesa_virgl
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m53_mesa_virgl_elf userspace/build/$(ARCH)/bin/m53_mesa_virgl > $@

# M52: programmable GLSL shader demo, sharing the same Mesa build as the OSMesa
# demo. Exercises the GL 2.x programmable pipeline (shaders, VBOs, varyings).
$(INC_DIR)/initramfs_m52_glsl.inc: userspace/bin/m52_glsl.c tools/demos/build-m52-mesa-demo.sh $(BUILD_DIR)/.mesa-built $(USERSPACE_DEPS)
	B1NIX_CXX_STDLIB=libc++ B1NIX_ARCH=$(ARCH) tools/demos/build-m52-mesa-demo.sh m52_glsl userspace/build/$(ARCH)/bin/m52_glsl
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m52_glsl_elf userspace/build/$(ARCH)/bin/m52_glsl > $@

# M59: EGL over the real Mesa OSMesa softpipe. tools/demos/build-m59-egl.sh compiles
# the OSMesa-backed EGL implementation (userspace/libegl/b1egl_mesa.c) together
# with the off-screen pbuffer smoke and links them against the same Mesa stack
# as the M52 OSMesa demo. The smoke renders entirely off-screen (no displayd).
$(INC_DIR)/initramfs_m59_smoke.inc: userspace/bin/m59_smoke.c userspace/libegl/b1egl_mesa.c userspace/include/EGL/egl.h tools/demos/build-m59-egl.sh $(BUILD_DIR)/.mesa-built $(USERSPACE_DEPS)
	B1NIX_ARCH=$(ARCH) B1NIX_CXX_STDLIB=libc++ tools/demos/build-m59-egl.sh userspace/build/$(ARCH)/bin/m59_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m59_smoke_elf userspace/build/$(ARCH)/bin/m59_smoke > $@

# M91: Skia 2D graphics library (standalone build with Ganesh GPU backend).
$(BUILD_DIR)/.skia-built: tools/ports/build-skia.sh $(USERSPACE_DEPS)
	@mkdir -p $(dir $@)
	B1NIX_CXX_STDLIB=libc++ B1NIX_ARCH=$(ARCH) tools/ports/build-skia.sh >/dev/null
	@touch $@

$(INC_DIR)/initramfs_m91_skia_smoke.inc: userspace/bin/m91_skia_smoke.cpp tools/demos/build-m91-skia-demo.sh $(BUILD_DIR)/.skia-built $(BUILD_DIR)/.mesa-built $(USERSPACE_DEPS)
	B1NIX_CXX_STDLIB=libc++ B1NIX_ARCH=$(ARCH) tools/demos/build-m91-skia-demo.sh m91_skia_smoke userspace/build/$(ARCH)/bin/m91_skia_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m91_skia_smoke_elf userspace/build/$(ARCH)/bin/m91_skia_smoke > $@

# M91: shared-library deps for Skia (libskia.so, libraw_ptr.so, libfontconfig.so,
# libb1gui.so, libGLESv2.so, libEGL.so). build-skia-shared-deps.sh builds all .so
# from the port trees; xxd converts each to an initramfs .inc.

# M91: Skia dm testing tool — too large for initramfs (400MB+).
# Ships in rootfs.img as /bin/skia-dm via root-image target.
# initramfs gets a 1-byte placeholder.
$(INC_DIR)/initramfs_m91_skia_dm.inc: $(BUILD_DIR)/.skia-built
	@mkdir -p $(dir $@)
	@echo '/* dm not built — ships in rootfs.img */' > $@
	@echo 'static const unsigned char vfs_m91_skia_dm_elf[1] = {0};' >> $@
	@echo 'static const unsigned int vfs_m91_skia_dm_elf_len = 0;' >> $@
M91_SHARED_DEPS_STAMP := $(BUILD_DIR)/.m91-shared-deps-stamp
$(M91_SHARED_DEPS_STAMP): tools/ports/build-skia-shared-deps.sh $(BUILD_DIR)/.skia-built $(BUILD_DIR)/.mesa-built $(FONTCONFIG_LIB)
	@mkdir -p $(dir $@)
	B1NIX_ARCH=$(ARCH) sh tools/ports/build-skia-shared-deps.sh
	@# Replace sysroot stubs with real .so so cross-cc link step finds them
	@SYSROOT_LIB=$(CXX_RUNTIME_LIB); \
	for so in libEGL.so libGLESv2.so libfontconfig.so; do \
		if [ -f userspace/build/$(ARCH)/$$so ]; then \
			cp -f userspace/build/$(ARCH)/$$so "$$SYSROOT_LIB/$$so"; \
		fi; \
	done
	@touch $@

# Generic .so -> .inc rule for M91 shared libs. The pattern matches
# userspace/build/$(ARCH)/lib<name>.so -> $(INC_DIR)/initramfs_lib<name>.inc.
# The initramfs_lib<name>.inc target must match one of the $(INITRAMFS_M91_SO_INCS)
# entries so the dependency chain from kernel/fs/initramfs.o pulls it in.
$(INC_DIR)/initramfs_lib%.inc: userspace/build/$(ARCH)/lib%.so $(M91_SHARED_DEPS_STAMP)
	@mkdir -p $(dir $@)
	xxd -i -n vfs_lib$*_so $< > $@

# M55: validate the C++ runtime with litehtml (real HTML/CSS layout engine).
# tools/demos/build-m55-litehtml.sh builds litehtml+gumbo (build-litehtml.sh) and
# links the parse/layout/draw acceptance test against them. M89: litehtml is built
# against the shared LLVM libc++ (B1NIX_CXX_STDLIB=libc++) — NetSurf's only C++
# component, so this also moves the NetSurf C++ stack off GCC libstdc++.
$(INC_DIR)/initramfs_m55_litehtml.inc: userspace/bin/m55_litehtml.cpp tools/demos/build-m55-litehtml.sh $(LITEHTML_LIB) $(LIBM_LIB) $(USERSPACE_DEPS)
	B1NIX_CXX_STDLIB=libc++ B1NIX_ARCH=$(ARCH) tools/demos/build-m55-litehtml.sh userspace/build/$(ARCH)/bin/m55_litehtml
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m55_litehtml_elf userspace/build/$(ARCH)/bin/m55_litehtml > $@

$(INC_DIR)/initramfs_m32_pcre2_smoke.inc: userspace/bin/m32_pcre2_smoke.c $(USERSPACE_DEPS) $(PCRE2_LIB)
	@mkdir -p $(dir $@)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m32_pcre2_smoke
	xxd -i -n vfs_m32_pcre2_smoke_elf userspace/build/$(ARCH)/bin/m32_pcre2_smoke > $@

# M53: zlib (image-codec dependency for the NetSurf browser platform).
ZLIB_LIB := build/$(ARCH)/ports/zlib/install/lib/libz.a
$(ZLIB_LIB): tools/ports/build-zlib.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-zlib.sh >/dev/null

$(INC_DIR)/initramfs_m53_zlib_smoke.inc: userspace/bin/m53_zlib_smoke.c $(USERSPACE_DEPS) $(ZLIB_LIB)
	@mkdir -p $(dir $@)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_zlib_smoke
	xxd -i -n vfs_m53_zlib_smoke_elf userspace/build/$(ARCH)/bin/m53_zlib_smoke > $@

# M53: libpng (over zlib + libm) — NetSurf image-codec dependency.
LIBPNG_LIB := build/$(ARCH)/ports/libpng/install/lib/libpng16.a
$(LIBPNG_LIB): tools/ports/build-libpng.sh $(ZLIB_LIB)
	B1NIX_ARCH=$(ARCH) tools/ports/build-libpng.sh >/dev/null

$(INC_DIR)/initramfs_m53_libpng_smoke.inc: userspace/bin/m53_libpng_smoke.c $(USERSPACE_DEPS) $(LIBPNG_LIB) $(ZLIB_LIB) $(LIBM_LIB)
	@mkdir -p $(dir $@)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_libpng_smoke
	xxd -i -n vfs_m53_libpng_smoke_elf userspace/build/$(ARCH)/bin/m53_libpng_smoke > $@

# M53: libjpeg (IJG) — NetSurf image-codec dependency.
LIBJPEG_LIB := build/$(ARCH)/ports/libjpeg/install/lib/libjpeg.a
$(LIBJPEG_LIB): tools/ports/build-libjpeg.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libjpeg.sh >/dev/null

$(INC_DIR)/initramfs_m53_libjpeg_smoke.inc: userspace/bin/m53_libjpeg_smoke.c $(USERSPACE_DEPS) $(LIBJPEG_LIB)
	@mkdir -p $(dir $@)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_libjpeg_smoke
	xxd -i -n vfs_m53_libjpeg_smoke_elf userspace/build/$(ARCH)/bin/m53_libjpeg_smoke > $@

# M53: libwebp (image + VP8 video-keyframe codec) — NetSurf codec dependency.
LIBWEBP_LIB := build/$(ARCH)/ports/libwebp/install/lib/libwebp.a
$(LIBWEBP_LIB): tools/ports/build-libwebp.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libwebp.sh >/dev/null

$(INC_DIR)/initramfs_m53_libwebp_smoke.inc: userspace/bin/m53_libwebp_smoke.c $(USERSPACE_DEPS) $(LIBWEBP_LIB) $(LIBM_LIB)
	@mkdir -p $(dir $@)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_libwebp_smoke
	xxd -i -n vfs_m53_libwebp_smoke_elf userspace/build/$(ARCH)/bin/m53_libwebp_smoke > $@

# M53: libvpx (VP8 full-motion video decode) — NetSurf/WebM video codec.
LIBVPX_LIB := build/$(ARCH)/ports/libvpx/install/lib/libvpx.a
$(LIBVPX_LIB): tools/ports/build-libvpx.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libvpx.sh >/dev/null

$(INC_DIR)/initramfs_m53_libvpx_smoke.inc: userspace/bin/m53_libvpx_smoke.c $(USERSPACE_DEPS) $(LIBVPX_LIB) $(LIBWEBP_LIB) $(LIBM_LIB)
	@mkdir -p $(dir $@)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_libvpx_smoke
	xxd -i -n vfs_m53_libvpx_smoke_elf userspace/build/$(ARCH)/bin/m53_libvpx_smoke > $@

# M53: libwapcaplet (string internment) — first NetSurf browser-library dep.
LWC_LIB := build/$(ARCH)/ports/libwapcaplet/install/lib/liblwc.a
$(LWC_LIB): tools/ports/build-libwapcaplet.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libwapcaplet.sh >/dev/null

$(INC_DIR)/initramfs_m53_wapcaplet_smoke.inc: userspace/bin/m53_wapcaplet_smoke.c $(USERSPACE_DEPS) $(LWC_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_wapcaplet_smoke
	xxd -i -n vfs_m53_wapcaplet_smoke_elf userspace/build/$(ARCH)/bin/m53_wapcaplet_smoke > $@

# M53: libparserutils (input streams + bundled charset codecs) — NetSurf dep.
PU_LIB := build/$(ARCH)/ports/libparserutils/install/lib/libparserutils.a
$(PU_LIB): tools/ports/build-libparserutils.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libparserutils.sh >/dev/null

$(INC_DIR)/initramfs_m53_parserutils_smoke.inc: userspace/bin/m53_parserutils_smoke.c $(USERSPACE_DEPS) $(PU_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_parserutils_smoke
	xxd -i -n vfs_m53_parserutils_smoke_elf userspace/build/$(ARCH)/bin/m53_parserutils_smoke > $@

# M53: libhubbub (HTML5 tokeniser + tree builder) over libparserutils — NetSurf.
HUBBUB_LIB := build/$(ARCH)/ports/libhubbub/install/lib/libhubbub.a
$(HUBBUB_LIB): tools/ports/build-libhubbub.sh $(PU_LIB)
	B1NIX_ARCH=$(ARCH) tools/ports/build-libhubbub.sh >/dev/null

$(INC_DIR)/initramfs_m53_hubbub_smoke.inc: userspace/bin/m53_hubbub_smoke.c $(USERSPACE_DEPS) $(HUBBUB_LIB) $(PU_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_hubbub_smoke
	xxd -i -n vfs_m53_hubbub_smoke_elf userspace/build/$(ARCH)/bin/m53_hubbub_smoke > $@

# M53: libcss (CSS parser + selection) over libwapcaplet + libparserutils.
LIBCSS_LIB := build/$(ARCH)/ports/libcss/install/lib/libcss.a
$(LIBCSS_LIB): tools/ports/build-libcss.sh $(LWC_LIB) $(PU_LIB)
	B1NIX_ARCH=$(ARCH) tools/ports/build-libcss.sh >/dev/null

$(INC_DIR)/initramfs_m53_libcss_smoke.inc: userspace/bin/m53_libcss_smoke.c $(USERSPACE_DEPS) $(LIBCSS_LIB) $(LWC_LIB) $(PU_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_libcss_smoke
	xxd -i -n vfs_m53_libcss_smoke_elf userspace/build/$(ARCH)/bin/m53_libcss_smoke > $@

# M53: libdom (DOM) + hubbub binding over libhubbub + libparserutils + lwc.
LIBDOM_LIB := build/$(ARCH)/ports/libdom/install/lib/libdom.a
$(LIBDOM_LIB): tools/ports/build-libdom.sh $(HUBBUB_LIB)
	B1NIX_ARCH=$(ARCH) tools/ports/build-libdom.sh >/dev/null

$(INC_DIR)/initramfs_m53_libdom_smoke.inc: userspace/bin/m53_libdom_smoke.c $(USERSPACE_DEPS) $(LIBDOM_LIB) $(HUBBUB_LIB) $(PU_LIB) $(LWC_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_libdom_smoke
	xxd -i -n vfs_m53_libdom_smoke_elf userspace/build/$(ARCH)/bin/m53_libdom_smoke > $@

# M53: NetSurf helper/decoder libs — libnsutils, libnsgif, libnsbmp, libnslog.
NSUTILS_LIB := build/$(ARCH)/ports/libnsutils/install/lib/libnsutils.a
NSGIF_LIB := build/$(ARCH)/ports/libnsgif/install/lib/libnsgif.a
NSBMP_LIB := build/$(ARCH)/ports/libnsbmp/install/lib/libnsbmp.a
NSLOG_LIB := build/$(ARCH)/ports/libnslog/install/lib/libnslog.a
$(NSUTILS_LIB): tools/ports/build-libnsutils.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libnsutils.sh >/dev/null
$(NSGIF_LIB): tools/ports/build-libnsgif.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libnsgif.sh >/dev/null
$(NSBMP_LIB): tools/ports/build-libnsbmp.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libnsbmp.sh >/dev/null
$(NSLOG_LIB): tools/ports/build-libnslog.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libnslog.sh >/dev/null

$(INC_DIR)/initramfs_m53_nslibs_smoke.inc: userspace/bin/m53_nslibs_smoke.c $(USERSPACE_DEPS) $(NSUTILS_LIB) $(NSGIF_LIB) $(NSBMP_LIB) $(NSLOG_LIB)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_nslibs_smoke
	xxd -i -n vfs_m53_nslibs_smoke_elf userspace/build/$(ARCH)/bin/m53_nslibs_smoke > $@

# M53: userspace VirGL smoke — drives /dev/virtio-gpu (host-GPU-accelerated 3D).
$(INC_DIR)/initramfs_m53_virgl_smoke.inc: userspace/bin/m53_virgl_smoke.c $(USERSPACE_DEPS)
	@mkdir -p $(dir $@)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_virgl_smoke
	xxd -i -n vfs_m53_virgl_smoke_elf userspace/build/$(ARCH)/bin/m53_virgl_smoke > $@

$(CURL_ELF): tools/ports/build-curl.sh tools/toolchain/bin/b1nix-autotools-cc $(USERSPACE_DEPS) $(MBEDTLS_LIB)
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

# zsh — the default interactive shell and login shell (M98). It replaced GNU
# bash, whose GPLv3 licence was the last one in the shipped userland. Needs the
# netbsd-curses port for terminal handling, which build-zsh.sh resolves itself.
$(ZSH_ELF): tools/ports/build-zsh.sh tools/ports/build-netbsd-curses.sh tools/toolchain/bin/b1nix-musl-autotools-cc $(USERSPACE_DEPS)
	B1NIX_ARCH=$(ARCH) tools/ports/build-zsh.sh >/dev/null

# In-guest build tools, both GNU-free (M98): bmake (BSD 3-clause NetBSD make)
# ships as /bin/make and samurai (0BSD Ninja reimplementation) as /bin/samu with
# a /bin/ninja alias. Together they replace the retired GNU Make port.
$(BMAKE_ELF): tools/ports/build-bmake.sh tools/toolchain/bin/b1nix-musl-autotools-cc $(USERSPACE_DEPS)
	B1NIX_ARCH=$(ARCH) tools/ports/build-bmake.sh >/dev/null

$(SAMU_ELF): tools/ports/build-samurai.sh tools/toolchain/bin/b1nix-musl-autotools-cc $(USERSPACE_DEPS)
	B1NIX_ARCH=$(ARCH) tools/ports/build-samurai.sh >/dev/null

OPENSSL_LIB := build/$(ARCH)/ports/openssl/install/lib/libssl.a
$(OPENSSL_LIB): tools/ports/build-openssl.sh tools/toolchain/bin/b1nix-autotools-cc
	tools/ports/build-openssl.sh >/dev/null

LIBIDN2_LIB := build/$(ARCH)/ports/libidn2/install/lib/libidn2.a
$(LIBIDN2_LIB): tools/ports/build-libidn2.sh $(LIBUNISTRING_LIB) tools/toolchain/bin/b1nix-autotools-cc
	tools/ports/build-libidn2.sh >/dev/null

LIBPSL_LIB := build/$(ARCH)/ports/libpsl/install/lib/libpsl.a
$(LIBPSL_LIB): tools/ports/build-libpsl.sh tools/toolchain/bin/b1nix-autotools-cc
	tools/ports/build-libpsl.sh >/dev/null

$(LIBFFI_LIB): tools/ports/build-libffi.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libffi.sh >/dev/null

$(LITEHTML_LIB): tools/ports/build-litehtml.sh
	B1NIX_CXX_STDLIB=libc++ B1NIX_ARCH=$(ARCH) tools/ports/build-litehtml.sh >/dev/null

$(MBEDTLS_LIB): tools/ports/build-mbedtls.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-mbedtls.sh >/dev/null

$(LIBUNISTRING_LIB): tools/ports/build-libunistring.sh
	B1NIX_ARCH=$(ARCH) tools/ports/build-libunistring.sh >/dev/null


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
$(INC_DIR)/initramfs_m53_httpsd.inc: userspace/bin/m53_httpsd.c $(USERSPACE_DEPS) $(NSFB_ELF)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m53_httpsd
	xxd -i -n vfs_m53_httpsd_elf userspace/build/$(ARCH)/bin/m53_httpsd > $@

# M53: build the NetSurf framebuffer browser (the real nsfb binary) and package
# it + its runtime resources + a test page into the initramfs.
$(NSFB_ELF): tools/ports/build-netsurf-fb.sh tools/ports/build-libnsfb.sh $(ZLIB_LIB) $(LIBPNG_LIB) $(LIBJPEG_LIB) $(FREETYPE_LIB) $(CURL_ELF)
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

$(INC_DIR)/initramfs_m30_pie.inc: userspace/bin/m30_pie.c $(USERSPACE_DEPS)
	@mkdir -p $(dir $@)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m30_pie
	xxd -i -n vfs_m30_pie_elf userspace/build/$(ARCH)/bin/m30_pie > $@

$(INC_DIR)/initramfs_m30_dynamic.inc: userspace/bin/m30_dynamic.c $(USERSPACE_DEPS)
	@mkdir -p $(dir $@)
	@$(MAKE) -C userspace build/$(ARCH)/bin/m30_dynamic
	xxd -i -n vfs_m30_dynamic_elf userspace/build/$(ARCH)/bin/m30_dynamic > $@

ifeq ($(ARCH),x86_64)
ifdef LIBC_SO
$(LIBC_SO):
	@B1NIX_ARCH=$(ARCH) tools/ports/build-musl.sh

$(INITRAMFS_LD_MUSL_INC): $(LIBC_SO)
	@mkdir -p $(dir $@)
	xxd -i -n $(LIBC_INC_SYM) $(LIBC_SO) > $@
endif

$(INC_DIR)/initramfs_m92_musl_dyn_smoke.inc: userspace/bin/m92_musl_dyn_test.c $(USERSPACE_DEPS) $(MUSL_INSTALLED)
	@mkdir -p $(dir $@)
	tools/b1nix-musl-cc -dynamic $< -o $(BUILD_DIR)/m92-musl-dyn-smoke
	xxd -i -n vfs_m92_musl_dyn_smoke_elf $(BUILD_DIR)/m92-musl-dyn-smoke > $@

$(INC_DIR)/initramfs_m92_musl_ldso_smoke.inc: userspace/bin/m92_musl_ldso_test.c $(USERSPACE_DEPS) $(MUSL_INSTALLED)
	@mkdir -p $(dir $@)
	tools/b1nix-musl-cc -ldso $< -o $(BUILD_DIR)/m92-musl-ldso-smoke
	xxd -i -n vfs_m92_musl_ldso_smoke_elf $(BUILD_DIR)/m92-musl-ldso-smoke > $@

$(INC_DIR)/initramfs_musl_posix_smoke.inc: userspace/bin/musl_posix_smoke.c $(USERSPACE_DEPS) $(MUSL_INSTALLED)
	@mkdir -p $(dir $@)
	tools/b1nix-musl-cc -dynamic $< -o $(BUILD_DIR)/musl-posix-smoke
	xxd -i -n vfs_musl_posix_smoke_elf $(BUILD_DIR)/musl-posix-smoke > $@

$(INC_DIR)/initramfs_m92_musl_hello.inc: userspace/bin/m92_musl_hello.c $(USERSPACE_DEPS) $(MUSL_INSTALLED)
	@mkdir -p $(dir $@)
	tools/b1nix-musl-cc -dynamic $< -o $(BUILD_DIR)/m92-musl-hello
	xxd -i -n vfs_m92_musl_hello_elf $(BUILD_DIR)/m92-musl-hello > $@

$(INC_DIR)/initramfs_m92_musl_step2.inc: userspace/bin/m92_musl_step2.c $(USERSPACE_DEPS) $(MUSL_INSTALLED)
	@mkdir -p $(dir $@)
	tools/b1nix-musl-cc -dynamic $< -o $(BUILD_DIR)/m92-musl-step2
	xxd -i -n vfs_m92_musl_step2_elf $(BUILD_DIR)/m92-musl-step2 > $@

$(INC_DIR)/initramfs_m92_musl_raw_diag.inc: userspace/bin/m92_musl_raw_diag.c $(USERSPACE_DEPS) $(MUSL_INSTALLED)
	@mkdir -p $(dir $@)
	tools/b1nix-musl-cc -dynamic $< -o $(BUILD_DIR)/m92-musl-raw-diag
	xxd -i -n vfs_m92_musl_raw_diag_elf $(BUILD_DIR)/m92-musl-raw-diag > $@

# /lib/libc++.so.1 + /lib/libc++abi.so.1 — shared LLVM C++ stdlib (M89). One
# build-libcxx-shared.sh run links BOTH .so from the PIC libc++.a/libc++abi.a; the
# abi .inc rule depends on the libc++ .inc so the script runs exactly once.
ifndef MUSL_INSTALLED
$(INITRAMFS_LIBCXX_INC): tools/toolchain/build-libcxx-shared.sh $(dir $(CROSS_TOOLCHAIN_ROOT))llvm-runtimes-build/libcxx-install/lib/libc++.a $(INITRAMFS_SHARED_LIBC_INC)
	@mkdir -p $(dir $@)
	ARCH=$(ARCH) tools/toolchain/build-libcxx-shared.sh >/dev/null
	xxd -i -n vfs_libcxx_elf $(CROSS_TOOLCHAIN_ROOT)/$(B1NIX_TRIPLET)/lib/libc++.so.1 > $@

$(INITRAMFS_LIBCXXABI_INC): $(INITRAMFS_LIBCXX_INC)
	@mkdir -p $(dir $@)
	xxd -i -n vfs_libcxxabi_so1 $(CROSS_TOOLCHAIN_ROOT)/$(B1NIX_TRIPLET)/lib/libc++abi.so.1 > $@
endif # !MUSL_INSTALLED
ifdef MUSL_INSTALLED
# Under musl the C++ shared runtime is the version built against musl libc.
# The .so files live in $(LIBC_ROOT)/lib/ (same tree as musl libc.so) and are
# already linked correctly: libc++.so.1 NEEDS libc++abi.so.1 + libc.so, and
# libc++abi.so.1 NEEDS libc.so — both resolve to ld-musl-x86_64.so.1 via the
# /lib/libc.so symlink that initramfs.c registers in B1NIX_MUSL mode.
MUSL_LIBCXX_COMPILER_RT := build/$(ARCH)/toolchain/llvm-runtimes-build/install/lib/libcompiler_rt.a

$(MUSL_LIBCXX_COMPILER_RT): $(LIBC_SO)
	@mkdir -p $(dir $@)
	tools/toolchain/build-llvm-runtimes.sh

$(MUSL_LIBCXX_STAMP): tools/ports/build-libcxx-musl.sh $(LIBC_ROOT)/lib/libc.so $(MUSL_LIBCXX_COMPILER_RT)
	B1NIX_ARCH=$(ARCH) tools/ports/build-libcxx-musl.sh
	@mkdir -p $(dir $@)
	@touch $@

$(LIBC_ROOT)/lib/libc++.so.1 $(LIBC_ROOT)/lib/libc++abi.so.1: $(MUSL_LIBCXX_STAMP)

$(INITRAMFS_LIBCXX_INC): $(LIBC_ROOT)/lib/libc++.so.1
	@mkdir -p $(dir $@)
	xxd -i -n vfs_libcxx_elf $< > $@

$(INITRAMFS_LIBCXXABI_INC): $(LIBC_ROOT)/lib/libc++abi.so.1
	@mkdir -p $(dir $@)
	xxd -i -n vfs_libcxxabi_so1 $< > $@
endif # MUSL_INSTALLED
endif

$(INITRAMFS_BUSYBOX_INC): tools/ports/build-busybox.sh tools/configs/busybox-1.38.0.config $(USERSPACE_DEPS)
	B1NIX_ARCH=$(ARCH) tools/ports/build-busybox.sh
	@mkdir -p $(dir $@)
	xxd -i -n vfs_upstream_busybox_elf build/$(ARCH)/ports/busybox/busybox > $@




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

iso: check-b1cc-sync root-image check-dynamic $(KERNEL_ELF)
	@$(MKISO) --stage $(BUILD_DIR)/iso --out $(BUILD_DIR)/b1nix.iso \
	    --arch $(ARCH) --kernel $(KERNEL_ELF) --timeout $(BOOT_TIMEOUT) \
	    --cmdline "$(KERNEL_CMDLINE)" --module $(BUILD_DIR)/root.ext4:rootfs.img
	@echo "============================================================"
	@echo " b1nix build summary ($(ARCH))"
	@echo " ISO: $(BUILD_DIR)/b1nix.iso"
	@if command -v ccache >/dev/null 2>&1 && [ "${B1NIX_NO_CCACHE:-0}" != "1" ]; then \
		echo " ccache statistics:"; \
		ccache -s 2>/dev/null | sed 's/^/   /'; \
	fi
	@echo "============================================================"

# Smoke-suite ISOs: single pattern rule for all categories.
# Each suite gets its own cmdline; kernel ELF + root.ext4 are shared.
SMOKE_CMDLINE_sys=init=/sbin/openrc-init b1nix.test=1 b1nix.kvtest=abc123 b1nix.ssh-loopback=1 b1nix.aslr b1nix.smoke=sys
SMOKE_CMDLINE_gfx=init=/sbin/openrc-init b1nix.test=1 b1nix.kvtest=abc123 b1nix.ssh-loopback=1 b1nix.aslr b1nix.smoke=gfx
SMOKE_CMDLINE_posix=init=/sbin/openrc-init b1nix.test=1 b1nix.kvtest=abc123 b1nix.ssh-loopback=1 b1nix.aslr b1nix.smoke=posix
SMOKE_CMDLINE_blk=init=/sbin/openrc-init b1nix.test=1 b1nix.kvtest=abc123 b1nix.ssh-loopback=1 b1nix.aslr b1nix.smoke=blk
# OpenRC ctltest: boots the real init system as PID 1 and drives sysinit/boot/default,
# then a local.d hook asks PID 1 to power off through /run/openrc/init.ctl — the
# control-FIFO path openrc-shutdown and telinit use. A clean poweroff proves the channel works.
SMOKE_CMDLINE_openrc=init=/sbin/openrc-init b1nix.test=1 b1nix.openrc-ctltest

iso-sys iso-gfx iso-posix iso-blk iso-openrc: root-image check-dynamic $(KERNEL_ELF)
	@$(MKISO) --stage $(BUILD_DIR)/$@ --out $(BUILD_DIR)/b1nix-$(@:iso-%=%).iso \
	    --arch $(ARCH) --kernel $(KERNEL_ELF) --timeout $(BOOT_TIMEOUT) \
	    --cmdline "$(SMOKE_CMDLINE_$(@:iso-%=%))" \
	    --module $(BUILD_DIR)/root.ext4:rootfs.img

# V8 run instance: the kernel hook (gated by b1nix.v8run) mounts sata0 -> /mnt/v8
# and runs d8 on m58.js. It boots in test mode (b1nix.test=1) on purpose: the hook
# loads d8 off the disk early — before the rc's M14 test touches sata0 — and the
# active rc keeps the scheduler busy so d8's thread runs. (Without test mode, init
# drops to an interactive getty/shell that starves d8 on a single CPU.) Reuses the
# shared kernel.elf — no recompile, just a different boot cmdline. The d8 binary +
# m58.js ride on build/v8-out/v8-ext4.img, attached as sata0 by tests/smoke.sh.
iso-v8: $(KERNEL_ELF) root-image
	@$(MKISO) --stage $(BUILD_DIR)/iso-v8 --out $(BUILD_DIR)/b1nix-v8.iso \
	    --arch $(ARCH) --kernel $(KERNEL_ELF) --timeout $(BOOT_TIMEOUT) \
	    --cmdline "init=/sbin/openrc-init b1nix.test=1 b1nix.v8run b1nix.smoke=v8" \
	    --module $(BUILD_DIR)/root.ext4:rootfs.img \
	    --module $(BUILD_ROOT)/v8-out/v8-ext4.img:v8.img
# NOTE: the smoke v8 instance runs JITLESS (no b1nix.v8jit) — that is the proven
# config and matches the jitless d8 on v8-ext4.img. The JIT d8 is exercised
# manually (build the default ISO with b1nix.v8jit + the v8-jit-ext4.img disk).

iso-live: root-image check-dynamic $(KERNEL_ELF)
	@$(MKISO) --stage $(BUILD_DIR)/iso-live --out $(BUILD_DIR)/b1nix-live.iso \
	    --arch $(ARCH) --kernel $(KERNEL_ELF) --timeout $(BOOT_TIMEOUT) \
	    --cmdline "$(KERNEL_CMDLINE)" --module $(BUILD_DIR)/root.ext4:rootfs.img

# Installer ISO: a live ISO that ALSO carries b1nix-disk.img so that, after
# booting it, `b1nix_install /dev/<disk>` installs b1nix to that disk (the
# installer auto-finds /mnt/iso/boot/b1nix-disk.img). Bigger ISO since it ships
# both the live rootfs.img and the disk image. b1nix-disk.img just rides along
# in the ISO root — it is a payload file, not a Multiboot2 module.
disk-iso: disk-image iso-live
	cp $(BUILD_DIR)/b1nix-disk.img $(BUILD_DIR)/iso-live/boot/b1nix-disk.img
	@$(MKISO) --stage $(BUILD_DIR)/iso-live --out $(BUILD_DIR)/b1nix-installer.iso \
	    --arch $(ARCH) --kernel $(KERNEL_ELF) --timeout $(BOOT_TIMEOUT) \
	    --cmdline "$(KERNEL_CMDLINE)" --module $(BUILD_DIR)/root.ext4:rootfs.img
	@printf 'boot it, then run:  b1nix_install /dev/<target-disk>\n'

iso-test: root-image check-dynamic $(KERNEL_ELF)
	@$(MKISO) --stage $(BUILD_DIR)/iso-test --out $(BUILD_DIR)/b1nix-test.iso \
	    --arch $(ARCH) --kernel $(KERNEL_ELF) --timeout $(BOOT_TIMEOUT) \
	    --cmdline "$(KERNEL_CMDLINE) init=/sbin/openrc-init b1nix.test=1" \
	    --module $(BUILD_DIR)/root.ext4:rootfs.img

userspace: $(USERSPACE_DEPS)

.PHONY: check-b1cc-sync
check-b1cc-sync:
	@tools/check-b1cc-sync.sh

userspace-install: userspace
	@$(MAKE) -C userspace B1NIX_ARCH=$(ARCH) install

# Cross-toolchain (LLVM/Clang + musl sysroot). Required by busybox and all
# native-compiled ports. Preserved across `make clean` (lives under build/<arch>/toolchain/).
toolchain:
	@if [ ! -d "$(CROSS_TOOLCHAIN_ROOT)/bin" ]; then \
		echo "[TOOLCHAIN] Building cross-toolchain for $(ARCH)..."; \
		B1NIX_ARCH=$(ARCH) sh tools/toolchain/build-toolchain.sh; \
	else \
		echo "[TOOLCHAIN] Cross-toolchain already present at $(CROSS_TOOLCHAIN_ROOT)"; \
	fi

busybox-package: toolchain
	B1NIX_ARCH=$(ARCH) tools/ports/build-busybox.sh

# Native toolchain for b1nix self-host: b1nix-native Clang/LLVM only (GCC retired).
# Prefer the DYNAMIC native clang/lld (b1nix-dyn/usr: 44 MB clang + 5.5 MB lld +
# demand-paged libLLVM-22.so) over the static 94 MB clang when it has been built.
NATIVE_CLANG_ROOT := $(shell \
	for p in build/native-clang/b1nix-libcxx/usr build/native-clang/b1nix-dyn/usr build/native-clang/b1nix/usr; do \
		if [ -d "$$p/bin" ]; then echo "$$p"; break; fi; \
	done)
install-native-toolchain:
	@if [ -n "$(NATIVE_CLANG_ROOT)" ]; then \
		echo "Installing native Clang toolchain from $(NATIVE_CLANG_ROOT) to rootfs..."; \
		mkdir -p $(BUILD_DIR)/rootfs/usr/bin $(BUILD_DIR)/rootfs/usr/lib $(BUILD_DIR)/rootfs/lib; \
		cp -R $(NATIVE_CLANG_ROOT)/bin/. $(BUILD_DIR)/rootfs/usr/bin/ 2>/dev/null || true; \
		cp -R $(NATIVE_CLANG_ROOT)/lib/. $(BUILD_DIR)/rootfs/usr/lib/ 2>/dev/null || true; \
		if [ -f $(NATIVE_CLANG_ROOT)/lib/libLLVM.so ]; then \
			cp $(NATIVE_CLANG_ROOT)/lib/libLLVM.so $(BUILD_DIR)/rootfs/lib/; \
			echo "  dynamic clang: libLLVM.so -> rootfs/lib/ (loader search path)"; \
		elif [ -f $(NATIVE_CLANG_ROOT)/lib/libLLVM-22.so ]; then \
			cp $(NATIVE_CLANG_ROOT)/lib/libLLVM-22.so $(BUILD_DIR)/rootfs/lib/; \
			echo "  dynamic clang: libLLVM-22.so -> rootfs/lib/ (loader search path)"; \
		fi; \
		echo "Native Clang toolchain installed to rootfs/usr/"; \
	else \
		echo "Note: native Clang toolchain not built."; \
		echo "      Run tools/build-native-clang.sh --b1nix-elf."; \
	fi

install-ports: userspace-install busybox-package install-native-toolchain $(ZSH_ELF) $(CURL_ELF) $(DROPBEAR_ELF) $(NSFB_ELF) $(BMAKE_ELF) $(SAMU_ELF)
	tools/packages/install-ports.sh $(BUILD_DIR)/rootfs $(ARCH) $(PORTS_SOURCE) $(PACKAGE_INDEX_URL)
	@# The published dev package may carry older libc headers than this checkout.
	@# Restore the current userspace ABI after package extraction so cross C++
	@# ports (notably libc++/Mesa) see the same wchar/locale surface as the build.
	@$(MAKE) -C userspace B1NIX_ARCH=$(ARCH) install

# Stage kernel + userspace + build harness source into the rootfs so the
# in-guest toolchain can rebuild b1nix from inside b1nix (M26 self-host).
# Excludes generated artifacts (build/, *.o, *.a, *.elf, .git).
install-kernel-source:
	@echo "Staging b1nix source tree into $(BUILD_DIR)/rootfs/usr/src/b1nix..."
	@rm -rf $(BUILD_DIR)/rootfs/usr/src/b1nix
	@mkdir -p $(BUILD_DIR)/rootfs/usr/src/b1nix
	@for d in kernel userspace tools tests docs; do \
		if [ -d "$$d" ]; then \
			tar -cf - --exclude='build/' --exclude='*.o' --exclude='*.a' \
				--exclude='*.elf' --exclude='*.bin' --exclude='*.iso' --exclude='.git/' \
				"$$d" | tar -xf - -C $(BUILD_DIR)/rootfs/usr/src/b1nix/ ; \
		fi; \
	done
	@cp Makefile $(BUILD_DIR)/rootfs/usr/src/b1nix/
	@if [ -f README.md ]; then cp README.md $(BUILD_DIR)/rootfs/usr/src/b1nix/; fi
	@# The in-guest kernel build (self-host) compiles lapic.c and initramfs.c,
	@# which #include generated artifacts from build/$(ARCH) (ap_trampoline.inc and
	@# the initramfs_*.inc byte arrays). build/ is rsync-excluded above as it is
	@# host output, so stage just these generated *.inc inputs the compile needs.
	@# Exclude large Mesa/Skia/NetSurf .inc files (not needed for self-host).
	@mkdir -p $(BUILD_DIR)/rootfs/usr/src/b1nix/build/$(ARCH)
	@for f in $(BUILD_DIR)/*.inc; do \
		case "$$(basename "$$f")" in \
			initramfs_m52_*|initramfs_m53_*|initramfs_m59_*|initramfs_m91_*|initramfs_lib*) continue ;; \
		esac; \
		cp "$$f" $(BUILD_DIR)/rootfs/usr/src/b1nix/build/$(ARCH)/; \
	done 2>/dev/null || true
	@du -sh $(BUILD_DIR)/rootfs/usr/src/b1nix | sed 's/^/source tree size: /'

# A full ISO must carry the staged rootfs. The old dependency on plain `iso`
# built the toolchain and then omitted it from the resulting image.
iso-full: iso-live

# Standalone-bootable disk image (MBR + Limine + ext4 root), excluding
# V8/Chromium. The in-guest installer (/bin/b1nix_install) copies this onto a
# target disk. Runs entirely unprivileged: `limine bios-install` writes the boot
# stages straight into the image file, so no losetup/mount/root is involved.
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

run-root: iso userspace-install root-image
	@command -v $(QEMU_X86_64) >/dev/null || (echo "missing qemu-system-x86_64"; exit 1)
	$(QEMU_X86_64) -cdrom $(BUILD_DIR)/b1nix.iso -serial stdio -no-reboot -boot d \
		-drive file=$(BUILD_DIR)/root.ext4,format=raw,if=virtio \
		-netdev user,id=n0 -device virtio-net-pci,netdev=n0

root-image: $(KERNEL_ELF) $(USERSPACE_DEPS) install-ports $(M91_SHARED_DEPS_STAMP)
	@mkdir -p $(BUILD_DIR)/rootfs/bin $(BUILD_DIR)/rootfs/etc $(BUILD_DIR)/rootfs/dev $(BUILD_DIR)/rootfs/home $(BUILD_DIR)/rootfs/tmp $(BUILD_DIR)/rootfs/var
	@mkdir -p $(BUILD_DIR)/rootfs/proc $(BUILD_DIR)/rootfs/sys $(BUILD_DIR)/rootfs/mnt
	@mkdir -p $(BUILD_DIR)/rootfs/mnt/ext1 $(BUILD_DIR)/rootfs/mnt/ext2 $(BUILD_DIR)/rootfs/mnt/ext3 $(BUILD_DIR)/rootfs/mnt/ext4 $(BUILD_DIR)/rootfs/mnt/ext4nvme
	@ln -sfn . $(BUILD_DIR)/rootfs/persist
	@echo "b1nix persistent root" > $(BUILD_DIR)/rootfs/etc/motd
	@# Smoke test runner (static file, not generated — edit tools/ports/00-smoke.start)
	@mkdir -p $(BUILD_DIR)/rootfs/etc/local.d
	@cp tools/ports/00-smoke.start $(BUILD_DIR)/rootfs/etc/local.d/00-smoke.start
	@chmod +x $(BUILD_DIR)/rootfs/etc/local.d/00-smoke.start
	@# M51 test font. The FreeType/HarfBuzz/Cairo/Fontconfig smokes open
	@# /share/fonts/B1nixMono-Regular.ttf; it used to arrive via the xxd
	@# initramfs (bootstrap-only since the ext4-root migration), so stage it
	@# into the rootfs the tests actually run against.
	@mkdir -p $(BUILD_DIR)/rootfs/share/fonts
	@cp -f userspace/share/fonts/B1nixMono-Regular.ttf $(BUILD_DIR)/rootfs/share/fonts/
	@# M38: Stage /test.wav for the m38_sound smoke (WAV parse/playback). The
	@# file is generated by tools/images/gen_test_wav.py and must exist in the
	@# rootfs the test actually runs against.
	@$(MAKE) --no-print-directory $(INITRAMFS_TESTWAV_INC)
	@cp -f $(BUILD_DIR)/test.wav $(BUILD_DIR)/rootfs/test.wav
	@# Self-contained TLS test PKI. The loopback HTTPS smokes (M32 curl, M53
	@# NetSurf-over-TLS) verify the server cert against this CA, so the PEMs
	@# have to exist in the rootfs the tests actually run against.
	@$(MAKE) --no-print-directory $(TLS_TEST_DIR)/ca.pem
	@mkdir -p $(BUILD_DIR)/rootfs/etc/tls-test
	@cp -f $(TLS_TEST_DIR)/ca.pem $(TLS_TEST_DIR)/server-cert.pem \
	       $(TLS_TEST_DIR)/server-key.pem $(BUILD_DIR)/rootfs/etc/tls-test/
	@# M91: Stage Mesa/Skia shared libraries into rootfs/lib/ for the dynamic linker
	@mkdir -p $(BUILD_DIR)/rootfs/lib
	@# Mesa install dir (softpipe/virgl build) — copy .so files and create
	@# SONAME copies (not symlinks: b1nix ext4 driver may not follow them).
	@MESA_LIB_DIR=build/$(ARCH)/ports/mesa/install/lib; \
	for so in "$$MESA_LIB_DIR"/libOSMesa.so.8.0.0 "$$MESA_LIB_DIR"/libglapi.so.8.0.0 "$$MESA_LIB_DIR"/libglapi.so.0.0.0; do \
		if [ -f "$$so" ]; then \
			base=$$(basename "$$so"); soname=$$(llvm-readelf -d "$$so" 2>/dev/null | grep SONAME | sed 's/.*\[//;s/\].*//' ); \
			cp -f "$$so" $(BUILD_DIR)/rootfs/lib/; \
			if [ -n "$$soname" ] && [ "$$soname" != "$$base" ]; then \
				cp -f "$$so" "$(BUILD_DIR)/rootfs/lib/$$soname"; \
			fi; \
		fi; \
	done
	@# Copy any other Mesa install .so files (libEGL, libGLESv2, etc.)
	@# An unmatched glob expands to itself, so skip non-files rather than
	@# letting the final [ -f ] decide the loop's (and the recipe's) status.
	@for so in build/$(ARCH)/ports/mesa/install/lib/lib*.so.*; do \
		if [ -f "$$so" ] && ! [ -L "$$so" ]; then cp -f "$$so" $(BUILD_DIR)/rootfs/lib/; fi; \
	done
	@# Also stage from userspace build dir
	@for so in userspace/build/$(ARCH)/lib*.so userspace/build/$(ARCH)/lib*.so.*; do \
		if [ -f "$$so" ] && ! [ -L "$$so" ]; then cp -f "$$so" $(BUILD_DIR)/rootfs/lib/; fi; \
	done
	@# M69: the runtime-dlopen plugin. Under musl it is served from the ext4
	@# rootfs (not the initramfs, which only embeds it in the non-musl build), so
	@# the M30/M69 dlopen smoke can dlopen("/lib/m69_plugin.so") at runtime. Build
	@# it (it lives in bin/ with a non-lib* name, so the generic glob above misses
	@# it) and stage it into rootfs/lib.
	@$(MAKE) -C userspace build/$(ARCH)/bin/m69_plugin.so >/dev/null 2>&1 || true
	@if [ -f userspace/build/$(ARCH)/bin/m69_plugin.so ]; then \
		cp -f userspace/build/$(ARCH)/bin/m69_plugin.so $(BUILD_DIR)/rootfs/lib/m69_plugin.so; \
	fi
	@# Stage sysroot C++ runtime .so (real, not stubs — EGL/GLESv2/fontconfig
	@# come from userspace/build/ via build-skia-shared-deps.sh above)
	@SYSROOT_LIB=$(CXX_RUNTIME_LIB); \
	for so in "$$SYSROOT_LIB"/libc++.so.1 \
	          "$$SYSROOT_LIB"/libc++abi.so.1 "$$SYSROOT_LIB"/libunwind.so.1; do \
		if [ -f "$$so" ]; then cp -f "$$so" $(BUILD_DIR)/rootfs/lib/; fi; \
	done
ifdef LIBC_SO
	@# The libc blob doubles as the dynamic loader: rootfs binaries request it by
	@# PT_INTERP (/lib/$(LIBC_LDSO_NAME)) and by DT_NEEDED (libc.so), and reach
	@# math/threads/timers/dlopen through the same file. Clear any name a previous
	@# libc left behind first — copying onto a dangling or symlinked name writes
	@# through it and leaves the tree describing a layout that no longer exists.
	@rm -f $(BUILD_DIR)/rootfs/lib/libc.so $(BUILD_DIR)/rootfs/lib/libc.so.1 \
	       $(BUILD_DIR)/rootfs/lib/$(LIBC_LDSO_NAME) \
	       $(BUILD_DIR)/rootfs/lib/libm.so $(BUILD_DIR)/rootfs/lib/libm.so.1 \
	       $(BUILD_DIR)/rootfs/lib/libpthread.so $(BUILD_DIR)/rootfs/lib/librt.so \
	       $(BUILD_DIR)/rootfs/lib/libdl.so $(BUILD_DIR)/rootfs/lib/libcrypt.so \
	       $(BUILD_DIR)/rootfs/lib/libutil.so $(BUILD_DIR)/rootfs/lib/libresolv.so
	@cp -f $(LIBC_SO) $(BUILD_DIR)/rootfs/lib/$(LIBC_LDSO_NAME)
	@# The ext4 driver may not follow symlinks, so give every name a real
	@# directory entry. Hard links share one inode: the image carries the bytes
	@# once no matter how many names point at them.
	@for name in libc.so libm.so libm.so.1 libpthread.so librt.so libdl.so \
	             libcrypt.so libutil.so libresolv.so; do \
		ln -f $(BUILD_DIR)/rootfs/lib/$(LIBC_LDSO_NAME) $(BUILD_DIR)/rootfs/lib/$$name 2>/dev/null || \
			cp -f $(LIBC_SO) $(BUILD_DIR)/rootfs/lib/$$name; \
	done
endif
	@# Create SONAME hard copies for any .so.N.M files in rootfs/lib
	@for f in $(BUILD_DIR)/rootfs/lib/lib*.so.*.*.*; do \
		[ -f "$$f" ] || continue; \
		soname=$$(llvm-readelf -d "$$f" 2>/dev/null | grep SONAME | sed 's/.*\[//;s/\].*//'); \
		if [ -n "$$soname" ] && ! [ -e "$(BUILD_DIR)/rootfs/lib/$$soname" ]; then \
			cp -f "$$f" "$(BUILD_DIR)/rootfs/lib/$$soname"; \
		fi; \
	done
	@SKIA_DM="$$(ls build/$(ARCH)/ports/skia/build/out/b1nix/dm build/src/skia/out/b1nix/dm 2>/dev/null | head -1)"; \
	if [ -n "$$SKIA_DM" ] && [ -f "$$SKIA_DM" ]; then \
		llvm-strip --strip-debug "$$SKIA_DM" -o $(BUILD_DIR)/rootfs/bin/skia-dm 2>/dev/null || \
		cp "$$SKIA_DM" $(BUILD_DIR)/rootfs/bin/skia-dm; \
		chmod +x $(BUILD_DIR)/rootfs/bin/skia-dm; \
	fi
	@# M98: zsh is the interactive/login shell in place of GNU bash. Its shell
	@# functions (completion, prompts) are read from $$fpath at runtime, so they
	@# ship alongside the binary.
	@if [ -f $(ZSH_ELF) ]; then \
		cp -f $(ZSH_ELF) $(BUILD_DIR)/rootfs/bin/zsh; \
		chmod +x $(BUILD_DIR)/rootfs/bin/zsh; \
		mkdir -p $(BUILD_DIR)/rootfs/usr/share/zsh; \
		cp -R build/$(ARCH)/ports/zsh/install/share/zsh/. \
			$(BUILD_DIR)/rootfs/usr/share/zsh/ 2>/dev/null || true; \
	fi
	@# M98: the GNU-free in-guest build tools. bmake is /bin/make (nothing on the
	@# target should have to know it is not GNU Make) and samurai is /bin/samu
	@# plus a /bin/ninja alias. Real copies, not symlinks — the ext4 driver does
	@# not follow them. bmake reads its system makefiles from /usr/share/mk.
	@if [ -f $(BMAKE_ELF) ]; then \
		cp -f $(BMAKE_ELF) $(BUILD_DIR)/rootfs/bin/make; \
		cp -f $(BMAKE_ELF) $(BUILD_DIR)/rootfs/bin/bmake; \
		chmod +x $(BUILD_DIR)/rootfs/bin/make $(BUILD_DIR)/rootfs/bin/bmake; \
		mkdir -p $(BUILD_DIR)/rootfs/usr/share/mk; \
		cp -f build/$(ARCH)/ports/bmake/install/share/mk/*.mk \
			$(BUILD_DIR)/rootfs/usr/share/mk/ 2>/dev/null || true; \
	fi
	@if [ -f $(SAMU_ELF) ]; then \
		cp -f $(SAMU_ELF) $(BUILD_DIR)/rootfs/bin/samu; \
		cp -f $(SAMU_ELF) $(BUILD_DIR)/rootfs/bin/ninja; \
		chmod +x $(BUILD_DIR)/rootfs/bin/samu $(BUILD_DIR)/rootfs/bin/ninja; \
	fi
	@# M40/M67: committed static ELF blobs (Linux ABI compat + Rust std smoke).
	@# Only ever wired into the legacy xxd/.inc initramfs path (kernel/fs/
	@# initramfs.c never gained a #include for them after the ext4-root
	@# migration made that path bootstrap-only) — stage them into rootfs/bin
	@# directly, same as skia-dm above, so /bin/init's discovery loop picks
	@# them up.
	@if [ -f tools/m40/linux_hello.bin ]; then \
		cp -f tools/m40/linux_hello.bin $(BUILD_DIR)/rootfs/bin/m40-linux-hello; \
		chmod +x $(BUILD_DIR)/rootfs/bin/m40-linux-hello; \
	fi
	@if [ -f tools/m40/linux_abi_test.bin ]; then \
		cp -f tools/m40/linux_abi_test.bin $(BUILD_DIR)/rootfs/bin/m40-linux-abi; \
		chmod +x $(BUILD_DIR)/rootfs/bin/m40-linux-abi; \
	fi
	@if [ -f tools/m67/hello_b1nix.elf ]; then \
		cp -f tools/m67/hello_b1nix.elf $(BUILD_DIR)/rootfs/bin/m67-rust; \
		chmod +x $(BUILD_DIR)/rootfs/bin/m67-rust; \
	fi
	@# M52/M53/M55/M59/M91 Mesa/Skia/litehtml demo smoke ELFs: built via their own
	@# tools/demos/build-*.sh scripts (against the ported Mesa/Skia/litehtml
	@# static libs), NOT the plain userspace/Makefile BINARIES pattern — so
	@# userspace-install never copies them. Build (if missing) then stage into
	@# rootfs/bin here, same as skia-dm above.
	@UD=userspace/build/$(ARCH)/bin; \
	[ -f "$$UD/m52_osmesa" ] || B1NIX_CXX_STDLIB=libc++ B1NIX_ARCH=$(ARCH) tools/demos/build-m52-mesa-demo.sh m52_osmesa "$$UD/m52_osmesa" || true; \
	[ -f "$$UD/m52_glsl" ] || B1NIX_CXX_STDLIB=libc++ B1NIX_ARCH=$(ARCH) tools/demos/build-m52-mesa-demo.sh m52_glsl "$$UD/m52_glsl" || true; \
	[ -f "$$UD/m53_mesa_virgl" ] || B1NIX_CXX_STDLIB=libc++ B1NIX_ARCH=$(ARCH) tools/demos/build-m53-mesa-virgl.sh "$$UD/m53_mesa_virgl" || true; \
	[ -f "$$UD/m59_smoke" ] || B1NIX_ARCH=$(ARCH) B1NIX_CXX_STDLIB=libc++ tools/demos/build-m59-egl.sh "$$UD/m59_smoke" || true; \
	[ -f "$$UD/m91_skia_smoke" ] || B1NIX_CXX_STDLIB=libc++ B1NIX_ARCH=$(ARCH) tools/demos/build-m91-skia-demo.sh m91_skia_smoke "$$UD/m91_skia_smoke" || true; \
	[ -f "$$UD/m55_litehtml" ] || B1NIX_CXX_STDLIB=libc++ B1NIX_ARCH=$(ARCH) tools/demos/build-m55-litehtml.sh "$$UD/m55_litehtml" || true; \
	for b in m52_osmesa m52_glsl m53_mesa_virgl m59_smoke m91_skia_smoke m55_litehtml; do \
		if [ -f "$$UD/$$b" ]; then \
			cp -f "$$UD/$$b" $(BUILD_DIR)/rootfs/bin/$$b; \
			chmod +x $(BUILD_DIR)/rootfs/bin/$$b; \
		fi; \
	done
	@# Trim rootfs: remove LLVM static archives and shared lib (200+ MB) that
	@# are only needed for self-hosting, not for smoke.  Keep Mesa .so files.
	@rm -f $(BUILD_DIR)/rootfs/lib/libLLVM*.a $(BUILD_DIR)/rootfs/lib/libLLVM.so
	@# Remove .so.N.M files whose SONAME copy already exists (avoid duplicates)
	@for f in $(BUILD_DIR)/rootfs/lib/lib*.so.*.*.*; do \
		[ -f "$$f" ] || continue; \
		soname=$$(llvm-readelf -d "$$f" 2>/dev/null | grep SONAME | sed 's/.*\[//;s/\].*//'); \
		if [ -n "$$soname" ] && [ "$$soname" != "$$(basename $$f)" ] && [ -e "$(BUILD_DIR)/rootfs/lib/$$soname" ]; then \
			rm -f "$$f"; \
		fi; \
	done
	@# Remove .so symlinks (kernel doesn't follow them)
	@find $(BUILD_DIR)/rootfs/lib -type l -name '*.so' -delete 2>/dev/null || true
	@dd if=/dev/zero of=$(BUILD_DIR)/root.ext4 bs=1048576 count=$(ROOT_IMAGE_SIZE) 2>/dev/null
	@$(MKE2FS) -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q -L b1nix-root -d $(BUILD_DIR)/rootfs $(BUILD_DIR)/root.ext4 2>/dev/null || \
	 $(MKE2FS) -t ext4 -q -L b1nix-root -d $(BUILD_DIR)/rootfs $(BUILD_DIR)/root.ext4
	@debugfs -w -R "sif /bin/m31_setuid uid 0" $(BUILD_DIR)/root.ext4 2>/dev/null || true
	@debugfs -w -R "sif /bin/m31_setuid mode 0104755" $(BUILD_DIR)/root.ext4 2>/dev/null || true
	@debugfs -w -R "sif /bin/su uid 0" $(BUILD_DIR)/root.ext4 2>/dev/null || true
	@debugfs -w -R "sif /bin/su mode 0104755" $(BUILD_DIR)/root.ext4 2>/dev/null || true
	@debugfs -w -R "sif /bin/passwd uid 0" $(BUILD_DIR)/root.ext4 2>/dev/null || true
	@debugfs -w -R "sif /bin/passwd mode 0104755" $(BUILD_DIR)/root.ext4 2>/dev/null || true
	@debugfs -w -R "sif /etc/shadow uid 0" $(BUILD_DIR)/root.ext4 2>/dev/null || true
	@debugfs -w -R "sif /etc/shadow mode 0100400" $(BUILD_DIR)/root.ext4 2>/dev/null || true
	@printf 'created %s (%s)\n' "$(BUILD_DIR)/root.ext4" "$$(du -sh $(BUILD_DIR)/root.ext4 | cut -f1)"

# Everything in the rootfs links dynamically against /lib/libc.so. This gate
# fails the build on a statically linked executable that is not listed (with a
# reason) in tools/configs/static-allowlist.txt — so a `-static` slipped into a
# port script breaks the ISO instead of quietly shipping a second copy of libc.
check-dynamic:
	@sh tools/check-dynamic.sh $(BUILD_DIR)/rootfs

check-ports:
	@leaked=0; \
	for pc in $$(find -L build -path '*/install/lib/pkgconfig/*.pc' -o -path '*/netsurf-sysroot/*/lib/pkgconfig/*.pc' 2>/dev/null); do \
		hit=$$(grep -lE '(^|[=: ])(/usr(/local)?|/opt/(homebrew|local)|'"$${HOME}"')(/|$$)' "$$pc" 2>/dev/null || true); \
		if [ -n "$$hit" ]; then \
			echo "check-ports: host path leak in $$pc:"; \
			grep -nE '(^|[=: ])(/usr(/local)?|/opt/(homebrew|local)|'"$${HOME}"')(/|$$)' "$$pc" | sed 's/^/    /'; \
			leaked=1; \
		fi; \
	done; \
	if [ "$$leaked" = 1 ]; then \
		echo "check-ports: FAILED — host paths leaked into generated .pc files"; \
		exit 1; \
	else \
		echo "check-ports: OK — no host path leaks in generated .pc files"; \
	fi

check-tools:
	@command -v $(CC) >/dev/null || (echo "missing $(CC)"; exit 1)
	@command -v $(LD) >/dev/null || (echo "missing $(LD)"; exit 1)
	@test -n "$(LIMINE)" || echo "optional: missing limine for ISO creation"
	@command -v xorriso >/dev/null || echo "optional: missing xorriso for ISO creation"
	@command -v $(QEMU_X86_64) >/dev/null || echo "optional: missing qemu-system-x86_64 for running"

clean:
	@# Preserve build/src/ (source cache) and build/$(ARCH)/toolchain/ (cross/native
	@# compiler trees). Use `make distclean` to wipe everything including sources.
	@if [ -d build ]; then \
		TC_SAVE=$$(mktemp -d); \
		[ -d $(BUILD_DIR)/toolchain ] && cp -a $(BUILD_DIR)/toolchain "$$TC_SAVE/" 2>/dev/null || true; \
		find -L build -mindepth 1 -maxdepth 1 ! -name src -exec rm -rf {} +; \
		[ -d "$$TC_SAVE/toolchain" ] && mv "$$TC_SAVE/toolchain" $(BUILD_DIR)/toolchain 2>/dev/null || true; \
		rm -rf "$$TC_SAVE"; \
	fi
	@$(MAKE) -C userspace clean

distclean: clean
	rm -rf build

distclean-src:
	rm -rf build/src

# ── Smoke Tests ──
smoke:
	@echo "Running parallel full smoke tests for $(ARCH)..."
	sh tests/smoke.sh $(ARCH)

smoke-quick:
	@echo "Running quick smoke tests for $(ARCH)..."
	SMOKE_QUICK=1 sh tests/smoke.sh $(ARCH)

# Fast, focused smoke for b1cc / native-compiler iteration: minimal initramfs
# (~20s build, 3 MB kernel), a single QEMU, only the b1cc/native markers. Use
# this on every compiler tweak instead of the heavy full suite.
smoke-b1cc:
	sh tests/smoke-b1cc.sh $(ARCH)

# Host-only b1cc test suite: builds b1cc natively and runs test.sh directly on
# the developer machine without starting QEMU or building kernel ISO.
test-b1cc:
	@echo "Running b1cc host tests..."
	$(MAKE) -C userspace/b1cc test

# M64 native-Clang self-host proof: ship clang-22 in an ext4 Multiboot2 module and run
# it on b1nix (clang --version + clang -c hello.c). Needs build/native-clang/b1nix
# (tools/build-native-clang.sh --b1nix-elf) and a kernel (make ARCH=x86_64 iso).
clang-proof:
	sh tools/clang/clang-proof.sh

# M26 native-Clang KERNEL self-host: b1nix compiles its own kernel's C TUs with
# its native clang and links a complete kernel.elf with native ld.lld, in-guest.
# Needs build/native-clang/b1nix (clang + ld.lld) and a host kernel build (make
# ARCH=x86_64 iso) for the staged .S/kallsyms objects. Wants >=16GB guest RAM.
selfhost-clang:
	sh tools/inguest/selfhost-proof.sh

graphics-smoke:
	sh tests/graphics-smoke.sh $(ARCH)

memory-smoke:
	@sh tests/memory-smoke.sh
