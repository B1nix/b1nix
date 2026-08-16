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
PCRE2_LIB := build/$(ARCH)/pkg/pcre2/lib/libpcre2-8.a
# libm is musl's built-in stub (all math symbols live in libc.so).
LIBM_LIB := build/$(ARCH)/ports/musl/install/lib/libm.a
PIXMAN_LIB := build/$(ARCH)/pkg/pixman/lib/libpixman-1.a
FREETYPE_LIB := build/$(ARCH)/pkg/freetype/lib/libfreetype.a
CAIRO_LIB := build/$(ARCH)/pkg/cairo/lib/libcairo.so
XKB_LIB := build/$(ARCH)/pkg/xkbcommon/lib/libxkbcommon.a
FFI_LIB := build/$(ARCH)/pkg/libffi/lib/libffi.so.8
WAYLAND_CLIENT_LIB := build/$(ARCH)/pkg/wayland/lib/libwayland-client.so.0
WAYLAND_SERVER_LIB := build/$(ARCH)/pkg/wayland/lib/libwayland-server.so.0
HB_LIB := build/$(ARCH)/pkg/harfbuzz/lib/libharfbuzz.a
EXPAT_LIB := build/$(ARCH)/pkg/expat/lib/libexpat.a
FONTCONFIG_LIB := build/$(ARCH)/pkg/fontconfig/lib/libfontconfig.a
ZLIB_LIB := build/$(ARCH)/pkg/zlib/lib/libz.a
LIBPNG_LIB := build/$(ARCH)/pkg/libpng/lib/libpng16.a
LIBJPEG_LIB := build/$(ARCH)/pkg/libjpeg/lib/libjpeg.a
LIBWEBP_LIB := build/$(ARCH)/pkg/libwebp/lib/libwebp.a
LIBVPX_LIB := build/$(ARCH)/pkg/libvpx/lib/libvpx.a
OPENSSL_LIB := build/$(ARCH)/pkg/openssl/lib/libssl.a
IDN2_LIB := build/$(ARCH)/pkg/libidn2/lib/libidn2.a
LIBPSL_LIB := build/$(ARCH)/pkg/libpsl/lib/libpsl.a
MBEDTLS_LIB := build/$(ARCH)/pkg/mbedtls/lib/libmbedtls.a
UNISTRING_LIB := build/$(ARCH)/pkg/libunistring/lib/libunistring.a
PAM_LIB := build/$(ARCH)/pkg/pam/lib/libpam.so

# Ensure all port libraries depend on headers stamp so that they are compiled
# only after libc.so.1 and headers are fully built and installed.
$(PCRE2_LIB) $(PIXMAN_LIB) $(FREETYPE_LIB) $(CAIRO_LIB) $(XKB_LIB) \
$(WAYLAND_CLIENT_LIB) $(HB_LIB) $(EXPAT_LIB) $(FONTCONFIG_LIB) $(ZLIB_LIB) $(LIBPNG_LIB) $(LIBJPEG_LIB) $(LIBWEBP_LIB) $(LIBVPX_LIB) \
$(LWC_LIB) $(IDN2_LIB) \
$(OPENSSL_LIB) $(LIBPSL_LIB) $(FFI_LIB) \
$(MBEDTLS_LIB) $(UNISTRING_LIB) $(PAM_LIB): $(USERSPACE_HDR_DEPS)

KERNEL_ELF := $(BUILD_DIR)/kernel.elf
INITRAMFS_NATIVE_SMOKE_INC := $(INC_DIR)/initramfs_native_smoke.inc
# M95/M96: loadable kernel modules. Each is a relocatable ELF (.ko) built from
# the same kernel sources with -DMODULE and shipped in the initramfs under
# /lib/modules/$(B1NIX_RELEASE), together with the generated modules.dep /
# modules.alias. The release subdirectory is what BusyBox's modprobe/modinfo/
# depmod expect ($(uname -r), i.e. B1NIX_VERSION_STR).
B1NIX_RELEASE := $(shell sed -n 's/.*B1NIX_VERSION_STR "\([^"]*\)".*/\1/p' kernel/include/b1nix/version.h)
MODULE_OUT_DIR := $(BUILD_DIR)/modules
MODULE_NAMES := isofs ntfs btrfs hda ipv6 ndp ntp
MODULE_KOS := $(patsubst %,$(MODULE_OUT_DIR)/%.ko,$(MODULE_NAMES))
INITRAMFS_MODULES_INC := $(INC_DIR)/initramfs_modules.inc
# b1cc (in-tree C compiler + its M5/M32-M34 smoke corpus)
INITRAMFS_B1CC_M34_INC := $(INC_DIR)/initramfs_b1cc_m34.inc
INITRAMFS_CURL_INC := $(INC_DIR)/initramfs_curl.inc
INITRAMFS_CACERT_INC := $(INC_DIR)/initramfs_cacert.inc
INITRAMFS_TLSTEST_INC := $(INC_DIR)/initramfs_tlstest.inc
INITRAMFS_DROPBEAR_INC := $(INC_DIR)/initramfs_dropbear.inc
INITRAMFS_BUSYBOX_INC := $(INC_DIR)/initramfs_busybox.inc
INITRAMFS_TESTWAV_INC := $(INC_DIR)/initramfs_testwav.inc
INITRAMFS_TESTFONT_INC := $(INC_DIR)/initramfs_testfont.inc
# M40: a committed static Linux x86_64 ELF blob (tools/blobs/linux_hello.bin)
# embedded as /bin/m40-linux-hello to validate the Linux ABI compat layer.
INITRAMFS_M40_LINUX_INC := $(INC_DIR)/initramfs_m40_linux.inc
# M67: a prebuilt static Rust (x86_64-unknown-b1nix) ELF blob
# (tools/blobs/hello_b1nix.elf, regen via tools/blobs/build-rust-hello.sh) embedded as
# /bin/m67-rust to validate the Rust std cross-toolchain at runtime. x86_64-only.
INITRAMFS_M67_RUST_INC := $(INC_DIR)/initramfs_m67_rust.inc
# M53: NetSurf framebuffer browser + resources + test page.

# Applet manifest for /bin replacement (M42 items 3 and 4).
APPLET_MANIFEST := tools/configs/applet-manifest.conf
APPLET_SYMLINKS_INC := $(INC_DIR)/initramfs_applet_symlinks.inc
APPLET_REGISTRATION_INC := $(INC_DIR)/initramfs_applet_registration.inc

# The list of userspace programs that used to be embedded in the kernel image as
# xxd byte arrays lived here, together with one .inc rule per program. Under
# musl every one of them ships in the ext4 rootfs instead, and the variable that
# collected those rules was never named by any target — so the rules had not run
# since the migration, and neither had the list. Both are gone; the pattern rule
# below still serves the handful of things the initramfs genuinely carries.
ifeq ($(ARCH),x86_64)
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
LIBC_ROOT := build/$(ARCH)/pkg/musl
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
# smoke binaries (cxx_smoke/m55_iostream/m64_clang) link these via
# the libc++-default b1nix-c++; libc++abi.so.1 folds the libunwind DWARF unwinder.
INITRAMFS_LIBCXX_INC := $(INC_DIR)/initramfs_libcxx.inc
INITRAMFS_LIBCXXABI_INC := $(INC_DIR)/initramfs_libcxxabi.inc
endif
endif

AP_TRAMPOLINE_INC := $(INC_DIR)/ap_trampoline.inc
# Byte offsets of the trampoline's data block, generated from the very object
# the blob is linked from. The BSP patches those fields by offset before SIPI;
# keeping the numbers in C by hand meant that growing the trampoline's code
# silently moved the block and every AP triple-faulted on a garbage CR3.
AP_TRAMPOLINE_OFFSETS := $(INC_DIR)/ap_trampoline_offsets.h
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
	$(INITRAMFS_MODULES_INC) \
	$(INITRAMFS_LD_MUSL_INC)
GENERATED_INCS := $(AP_TRAMPOLINE_INC) $(AP_TRAMPOLINE_OFFSETS) $(INITRAMFS_INCS) $(APPLET_SYMLINKS_INC) $(APPLET_REGISTRATION_INC)
DROPBEAR_VERSION := 2022.83
# The programs on the image are Alpine packages, unpacked with their own paths
# into one staging root that the root-image rule copies over the image. These
# name individual binaries in it, for the rules that embed or copy one.
PKGROOT := build/$(ARCH)/pkgroot
CURL_ELF := $(PKGROOT)/usr/bin/curl
DROPBEAR_ELF := $(PKGROOT)/usr/sbin/dropbear
BMAKE_ELF := $(PKGROOT)/usr/bin/bmake
SAMU_ELF := $(PKGROOT)/usr/bin/samu
B1NIX_TLS ?= mbedtls
PORTS_SOURCE ?= download
PACKAGE_INDEX_URL ?= https://cdn.jsdelivr.net/gh/B1nix/b1nix-pkgs@main/pkgs/index

# Kernel build toolchain selector (Clang/LLVM).
TOOLCHAIN ?= clang
MKE2FS := $(shell command -v mke2fs 2>/dev/null || command -v /sbin/mke2fs 2>/dev/null || printf '%s' /opt/homebrew/opt/e2fsprogs/sbin/mke2fs)
# Same resolution as MKE2FS: macOS ships neither, and a bare `debugfs` silently
# resolved to nothing here — every `debugfs ... || true` below (the setuid bits,
# /etc/shadow's mode, and the root-ownership pass) was quietly skipped.
DEBUGFS := $(shell command -v debugfs 2>/dev/null || command -v /sbin/debugfs 2>/dev/null || printf '%s' /opt/homebrew/opt/e2fsprogs/sbin/debugfs)
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
ROOT_IMAGE_SIZE ?= 512

# Locate the native toolchain that tools/build-native-clang.sh --b1nix-elf produced.
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
# Bare `llvm-readelf` is often not on PATH (Homebrew LLVM isn't linked into
# /opt/homebrew/bin by default) — the SONAME-detection steps below silently
# get an empty string and skip creating the SONAME-named .so copy, which
# then makes the dynamic linker report the library "not found" even though
# a same-named-but-unversioned copy sits right next to it. Point at the real
# binary explicitly instead of assuming it resolves.
READELF := $(shell command -v llvm-readelf 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-readelf)
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
ARCH_SOURCES := kernel/arch/x86_64/arch.c kernel/arch/x86_64/console.c kernel/arch/x86_64/fb_console.c kernel/arch/x86_64/interrupts.c kernel/arch/x86_64/io.c kernel/arch/x86_64/paging.c kernel/arch/x86_64/serial.c kernel/arch/x86_64/rtc.c kernel/arch/x86_64/signal.c kernel/arch/x86_64/lapic.c kernel/arch/x86_64/tlb.c kernel/arch/x86_64/coredump.c kernel/arch/x86_64/gdbstub.c kernel/arch/x86_64/memtype.c
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
	kernel/mm/module_alloc.c \
	kernel/module/module.c \
	kernel/module/ksyms.c \
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
	kernel/fs/exfat.c \
	kernel/fs/ext2.c \
	kernel/fs/ext1.c \
	kernel/fs/ext3.c \
	kernel/fs/ext4.c \
	kernel/fs/procfs.c \
	kernel/fs/tmpfs.c \
	kernel/fs/sysfs.c \
	kernel/fs/sysfs_attr.c \
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
	kernel/dev/iommu.c \
	kernel/dev/amdvi.c \
	kernel/dev/virtio.c \
	kernel/dev/virtio_blk.c \
	kernel/dev/virtio_net.c \
	kernel/dev/e1000.c \
	kernel/dev/r8169.c \
	kernel/dev/ahci.c \
	kernel/dev/nvme.c \
	kernel/dev/ps2_kbd.c \
	kernel/dev/vt.c \
	kernel/dev/kmsg.c \
	kernel/dev/rtc_dev.c \
	kernel/dev/watchdog.c \
	kernel/dev/i2c.c \
	kernel/dev/ps2_mouse.c \
	kernel/dev/usb_xhci.c \
	kernel/dev/ac97.c \
	kernel/dev/mixer.c \
	kernel/dev/pty.c \
 	kernel/dev/serial_tty.c \
 	kernel/dev/virtio_gpu.c \
 	kernel/dev/virtio_input.c \
	kernel/dev/drm.c \
	kernel/dev/drm_card1.c \
	kernel/dev/fw_cfg.c \
	kernel/dev/netconsole.c \
	kernel/lkpi/lkpi_core.c \
	kernel/lkpi/idr.c \
	kernel/lkpi/completion.c \
	kernel/lkpi/workqueue.c \
	kernel/lkpi/scatterlist.c \
	kernel/lkpi/firmware.c \
	kernel/lkpi/wait.c \
	kernel/lkpi/ww_mutex.c \
	kernel/lkpi/rbtree.c \
	kernel/lkpi/interval_tree.c \
	kernel/lkpi/xarray.c \
	kernel/lkpi/kthread_worker.c \
	kernel/lkpi/rcu.c \
	kernel/lkpi/page.c \
	kernel/lkpi/device.c \
	kernel/lkpi/devres.c \
	kernel/lkpi/ida.c \
	kernel/lkpi/lock.c \
	kernel/lkpi/env.c \
	kernel/lkpi/dma_resv.c \
	kernel/lkpi/linux_compat.c \
	kernel/lkpi/linux_file.c \
	kernel/lkpi/dma_buf.c \
	kernel/lkpi/dma_fence_chain.c \
	kernel/lkpi/timer.c \
	kernel/lkpi/i2c_bit.c \
	kernel/lkpi/linux_misc.c \
	kernel/lkpi/lkpi_test.c \
	kernel/lkpi/lkpi_m101_test.c \
	kernel/drm/dma_fence.c \
	kernel/drm/gpu_scheduler.c \
	kernel/drm/drm_selftest.c \
	kernel/dev/fb.c \
	kernel/dev/input.c \
	kernel/net/net.c \
	kernel/net/socket.c \
	kernel/net/netlink.c \
	kernel/net/unix.c \
	kernel/net/arp.c \
	kernel/net/ethernet.c \
	kernel/net/ipv4.c \
	kernel/net/route.c \
	kernel/net/icmp.c \
	kernel/net/tcp.c \
	kernel/net/udp.c \
	kernel/net/dhcp.c \
	kernel/net/dhcpv6.c \
	kernel/net/dns.c \
	kernel/net/proto.c
endif


# ── M101: imported DRM core ───────────────────────────────────────────────
#
# Upstream source, compiled exactly as written. It is a separate set because it
# needs different flags, and every one of them is load-bearing:
#
#   -nostdinc          the kernel's own CFLAGS do not pass it, so the host's
#                      /usr/include quietly satisfies <linux/types.h> and the
#                      result is a mix of our shim and the host's kernel headers
#                      that happens to link. Only clang's resource include is
#                      allowed back in, for stddef/stdarg.
#   -include ...       upstream compiles DRM with -include linux/compiler_types.h
#                      in KBUILD_CFLAGS, so files that open with <drm/...> and
#                      never reach a linux header still get __must_check. Without
#                      it the error reads "expected ';'" and points at a function
#                      name rather than a missing attribute.
#   -D__linux__        the uapi headers pick the BSD branch otherwise.
#   -w                 imported source is not ours to make warning-clean; a
#                      warning here would be noise we cannot act on without
#                      editing it, which the milestone forbids.
#
# The staged tree is produced by tools/drm/fetch-drm-core.sh and is not tracked.
# Nothing under it is ever edited: a patch to imported source is a bug in the
# shim.
DRM_IMPORT_DIR := build/src/drm-core-6.6

# Which files to build comes from the staged tree's own B1NIX-OBJECTS, which
# fetch-drm-core.sh derived from upstream's drm-y. Choosing them here instead
# would let the list drift from the pinned source — and picking every .c in the
# directory is wrong for a different reason: Kconfig excludes some, and
# drm_of.c collides with its own header's stub when built without CONFIG_OF.
DRM_IMPORT_NAMES := $(shell cat $(DRM_IMPORT_DIR)/B1NIX-OBJECTS 2>/dev/null)
# Everything in the list lives under drivers/gpu/drm, including the display/
# and ttm/ subdirectories, except hdmi.c — which upstream keeps with the video
# helpers and which is named here rather than pattern-matched, so that adding a
# subdirectory to the list does not silently send it to the wrong tree.
DRM_IMPORT_VIDEO_NAMES := hdmi.c
DRM_IMPORT_SOURCES := \
	$(foreach n,$(filter-out $(DRM_IMPORT_VIDEO_NAMES),$(DRM_IMPORT_NAMES)),$(DRM_IMPORT_DIR)/drivers/gpu/drm/$(n)) \
	$(foreach n,$(filter $(DRM_IMPORT_VIDEO_NAMES),$(DRM_IMPORT_NAMES)),$(DRM_IMPORT_DIR)/drivers/video/$(n))
DRM_IMPORT_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(DRM_IMPORT_SOURCES))

CLANG_RESOURCE_INC := $(shell $(CC) -print-resource-dir)/include

# -MMD -MP so a change to the shim rebuilds the imported objects that include
# it. Without them make only ever saw the staged .c files, which never change —
# so a shim fix left every drm object stale, and the kernel ran a mixture of old
# and new headers that no source tree in the repo corresponds to.
DRM_IMPORT_CFLAGS := -std=gnu11 -nostdinc -ffreestanding -fno-builtin \
	-fno-stack-protector -fno-pic -mno-red-zone -w -g -MMD -MP \
	-D__KERNEL__ -D__linux__ -DKBUILD_MODNAME='"drm"' \
	-DCONFIG_X86=1 -DCONFIG_X86_64=1 \
	-isystem $(CLANG_RESOURCE_INC) \
	-I kernel/include -I kernel/include/uapi \
	-I $(DRM_IMPORT_DIR)/include -I $(DRM_IMPORT_DIR)/include/uapi \
	-include linux/compiler_types.h -include linux/types.h

#
# A stamp that changes when the compiler flags change.
#
# make compares timestamps of files; it cannot see that a -D was added. That is
# not a theoretical gap — it bit twice on this driver, and both times the
# symptom pointed somewhere else entirely. Adding -DB1NIX_I915 left a stale
# main.o whose init call had been compiled out, so the driver was linked into
# the image and never started; adding -DCONFIG_X86 left a stale drm_cache.o,
# which silently invalidated a bisect and sent the search down a wrong path.
#
# The stamp's *name* carries a hash of the flags, so a change makes a different
# file, which does not exist, which rebuilds everything that depends on it.
# Recorded per flag set, because the three sets change independently.
#
DRM_FLAGS_HASH := $(firstword $(shell printf '%s' \
	'$(DRM_IMPORT_CFLAGS) $(I915_IMPORT_CFLAGS) $(ARCH_CFLAGS) $(COMMON_CFLAGS)' \
	| cksum))
DRM_FLAGS_STAMP := $(BUILD_DIR)/.import-flags-$(DRM_FLAGS_HASH)

$(DRM_FLAGS_STAMP):
	@mkdir -p $(dir $@)
	@rm -f $(BUILD_DIR)/.import-flags-*
	@touch $@

$(BUILD_DIR)/$(DRM_IMPORT_DIR)/%.o: $(DRM_IMPORT_DIR)/%.c $(DRM_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(DRM_IMPORT_CFLAGS) $(ARCH_CFLAGS) -c $< -o $@

# Our own code that calls into the imported core needs the same include paths
# and the same force-includes, so it is built with the imported flags — minus
# -w, because this file IS ours and has to stay warning-clean.
LKPI_IMPORT_SOURCES := \
	kernel/lkpi/drm_import_test.c \
	kernel/lkpi/seq_file.c \
	kernel/lkpi/sysfs.c \
	kernel/lkpi/drm_b1nix_kms.c \
	kernel/lkpi/drm_chardev.c \
	kernel/lkpi/linux_support.c

LKPI_IMPORT_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(LKPI_IMPORT_SOURCES))

# Named explicitly, not as a pattern over kernel/lkpi/: the rest of that
# directory is b1nix-side code that must keep b1nix's own flags, and a pattern
# rule here would outrank the general one and quietly rebuild all of it against
# the Linux headers.
$(LKPI_IMPORT_OBJECTS): $(BUILD_DIR)/%.o: %.c $(DRM_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -w,$(DRM_IMPORT_CFLAGS)) -Wall -Wextra $(ARCH_CFLAGS) -c $< -o $@

DRM_IMPORT_OBJECTS += $(LKPI_IMPORT_OBJECTS)

# ── M102a: Intel i915 — imported, and optional ────────────────────────────
#
# The DRM core is staged unconditionally because the kernel links it. i915 is
# not: 13 MiB and 262 objects is a real cost for someone working on the
# filesystem or the network stack, and they should not pay it to build a kernel
# they are not putting a GPU in.
#
# So it is gated twice, and both gates are deliberate:
#
#   - the staged tree has to exist. `make i915-fetch` puts it there; without it
#     I915_IMPORT_NAMES is empty and everything below evaluates to nothing.
#   - B1NIX_I915=1 has to be asked for. While the driver does not yet compile
#     against the shim, merely having staged the source must not break the build
#     for someone who staged it to read it.
#
# When the driver builds, the second gate is the one to remove — not the first.
I915_IMPORT_DIR := build/src/i915-6.6
B1NIX_I915 ?= 1

ifeq ($(B1NIX_I915),1)
# main.c calls the driver's module init only when the driver is in the build.
#
# A target-specific variable, not CFLAGS_EXTRA: COMMON_CFLAGS is assigned with
# := hundreds of lines above this block, so it has already expanded
# CFLAGS_EXTRA by the time we get here and appending would be silently lost —
# which is exactly what happened, leaving a kernel with the whole driver linked
# in and nothing calling its init. Target-specific variables expand when the
# rule runs, so ordering cannot bite.
$(BUILD_DIR)/kernel/main.o: COMMON_CFLAGS += -DB1NIX_I915=1
I915_IMPORT_NAMES := $(shell cat $(I915_IMPORT_DIR)/B1NIX-OBJECTS 2>/dev/null)
I915_IMPORT_SOURCES := \
	$(addprefix $(I915_IMPORT_DIR)/drivers/gpu/drm/i915/,$(I915_IMPORT_NAMES))
I915_IMPORT_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(I915_IMPORT_SOURCES))

# Our own code that stands in for an i915 source we do not import. It needs the
# driver's include roots, so it is built with the driver's flags — minus -w,
# because this file is ours and stays warning-clean.
I915_SHIM_SOURCES := kernel/lkpi/i915_acpi.c kernel/lkpi/i915_display_probe.c
I915_SHIM_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(I915_SHIM_SOURCES))
I915_IMPORT_OBJECTS += $(I915_SHIM_OBJECTS)

# The core's flags plus i915's own include roots, and the same -MMD -MP: without
# them a shim fix leaves every imported object stale, which is how M101 ended up
# running a mixture of old and new headers.
# -O2 is not a performance choice, it is a correctness requirement: i915 writes
# BUILD_BUG_ON(!__builtin_constant_p(x)) to assert that a register accessor was
# handed a compile-time constant, and without optimisation the compiler does not
# fold it, so the assertion fires on perfectly correct code. Upstream builds this
# driver optimised and its static assertions are written against that.
#
# kernel/include/i915-shim carries the two tracepoint headers upstream keeps
# under plain GPL-2.0, rewritten as MIT no-ops. It comes AFTER the driver's own
# include roots on purpose: a quoted include resolves against the including
# file's directory first, so if a future rebase ever brings a real i915_trace.h
# back into the staged tree, that one wins and this stops being reachable —
# which is a visible change rather than a silent one.
I915_IMPORT_CFLAGS := $(DRM_IMPORT_CFLAGS) \
	-I $(I915_IMPORT_DIR)/drivers/gpu/drm/i915 \
	-I $(I915_IMPORT_DIR)/drivers/gpu/drm/i915/display \
	-I $(DRM_IMPORT_DIR)/drivers/gpu/drm \
	-I kernel/include/i915-shim \
	-I kernel/include/i915-shim/display \
	-I kernel/include/i915-shim/rel/drivers/gpu/drm/i915 \
	-include i915_kconfig.h \
	-O2

$(BUILD_DIR)/$(I915_IMPORT_DIR)/%.o: $(I915_IMPORT_DIR)/%.c $(DRM_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(I915_IMPORT_CFLAGS) $(ARCH_CFLAGS) -c $< -o $@

# Named explicitly rather than by pattern: the rest of kernel/lkpi is b1nix-side
# code that must keep b1nix's flags, and a pattern rule here would outrank the
# general one and rebuild all of it against the driver's headers.
$(I915_SHIM_OBJECTS): $(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -w,$(I915_IMPORT_CFLAGS)) -Wall -Wextra $(ARCH_CFLAGS) -c $< -o $@
else
I915_IMPORT_OBJECTS :=
endif

.PHONY: kernel-dist i915-fetch
i915-fetch:
	@sh tools/drm/fetch-i915.sh

OBJECTS := \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(KERNEL_SOURCES)) \
	$(patsubst %.S,$(BUILD_DIR)/%.o,$(ASM_SOURCES)) \
	$(DRM_IMPORT_OBJECTS) \
	$(I915_IMPORT_OBJECTS)
KERNEL_DEPS := $(patsubst %.c,$(BUILD_DIR)/%.d,$(KERNEL_SOURCES)) \
	$(patsubst %.S,$(BUILD_DIR)/%.d,$(ASM_SOURCES)) $(MODULE_KOS:.ko=.d) \
	$(DRM_IMPORT_OBJECTS:.o=.d) $(I915_IMPORT_OBJECTS:.o=.d)

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

.PHONY: all analyze objects FORCE iso iso-sys iso-gfx iso-posix iso-blk iso-openrc iso-init iso-live iso-test iso-full check-dynamic \
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

# B1NIX_I915 is a compiler flag, and make does not watch compiler flags: main.c
# reads it to decide whether to call the driver's module init, so toggling the
# variable left a stale main.o that silently did not call it — the driver was
# linked in, complete, and never started. The stamp file records the value and
# is rewritten only when it changes, so main.o rebuilds exactly when it must.
B1NIX_I915_STAMP := $(BUILD_DIR)/.b1nix-i915-$(B1NIX_I915)
$(B1NIX_I915_STAMP):
	@mkdir -p $(dir $@)
	@rm -f $(BUILD_DIR)/.b1nix-i915-*
	@touch $@
$(BUILD_DIR)/kernel/main.o: $(B1NIX_I915_STAMP)

# ── M95/M96: loadable kernel modules ──────────────────────────────────────
# A .ko is the relocatable object itself: the loader allocates its sections in
# the module region, resolves undefined symbols against the kernel's
# EXPORT_SYMBOL table and applies the x86_64 relocations. -DMODULE turns on the
# vermagic stamp in <b1nix/module.h>.
MODULE_CFLAGS := $(COMMON_CFLAGS) $(ARCH_CFLAGS) -DMODULE

define B1NIX_MODULE_RULE
$$(MODULE_OUT_DIR)/$(1).ko: $(2)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(MODULE_CFLAGS) -c $$< -o $$@
endef

$(eval $(call B1NIX_MODULE_RULE,isofs,kernel/fs/isofs.c))
$(eval $(call B1NIX_MODULE_RULE,ntfs,kernel/fs/ntfs.c))
$(eval $(call B1NIX_MODULE_RULE,btrfs,kernel/fs/btrfs.c))
$(eval $(call B1NIX_MODULE_RULE,hda,kernel/dev/hda.c))
$(eval $(call B1NIX_MODULE_RULE,ipv6,kernel/net/ipv6.c))
$(eval $(call B1NIX_MODULE_RULE,ndp,kernel/net/ndp.c))
$(eval $(call B1NIX_MODULE_RULE,ntp,kernel/net/ntp.c))

.PHONY: modules
modules: $(MODULE_KOS)
	sh tools/kernel/check-module-syms.sh $(MODULE_KOS)

# Packaging also runs the symbol gate: a module with a symbol neither the
# kernel nor another module exports fails the build instead of failing insmod.
$(INITRAMFS_MODULES_INC): $(MODULE_KOS) tools/kernel/gen_modules_initramfs.sh \
                          kernel/module/ksyms.c kernel/include/b1nix/version.h
	@mkdir -p $(dir $@)
	sh tools/kernel/check-module-syms.sh $(MODULE_KOS)
	NM='$(NM)' RELEASE='$(B1NIX_RELEASE)' sh tools/kernel/gen_modules_initramfs.sh $@ $(MODULE_KOS)

# M36: only the ftrace demo TU is instrumented, so __cyg_profile hooks fire
# there and nowhere else (global instrumentation would recurse / slow the
# whole kernel).
$(BUILD_DIR)/kernel/lib/ftrace_demo.o: INSTRUMENT_FLAGS := -finstrument-functions

$(BUILD_DIR)/kernel/arch/$(ARCH)/lapic.o: $(AP_TRAMPOLINE_INC) $(AP_TRAMPOLINE_OFFSETS)
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
	$(wildcard userspace/include/*.h) \
	$(wildcard userspace/include/*/*.h) \
	$(wildcard userspace/include/*/*/*.h) \
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
# pulls in transitively (wayland, libffi, pcre2, zlib, pixman,
# freetype, cairo, xkbcommon, harfbuzz, fontconfig, expat, libjpeg, libpng,
# libwebp, libvpx, all NetSurf platform libs, libidn2).
$(BUILD_DIR)/.userspace-bins-built: $(BUILD_DIR)/.userspace-headers-installed \
	$(wildcard userspace/bin/*/*.c) $(wildcard userspace/bin/*/*.cpp) \
	$(wildcard userspace/bin/*/*.S) \
	$(wildcard userspace/src/*.c) \
	$(wildcard userspace/b1cc/src/*.c userspace/b1cc/src/*/*.c) \
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
		$(ZLIB_LIB) \
	$(LIBPNG_LIB) \
	$(LIBJPEG_LIB) \
	$(LIBWEBP_LIB) \
	$(LIBVPX_LIB) \
						$(NSUTILS_LIB) 	$(LIBIDN2_LIB) \
	$(MBEDTLS_LIB) \
	$(PAM_LIB)
	@$(MAKE) -C userspace B1NIX_ARCH=$(ARCH) install
	@touch $@




$(INITRAMFS_NATIVE_SMOKE_INC): userspace/bin/helpers/native_smoke.S $(USERSPACE_DEPS)
	@$(MAKE) -C userspace build/$(ARCH)/bin/native_smoke
	@mkdir -p $(dir $@)
	xxd -i -n vfs_native_smoke_elf userspace/build/$(ARCH)/bin/native_smoke > $@

$(INITRAMFS_B1CC_M34_INC): tools/images/gen_b1cc_m34_initramfs.sh userspace/bin/compiler/b1cc_m34_corpus.c userspace/Makefile $(wildcard userspace/b1cc/tests/*.c) $(USERSPACE_DEPS)
	@mkdir -p $(dir $@)
	B1NIX_ARCH=$(ARCH) sh tools/images/gen_b1cc_m34_initramfs.sh $@

# userspace/bin is grouped by purpose (see userspace/Makefile's BIN_CATS), so a
# program's source is looked up by name rather than assumed to sit at a fixed
# path — moving one between categories does not touch this rule.
USER_BIN_CATS := smoke gfx helpers tools gui compiler
user_bin_src = $(firstword $(wildcard $(addsuffix /$(1).c,$(addprefix userspace/bin/,$(USER_BIN_CATS))) $(addsuffix /$(1).cpp,$(addprefix userspace/bin/,$(USER_BIN_CATS)))) userspace/bin/$(1).c)
.SECONDEXPANSION:
$(INC_DIR)/initramfs_%.inc: $$(call user_bin_src,$$*) $(USERSPACE_DEPS)
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
# One directory deep as well — see the note in userspace/Makefile.
B1CC_SELFHOST_SRCS := $(wildcard $(or $(B1CC_SRCDIR),userspace/b1cc/src)/*.c \
                                 $(or $(B1CC_SRCDIR),userspace/b1cc/src)/*/*.c)
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
# PCRE2: cross-build the static 8-bit library, then link the smoke against it.
PCRE2_LIB := build/$(ARCH)/pkg/pcre2/lib/libpcre2-8.a
$(PCRE2_LIB): $(PKG_DEPS)
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh pcre2 >/dev/null

# M51: pixman (generic C), cross-built static, linked into m51_pixman_smoke.
PIXMAN_LIB := build/$(ARCH)/pkg/pixman/lib/libpixman-1.a
$(PIXMAN_LIB): $(PKG_DEPS)
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh pixman >/dev/null

# M51: FreeType (TrueType + smooth rasterizer), cross-built static.
FREETYPE_LIB := build/$(ARCH)/pkg/freetype/lib/libfreetype.a
$(FREETYPE_LIB): $(PKG_DEPS)
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh freetype >/dev/null

# M51: Cairo — Alpine package (shared library, X11 backend accepted).
CAIRO_LIB := build/$(ARCH)/pkg/cairo/lib/libcairo.so
$(CAIRO_LIB): $(PKG_DEPS)
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh cairo > /dev/null
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh libx11 > /dev/null
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh libxext > /dev/null
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh libxrender > /dev/null
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh libxcb > /dev/null

# M51: xkbcommon (keymap compile + keysym translation), cross-built static.
XKB_LIB := build/$(ARCH)/pkg/xkbcommon/lib/libxkbcommon.a
$(XKB_LIB): $(PKG_DEPS)
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh xkbcommon >/dev/null

# M49: libwayland-client/server share one generated source/build tree. Build it
# once at the top level so parallel initramfs packaging cannot race two nested
# userspace make invocations through tools/packages/pkg-prefix.sh.
WAYLAND_CLIENT_LIB := build/$(ARCH)/pkg/wayland/lib/libwayland-client.so.0
WAYLAND_SERVER_LIB := build/$(ARCH)/pkg/wayland/lib/libwayland-server.so.0
$(WAYLAND_CLIENT_LIB): $(PKG_DEPS)
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh wayland >/dev/null
$(WAYLAND_SERVER_LIB): $(WAYLAND_CLIENT_LIB)

# M51: HarfBuzz (HB_TINY, unified C++ build via cross g++), cross-built static.
HB_LIB := build/$(ARCH)/pkg/harfbuzz/lib/libharfbuzz.a
$(HB_LIB): $(PKG_DEPS)
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh harfbuzz >/dev/null

# M51: expat (XML) + Fontconfig (font discovery), cross-built static.
EXPAT_LIB := build/$(ARCH)/pkg/expat/lib/libexpat.a
$(EXPAT_LIB): $(PKG_DEPS)
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh expat >/dev/null
FONTCONFIG_LIB := build/$(ARCH)/pkg/fontconfig/lib/libfontconfig.a
$(FONTCONFIG_LIB): $(PKG_DEPS) $(EXPAT_LIB) $(FREETYPE_LIB)
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh fontconfig >/dev/null

# M104: Linux-PAM (authentication stack + pam_unix.so), Alpine package.
PAM_LIB := build/$(ARCH)/pkg/pam/lib/libpam.so
$(PAM_LIB): $(PKG_DEPS)
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh pam >/dev/null

# M55: validate the C++ runtime with litehtml (real HTML/CSS layout engine).
# tools/ports/build-litehtml.sh builds litehtml+gumbo, and userspace/Makefile
# links the parse/layout/draw acceptance test against them. M89: litehtml is built
# against the shared LLVM libc++ (B1NIX_CXX_STDLIB=libc++) — NetSurf's only C++
# component, so this also moves the NetSurf C++ stack off GCC libstdc++.
# M53: zlib (image-codec dependency for the NetSurf browser platform).
#
# Alpine's package, not a from-source port: it is the same zlib, built for the
# same musl and the same architecture, and taking it deletes a build script
# along with the duty to keep it building. tools/packages/alpine-fetch.sh pins
# every package's sha256 in tools/packages/alpine.lock. First of the migration
# described in docs/ports-migration-plan.md.
#
# Both shapes come down: libz.so.1 is what binaries link and what the image
# carries, and libz.a is still wanted by the ports that have not moved yet
# (libpng, NetSurf) and link it statically.
# What a package-backed port depends on: the fetcher, the pinned hashes, and the
# port-to-package table. Any of the three changing re-runs the install.
PKG_DEPS := tools/packages/pkg-prefix.sh tools/packages/alpine-fetch.sh \
            tools/packages/alpine.lock tools/packages/alpine-ports.map

ZLIB_PREFIX := build/$(ARCH)/pkg/zlib
ZLIB_LIB := $(ZLIB_PREFIX)/lib/libz.a
ZLIB_SO := $(ZLIB_PREFIX)/lib/libz.so.1
$(ZLIB_LIB) $(ZLIB_SO): $(PKG_DEPS)
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh zlib >/dev/null

# M53: libpng (over zlib + libm) — NetSurf image-codec dependency.
LIBPNG_LIB := build/$(ARCH)/pkg/libpng/lib/libpng16.a
$(LIBPNG_LIB): $(PKG_DEPS)
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh libpng >/dev/null

# M53: libjpeg (IJG) — NetSurf image-codec dependency.
LIBJPEG_LIB := build/$(ARCH)/pkg/libjpeg/lib/libjpeg.a
$(LIBJPEG_LIB): $(PKG_DEPS)
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh libjpeg >/dev/null

# M53: libwebp (image + VP8 video-keyframe codec) — NetSurf codec dependency.
LIBWEBP_LIB := build/$(ARCH)/pkg/libwebp/lib/libwebp.a
$(LIBWEBP_LIB): $(PKG_DEPS)
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh libwebp >/dev/null

# M53: libvpx (VP8 full-motion video decode) — NetSurf/WebM video codec.
LIBVPX_LIB := build/$(ARCH)/pkg/libvpx/lib/libvpx.a
$(LIBVPX_LIB): $(PKG_DEPS)
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh libvpx >/dev/null







# M53: userspace VirGL smoke — drives /dev/virtio-gpu (host-GPU-accelerated 3D).
# OpenRC and Crashpad install straight into the staging rootfs, and until now
# nothing invoked them: both had been run by hand once, years apart, and their
# output simply lived in build/$(ARCH)/rootfs from then on. Deleting that
# directory — which is a build directory, and ought to be disposable — produced
# an image with no init and no crash handler, and no rule to rebuild either.
#
# The targets are the installed files rather than a stamp under build/, so a
# wiped rootfs is a missing target and the port runs again.
OPENRC_INIT := $(BUILD_DIR)/rootfs/sbin/openrc-init
$(OPENRC_INIT): tools/ports/build-openrc.sh $(LIBC_SO)
	B1NIX_ARCH=$(ARCH) sh tools/ports/build-openrc.sh >/dev/null

CURLBUILD_STAMP := build/$(ARCH)/pkg/curlbuild/lib/libcurl.a
$(CURLBUILD_STAMP): $(PKG_DEPS)
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh curlbuild >/dev/null

PKGROOT_STAMP := $(PKGROOT)/.installed
$(PKGROOT_STAMP): $(PKG_DEPS)
	B1NIX_ARCH=$(ARCH) ALPINE_LAYOUT=native \
		tools/packages/pkg-prefix.sh --into $(PKGROOT) programs >/dev/null
	@touch $@
$(CURL_ELF) $(DROPBEAR_ELF) $(BMAKE_ELF) $(SAMU_ELF): $(PKGROOT_STAMP)


$(INITRAMFS_CURL_INC): $(CURL_ELF)
	@mkdir -p $(dir $@)
	xxd -i -n vfs_curl_elf $(CURL_ELF) > $@

# Dropbear SSH server (dropbearmulti: server + dropbearkey + dropbearconvert,
# dispatched by argv[0]). Built static against the b1nix userspace libc.

$(INITRAMFS_DROPBEAR_INC): $(DROPBEAR_ELF)
	@mkdir -p $(dir $@)
	xxd -i -n vfs_dropbear_elf $(DROPBEAR_ELF) > $@

# In-guest build tools, both GNU-free (M98): bmake (BSD 3-clause NetBSD make)
# ships as /bin/make and samurai (0BSD Ninja reimplementation) as /bin/samu with
# a /bin/ninja alias. Together they replace the retired GNU Make port.


OPENSSL_LIB := build/$(ARCH)/pkg/openssl/lib/libssl.a
$(OPENSSL_LIB): $(PKG_DEPS)
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh openssl >/dev/null

LIBIDN2_LIB := build/$(ARCH)/pkg/libidn2/lib/libidn2.a
$(LIBIDN2_LIB): $(PKG_DEPS)
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh libidn2 >/dev/null

LIBPSL_LIB := build/$(ARCH)/pkg/libpsl/lib/libpsl.a
$(LIBPSL_LIB): $(PKG_DEPS)
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh libpsl >/dev/null

LIBFFI_LIB := $(FFI_LIB)
$(LIBFFI_LIB): $(PKG_DEPS)
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh libffi >/dev/null

$(MBEDTLS_LIB): $(PKG_DEPS)
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh mbedtls >/dev/null

$(LIBUNISTRING_LIB): $(PKG_DEPS)
	B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh libunistring >/dev/null


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
# (regenerated by hand via tools/blobs/build-linux-hello.sh) so the kernel build
# does not require a Linux assembler on the build host.
$(INITRAMFS_M40_LINUX_INC): tools/blobs/linux_hello.bin
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m40_linux_hello tools/blobs/linux_hello.bin > $@

# M67: embed the committed prebuilt static Rust ELF blob. Checked in (regenerated
# by hand via tools/blobs/build-rust-hello.sh) so the kernel build needs no Rust
# toolchain. Same pattern as the M40 Linux blob above.
$(INITRAMFS_M67_RUST_INC): tools/blobs/hello_b1nix.elf
	@mkdir -p $(dir $@)
	xxd -i -n vfs_m67_rust_elf tools/blobs/hello_b1nix.elf > $@

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

ifeq ($(ARCH),x86_64)
ifdef LIBC_SO
# Musl libc + dev + linux-headers from Alpine packages.
$(LIBC_SO): $(PKG_DEPS)
	@B1NIX_ARCH=$(ARCH) tools/packages/pkg-prefix.sh musl >/dev/null
	@mkdir -p build/$(ARCH)/ports/musl
	@ln -sfn ../../pkg/musl build/$(ARCH)/ports/musl/install

$(INITRAMFS_LD_MUSL_INC): $(LIBC_SO)
	@mkdir -p $(dir $@)
	xxd -i -n $(LIBC_INC_SYM) $(LIBC_SO) > $@
endif

$(INC_DIR)/initramfs_m92_musl_dyn_smoke.inc: userspace/bin/helpers/m92_musl_dyn_test.c $(USERSPACE_DEPS) $(MUSL_INSTALLED)
	@mkdir -p $(dir $@)
	tools/b1nix-musl-cc -dynamic $< -o $(BUILD_DIR)/m92-musl-dyn-smoke
	xxd -i -n vfs_m92_musl_dyn_smoke_elf $(BUILD_DIR)/m92-musl-dyn-smoke > $@

$(INC_DIR)/initramfs_m92_musl_ldso_smoke.inc: userspace/bin/helpers/m92_musl_ldso_test.c $(USERSPACE_DEPS) $(MUSL_INSTALLED)
	@mkdir -p $(dir $@)
	tools/b1nix-musl-cc -ldso $< -o $(BUILD_DIR)/m92-musl-ldso-smoke
	xxd -i -n vfs_m92_musl_ldso_smoke_elf $(BUILD_DIR)/m92-musl-ldso-smoke > $@

$(INC_DIR)/initramfs_musl_posix_smoke.inc: userspace/bin/smoke/musl_posix_smoke.c $(USERSPACE_DEPS) $(MUSL_INSTALLED)
	@mkdir -p $(dir $@)
	tools/b1nix-musl-cc -dynamic $< -o $(BUILD_DIR)/musl-posix-smoke
	xxd -i -n vfs_musl_posix_smoke_elf $(BUILD_DIR)/musl-posix-smoke > $@

$(INC_DIR)/initramfs_m92_musl_hello.inc: userspace/bin/helpers/m92_musl_hello.c $(USERSPACE_DEPS) $(MUSL_INSTALLED)
	@mkdir -p $(dir $@)
	tools/b1nix-musl-cc -dynamic $< -o $(BUILD_DIR)/m92-musl-hello
	xxd -i -n vfs_m92_musl_hello_elf $(BUILD_DIR)/m92-musl-hello > $@

$(INC_DIR)/initramfs_m92_musl_step2.inc: userspace/bin/helpers/m92_musl_step2.c $(USERSPACE_DEPS) $(MUSL_INSTALLED)
	@mkdir -p $(dir $@)
	tools/b1nix-musl-cc -dynamic $< -o $(BUILD_DIR)/m92-musl-step2
	xxd -i -n vfs_m92_musl_step2_elf $(BUILD_DIR)/m92-musl-step2 > $@

$(INC_DIR)/initramfs_m92_musl_raw_diag.inc: userspace/bin/helpers/m92_musl_raw_diag.c $(USERSPACE_DEPS) $(MUSL_INSTALLED)
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

# One #define per data field, straight out of the symbol table: the offsets can
# no longer disagree with the assembly, because they ARE the assembly's.
$(AP_TRAMPOLINE_OFFSETS): $(AP_TRAMP_OBJ)
	@mkdir -p $(dir $@)
	@printf '/* Generated from ap_trampoline.S — do not edit. */\n#ifndef B1NIX_AP_TRAMPOLINE_OFFSETS_H\n#define B1NIX_AP_TRAMPOLINE_OFFSETS_H\n' > $@
	@$(NM) -n $< | awk '\
		$$3 == "tramp_magic"  { printf "#define TRAMP_MAGIC_OFF   0x%s\n", $$1 } \
		$$3 == "pml4_phys"    { printf "#define TRAMP_PML4_OFF    0x%s\n", $$1 } \
		$$3 == "stack_ptr"    { printf "#define TRAMP_STACK_OFF   0x%s\n", $$1 } \
		$$3 == "percpu_ptr"   { printf "#define TRAMP_PCPU_OFF    0x%s\n", $$1 } \
		$$3 == "cpu_id"       { printf "#define TRAMP_CPU_OFF     0x%s\n", $$1 } \
		$$3 == "ready_flag"   { printf "#define TRAMP_READY_OFF   0x%s\n", $$1 } \
		$$3 == "ap_main_ptr"  { printf "#define TRAMP_APMAIN_OFF  0x%s\n", $$1 }' \
		| sed 's/0x00*\([0-9a-f]\)/0x\1/' >> $@
	@printf '#endif\n' >> $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) $(ARCH_CFLAGS) -c $< -o $@

# What a release carries instead of the build tree's kernel.
#
# The linked kernel is 33 MB, of which 28 MB is debug information: useful when
# a fault report needs symbolising, dead weight on a machine that is only going
# to boot it. Distributions split those into two packages and so do we — see
# tools/packages/b1nix-packages.list.
KERNEL_DIST := $(BUILD_DIR)/dist/kernel.elf
KERNEL_DIST_DEBUG := $(BUILD_DIR)/dist/kernel.elf.debug

kernel-dist: $(KERNEL_ELF)
	@mkdir -p $(BUILD_DIR)/dist
	@llvm-objcopy --only-keep-debug $(KERNEL_ELF) $(KERNEL_DIST_DEBUG)
	@llvm-strip -o $(KERNEL_DIST) $(KERNEL_ELF)
	@ls -l $(KERNEL_DIST) $(KERNEL_DIST_DEBUG) | awk '{printf "  %-40s %6.1f MB\n", $$9, $$5/1048576}'

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
# No `init=` on these: they boot the DEFAULT PID 1, /sbin/init (BusyBox init),
# with /etc/inittab handing the runlevels to OpenRC — the configuration an
# ordinary boot uses, so the whole suite runs on it rather than on a variant.
SMOKE_CMDLINE_sys=b1nix.test=1 b1nix.kvtest=abc123 b1nix.ssh-loopback=1 b1nix.aslr b1nix.smoke=sys
SMOKE_CMDLINE_gfx=b1nix.test=1 b1nix.kvtest=abc123 b1nix.ssh-loopback=1 b1nix.aslr b1nix.smoke=gfx
SMOKE_CMDLINE_posix=b1nix.test=1 b1nix.kvtest=abc123 b1nix.ssh-loopback=1 b1nix.aslr b1nix.smoke=posix
SMOKE_CMDLINE_blk=b1nix.test=1 b1nix.kvtest=abc123 b1nix.ssh-loopback=1 b1nix.aslr b1nix.smoke=blk
# OpenRC ctltest: boots the real init system as PID 1 and drives sysinit/boot/default,
# then a local.d hook asks PID 1 to power off through /run/openrc/init.ctl — the
# control-FIFO path openrc-shutdown and telinit use. A clean poweroff proves the channel works.
SMOKE_CMDLINE_openrc=init=/sbin/openrc-init b1nix.test=1 b1nix.openrc-ctltest b1nix.acs-keep=00:0e.0
# M108 init: the default boot, checked as such. PID 1 is /sbin/init (BusyBox
# init, no `init=` needed) and /etc/inittab drives OpenRC's runlevels under it.
# This instance runs no part of the ordinary suite — it exists to prove the
# things only PID 1 can be asked about: its own identity, orphan reaping, getty
# respawn, and that OpenRC's default runlevel really ran underneath.
SMOKE_CMDLINE_init=b1nix.test=1 b1nix.smoke=init

# Hardware-passthrough instance, beside the smoke ISOs rather than instead of
# the ordinary one.
#
# `make iso` and the smoke ISOs both wrote build/$(ARCH)/b1nix.iso, so whichever
# ran last decided which cmdline the other's run booted with — a wrong-image
# trap that costs a whole passthrough boot to notice. This has its own name, so
# one make invocation can produce the passthrough image and the smoke images
# from the same kernel and the same root.ext4.
SMOKE_CMDLINE_pass=b1nix.i915sway b1nix.use-cage b1nix.drm-debug b1nix.drm-debug-atomic
# The same run under sway rather than cage, for the questions only the
# compositor can answer. cage 0.1.5 has no debug switch at all — its -d means
# "no client-side decorations" — so it says nothing about which modes it saw or
# which one it chose; sway -d logs both, on the same wlroots.
# The connector surface only: enumerate from ring 3 and stop. Seconds, not
# minutes, and it carries the buffer-bounds check against a real EDID.
SMOKE_CMDLINE_pass-probe=b1nix.i915sway b1nix.drm-probe-only b1nix.drm-enumerate b1nix.drm-debug
SMOKE_CMDLINE_pass-sway=b1nix.i915sway b1nix.sway-clients b1nix.bright b1nix.drm-debug b1nix.vma-check
SMOKE_CMDLINE_pass-headless=b1nix.i915sway b1nix.sway-headless b1nix.vma-check
# The browser, under cage on the passed-through GPU. Its packages are fetched at
# run time — 248 MB installed is not something to carry in an image for a test
# that is run occasionally.
SMOKE_CMDLINE_pass-chromium=b1nix.i915sway b1nix.chromium b1nix.drm-debug
# The same cage run with a frame built to be photographed: saturated colour
# across the whole screen and a client whose output changes, so a camera shot
# says whether the panel is showing our picture rather than merely being lit.
SMOKE_CMDLINE_pass-bright=b1nix.i915sway b1nix.use-cage b1nix.bright b1nix.drm-debug

iso-sys iso-gfx iso-posix iso-blk iso-openrc iso-init iso-pass iso-pass-sway iso-pass-bright iso-pass-probe iso-pass-headless iso-pass-chromium: root-image check-dynamic $(KERNEL_ELF)
	@$(MKISO) --stage $(BUILD_DIR)/$@ --out $(BUILD_DIR)/b1nix-$(@:iso-%=%).iso \
	    --arch $(ARCH) --kernel $(KERNEL_ELF) --timeout $(BOOT_TIMEOUT) \
	    --cmdline "$(SMOKE_CMDLINE_$(@:iso-%=%))" \
	    --module $(BUILD_DIR)/root.ext4:rootfs.img

# V8 run instance: the kernel hook (gated by b1nix.v8run) mounts ram0 -> /mnt/v8
# and runs d8 on m58.js. It boots in test mode (b1nix.test=1) on purpose: the hook
# loads d8 off the disk early — before the rc's M14 test touches sda — and the
# active rc keeps the scheduler busy so d8's thread runs. (Without test mode, init
# drops to an interactive getty/shell that starves d8 on a single CPU.) Reuses the
# shared kernel.elf — no recompile, just a different boot cmdline. The d8 binary +
# m58.js ride on build/v8-out/v8-ext4.img, handed to the kernel as the ram0
# Multiboot2 module by tests/smoke.sh.
# Display bring-up instance: the kernel and nothing else.
#
# Deliberately carries no rootfs module. The in-kernel DRM client is what drives
# the display, so userspace contributes nothing here — and shipping it costs
# 154 MB in the image plus a full rootfs build and an init that takes minutes to
# reach a state this boot never uses. Paired with b1nix.gfx-only, which powers
# the machine off as soon as the frame exists.
#
# Depends on the kernel alone, so an edit to a shim rebuilds one object and
# relinks, instead of rebuilding userspace and repacking a filesystem image.
iso-i915: $(KERNEL_ELF)
	@$(MKISO) --stage $(BUILD_DIR)/iso-i915 --out $(BUILD_DIR)/b1nix-i915.iso \
	    --arch $(ARCH) --kernel $(KERNEL_ELF) --timeout $(BOOT_TIMEOUT) \
	    --cmdline "b1nix.gfx-only $(I915_EXTRA_CMDLINE)" \
	    $(if $(I915_EDID),--module $(I915_EDID):edid.bin)

iso-v8: $(KERNEL_ELF) root-image
	@$(MKISO) --stage $(BUILD_DIR)/iso-v8 --out $(BUILD_DIR)/b1nix-v8.iso \
	    --arch $(ARCH) --kernel $(KERNEL_ELF) --timeout $(BOOT_TIMEOUT) \
	    --cmdline "b1nix.test=1 b1nix.v8run b1nix.smoke=v8" \
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
	    --cmdline "$(KERNEL_CMDLINE) b1nix.test=1" \
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

install-ports: userspace-install busybox-package install-native-toolchain $(PKGROOT_STAMP) $(OPENRC_INIT)
	tools/packages/install-ports.sh $(BUILD_DIR)/rootfs $(ARCH) $(PORTS_SOURCE) $(PACKAGE_INDEX_URL)
	@# The published dev package may carry older libc headers than this checkout.
	@# Restore the current userspace ABI after package extraction so cross C++
	@# ports (notably libc++) see the same wchar/locale surface as the build.
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
	@# Exclude large Skia/NetSurf .inc files (not needed for self-host).
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

root-image: $(KERNEL_ELF) $(USERSPACE_DEPS) install-ports $(INITRAMFS_MODULES_INC)
	@# M95: the same .ko images the initramfs carries, plus the generated
	@# modules.dep / modules.alias, so insmod/rmmod/modprobe keep working after
	@# the real root is mounted (which hides the initramfs).
	@mkdir -p $(BUILD_DIR)/rootfs/lib/modules/$(B1NIX_RELEASE)
	@cp -f $(MODULE_KOS) $(BUILD_DIR)/rootfs/lib/modules/$(B1NIX_RELEASE)/
	@cp -f $(INC_DIR)/.modules-stage/modules.dep $(INC_DIR)/.modules-stage/modules.alias $(BUILD_DIR)/rootfs/lib/modules/$(B1NIX_RELEASE)/
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
	@# M104: public trust anchors, for talking to real repositories over HTTPS
	@# (bpkg and curl both read /etc/ssl/certs/ca-certificates.crt — the path
	@# build-curl.sh already configured as --with-ca-bundle). Taken from the
	@# build host's own store rather than vendored into git, so the image never
	@# ships a CA list that silently goes stale. B1NIX_CA_BUNDLE overrides the
	@# search. Absent on the host, https:// simply fails with a clear error
	@# instead of falling back to an unverified connection.
	@mkdir -p $(BUILD_DIR)/rootfs/etc/ssl/certs
	@CA=""; for c in $(B1NIX_CA_BUNDLE) /etc/ssl/cert.pem /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt; do \
		if [ -n "$$c" ] && [ -f "$$c" ]; then CA="$$c"; break; fi; \
	done; \
	if [ -n "$$CA" ]; then \
		cp -f "$$CA" $(BUILD_DIR)/rootfs/etc/ssl/certs/ca-certificates.crt; \
		echo "  CA      $$CA -> /etc/ssl/certs/ca-certificates.crt"; \
	else \
		echo "  CA      no host trust store found — https:// will fail in the guest"; \
	fi
	@mkdir -p $(BUILD_DIR)/rootfs/lib
	@# Shared libraries that come from Alpine packages rather than from a port.
	@# Copied by real name and by SONAME, not symlinked: the ext4 driver is not
	@# asked to follow a link during early loader work.
	@for so in build/$(ARCH)/pkg/*/lib/lib*.so.*; do \
		if [ -f "$$so" ] && ! [ -L "$$so" ]; then \
			cp -f "$$so" $(BUILD_DIR)/rootfs/lib/; \
			soname=$$($(READELF) -d "$$so" 2>/dev/null | grep SONAME | sed 's/.*\[//;s/\].*//'); \
			if [ -n "$$soname" ] && [ "$$soname" != "$$(basename $$so)" ]; then \
				cp -f "$$so" "$(BUILD_DIR)/rootfs/lib/$$soname"; \
			fi; \
		fi; \
	done
	@# Mesa is Alpine's (mesa-egl/-gles/-gbm/-gl/-glapi, tools/packages/
	@# alpine-ports.map), so it arrives through $(PKGROOT) with everything else
	@# on the image — there is no Mesa staging of its own here any more.
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
	@# M104: Linux-PAM (from Alpine packages) — libpam.so.0 is staged by the
	@# generic pkg loop above. Stage security/*.so modules into rootfs/lib/security
	@# and write PAM policy files.
	@mkdir -p $(BUILD_DIR)/rootfs/lib/security $(BUILD_DIR)/rootfs/etc/pam.d $(BUILD_DIR)/rootfs/include/security
	@if [ -d build/$(ARCH)/pkg/pam/lib/security ]; then \
		cp -f build/$(ARCH)/pkg/pam/lib/security/*.so $(BUILD_DIR)/rootfs/lib/security/; \
	fi
	@if [ -d build/$(ARCH)/pkg/pam/include/security ]; then \
		cp -f build/$(ARCH)/pkg/pam/include/security/*.h $(BUILD_DIR)/rootfs/include/security/; \
	fi
	@if [ -d build/$(ARCH)/pkg/pam/sbin ]; then \
		mkdir -p $(BUILD_DIR)/rootfs/sbin; \
		cp -f build/$(ARCH)/pkg/pam/sbin/* $(BUILD_DIR)/rootfs/sbin/; \
		if [ -f $(BUILD_DIR)/rootfs/sbin/unix_chkpwd ]; then \
			python3 -c "import sys; f=open(sys.argv[1],'r+b'); f.seek(7); f.write(bytes([3])); f.close()" $(BUILD_DIR)/rootfs/sbin/unix_chkpwd; \
		fi; \
	fi
	@printf '# M104 smoke policy (userspace/bin/smoke/m104_pam_smoke.c)\nauth       required     pam_unix.so\naccount    required     pam_unix.so\nsession    required     pam_unix.so\n' > $(BUILD_DIR)/rootfs/etc/pam.d/m104-pam-smoke
	@printf '# Default policy for services without a specific /etc/pam.d/<service> file.\nauth       required     pam_unix.so\naccount    required     pam_unix.so\nsession    required     pam_unix.so\n' > $(BUILD_DIR)/rootfs/etc/pam.d/other
	@printf '# b1nix PAM policy for dropbear sshd\nauth       required     pam_unix.so\naccount    required     pam_unix.so\nsession    required     pam_unix.so\n' > $(BUILD_DIR)/rootfs/etc/pam.d/sshd
	@printf '# M108 smoke policy (userspace/bin/smoke/m108_smoke.c): pam_unix.so reads\n# the same /etc/shadow "$$6$$" hashes BusyBox su/passwd read and write.\nauth       required     pam_unix.so\naccount    required     pam_unix.so\nsession    required     pam_unix.so\n' > $(BUILD_DIR)/rootfs/etc/pam.d/m108-smoke
	@# M108: the su/passwd/init smoke ELF links libpam.so as DT_NEEDED.
	@$(MAKE) -C userspace build/$(ARCH)/bin/m108_smoke >/dev/null 2>&1 || true
	@if [ -f userspace/build/$(ARCH)/bin/m108_smoke ]; then \
		cp -f userspace/build/$(ARCH)/bin/m108_smoke $(BUILD_DIR)/rootfs/bin/m108_smoke; \
		chmod +x $(BUILD_DIR)/rootfs/bin/m108_smoke; \
	fi
	@# M104: the PAM smoke ELF (links libpam.so as DT_NEEDED).
	@$(MAKE) -C userspace build/$(ARCH)/bin/m104_pam_smoke >/dev/null 2>&1 || true
	@if [ -f userspace/build/$(ARCH)/bin/m104_pam_smoke ]; then \
		cp -f userspace/build/$(ARCH)/bin/m104_pam_smoke $(BUILD_DIR)/rootfs/bin/m104_pam_smoke; \
		chmod +x $(BUILD_DIR)/rootfs/bin/m104_pam_smoke; \
	fi
	@# M104/dropbear: dropbearmulti (server + dbclient + dropbearkey +
	@# dropbearconvert, dispatched by argv[0]) was built by install-ports
	@# ($(DROPBEAR_ELF)) but — like every binary here — needs a real copy in
	@# rootfs/bin; nothing staged it before M104 added PAM support, so
	@# userspace/rootfs-overlay/etc/init.d/sshd's /bin/dropbear* calls had no
	@# binary to find. Real copies (not symlinks), same reasoning as above.

	@# Stage sysroot C++ runtime .so.
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
	@# EI_OSABI = ELFOSABI_LINUX on the loader itself. As PT_INTERP its
	@# personality comes from the program it interprets, but run DIRECTLY
	@# (`ld-musl-x86_64.so.1 --list prog`, which is what `ldd` is) it IS the
	@# program — and with OSABI 0 and no PT_INTERP of its own, nothing marked it
	@# as a Linux binary. Its arch_prctl(ARCH_SET_FS) then went untranslated,
	@# __init_tp() failed, and musl did what it does on that path: executed
	@# `hlt`, which #GPs in ring 3. Every other musl binary is stamped the same
	@# way (tools/b1nix-musl-cc); the loader was the one that was not.
	@python3 -c "import sys; f=open(sys.argv[1],'r+b'); f.seek(7); f.write(bytes([3])); f.close()" \
		$(BUILD_DIR)/rootfs/lib/$(LIBC_LDSO_NAME)
	@# The ext4 driver may not follow symlinks, so give every name a real
	@# directory entry. Hard links share one inode: the image carries the bytes
	@# once no matter how many names point at them.
	@# libc.musl-x86_64.so.1 is the name Alpine's own binaries and libraries
	@# record as DT_NEEDED — it is their libc's SONAME. Anything installed from
	@# their repository asks for it, so the loader has to be reachable under it
	@# too, or every Alpine library fails to load for want of a libc that is
	@# already there under three other names.
	@# Two names, plus one that is on its way out.
	@#
	@# musl is a single blob and this used to give it eleven names, on the
	@# theory that anything might ask for any of them. Nothing does: across
	@# every binary and library on the image, 459 record libc.musl-x86_64.so.1
	@# (musl's own SONAME, and what Alpine's binaries ask for), the loader is
	@# reached through PT_INTERP as ld-musl-x86_64.so.1, and libm, libpthread,
	@# librt, libdl, libcrypt, libutil and libresolv are recorded by nobody at
	@# all — they were folded into libc before any of this was built. libc.so
	@# is the name our own toolchain used to stamp, and it stays until the last
	@# binaries carrying it are relinked.
	@for name in libc.musl-x86_64.so.1 libc.so; do \
		ln -f $(BUILD_DIR)/rootfs/lib/$(LIBC_LDSO_NAME) $(BUILD_DIR)/rootfs/lib/$$name 2>/dev/null || \
			cp -f $(LIBC_SO) $(BUILD_DIR)/rootfs/lib/$$name; \
	done
endif
	@# Create SONAME hard copies for any .so.N.M files in rootfs/lib
	@for f in $(BUILD_DIR)/rootfs/lib/lib*.so.*.*.*; do \
		[ -f "$$f" ] || continue; \
		soname=$$($(READELF) -d "$$f" 2>/dev/null | grep SONAME | sed 's/.*\[//;s/\].*//'); \
		if [ -n "$$soname" ] && ! [ -e "$(BUILD_DIR)/rootfs/lib/$$soname" ]; then \
			cp -f "$$f" "$(BUILD_DIR)/rootfs/lib/$$soname"; \
		fi; \
	done
	@# Every program on the image that is not ours and not part of how it boots:
	@# zsh, curl, dropbear, bmake and samurai, all Alpine packages. Unpacked with
	@# their own paths — a program looks for its files where it was compiled to
	@# look, zsh's modules under /usr/lib/zsh, bmake's makefiles under
	@# /usr/share/mk — into one staging root, which is merged over the image
	@# here. Directories are created rather than replaced, because /usr/lib in
	@# the image is a link to /lib.
	@$(MAKE) --no-print-directory $(PKGROOT_STAMP)
	@(cd $(PKGROOT) && find . -type d -exec mkdir -p $(CURDIR)/$(BUILD_DIR)/rootfs/{} \;)
	@(cd $(PKGROOT) && find . ! -type d ! -name .installed \
		-exec cp -a --remove-destination {} $(CURDIR)/$(BUILD_DIR)/rootfs/{} \;)
	@# Names the rest of the tree asks for by path. Real copies, not symlinks:
	@# the ext4 driver is not asked to follow one. bmake answers to /bin/make
	@# because nothing on the target should have to know it is not GNU Make;
	@# samurai answers to /bin/ninja for the same reason; and the init script
	@# for sshd calls /bin/dropbear*, while Alpine puts those in /usr/*bin.
	@# --remove-destination on every one of these, and the retired multi-call
	@# binary removed first: the staging rootfs keeps whatever earlier builds
	@# left, and dropbear used to be one binary with a symlink per tool. Copying
	@# onto such a symlink writes THROUGH it, so each tool in turn overwrote the
	@# single file all the names pointed at, and the guest got dropbearkey when
	@# it asked for dbclient.
	@rm -f $(BUILD_DIR)/rootfs/bin/dropbearmulti \
		$(BUILD_DIR)/rootfs/bin/dropbear $(BUILD_DIR)/rootfs/bin/dbclient \
		$(BUILD_DIR)/rootfs/bin/dropbearkey $(BUILD_DIR)/rootfs/bin/dropbearconvert
	@cp -f --remove-destination $(BMAKE_ELF) $(BUILD_DIR)/rootfs/bin/make
	@cp -f --remove-destination $(BMAKE_ELF) $(BUILD_DIR)/rootfs/bin/bmake
	@cp -f --remove-destination $(SAMU_ELF) $(BUILD_DIR)/rootfs/bin/samu
	@cp -f --remove-destination $(SAMU_ELF) $(BUILD_DIR)/rootfs/bin/ninja
	@cp -f --remove-destination $(CURL_ELF) $(BUILD_DIR)/rootfs/bin/curl
	@cp -f --remove-destination $(DROPBEAR_ELF) $(BUILD_DIR)/rootfs/bin/dropbear
	@for n in dbclient dropbearkey scp ssh; do \
		[ -f $(PKGROOT)/usr/bin/$$n ] && \
			cp -f --remove-destination $(PKGROOT)/usr/bin/$$n \
				$(BUILD_DIR)/rootfs/bin/$$n; \
	done; true
	@chmod +x $(BUILD_DIR)/rootfs/bin/make $(BUILD_DIR)/rootfs/bin/bmake \
		$(BUILD_DIR)/rootfs/bin/samu $(BUILD_DIR)/rootfs/bin/ninja \
		$(BUILD_DIR)/rootfs/bin/curl $(BUILD_DIR)/rootfs/bin/dropbear
	@# M98: the GNU-free in-guest build tools. bmake is /bin/make (nothing on the
	@# target should have to know it is not GNU Make) and samurai is /bin/samu
	@# plus a /bin/ninja alias. Real copies, not symlinks — the ext4 driver does
	@# not follow them. bmake reads its system makefiles from /usr/share/mk.

	@# M40/M67: committed static ELF blobs (Linux ABI compat + Rust std smoke).
	@# Only ever wired into the legacy xxd/.inc initramfs path (kernel/fs/
	@# initramfs.c never gained a #include for them after the ext4-root
	@# migration made that path bootstrap-only) — stage them into rootfs/bin
	@# directly, so /bin/init's discovery loop picks
	@# them up.
	@if [ -f tools/blobs/linux_hello.bin ]; then \
		cp -f tools/blobs/linux_hello.bin $(BUILD_DIR)/rootfs/bin/m40-linux-hello; \
		chmod +x $(BUILD_DIR)/rootfs/bin/m40-linux-hello; \
	fi
	@if [ -f tools/blobs/linux_abi_test.bin ]; then \
		cp -f tools/blobs/linux_abi_test.bin $(BUILD_DIR)/rootfs/bin/m40-linux-abi; \
		chmod +x $(BUILD_DIR)/rootfs/bin/m40-linux-abi; \
	fi
	@if [ -f tools/blobs/hello_b1nix.elf ]; then \
		cp -f tools/blobs/hello_b1nix.elf $(BUILD_DIR)/rootfs/bin/m67-rust; \
		chmod +x $(BUILD_DIR)/rootfs/bin/m67-rust; \
	fi
	@# Trim rootfs: remove LLVM static archives and shared lib (200+ MB) that
	@# are only needed for self-hosting, not for smoke.
	@rm -f $(BUILD_DIR)/rootfs/lib/libLLVM*.a $(BUILD_DIR)/rootfs/lib/libLLVM.so
	@# Remove .so.N.M files whose SONAME copy already exists (avoid duplicates).
	@# Only when that SONAME entry is a real file: a package names its library
	@# by SONAME through a symlink to exactly this versioned file, and deleting
	@# the file then leaves the link pointing at nothing — which the loader
	@# reports as a library full of missing symbols rather than as an absent
	@# file, and cost an afternoon on Alpine's libcurl.
	@for f in $(BUILD_DIR)/rootfs/lib/lib*.so.*.*.*; do \
		[ -f "$$f" ] || continue; \
		soname=$$($(READELF) -d "$$f" 2>/dev/null | grep SONAME | sed 's/.*\[//;s/\].*//'); \
		if [ -n "$$soname" ] && [ "$$soname" != "$$(basename $$f)" ] && \
		   [ -f "$(BUILD_DIR)/rootfs/lib/$$soname" ] && \
		   ! [ -L "$(BUILD_DIR)/rootfs/lib/$$soname" ]; then \
			rm -f "$$f"; \
		fi; \
	done
	@# The ext4 driver does not follow a symlink, so a .so link is a name the
	@# image does not have. Replace it with a copy of what it pointed at rather
	@# than deleting it: Alpine's nss is staged as libnss3.so -> libnss3.so.105
	@# and the SONAME every dependent records is libnss3.so, so deleting the
	@# link left chromium and four nss libraries naming a library that was no
	@# longer in the image. A dangling link is still deleted — there is nothing
	@# to copy, and leaving it is the one case the loader reports as a library
	@# full of missing symbols instead of an absent file.
	@find $(BUILD_DIR)/rootfs/lib -type l -name '*.so' | while read -r l; do \
		if [ -f "$$l" ]; then \
			t=$$(readlink -f "$$l"); rm -f "$$l"; cp -f "$$t" "$$l"; \
		else \
			rm -f "$$l"; \
		fi; \
	done 2>/dev/null || true
	@# Repacked only when the staged tree actually changed.
	@#
	@# Building the image is a fresh 300 MB file and a full mke2fs of the tree —
	@# minutes, every time, including the many rebuilds where only the kernel
	@# moved. The test is the same one make would apply: is any staged file newer
	@# than the image. ROOT_IMAGE_FORCE=1 rebuilds it regardless.
	@if [ "$(ROOT_IMAGE_FORCE)" != "1" ] && [ -f $(BUILD_DIR)/root.ext4 ] && \
	   [ -z "$$(find $(BUILD_DIR)/rootfs -newer $(BUILD_DIR)/root.ext4 -print -quit 2>/dev/null)" ]; then \
		echo "  root.ext4 up to date"; \
	else \
		dd if=/dev/zero of=$(BUILD_DIR)/root.ext4 bs=1048576 count=$(ROOT_IMAGE_SIZE) 2>/dev/null; \
		$(MKE2FS) -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q -L b1nix-root -E root_owner=0:0 -d $(BUILD_DIR)/rootfs $(BUILD_DIR)/root.ext4 2>/dev/null || \
		$(MKE2FS) -t ext4 -q -L b1nix-root -E root_owner=0:0 -d $(BUILD_DIR)/rootfs $(BUILD_DIR)/root.ext4; \
	fi
	@# Everything in a Unix root filesystem belongs to root. `mke2fs -d` instead
	@# copies the BUILD HOST's uid/gid onto every file (501:20 on a macOS
	@# checkout), so the guest saw a rootfs owned by a nonexistent user. That
	@# breaks any in-guest ownership check: OpenPAM refuses to read a policy file
	@# it does not see as root-owned and pam_start() failed with PAM_SYSTEM_ERR.
	@# One batched debugfs pass over ~900 paths, not one process per file.
	@cd $(BUILD_DIR)/rootfs && find . \( -type f -o -type d -o -type l \) -print | \
	  sed -e 's|^\.||' -e '/^$$/d' | \
	  awk '{ printf "sif \"%s\" uid 0\nsif \"%s\" gid 0\n", $$0, $$0 }' > ../root.ext4.own
	@$(DEBUGFS) -w -f $(BUILD_DIR)/root.ext4.own $(BUILD_DIR)/root.ext4 >/dev/null 2>&1 || true
	@rm -f $(BUILD_DIR)/root.ext4.own
	@$(DEBUGFS) -w -R "sif /bin/m31_setuid uid 0" $(BUILD_DIR)/root.ext4 2>/dev/null || true
	@$(DEBUGFS) -w -R "sif /bin/m31_setuid mode 0104755" $(BUILD_DIR)/root.ext4 2>/dev/null || true
	@# M108: /bin/{su,passwd,login} are symlinks onto the BusyBox multicall ELF
	@# now, so the setuid bit belongs on the inode they resolve to — the
	@# dedicated busybox-suid copy — and NOT on the symlinks (stamping a mode on
	@# a symlink inode would only corrupt it). The plain /opt/busybox/bin/busybox
	@# that every other applet resolves to is deliberately left non-setuid.
	@$(DEBUGFS) -w -R "sif /opt/busybox/bin/busybox-suid uid 0" $(BUILD_DIR)/root.ext4 2>/dev/null || true
	@$(DEBUGFS) -w -R "sif /opt/busybox/bin/busybox-suid gid 0" $(BUILD_DIR)/root.ext4 2>/dev/null || true
	@$(DEBUGFS) -w -R "sif /opt/busybox/bin/busybox-suid mode 0104755" $(BUILD_DIR)/root.ext4 2>/dev/null || true
	@$(DEBUGFS) -w -R "sif /sbin/unix_chkpwd uid 0" $(BUILD_DIR)/root.ext4 2>/dev/null || true
	@$(DEBUGFS) -w -R "sif /sbin/unix_chkpwd gid 0" $(BUILD_DIR)/root.ext4 2>/dev/null || true
	@$(DEBUGFS) -w -R "sif /sbin/unix_chkpwd mode 0104755" $(BUILD_DIR)/root.ext4 2>/dev/null || true
	@$(DEBUGFS) -w -R "sif /etc/shadow uid 0" $(BUILD_DIR)/root.ext4 2>/dev/null || true
	@$(DEBUGFS) -w -R "sif /etc/shadow mode 0100400" $(BUILD_DIR)/root.ext4 2>/dev/null || true
	@printf 'created %s (%s)\n' "$(BUILD_DIR)/root.ext4" "$$(du -sh $(BUILD_DIR)/root.ext4 | cut -f1)"

# Everything in the rootfs links dynamically against /lib/libc.so. This gate
# fails the build on a statically linked executable that is not listed (with a
# reason) in tools/configs/static-allowlist.txt — so a `-static` slipped into a
# port script breaks the ISO instead of quietly shipping a second copy of libc.
check-dynamic:
	@sh tools/check-dynamic.sh $(BUILD_DIR)/rootfs
	@sh tools/check-rootfs-links.sh $(BUILD_DIR)/rootfs

check-ports:
	@leaked=0; \
	for pc in $$(find -L build -path '*/install/lib/pkgconfig/*.pc' 2>/dev/null); do \
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
	sh tools/inguest/clang-proof.sh

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
