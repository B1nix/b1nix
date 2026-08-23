# B1NIX Userspace Makefile
#
# Builds the userspace example/smoke ELFs and GUI archive against musl.
# All binaries are ELF executables loadable by the B1NIX kernel.

B1NIX_ARCH ?= x86_64
ifeq ($(B1NIX_ARCH),aarch64)
TARGET   := aarch64-unknown-elf
LDFLAGS  := -m aarch64elf
B1NIX_TRIPLET := aarch64-b1nix
FLOAT_FLAGS :=
else
TARGET   := x86_64-unknown-elf
LDFLAGS  := -m elf_x86_64
B1NIX_TRIPLET := x86_64-b1nix
FLOAT_FLAGS := -msoft-float -mno-implicit-float
endif
# Prefix ccache when present so userspace ELFs recompile from cache after a
# `make clean` (explicit `make CC=...` still wins — command-line beats `:=`).
# Apple's /usr/bin/clang only implements the Darwin link driver and its
# `-print-resource-dir` has no lib/linux/ subdirectory (see build-llvm-
# runtimes.sh for the fuller writeup) — prefer the real Homebrew LLVM clang
# so BUILTINS_LIB below actually resolves to a usable compiler-rt archive.
CLANG_BIN := $(shell command -v /opt/homebrew/opt/llvm/bin/clang 2>/dev/null || command -v clang 2>/dev/null || echo clang)
CCACHE   := $(shell command -v ccache 2>/dev/null)
CC       := $(if $(CCACHE),$(CCACHE) $(CLANG_BIN),$(CLANG_BIN))
LD       := $(shell command -v ld.lld 2>/dev/null || echo /opt/homebrew/bin/ld.lld)
AR       := $(shell command -v llvm-ar 2>/dev/null || command -v /opt/homebrew/opt/llvm/bin/llvm-ar 2>/dev/null || command -v ar 2>/dev/null || echo ar)
LINK ?= musl
HOST_CC  ?= cc
B1NIX_CXX ?= ../tools/toolchain/bin/b1nix-c++
# The on-device b1cc bundle is compiled against musl (the old in-house
# libc + its static b1nix-cc/crt0.o path were removed in the M92 migration).
MUSL_CC := ../tools/b1nix-musl-cc
B1CC_BINS_CC := $(realpath ../tools/b1nix-musl-cc)
# b1cc otherwise locates the target headers through cwd-relative guesses, which
# only resolve when it is invoked from the repository root. Hand it the sysroot
# explicitly so every invocation works from any directory.
# b1cc's own C library headers.
#
# These used to be userspace/include -- a full libc header tree sitting beside
# the tests, which made it look like a second libc we maintained. It is not:
# every binary here compiles against musl's headers (see CFLAGS below), and
# this tree exists solely because b1cc's parser cannot read musl's. Deleting
# it as a duplicate broke b1cc on its own corpus, with the parser stopping on
# a musl typedef -- so it lives with the compiler that needs it, the way tcc
# ships its own include directory, and userspace/include is left holding only
# what describes this kernel's interfaces.
B1CC_SYSROOT_INCLUDE := $(realpath include)
B1CC_DIR := b1cc
B1CC := $(B1CC_DIR)/build/b1cc
# b1cc's sources live one directory deep (frontend/, ir/, backend/, linker/,
# utils/) since its M38 reorganisation, so the glob has to descend. A flat
# b1cc/src/*.c matches only b1cc.c and would link a compiler with no parser.
B1CC_SRC_GLOB := b1cc/src/*.c b1cc/src/*/*.c
# ...and each of those directories carries its own headers, included by name
# rather than by path. Same set b1cc's own Makefile passes — twice, because the
# self-host compile runs from here and the host build runs from inside b1cc/.
B1CC_INC_DIRS := src src/frontend src/ir src/backend src/linker src/utils
B1CC_INCS := $(addprefix -Ib1cc/,$(B1CC_INC_DIRS))
B1CC_INCS_HOST := $(addprefix -I,$(B1CC_INC_DIRS))
B1CC_SELF_SRCS := $(filter-out b1cc/src/b1cc_token_lexer.c, $(wildcard $(B1CC_SRC_GLOB)))
B1CC_BINS := b1cc_argv b1cc_better_c b1cc_file_write b1cc_hello b1cc_m34 b1cc_m34_corpus b1cc_stderr_exit
LIBS_DIR := ../build/$(B1NIX_ARCH)/libs
# ── musl paths (must be before CFLAGS so $(MUSL_INCLUDE) expands) ───────
MUSL_INSTALL := ../build/$(B1NIX_ARCH)/ports/musl/install
MUSL_INCLUDE := $(MUSL_INSTALL)/include
MUSL_LIB     := $(MUSL_INSTALL)/lib
# libm is musl's built-in stub: all math symbols live in libc.so; the .a is an
# empty compatibility archive produced by build-musl.sh. openlibm is gone.
LIBM_DIR := $(MUSL_INSTALL)
# The blob that -lc below actually resolves against, so the dependency edge and
# the link input name the same file.
MUSL_LIBSO   := $(MUSL_LIB)/libc.so
MUSL_CRT1    := $(MUSL_LIB)/Scrt1.o
MUSL_CRTI    := $(MUSL_LIB)/crti.o
MUSL_CRTN    := $(MUSL_LIB)/crtn.o
LIBCXX_INCLUDE := $(firstword $(wildcard \
  ../build/$(B1NIX_ARCH)/ports/musl/install/include/c++/v1 \
  ../build/$(B1NIX_ARCH)/toolchain/llvm-runtimes-build-musl/libcxx-install/include/c++/v1 \
  ../build/$(B1NIX_ARCH)/toolchain/$(B1NIX_TRIPLET)/llvm-runtimes-build-musl/libcxx-install/include/c++/v1))
# compiler-rt builtins: the helpers the compiler emits calls to but no library
# defines (128-bit division __udivti3 and friends). Linking through ld.lld
# directly means nothing supplies them implicitly, so name the archive.
# `clang -print-resource-dir` used to pin this to whichever compiler is in
# use, but that only works when the compiler's own resource dir actually
# ships a builtins archive for our *target* — Homebrew's LLVM resource dir
# ships none at all (it's built for compiling for the host, arm64-apple-
# darwin, not our x86_64-linux cross target). Use our own cross-built
# compiler-rt directly instead (see tools/toolchain/build-llvm-runtimes.sh).
BUILTINS_LIB  := $(firstword $(wildcard \
                   ../build/$(B1NIX_ARCH)/compiler-rt-builtins/libclang_rt.builtins-$(B1NIX_ARCH).a \
                   ../build/$(B1NIX_ARCH)/toolchain/sysroot/usr/lib/libcompiler_rt.a \
                   ../build/$(B1NIX_ARCH)/toolchain/llvm-runtimes-build/install/lib/libcompiler_rt.a \
                   ../build/$(B1NIX_ARCH)/toolchain/cross/$(B1NIX_TRIPLET)/lib/libcompiler_rt.a \
                   $(MUSL_LIB)/libclang_rt.builtins-$(B1NIX_ARCH).a \
                   $(MUSL_LIB)/libcompiler_rt.a \
                   $(CLANG_RES_DIR)/lib/*/libclang_rt.builtins-$(B1NIX_ARCH).a \
                   $(CLANG_RES_DIR)/lib/libclang_rt.builtins-$(B1NIX_ARCH).a))

# NOTE: -MD (not -MMD). Our headers live under `include/`, pulled in via
# `-isystem include`, which makes them *system* headers — and -MMD omits system
# headers from the generated .d files. With -MMD, editing a header in
# userspace/include/ would NOT trigger a recompile (observed: a struct-size
# change to <unistd.h> left a stale statvfs.o, crashing BusyBox df on i686).
# -MD records every header so header edits rebuild the affected objects.
CFLAGS   := --target=$(TARGET) -ffreestanding -fno-builtin -fno-stack-protector -nostdinc -isystem include -I include -Wall -Wextra -O2 $(FLOAT_FLAGS) -Db1nix -fno-pic -fno-pie -fdirect-access-external-data -MD -MP
# When LINK=musl, add musl headers (preferred for standard POSIX) and
# -D__linux__ so musl's internal arch detection works. b1nix-specific headers
# (<b1nix/syscall.h> etc.) stay via the original -I include as fallback.
ifeq ($(LINK),musl)
# musl headers FIRST (-isystem) so they win over b1nix's matching <errno.h> etc.
# b1nix-specific headers (<b1nix/syscall.h>) still resolve via -I include fallback.
CFLAGS := -isystem $(MUSL_INCLUDE) -D__linux__ $(filter-out -fno-pic -fno-pie -fdirect-access-external-data,$(CFLAGS)) -fPIC -D__b1nix__ -D_GNU_SOURCE
endif
BUILD    := build/$(B1NIX_ARCH)
ROOTFS   := ../build/$(B1NIX_ARCH)/rootfs
# Copy into the staging root only where the bytes differ. A plain cp stamps a
# fresh mtime on identical content, and the root image is repacked whenever
# anything in the tree is newer than it -- so unconditional copies here cost a
# full dd + mke2fs on every build, including the ones that changed nothing.
CIC      := ../tools/copy-if-changed.sh
# The account files are built by a chain of appends; assembling them here and
# copying the result keeps that shape while still leaving the image alone when
# it comes out identical.
ETCSTAGE := ../build/$(B1NIX_ARCH)/.etc-stage
# Strong utmp/utmpx overrides for musl's weak no-op stubs (real login accounting).
UTMP_OBJ  := $(BUILD)/compat/utmp.o
CRYPT_OBJ := $(BUILD)/compat/crypt.o
# No link script for the smoke binaries: they are musl PIEs, so the loader picks
# the base (randomized under b1nix.aslr) instead of a fixed address baked into a
# script — GEN_LINKER/BESPOKE_LINKER stay empty, see the -T guards below.
# linker.ld itself is still used by the tools/toolchain/bin/b1nix-*cc wrappers
# for legacy static links.

# ── Link model ────────────────────────────────────────────────────────────
# LINK=musl (default): real userspace ld.so — links against musl's libc.so
# through /lib/ld-musl-x86_64.so.1. The kernel loads the interpreter's own
# segments and jumps to its entry point; musl's ld.so links the binary itself.
# LINK=static: fully static ET_EXEC linking (historical fallback, b1cc + a few
# freestanding ABI tests).

# ── musl paths (shared across musl/dynamic link models) ──────────────────
MUSL_INSTALL := ../build/$(B1NIX_ARCH)/ports/musl/install
MUSL_INCLUDE := $(MUSL_INSTALL)/include
MUSL_LIB     := $(MUSL_INSTALL)/lib
# The blob that -lc below actually resolves against, so the dependency edge and
# the link input name the same file.
MUSL_LIBSO   := $(MUSL_LIB)/libc.so
MUSL_CRT1    := $(MUSL_LIB)/Scrt1.o
MUSL_CRTI    := $(MUSL_LIB)/crti.o
MUSL_CRTN    := $(MUSL_LIB)/crtn.o

# Generic base programs: PIE against musl through /lib/ld-musl-x86_64.so.1. No
# linker script — a real ld.so needs the ELF header and program headers inside
# the first PT_LOAD so AT_PHDR (load_base + e_phoff) resolves to genuine program
# headers, which the linker's default layout produces.
GEN_PIC        := -fPIC
GEN_CRT0       := $(MUSL_CRT1)
GEN_LINKER     :=
GEN_LIBDEP     := $(MUSL_LIBSO) $(UTMP_OBJ) $(CRYPT_OBJ)
GEN_LINK_EXTRA := -pie -z norelro --hash-style=sysv --allow-shlib-undefined \
                  --dynamic-linker /lib/ld-musl-$(B1NIX_ARCH).so.1 \
                  -L$(MUSL_LIB)
GEN_LINK_LIBS  := $(MUSL_CRTI) $(UTMP_OBJ) $(CRYPT_OBJ) -lc $(MUSL_CRTN) $(BUILTINS_LIB)
GEN_MUSL_POSTLINK := 1

# ── Post-link: EI_OSABI = ELFOSABI_LINUX (3) ─────────────────────────────
# The kernel decides which syscall ABI a program gets from EI_OSABI, and every
# binary here talks the Linux ABI (musl's arch_prctl, set_tid_address, clone3).
# ld.lld has no flag for the byte, so we stamp it. Same thing the loader itself
# gets in the top-level Makefile and the same thing tools/b1nix-musl-cc does.
#
# Each call site used to carry a copy of this that ALSO rewrote the DT_NEEDED
# string in .dynstr from "ld-musl-x86_64.so.1" to "libc.so". That rewrite was a
# workaround for libc.so having no DT_SONAME, which made lld record the file
# name we happened to link through. musl's real soname is set at build time now
# (tools/ports/build-musl.sh), so the linker writes "libc.so" itself and there
# is nothing left to rewrite — patching the bytes of a produced ELF should be
# the last resort it sounds like, not the routine step it had become.
#
# No `|| true`: if the stamp cannot be written, the binary that ships is one the
# kernel will run under the wrong ABI, which is not something to pass over.
define stamp_linux_osabi
python3 -c "import sys; fh=open(sys.argv[1],'r+b'); fh.seek(7); fh.write(bytes([3]))" $(1)
endef

# Bespoke ported-library / GUI binaries: non-PIE dynamic ET_EXEC against musl.
# Same shape as the generic musl link (PIC/PIE, no linker script) so the program
# headers stay inside the first PT_LOAD where AT_PHDR can find them.
BESPOKE_CRT0   := $(MUSL_CRT1)
BESPOKE_CXX_EXTRA := --eh-frame-hdr
BESPOKE_EXTRA  := -pie -z norelro --hash-style=sysv --allow-shlib-undefined \
                  --dynamic-linker /lib/ld-musl-$(B1NIX_ARCH).so.1 \
                  -L$(MUSL_LIB)
BESPOKE_LIBC   := $(MUSL_CRTI) $(UTMP_OBJ) $(CRYPT_OBJ) -lc $(MUSL_CRTN) $(BUILTINS_LIB)
BESPOKE_LIBC_NOPAM := $(MUSL_CRTI) $(UTMP_OBJ) $(CRYPT_OBJ) -lc $(MUSL_CRTN) $(BUILTINS_LIB)
BESPOKE_LIBCXX_EXTRA := $(BUILD)/libcxx_compat.o
BESPOKE_LINKER :=
BESPOKE_LIBDEP := $(MUSL_LIBSO) $(MUSL_CRT1) $(UTMP_OBJ) $(CRYPT_OBJ)
BESPOKE_LIBDEP_NOPAM := $(MUSL_LIBSO) $(MUSL_CRT1) $(UTMP_OBJ) $(CRYPT_OBJ)
BESPOKE_PIC    := -fPIC
BESPOKE_MUSL_POSTLINK := 1
# Same reasoning as CLANG_BIN/CC above: prefer the real Homebrew LLVM clang++.
CLANGXX_BIN    := $(shell command -v /opt/homebrew/opt/llvm/bin/clang++ 2>/dev/null || command -v clang++ 2>/dev/null || echo clang++)
CXX            := $(if $(CCACHE),$(CCACHE) $(CLANGXX_BIN),$(CLANGXX_BIN))
CXXFLAGS       := --target=$(TARGET) -ffreestanding -nostdinc++ \
                  -isystem $(LIBCXX_INCLUDE) -nostdinc -isystem $(MUSL_INCLUDE) \
                  -isystem $(CLANG_RES_DIR)/include -idirafter include -Wall -Wextra -O2 $(FLOAT_FLAGS) -Db1nix \
                  -fPIC -MD -MP -D__linux__ -D__b1nix__ -D_GNU_SOURCE



# The M53 checks link ported third-party libraries, and those libraries are
# built by a separate stage that itself needs a working userspace — so a tree
# whose ports have not been built yet cannot build these, and a hard reference
# deadlocks the two stages against each other. That is not hypothetical: after
# the build directory was cleaned, `make` could not produce a userspace because
# the ports were missing, and the ports could not be produced because they need
# a userspace.
#
# Build each one only when the library it links is actually present. A missing
# port then means one fewer check rather than a build that cannot start, and
# once the ports stage has run the full set comes back on its own.
PORTS_ROOT := ../build/$(B1NIX_ARCH)/ports
M53_PORT_SMOKES := $(BUILD)/bin/m53_zlib_smoke $(BUILD)/bin/m53_libpng_smoke \
	$(BUILD)/bin/m53_libjpeg_smoke $(BUILD)/bin/m53_libwebp_smoke \
	$(BUILD)/bin/m53_libvpx_smoke
ifneq ($(B1NIX_ARCH),aarch64)
M53_PORT_SMOKES += $(BUILD)/bin/m53_virgl_smoke
endif
ifneq ($(wildcard $(PORTS_ROOT)/libwapcaplet/install/include),)
M53_PORT_SMOKES += $(BUILD)/bin/m53_wapcaplet_smoke
endif
ifneq ($(wildcard $(PORTS_ROOT)/libparserutils/install/include),)
M53_PORT_SMOKES += $(BUILD)/bin/m53_parserutils_smoke
endif
ifneq ($(wildcard $(PORTS_ROOT)/libhubbub/install/include),)
M53_PORT_SMOKES += $(BUILD)/bin/m53_hubbub_smoke
endif
ifneq ($(wildcard $(PORTS_ROOT)/libcss/install/include),)
M53_PORT_SMOKES += $(BUILD)/bin/m53_libcss_smoke
endif
ifneq ($(wildcard $(PORTS_ROOT)/libdom/install/include),)
M53_PORT_SMOKES += $(BUILD)/bin/m53_libdom_smoke $(BUILD)/bin/m53_nslibs_smoke
endif

BINARIES := \
	$(BUILD)/bin/hello \
	$(BUILD)/bin/native_smoke \
	$(BUILD)/bin/m12_smoke \
	$(BUILD)/bin/m13_smoke \
	$(BUILD)/bin/m13_job_control \
	$(BUILD)/bin/m14_smoke \
	$(BUILD)/bin/m15_smoke \
	$(BUILD)/bin/m17_smoke \
	$(BUILD)/bin/m56_smoke \
	$(BUILD)/bin/m8_aio_test \
	$(BUILD)/bin/test_sync \
	$(BUILD)/bin/m25_smoke \
	$(BUILD)/bin/m26_smoke \
	$(BUILD)/bin/selfhost_build \
	$(BUILD)/bin/m24b_smoke \
	$(BUILD)/bin/m27_smoke \
	$(BUILD)/bin/m29_smoke \
	$(BUILD)/bin/m31_smoke \
	$(BUILD)/bin/m31_setuid \
	$(BUILD)/bin/m32_smoke \
	$(BUILD)/bin/m32_nettool \
	$(BUILD)/bin/m32_pcre2_smoke \
	$(M53_PORT_SMOKES) \
	$(BUILD)/bin/m34_smoke \
	$(BUILD)/bin/m35_smoke \
	$(BUILD)/bin/m36_smoke \
	$(BUILD)/bin/diskbench \
	$(BUILD)/bin/mdcreate \
	$(BUILD)/bin/telinit \
	$(BUILD)/bin/m30_pie \
	$(BUILD)/bin/m38_sound \
	$(BUILD)/bin/m79_sound \
	$(BUILD)/bin/m39_smoke \
	$(BUILD)/bin/m77_smoke \
	$(BUILD)/bin/m47_smoke
# The soak stressors. Ordinary programs in the image rather than smoke
# binaries: the overnight loop runs them directly, and having them installed
# means a failure can be reproduced by hand from a shell on a running guest.
BINARIES += $(BUILD)/bin/memstress
BINARIES += $(BUILD)/bin/vmstress
BINARIES += $(BUILD)/bin/cpustress
BINARIES += $(BUILD)/bin/diskstress
BINARIES += $(BUILD)/bin/netstress
BINARIES += $(BUILD)/bin/fdstress
BINARIES += $(BUILD)/bin/spawnstress
BINARIES += $(BUILD)/bin/shmstress
BINARIES += $(BUILD)/bin/timerstress
BINARIES += $(BUILD)/bin/m48_smoke
# Same reasoning as M53_PORT_SMOKES above: these link ported libraries (wayland
# and libffi) that a cleaned tree has not built yet, and a hard reference makes
# the userspace unbuildable until the ports stage has run — which cannot run
# without a userspace.
ifneq ($(wildcard ../build/$(B1NIX_ARCH)/pkg/libffi/lib),)
BINARIES += $(BUILD)/bin/m49_libwayland
BINARIES += $(BUILD)/bin/m49_libwayland_server
endif
BINARIES += $(BUILD)/bin/m50_smoke
# M101t: driven against the *pinned* upstream uapi headers out of the staged
# import tree, not a copy kept here. A copy is how a userspace ABI drifts from
# the kernel it talks to without anything failing to build.
BINARIES += $(BUILD)/bin/m101t_smoke
# -D__user= is what `make headers_install` does when it sanitises a uapi header
# for userspace: the annotation is a sparse marker with no meaning outside the
# kernel, and the raw staged tree has not been through that step.
$(BUILD)/bin/m101t_smoke: CFLAGS += -I ../build/src/drm-core-6.6/include/uapi \
	-D__user=
BINARIES += $(BUILD)/bin/m51_smoke
BINARIES += $(BUILD)/bin/m51_pixman_smoke
BINARIES += $(BUILD)/bin/m51_freetype_smoke
BINARIES += $(BUILD)/bin/m51_cairo_smoke
BINARIES += $(BUILD)/bin/m51_xkb_smoke
BINARIES += $(BUILD)/bin/m51_harfbuzz_smoke
BINARIES += $(BUILD)/bin/m51_fontconfig_smoke
# Reaches its libraries through dlopen, so it needs no headers and links like
# any other program — see the note at the top of bin/gfx/font_probe.c.
BINARIES += $(BUILD)/bin/font_probe
# M102a: the same dlopen trick for EGL/GLES — it renders off-screen on the
# passed-through GPU and checks the pixels it gets back, so it needs neither GL
# headers at build time nor a compositor at run time.
BINARIES += $(BUILD)/bin/gl_probe
# The pixel side of the compositor tests: reads back the PPM grim pulled out of
# a running compositor and checks it really carries the colour that was asked
# for. "grim exited 0" says a buffer arrived, not what is in it.
BINARIES += $(BUILD)/bin/framecheck
BINARIES += $(BUILD)/bin/cxx_smoke
BINARIES += $(BUILD)/bin/m55_iostream
# M57 (Mojo IPC core) and M75 are plain POSIX C over the libc.
BINARIES += $(BUILD)/bin/m57_smoke
BINARIES += $(BUILD)/bin/m75_smoke
# M108: BusyBox su/passwd/init smoke — links libpam (bespoke rule). The PAM
# package is an Alpine one and installs for both arches, so this is no longer
# arch-gated.
BINARIES += $(BUILD)/bin/m108_smoke
# M108: the login shell of the m108user account (an ELF, so su's login argv[0]
# survives — see bin/helpers/m108shell.c).
BINARIES += $(BUILD)/bin/m108shell
# M80's three x86 pieces have aarch64 equivalents now (getpid's ABI number,
# FPCR for MXCSR, q0 for ymm0) and all 25 of its checks pass here.
BINARIES += $(BUILD)/bin/m80_smoke

ifneq ($(B1NIX_ARCH),aarch64)
# Native clang runs inside the guest and is only built for x86_64.
BINARIES += $(BUILD)/bin/m64_clang_smoke
endif
# Kernel-feature smoke tests that need no ported library — they exercise
# ptrace, seccomp, loadable modules, netlink/loop/inotify, the VFS audit
# and per-thread accounting, so they belong on every arch, not just the
# one they were first written for.
BINARIES += $(BUILD)/bin/m86_smoke
BINARIES += $(BUILD)/bin/m95_smoke
BINARIES += $(BUILD)/bin/m96_smoke
BINARIES += $(BUILD)/bin/m107_smoke
# M109: AF_PACKET, pivot_root(2), volume identity, and UTS/mount namespaces.
# M109: the merged /dev listing, findfs/blkid, and mount(MS_MOVE).
# M109: inode attribute flags (chattr/lsattr), block discard (blkdiscard,
# fstrim), I/O priorities (ionice) and the serial line's hardware settings.
BINARIES += $(BUILD)/bin/m109_smoke
# M109: the uevent hot-plug channel - netlink uevents, /sys/dev/block, mdev.
BINARIES += $(BUILD)/bin/m109_uevent_smoke
# M98: GNU-free build tools (bmake as /bin/make, samurai as /bin/samu|ninja).
BINARIES += $(BUILD)/bin/m98_smoke
BINARIES += $(BUILD)/bin/m100_smoke
BINARIES += $(BUILD)/bin/m63_smoke
BINARIES += $(BUILD)/bin/m42_w5pre_smoke
BINARIES += $(BUILD)/bin/m46_smoke
BINARIES += $(BUILD)/bin/m73_smoke
# M104: PAM smoke — links libpam. The smallest program that makes ld.so load a
# SECOND shared library, which is what used to fail on aarch64.
BINARIES += $(BUILD)/bin/m104_pam_smoke
BINARIES += $(BUILD)/bin/m71_aslr
BINARIES += $(BUILD)/bin/lock_smoke
BINARIES += $(BUILD)/bin/futex_stress
BINARIES += $(BUILD)/bin/invite_stress
BINARIES += $(BUILD)/bin/pipe_eof
BINARIES += $(BUILD)/bin/fd_visibility
BINARIES += $(BUILD)/bin/clock_sanity
BINARIES += $(BUILD)/bin/yield_fairness
BINARIES += $(BUILD)/bin/ext_stress
BINARIES += $(BUILD)/bin/m24_stress
BINARIES += $(BUILD)/bin/m27_cmdline
BINARIES += $(BUILD)/bin/m11_pipe_smoke
BINARIES += $(BUILD)/bin/m22_smoke
BINARIES += $(BUILD)/bin/net_smoke
BINARIES += $(BUILD)/bin/m51_clipboard_smoke
BINARIES += $(BUILD)/bin/b1fetch
BINARIES += $(BUILD)/bin/gpuinfo
BINARIES += $(BUILD)/bin/mc
BINARIES += $(BUILD)/bin/ne
# M16 driver: runs mc/ne out-of-process and covers the rest of the TUI surface.
BINARIES += $(BUILD)/bin/m16_smoke
# POSIX memory/signal primitives: madvise, MAP_NORESERVE, sigaltstack
BINARIES += $(BUILD)/bin/m_posixmm_smoke
# Is a shared mapping actually shared between two processes? Every Wayland
# client depends on it and nothing else asserts it.
BINARIES += $(BUILD)/bin/shmshare_smoke
# M104: native package manager (Alpine APKINDEX/.apk + house flat-index repos).
# Its smoke coverage is the shell-driven /etc/bpkg-smoke.sh (see rootfs-
# overlay/etc/rc), matching the existing posix-smoke.sh/zsh-smoke.sh pattern,
# not a dedicated ELF -- bpkg's CLI already speaks a stable, testable
# interface (update/install/list/remove) so there's nothing an in-process
# smoke binary would exercise that the shell script doesn't.
BINARIES += $(BUILD)/bin/bpkg
# M92: native ABI smoke test (raw b1nix syscalls, no libc)
BINARIES += $(BUILD)/bin/m92_abi_smoke
# M92: musl POSIX smoke test (built with LINK=musl)
BINARIES += $(BUILD)/bin/musl_posix_smoke
# b1cc (standalone "better C" compiler + its M5/M7/M32-M34 smoke binaries).
# The corpus binaries are produced *by* b1cc, so they can only be built where
# b1cc has a code generator for this target. Ask b1cc itself rather than
# testing the arch name: the moment its aarch64 backend lands, these build
# again with no edit here.
# The arm64 backend still emits Mach-O assembly syntax (.cstring, @PAGE/@PAGEOFF,
# leading underscores), which the ELF assembler rejects — so probe all the way
# through assembly, not just code generation. Re-enables itself once the backend
# grows an ELF flavour.
CROSS_AS_PROBE := clang --target=$(B1NIX_ARCH)-unknown-elf -c
B1CC_TARGET_OK := $(shell [ -x $(B1CC) ] && printf 'int g;const char*s="x";int main(void){g=1;return s[0]+g;}\n' > $(BUILD)/.b1cc_probe.c && \
  $(B1CC) --target=$(B1NIX_ARCH)-b1nix -S $(BUILD)/.b1cc_probe.c -o $(BUILD)/.b1cc_probe.s >/dev/null 2>&1 && \
  $(CROSS_AS_PROBE) $(BUILD)/.b1cc_probe.s -o $(BUILD)/.b1cc_probe.o >/dev/null 2>&1 && echo yes)
ifeq ($(B1CC_TARGET_OK),yes)
BINARIES += $(addprefix $(BUILD)/bin/,$(B1CC_BINS)) $(BUILD)/bin/b1cc $(BUILD)/bin/b1cc_selfsmoke
else
$(info b1cc: cannot yet produce $(B1NIX_ARCH)-b1nix objects (arm64 codegen works; its assembly is still Mach-O syntax) — M5/M7/M32-M34 b1cc corpus not built)
endif

ifeq ($(B1NIX_ARCH),x86_64)
BINARIES += $(BUILD)/bin/m30_dynamic
endif

# the generic single-source BIN_OBJS pattern.
BIN_OBJS := $(patsubst $(BUILD)/bin/%,$(BUILD)/bin/%.o,$(filter-out $(BUILD)/bin/native_smoke $(BUILD)/bin/m30_pie $(BUILD)/bin/m30_dynamic $(BUILD)/bin/b1cc $(BUILD)/bin/b1cc_selfsmoke $(addprefix $(BUILD)/bin/,$(B1CC_BINS)) $(BUILD)/bin/mc $(BUILD)/bin/ne $(BUILD)/bin/m16_smoke $(BUILD)/bin/m57_smoke $(BUILD)/bin/m104_pam_smoke $(BUILD)/bin/m108_smoke,$(BINARIES)))
DEPS := $(BIN_OBJS:.o=.d)

.PHONY: all clean install install-headers-libs
.SECONDARY:

# musl provides its own libc/crt; the b1nix-native libc/gui archives are not
# linked into the ELFs, so `all` just builds the binaries.
all: $(BINARIES)

# utmp/utmpx overrides for musl's weak stubs (real login accounting).
$(CRYPT_OBJ): compat/crypt.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GEN_PIC) -c $< -o $@

$(UTMP_OBJ): compat/utmp.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GEN_PIC) -c $< -o $@

build/bin/%: build/$(B1NIX_ARCH)/bin/%
	@mkdir -p build/bin
	ln -sf ../$(B1NIX_ARCH)/bin/$* $@



# ── User programs ──
# Base programs are PIE ELFs linked against musl through /lib/ld-musl-x86_64.so.1
# (the GEN_* variables near the top of this file).
# b1cc's own CFLAGS select -std=c23, which is strict ISO C: it hides the
# POSIX/BSD declarations src/common.c uses (mkstemps, mkdtemp), and recent clang
# rejects the resulting implicit declarations outright. Add _DEFAULT_SOURCE from
# this side — b1cc is a submodule, so overriding its (`?=`-assigned) CFLAGS on
# the sub-make command line keeps that checkout clean for check-b1cc-sync.
B1CC_HOST_CFLAGS ?= -std=c23 -D_DEFAULT_SOURCE -Wall -Wextra -O3 -flto -march=native $(B1CC_INCS_HOST)

$(B1CC): $(B1CC_DIR)/Makefile $(wildcard $(B1CC_SRC_GLOB))
	$(MAKE) -C $(B1CC_DIR) CC="$(HOST_CC)" CFLAGS="$(B1CC_HOST_CFLAGS)"

# ── Source layout ──
# bin/ is grouped by purpose so the directory can be navigated at a glance:
#   bin/smoke/     kernel + POSIX smoke tests (mNN_smoke.c)
#   bin/gfx/       graphics/browser-stack tests and their servers (M47-M53)
#   bin/helpers/   programs a test drives rather than a test itself (fixtures,
#                  target ELFs, tiny servers, the musl bring-up diagnostics)
#   bin/tools/     b1nix-specific utilities that ship in the image
#   bin/stress/    the soak stressors: bounded, self-verifying load generators
#                  driven by /etc/soak.sh (see tools/soak/)
#   bin/gui/       the GUI demo applications
#   bin/compiler/  the b1cc corpus
# Rules refer to a source through $(call bin_src,NAME) so a program can move
# between categories without touching a build rule.
BIN_CATS := smoke gfx helpers tools gui compiler stress
# The trailing literal matters: with no fallback an unknown stem yields an empty
# prerequisite, and a pattern rule with no prerequisites matches ANY target —
# make then tried to build the -MD dependency files through it (bin/foo.d.o).
bin_src = $(firstword $(wildcard $(addsuffix /$(1).c,$(addprefix bin/,$(BIN_CATS))) $(addsuffix /$(1).cpp,$(addprefix bin/,$(BIN_CATS)))) bin/$(1).c)
.SECONDEXPANSION:

# The host b1cc links through the external musl driver (b1nix-musl-cc → musl
# crt1.o + libc). B1CC_INTERNAL_LD must be UNSET to select the external linker:
# the host build treats the variable's mere presence (any value, including 0) as
# "use the internal b1nix-crt0/libb1nix.a linker", so leave it unset for musl.
$(addprefix $(BUILD)/bin/,$(B1CC_BINS)): $(B1CC)
$(addprefix $(BUILD)/bin/,$(B1CC_BINS)): $(BUILD)/bin/%: $$(call bin_src,$$*)
	@mkdir -p $(dir $@)
	B1NIX_CC=$(B1CC_BINS_CC) B1CC_SYSROOT_INCLUDE=$(B1CC_SYSROOT_INCLUDE) $(B1CC) --target=$(B1NIX_ARCH)-b1nix $< -o $@

# M32: /bin/b1cc — b1cc compiled to a B1NIX ELF by b1nix-cc. Custom multi-source
# rule (all of b1cc/src except the token-lexer variant); C23 needs -std=gnu2x.
# -Db1nix selects b1cc's native-target behaviour: it links with its own internal
# ELF linker and reads its CRT/archives from /lib/b1cc, instead of shelling out
# to a host `cc`/ld.lld that does not exist inside the guest. Without it the
# on-device compiler dies with "sh: cc: not found" on the first link.
# The guest b1cc builds with the full parallel pthread emit pool. The m25
# float-check wedge (17 emit threads spinning forever) was NOT a b1cc bug: it
# was the kernel's Linux-ABI futex dispatch returning -EINVAL for musl's
# FUTEX_WAIT/WAKE|FUTEX_PRIVATE (0x80/0x81) ops, so musl __wait/__lock never
# parked and busy-spun instead. That is fixed in the kernel LX futex dispatch
# (kernel/syscall/syscall.c) — do not reintroduce -DB1CC_SERIAL_EMIT here.
$(BUILD)/bin/b1cc: $(B1CC_SELF_SRCS)
	@mkdir -p $(dir $@)
	B1NIX_ARCH=$(B1NIX_ARCH) $(MUSL_CC) -std=gnu2x -Db1nix $(B1CC_INCS) $(B1CC_SELF_SRCS) -o $@

# M32: /bin/b1cc-selfsmoke — the on-device self-host driver, statically linked
# against musl's libc.a (the minimal initramfs ships no shared libc.so).
$(BUILD)/bin/b1cc_selfsmoke: bin/compiler/b1cc_selfsmoke.c $(MUSL_LIB)/libc.a $(MUSL_CRT1)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $(BUILD)/bin/b1cc_selfsmoke.o
	$(LD) $(LDFLAGS) -static -o $@ $(MUSL_CRT1) $(MUSL_CRTI) $(BUILD)/bin/b1cc_selfsmoke.o \
	  $(MUSL_LIB)/libc.a $(MUSL_CRTN) $(BUILTINS_LIB)
	$(call stamp_linux_osabi,$@)

# M109: the initramfs /init of the switchroot instance. Linked exactly like
# every other program here — a PIE against musl's shared libc through
# /lib/ld-musl-x86_64.so.1, both of which the initramfs already carries — so it
# gets a load base the loader picks rather than the fixed 2 MiB a static link
# lands on, which is inside the kernel's identity mapping. Deliberately not in
# BINARIES: it ships inside the kernel image, not in the rootfs.
$(BUILD)/bin/m109_switchroot_init: bin/helpers/m109_switchroot_init.c $(GEN_LIBDEP) $(GEN_CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GEN_PIC) -c $< -o $(BUILD)/bin/m109_switchroot_init.o
	$(LD) $(LDFLAGS) $(GEN_LINK_EXTRA) -o $@ $(GEN_CRT0) \
	  $(BUILD)/bin/m109_switchroot_init.o $(GEN_LINK_LIBS)
ifeq ($(GEN_MUSL_POSTLINK),1)
	$(call stamp_linux_osabi,$@)
endif

$(BUILD)/src/tui_common.o: src/tui_common.c include/tui.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GEN_PIC) -c $< -o $@

$(BUILD)/bin/mc: bin/tools/mc.c $(BUILD)/src/tui_common.o $(GEN_LIBDEP) $(GEN_CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GEN_PIC) -c bin/tools/mc.c -o $(BUILD)/bin/mc.o
	$(LD) $(LDFLAGS) $(GEN_LINK_EXTRA) -o $@ $(GEN_CRT0) $(BUILD)/bin/mc.o $(BUILD)/src/tui_common.o $(GEN_LINK_LIBS)
ifeq ($(GEN_MUSL_POSTLINK),1)
	$(call stamp_linux_osabi,$@)
endif

$(BUILD)/bin/ne: bin/tools/ne.c $(BUILD)/src/tui_common.o $(GEN_LIBDEP) $(GEN_CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GEN_PIC) -c bin/tools/ne.c -o $(BUILD)/bin/ne.o
	$(LD) $(LDFLAGS) $(GEN_LINK_EXTRA) -o $@ $(GEN_CRT0) $(BUILD)/bin/ne.o $(BUILD)/src/tui_common.o $(GEN_LINK_LIBS)
ifeq ($(GEN_MUSL_POSTLINK),1)
	$(call stamp_linux_osabi,$@)
endif

# M16 driver links tui_common.o for the shared key-sequence decoder it checks.
$(BUILD)/bin/m16_smoke: bin/smoke/m16_smoke.c $(BUILD)/src/tui_common.o $(GEN_LIBDEP) $(GEN_CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GEN_PIC) -c bin/smoke/m16_smoke.c -o $(BUILD)/bin/m16_smoke.o
	$(LD) $(LDFLAGS) $(GEN_LINK_EXTRA) -o $@ $(GEN_CRT0) $(BUILD)/bin/m16_smoke.o $(BUILD)/src/tui_common.o $(GEN_LINK_LIBS)
ifeq ($(GEN_MUSL_POSTLINK),1)
	$(call stamp_linux_osabi,$@)
endif

$(BUILD)/src/mojo_core.o: src/mojo_core.c include/mojo/mojo.h include/mojo.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GEN_PIC) -c $< -o $@

$(BUILD)/bin/m57_smoke: bin/smoke/m57_smoke.c $(BUILD)/src/mojo_core.o $(GEN_LIBDEP) $(GEN_CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GEN_PIC) -c bin/smoke/m57_smoke.c -o $(BUILD)/bin/m57_smoke.o
	$(LD) $(LDFLAGS) $(GEN_LINK_EXTRA) -o $@ $(GEN_CRT0) $(BUILD)/bin/m57_smoke.o $(BUILD)/src/mojo_core.o $(GEN_LINK_LIBS)
ifeq ($(GEN_MUSL_POSTLINK),1)
	$(call stamp_linux_osabi,$@)
endif



$(BUILD)/bin/%: $$(call bin_src,$$*) $(GEN_LIBDEP) $(GEN_CRT0) $(GEN_LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GEN_PIC) -I ../kernel/arch/x86_64 -c $< -o $(BUILD)/bin/$*.o
	$(LD) $(LDFLAGS) $(GEN_LINK_EXTRA) $(if $(GEN_LINKER),-T $(GEN_LINKER)) -o $@ $(GEN_CRT0) $(BUILD)/bin/$*.o $(GEN_LINK_LIBS)
ifeq ($(GEN_MUSL_POSTLINK),1)
	$(call stamp_linux_osabi,$@)
endif

MBEDTLS_DIR := ../build/$(B1NIX_ARCH)/pkg/mbedtls

# M104: bpkg gains HTTPS when the static mbedTLS archives are present. Every
# real package repository is HTTPS-only, so without TLS the package manager can
# only reach a local file:// index. The TLS code in bpkg.c is behind
# B1NIX_HAVE_MBEDTLS; a B1NIX_TLS=none build still produces a working
# HTTP/file-only bpkg that rejects https:// URLs with an explicit error.
$(BUILD)/bin/bpkg: bin/tools/bpkg.c $(GEN_LIBDEP) $(GEN_CRT0) $(GEN_LINKER) $(wildcard $(MBEDTLS_DIR)/lib/libmbed*.a)
	@mkdir -p $(dir $@)
	@if [ -f $(MBEDTLS_DIR)/lib/libmbedtls.a ]; then \
	  echo "  CC+TLS  bpkg"; \
	  $(CC) $(CFLAGS) $(GEN_PIC) -DB1NIX_HAVE_MBEDTLS -I $(MBEDTLS_DIR)/include -c $< -o $(BUILD)/bin/bpkg.o; \
	  $(LD) $(LDFLAGS) $(GEN_LINK_EXTRA) $(if $(GEN_LINKER),-T $(GEN_LINKER)) -o $@ $(GEN_CRT0) $(BUILD)/bin/bpkg.o \
	    --start-group \
	    $(MBEDTLS_DIR)/lib/libmbedtls.a $(MBEDTLS_DIR)/lib/libmbedx509.a $(MBEDTLS_DIR)/lib/libmbedcrypto.a \
	    $(GEN_LINK_LIBS) \
	    --end-group; \
	else \
	  echo "  CC      bpkg (HTTP/file only, no mbedTLS)"; \
	  $(CC) $(CFLAGS) $(GEN_PIC) -c $< -o $(BUILD)/bin/bpkg.o; \
	  $(LD) $(LDFLAGS) $(GEN_LINK_EXTRA) $(if $(GEN_LINKER),-T $(GEN_LINKER)) -o $@ $(GEN_CRT0) $(BUILD)/bin/bpkg.o $(GEN_LINK_LIBS); \
	fi
ifeq ($(GEN_MUSL_POSTLINK),1)
	$(call stamp_linux_osabi,$@)
endif

# M49: upstream libwayland, Alpine's shared build, with libffi for protocol
# dispatch. Shared rather than static on both counts: Alpine's libffi.a is not
# built -fPIC and a PIE cannot link it, and a library this size has no business
# being copied into every binary that speaks the protocol. The port also carried
# b1nix shims for eventfd and open_memstream that musl now provides outright,
# which is what makes the upstream binary usable unmodified.
WAYLAND_DIR := ../build/$(B1NIX_ARCH)/pkg/wayland
FFI_DIR := ../build/$(B1NIX_ARCH)/pkg/libffi
WAYLAND_LINK := -L$(WAYLAND_DIR)/lib -L$(FFI_DIR)/lib -rpath /lib
$(WAYLAND_DIR)/lib/libwayland-client.so: ../tools/packages/pkg-prefix.sh ../tools/packages/alpine-ports.map
	B1NIX_ARCH=$(B1NIX_ARCH) ../tools/packages/pkg-prefix.sh wayland >/dev/null
	B1NIX_ARCH=$(B1NIX_ARCH) ../tools/packages/pkg-prefix.sh libffi >/dev/null
$(WAYLAND_DIR)/lib/libwayland-server.so: $(WAYLAND_DIR)/lib/libwayland-client.so

$(BUILD)/bin/m49_libwayland: bin/gfx/m49_libwayland.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER)) \
		$(WAYLAND_DIR)/lib/libwayland-client.so
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(WAYLAND_DIR)/include -c $< -o $(BUILD)/bin/m49_libwayland.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m49_libwayland.o \
		$(WAYLAND_LINK) -lwayland-client -lffi \
		$(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  $(call stamp_linux_osabi,$@) \
	)

$(BUILD)/bin/m49_libwayland_server: bin/gfx/m49_libwayland_server.c $(BESPOKE_LIBDEP) \
		$(if $(BESPOKE_LINKER),$(BESPOKE_LINKER)) $(WAYLAND_DIR)/lib/libwayland-server.so
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(WAYLAND_DIR)/include -c $< -o $(BUILD)/bin/m49_libwayland_server.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m49_libwayland_server.o \
		$(WAYLAND_LINK) -lwayland-server -lffi \
		$(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  $(call stamp_linux_osabi,$@) \
	)

$(BUILD)/bin/native_smoke: bin/helpers/native_smoke.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $(BUILD)/bin/native_smoke.o
	$(LD) $(LDFLAGS) $(if $(LINKER),-T $(LINKER)) -o $@ $(BUILD)/bin/native_smoke.o
	python3 -c "import sys; f=sys.argv[1]; fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true

# M104: OpenPAM smoke — the one binary in this tree that links a THIRD-PARTY
# M104: Linux-PAM smoke. Links libpam.so.0 as DT_NEEDED.
PAM_DIR := ../build/$(B1NIX_ARCH)/pkg/pam
$(BUILD)/bin/m104_pam_smoke: bin/smoke/m104_pam_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER)) $(PAM_DIR)/lib/libpam.so
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(PAM_DIR)/include -c $< -o $(BUILD)/bin/m104_pam_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -L $(PAM_DIR)/lib -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m104_pam_smoke.o \
	  -lpam $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  $(call stamp_linux_osabi,$@) \
	)

# M108: BusyBox su/passwd/init smoke. Links libpam.so for the same reason
# m104_pam_smoke does — it has to ask the REAL pam_unix.so whether the password
# BusyBox's passwd applet wrote is accepted — so it needs the same bespoke rule
# rather than the generic single-source pattern.
$(BUILD)/bin/m108_smoke: bin/smoke/m108_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER)) $(PAM_DIR)/lib/libpam.so
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(PAM_DIR)/include -c $< -o $(BUILD)/bin/m108_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -L $(PAM_DIR)/lib -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m108_smoke.o \
	  -lpam $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  $(call stamp_linux_osabi,$@) \
	)

# m32_nettool gains an mbedTLS loopback HTTPS server when the static mbedTLS
# archives are present (built by tools/ports/build-curl.sh with B1NIX_TLS=mbedtls).
# Without them it builds HTTP-only so a `B1NIX_TLS=none` build still works.
$(BUILD)/bin/m32_nettool: bin/helpers/m32_nettool.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER)) $(wildcard $(MBEDTLS_DIR)/lib/libmbed*.a)
	@mkdir -p $(dir $@)
	@if [ -f $(MBEDTLS_DIR)/lib/libmbedtls.a ]; then \
	  echo "  CC+TLS  m32_nettool"; \
	  $(CC) $(CFLAGS) $(BESPOKE_PIC) -DB1NIX_HAVE_MBEDTLS -I $(MBEDTLS_DIR)/include -c $< -o $(BUILD)/bin/m32_nettool.o; \
	  $(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m32_nettool.o \
	    --start-group \
	    $(MBEDTLS_DIR)/lib/libmbedtls.a $(MBEDTLS_DIR)/lib/libmbedx509.a $(MBEDTLS_DIR)/lib/libmbedcrypto.a \
	    $(BESPOKE_LIBC) \
	    --end-group; \
	else \
	  echo "  CC      m32_nettool (HTTP-only, no mbedTLS)"; \
	  $(CC) $(CFLAGS) $(BESPOKE_PIC) -c $< -o $(BUILD)/bin/m32_nettool.o; \
	  $(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m32_nettool.o $(BESPOKE_LIBC); \
	fi
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  $(call stamp_linux_osabi,$@) \
	)
	ln -sf m32_nettool $(BUILD)/bin/m32-nettool


# M51: m51_smoke links against musl's libm stub (math symbols are in libc.so).
$(BUILD)/bin/m51_smoke: bin/gfx/m51_smoke.c $(BESPOKE_LIBDEP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -c $< -o $(BUILD)/bin/m51_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_smoke.o \
	  $(LIBM_DIR)/lib/libm.a $(BESPOKE_LIBC)

# M51: the desktop/text libraries are Alpine packages, linked shared.
#
# Shared and not static because Alpine builds several of them with GCC's slim
# LTO: the .a holds intermediate representation and no machine code, so ld.lld
# reports every symbol undefined with the archive right there on the command
# line. tools/packages/alpine-fetch.sh deletes those archives so the shared
# library is the only thing left to find. Their headers also sit one directory
# down (freetype2, harfbuzz, pixman-1), which is upstream's layout.
PKG_RPATH := -rpath /lib
PIXMAN_DIR := ../build/$(B1NIX_ARCH)/pkg/pixman
$(BUILD)/bin/m51_pixman_smoke: bin/gfx/m51_pixman_smoke.c $(BESPOKE_LIBDEP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(PIXMAN_DIR)/include/pixman-1 -c $< -o $(BUILD)/bin/m51_pixman_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_pixman_smoke.o \
	  -L$(PIXMAN_DIR)/lib -lpixman-1 $(PKG_RPATH) $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC)

# M51: m51_freetype_smoke links the ported static FreeType (+libm).
FT_DIR := ../build/$(B1NIX_ARCH)/pkg/freetype
$(BUILD)/bin/m51_freetype_smoke: bin/gfx/m51_freetype_smoke.c $(BESPOKE_LIBDEP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(FT_DIR)/include/freetype2 -c $< -o $(BUILD)/bin/m51_freetype_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_freetype_smoke.o \
	  -L$(FT_DIR)/lib -lfreetype $(PKG_RPATH) $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC)

# M51: m51_cairo_smoke links the full stack (Cairo + FreeType + pixman + libm).
CAIRO_DIR := ../build/$(B1NIX_ARCH)/pkg/cairo
FC_DIR := ../build/$(B1NIX_ARCH)/pkg/fontconfig
$(BUILD)/bin/m51_cairo_smoke: bin/gfx/m51_cairo_smoke.c $(BESPOKE_LIBDEP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(CAIRO_DIR)/include/cairo -I $(FT_DIR)/include/freetype2 -I $(FC_DIR)/include \
	  -c $< -o $(BUILD)/bin/m51_cairo_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_cairo_smoke.o \
	  -L$(CAIRO_DIR)/lib -lcairo -L$(FT_DIR)/lib -lfreetype \
	  -L$(PIXMAN_DIR)/lib -lpixman-1 $(PKG_RPATH) $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC)

# Hosted C++ smoke: built against LLVM libc++ (musl-linked shared objects). The
# triplet-named cross compiler is only a compatibility shim; GCC/libstdc++ are
# not part of the active toolchain.
$(BUILD)/libcxx_compat.o: libcxx_compat.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -c $< -o $@

$(BUILD)/bin/cxx_smoke: bin/smoke/cxx_smoke.cpp $(BESPOKE_LIBDEP) $(BESPOKE_LIBCXX_EXTRA)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $(BUILD)/bin/cxx_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(BESPOKE_CXX_EXTRA) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/cxx_smoke.o \
	  -l:libc++.so.1 -l:libc++abi.so.1 $(BESPOKE_LIBCXX_EXTRA) $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  $(call stamp_linux_osabi,$@) \
	)

$(BUILD)/bin/m64_clang_smoke: bin/smoke/m64_clang_smoke.cpp $(BESPOKE_LIBDEP) $(BESPOKE_LIBCXX_EXTRA)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $(BUILD)/bin/m64_clang_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(BESPOKE_CXX_EXTRA) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m64_clang_smoke.o \
	  -l:libc++.so.1 -l:libc++abi.so.1 $(BESPOKE_LIBCXX_EXTRA) $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  $(call stamp_linux_osabi,$@) \
	)

# M55: std::iostream (std::cout/cerr + locale facets) + std::filesystem over the
# b1nix VFS, built against LLVM libc++ via tools/toolchain/bin/b1nix-c++.
# -ffreestanding (in $(CXXFLAGS), inherited from the kernel's own build flags)
# makes clang define __STDC_HOSTED__=0, which makes libc++ define
# _LIBCPP_FREESTANDING and compile <filesystem> down to an empty namespace —
# appropriate for bare-metal kernel code with no OS underneath, but wrong
# here: userspace on b1nix has a real musl libc and real syscalls, i.e. is
# genuinely hosted. Override it back for this one target rather than
# touching the shared $(CXXFLAGS) (other translation units may rely on the
# freestanding semantics, e.g. to avoid clang assuming a specific main()
# signature/builtin behavior — this file specifically needs the hosted
# standard-library facilities that flag suppresses).
$(BUILD)/bin/m55_iostream: bin/smoke/m55_iostream.cpp $(BESPOKE_LIBDEP) $(BESPOKE_LIBCXX_EXTRA)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -U__STDC_HOSTED__ -D__STDC_HOSTED__=1 -c $< -o $(BUILD)/bin/m55_iostream.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(BESPOKE_CXX_EXTRA) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m55_iostream.o \
	  -l:libc++.so.1 -l:libc++abi.so.1 $(BESPOKE_LIBCXX_EXTRA) $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  $(call stamp_linux_osabi,$@) \
	)

# M51: m51_harfbuzz_smoke links the ported static HarfBuzz (+libm +compiler-rt
# for integer builtins). HarfBuzz is C++ but -fno-exceptions/-rtti keep it off
# libc++.
HB_DIR := ../build/$(B1NIX_ARCH)/pkg/harfbuzz
LLVM_RT := $(firstword $(wildcard ../build/$(B1NIX_ARCH)/toolchain/$(B1NIX_TRIPLET)/sysroot/usr/lib/libcompiler_rt.a ../build/$(B1NIX_ARCH)/toolchain/$(B1NIX_TRIPLET)/llvm-runtimes-build/install/lib/libcompiler_rt.a ../build/$(B1NIX_ARCH)/toolchain/$(B1NIX_TRIPLET)/cross/$(B1NIX_TRIPLET)/lib/libcompiler_rt.a))
$(BUILD)/bin/m51_harfbuzz_smoke: bin/gfx/m51_harfbuzz_smoke.c $(BESPOKE_LIBDEP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(HB_DIR)/include/harfbuzz -c $< -o $(BUILD)/bin/m51_harfbuzz_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_harfbuzz_smoke.o \
	  -L$(HB_DIR)/lib -lharfbuzz $(PKG_RPATH) $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC) $(LLVM_RT)

# M51: m51_fontconfig_smoke links Fontconfig + expat + FreeType + libm.
FC_DIR := ../build/$(B1NIX_ARCH)/pkg/fontconfig
EXPAT_DIR := ../build/$(B1NIX_ARCH)/pkg/expat
$(BUILD)/bin/m51_fontconfig_smoke: bin/gfx/m51_fontconfig_smoke.c $(BESPOKE_LIBDEP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(FC_DIR)/include -c $< -o $(BUILD)/bin/m51_fontconfig_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_fontconfig_smoke.o \
	  -L$(FC_DIR)/lib -lfontconfig -L$(EXPAT_DIR)/lib -lexpat \
	  -L$(FT_DIR)/lib -lfreetype $(PKG_RPATH) $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC)

# M51: m51_xkb_smoke links the ported static xkbcommon.
XKB_DIR := ../build/$(B1NIX_ARCH)/pkg/xkbcommon
XKB_INC := $(wildcard $(XKB_DIR)/include/xkbcommon/xkbcommon.h)
XKB_REAL := $(if $(XKB_INC),$(XKB_DIR)/include,$(XKB_DIR)/include/include)
$(BUILD)/bin/m51_xkb_smoke: bin/gfx/m51_xkb_smoke.c $(BESPOKE_LIBDEP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(XKB_REAL) -c $< -o $(BUILD)/bin/m51_xkb_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_xkb_smoke.o \
	  $(XKB_DIR)/lib/libxkbcommon.a $(BESPOKE_LIBC)

# m32_pcre2_smoke links the ported static PCRE2 (8-bit) library.
PCRE2_DIR := ../build/$(B1NIX_ARCH)/pkg/pcre2
$(BUILD)/bin/m32_pcre2_smoke: bin/smoke/m32_pcre2_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -DPCRE2_STATIC -I $(PCRE2_DIR)/include -c $< -o $(BUILD)/bin/m32_pcre2_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m32_pcre2_smoke.o \
	  $(PCRE2_DIR)/lib/libpcre2-8.a $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  $(call stamp_linux_osabi,$@) \
	)

# m53_zlib_smoke links zlib, which is Alpine's package rather than a port —
# same musl, same arch, built by people who do it for a living. Dynamically,
# against their libz.so.1: the archive in the same package is not -fPIC and a
# PIE cannot link it, and a shared library is what the image should carry
# anyway (see "Dynamic Linking Is Mandatory" in CLAUDE.md).
ZLIB_DIR := ../build/$(B1NIX_ARCH)/pkg/zlib
$(BUILD)/bin/m53_zlib_smoke: bin/gfx/m53_zlib_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(ZLIB_DIR)/include -c $< -o $(BUILD)/bin/m53_zlib_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_zlib_smoke.o \
	  -L$(ZLIB_DIR)/lib -lz -rpath /lib $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  $(call stamp_linux_osabi,$@) \
	)

# m53_libpng_smoke links the ported libpng over zlib (M53 image codec).
LIBPNG_DIR := ../build/$(B1NIX_ARCH)/pkg/libpng
$(BUILD)/bin/m53_libpng_smoke: bin/gfx/m53_libpng_smoke.c $(GEN_LIBDEP) $(GEN_LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GEN_PIC) -I $(LIBPNG_DIR)/include -I $(ZLIB_DIR)/include -c $< -o $(BUILD)/bin/m53_libpng_smoke.o
	# Dynamic PIE (default) like its m53 siblings. The old "must stay static"
	# note (libpng's setjmp/longjmp error path takes the absolute address of
	# `longjmp` → R_X86_64_32 a non-PIE dynamic link can't form against the
	# imported libc.so.1 symbol) no longer applies: the port archives are all
	# -fPIC (GOTPCREL/PLT32, zero R_X86_64_32) so a -pie link resolves the
	# imported longjmp through the GOT. setjmp/longjmp are exported by libc.so.1.
	$(LD) $(LDFLAGS) $(GEN_LINK_EXTRA) $(if $(GEN_LINKER),-T $(GEN_LINKER)) -o $@ $(GEN_CRT0) $(BUILD)/bin/m53_libpng_smoke.o \
	  $(LIBPNG_DIR)/lib/libpng16.a $(ZLIB_DIR)/lib/libz.a $(LIBM_DIR)/lib/libm.a \
	  $(GEN_LINK_LIBS)

# m53_libjpeg_smoke links the ported static IJG libjpeg (M53 image codec).
LIBJPEG_DIR := ../build/$(B1NIX_ARCH)/pkg/libjpeg
$(BUILD)/bin/m53_libjpeg_smoke: bin/gfx/m53_libjpeg_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(LIBJPEG_DIR)/include -c $< -o $(BUILD)/bin/m53_libjpeg_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_libjpeg_smoke.o \
	  $(LIBJPEG_DIR)/lib/libjpeg.a $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  $(call stamp_linux_osabi,$@) \
	)

# m53_libwebp_smoke links libwebp (M53 image/video codec), Alpine's package.
# Upstream splits the sharp YUV conversion into its own archive and Alpine ships
# that split, so libsharpyuv is named here; the port built one archive.
LIBWEBP_DIR := ../build/$(B1NIX_ARCH)/pkg/libwebp
$(BUILD)/bin/m53_libwebp_smoke: bin/gfx/m53_libwebp_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(LIBWEBP_DIR)/include -c $< -o $(BUILD)/bin/m53_libwebp_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_libwebp_smoke.o \
	  $(LIBWEBP_DIR)/lib/libwebp.a $(LIBWEBP_DIR)/lib/libsharpyuv.a \
	  $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  $(call stamp_linux_osabi,$@) \
	)

# m53_libvpx_smoke links libvpx (VP8 decode) + libwebp (VP8 keyframe source) +
# libm. M53 full-motion video codec.
#
# libvpx comes from Alpine and Alpine ships no static libvpx: the archive this
# used to name survived on disk from the days the port was built from source,
# and vanished the first time its prefix was re-extracted — a link that had been
# broken for some time without anything saying so. The shared library is what
# the package actually provides, and what every other consumer already uses.
LIBVPX_DIR := ../build/$(B1NIX_ARCH)/pkg/libvpx
$(BUILD)/bin/m53_libvpx_smoke: bin/gfx/m53_libvpx_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(LIBVPX_DIR)/include -I $(LIBWEBP_DIR)/include -c $< -o $(BUILD)/bin/m53_libvpx_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_libvpx_smoke.o \
	  -L$(LIBVPX_DIR)/lib -l:libvpx.so.9 -rpath /lib \
	  $(LIBWEBP_DIR)/lib/libwebp.a $(LIBWEBP_DIR)/lib/libsharpyuv.a \
	  $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  $(call stamp_linux_osabi,$@) \
	)

# m53_wapcaplet_smoke links the ported NetSurf libwapcaplet (string internment)
# — the lowest layer of the NetSurf browser library chain (M53).
LWC_DIR := ../build/$(B1NIX_ARCH)/ports/libwapcaplet/install
$(BUILD)/bin/m53_wapcaplet_smoke: bin/gfx/m53_wapcaplet_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(LWC_DIR)/include -c $< -o $(BUILD)/bin/m53_wapcaplet_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_wapcaplet_smoke.o \
	  $(LWC_DIR)/lib/liblwc.a $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  $(call stamp_linux_osabi,$@) \
	)

# m53_parserutils_smoke links the ported NetSurf libparserutils (input streams +
# bundled charset codecs) — browser lib chain step 2 (M53).
PU_DIR := ../build/$(B1NIX_ARCH)/ports/libparserutils/install
$(BUILD)/bin/m53_parserutils_smoke: bin/gfx/m53_parserutils_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(PU_DIR)/include -c $< -o $(BUILD)/bin/m53_parserutils_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_parserutils_smoke.o \
	  $(PU_DIR)/lib/libparserutils.a $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  $(call stamp_linux_osabi,$@) \
	)

# m53_hubbub_smoke links the ported NetSurf libhubbub (HTML5 tokeniser) over
# libparserutils — browser lib chain step 3 (M53).
HUBBUB_DIR := ../build/$(B1NIX_ARCH)/ports/libhubbub/install
$(BUILD)/bin/m53_hubbub_smoke: bin/gfx/m53_hubbub_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(HUBBUB_DIR)/include -I $(PU_DIR)/include -c $< -o $(BUILD)/bin/m53_hubbub_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_hubbub_smoke.o \
	  $(HUBBUB_DIR)/lib/libhubbub.a $(PU_DIR)/lib/libparserutils.a \
	  $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  $(call stamp_linux_osabi,$@) \
	)

# m53_libcss_smoke links the ported NetSurf libcss (CSS parser + selection) over
# libwapcaplet + libparserutils — browser lib chain step 4 (M53).
LIBCSS_DIR := ../build/$(B1NIX_ARCH)/ports/libcss/install
$(BUILD)/bin/m53_libcss_smoke: bin/gfx/m53_libcss_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(LIBCSS_DIR)/include -I $(LWC_DIR)/include -I $(PU_DIR)/include -c $< -o $(BUILD)/bin/m53_libcss_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_libcss_smoke.o \
	  $(LIBCSS_DIR)/lib/libcss.a $(LWC_DIR)/lib/liblwc.a $(PU_DIR)/lib/libparserutils.a \
	  $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  $(call stamp_linux_osabi,$@) \
	)

# m53_libdom_smoke links the ported NetSurf libdom (DOM) + its hubbub binding
# over libhubbub + libparserutils + libwapcaplet — browser lib chain step 5.
LIBDOM_DIR := ../build/$(B1NIX_ARCH)/ports/libdom/install
$(BUILD)/bin/m53_libdom_smoke: bin/gfx/m53_libdom_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(LIBDOM_DIR)/include -I $(HUBBUB_DIR)/include -I $(PU_DIR)/include -I $(LWC_DIR)/include -c $< -o $(BUILD)/bin/m53_libdom_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_libdom_smoke.o \
	  $(LIBDOM_DIR)/lib/libdom.a $(HUBBUB_DIR)/lib/libhubbub.a \
	  $(PU_DIR)/lib/libparserutils.a $(LWC_DIR)/lib/liblwc.a \
	  $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  $(call stamp_linux_osabi,$@) \
	)

# m53_nslibs_smoke links the ported NetSurf helper/decoder libraries: libnsutils
# + libnsgif + libnsbmp + libnslog — browser lib chain helpers (step 6, M53).
NSUTILS_DIR := ../build/$(B1NIX_ARCH)/ports/libnsutils/install
NSGIF_DIR := ../build/$(B1NIX_ARCH)/ports/libnsgif/install
NSBMP_DIR := ../build/$(B1NIX_ARCH)/ports/libnsbmp/install
NSLOG_DIR := ../build/$(B1NIX_ARCH)/ports/libnslog/install
$(BUILD)/bin/m53_nslibs_smoke: bin/gfx/m53_nslibs_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(NSUTILS_DIR)/include -I $(NSGIF_DIR)/include -I $(NSBMP_DIR)/include -I $(NSLOG_DIR)/include -c $< -o $(BUILD)/bin/m53_nslibs_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_nslibs_smoke.o \
	  $(NSUTILS_DIR)/lib/libnsutils.a $(NSGIF_DIR)/lib/libnsgif.a \
	  $(NSBMP_DIR)/lib/libnsbmp.a $(NSLOG_DIR)/lib/libnslog.a \
	  $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  $(call stamp_linux_osabi,$@) \
	)

# M30: PIE smoke binary. Compiled with -fPIC and linked with -pie so the
# linker emits ET_DYN with R_X86_64_RELATIVE relocations. No crt0 or
# libb1nix.a — the body is self-contained inline-asm syscalls; the
# kernel's PIE-aware loader takes care of base relocation.
# m30_pie: raw no-interp PIE (no libc, no ld.so) — the kernel loads it directly
# and applies its R_X86_64_RELATIVE relocations. --no-dynamic-linker keeps it
# interp-less; the linker's default layout puts the ELF header in the first
# PT_LOAD (entry at a non-zero vaddr) with no custom script.
$(BUILD)/bin/m30_pie: bin/helpers/m30_pie.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c $< -o $(BUILD)/bin/m30_pie.o
	$(LD) $(LDFLAGS) -pie --no-dynamic-linker -z norelro -o $@ $(BUILD)/bin/m30_pie.o

$(BUILD)/bin/m30_dynamic: bin/helpers/m30_dynamic.c $(MUSL_LIBSO) $(MUSL_CRT1)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c $< -o $(BUILD)/bin/m30_dynamic.o
	$(LD) $(LDFLAGS) -pie -z norelro --hash-style=sysv --dynamic-linker /lib/ld-musl-x86_64.so.1 \
		-o $@ $(MUSL_CRT1) $(MUSL_CRTI) $(BUILD)/bin/m30_dynamic.o -L$(MUSL_LIB) -l:libc.so $(BUILTINS_LIB) $(MUSL_CRTN)
	$(call stamp_linux_osabi,$@)

# M69: a runtime-dlopen'd shared object, linked against musl's libc.so. Uses the
# linker's default shared-object layout (no custom script): that keeps the ELF
# header + program headers inside the first PT_LOAD (so AT_PHDR resolves) and,
# crucially, starts .text at a non-zero vaddr. A custom "= 0" script put the
# first exported function at st_value 0, which musl's dlsym (find_sym2) rejects
# as undefined — dlopen would succeed but dlsym("m69_plugin_add") return NULL.
$(BUILD)/bin/m69_plugin.so: bin/helpers/m69_plugin.c $(MUSL_LIBSO)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c $< -o $(BUILD)/bin/m69_plugin.o
	$(LD) $(LDFLAGS) -shared -z norelro --hash-style=gnu -soname m69_plugin.so \
		-o $@ $(BUILD)/bin/m69_plugin.o -L$(MUSL_LIB) -l:libc.so

# ── Install to rootfs ── musl provides its own libc/ld.so, so only headers ship.
install-headers-libs:
	@echo "Installing musl userspace headers..."
	@mkdir -p $(ROOTFS)/include
	@mkdir -p $(ROOTFS)/lib
	@# --skip-if-in: musl's copy of a header wins (see below), so a header both
	@# trees ship is left to the musl pass instead of being written here and
	@# overwritten a moment later -- 165 files rewritten on every build, each
	@# one enough to make the root image look stale.
	$(CIC) --skip-if-in $(MUSL_INCLUDE) -r include/* $(ROOTFS)/include/
	@# musl owns every libc header in the image: in-guest compilers (b1cc,
	@# native clang) must see the same declarations the binaries were built
	@# against. The b1nix-only extras copied above (EGL/, b1nix/, tui.h, ...)
	@# stay; anything musl also ships is overwritten by musl's version. Skip
	@# c++/v1 (16 MB of libc++ headers) — the C toolchains don't need it.
	@# Guarded: aarch64 stages its musl headers elsewhere, and an unguarded
	@# `cd` into a directory that does not exist failed the whole target.
	@if [ -d $(MUSL_INCLUDE) ]; then \
		for d in $$(cd $(MUSL_INCLUDE) && find . -type d -not -path './c++*'); do \
			mkdir -p $(ROOTFS)/include/$$d; \
		done; \
		cd $(MUSL_INCLUDE) && find . -type f -name '*.h' -not -path './c++/*' \
			-print0 | xargs -0 -r $(abspath $(CIC)) --into $(abspath $(ROOTFS))/include; \
	fi
	@mkdir -p $(ROOTFS)/usr
	@$(call link_if_changed,../include,$(ROOTFS)/usr/include)
	@$(call link_if_changed,../lib,$(ROOTFS)/usr/lib)

# Create a symlink only when it is not already what it should be.
#
# `ln -sfn` unlinks and recreates unconditionally, which gives the link a new
# timestamp on every build. The root filesystem image is rebuilt whenever
# anything in the staging tree is newer than it, so a few hundred symlinks
# rewritten for nothing cost a full 189 MB image write — thirty seconds of
# every build, including the many where only the kernel changed.
# A no-op when the link is already right -- and also when the path is a real
# directory, where `ln -sfn` cannot replace it and instead drops a link INSIDE
# it (that is where /usr/include/include came from). Recreating that inner link
# on every build gave the staging tree a new timestamp and repacked the image.
define link_if_changed
	if [ -d $(2) ] && [ ! -L $(2) ]; then :; \
	elif [ "$$(readlink $(2) 2>/dev/null)" != "$(1)" ]; then ln -sfn $(1) $(2); fi
endef

install: all install-headers-libs
	@echo "Installing userspace binaries..."
	@mkdir -p $(ROOTFS)/bin
	@# Content-compared: an unchanged binary keeps its timestamp, and the root
	@# image is only rebuilt when something in the staging tree is newer than
	@# it. An unconditional copy makes that test true on every build.
	$(CIC) $(filter-out %/m31_setuid,$(BINARIES)) $(ROOTFS)/bin/
	@# Copied with its mode rather than chmod'ed afterwards, or it never matches
	@# its source again and is rewritten on every build.
	@$(CIC) --mode 4755 $(BUILD)/bin/m31_setuid $(ROOTFS)/bin/m31_setuid
ifneq (,)
endif
	@# M108: /bin/su and /bin/passwd are no longer ELFs of their own — they are
	@# BusyBox applet symlinks onto /opt/busybox/bin/busybox-suid, which carries
	@# the setuid bit (see the symlink block at the end of this target). Drop any
	@# stale copies an older tree installed here, otherwise `cp` would keep an
	@# unmaintained setuid-root binary alive in the image forever.
	@# A stale ELF copy goes; the setuid links created at the end of this target
	@# stay, or they are deleted and remade on every build and the image with
	@# them.
	@for n in su passwd; do \
		case "$$(readlink $(ROOTFS)/bin/$$n 2>/dev/null)" in \
		/opt/busybox/bin/busybox*) ;; \
		*) rm -f $(ROOTFS)/bin/$$n ;; \
		esac; \
	done
	@for b in m12_smoke:m12-smoke m13_smoke:m13-smoke m13_job_control:m13-job-control m14_smoke:m14-smoke m15_smoke:m15-smoke m17_smoke:m17-smoke m24b_smoke:m24b-smoke m25_smoke:m25-smoke m26_smoke:m26-smoke m27_smoke:m27-smoke m29_smoke:m29-smoke m31_smoke:m31-smoke m31_setuid:m31-setuid m32_smoke:m32-smoke m34_smoke:m34-smoke m35_smoke:m35-smoke m36_smoke:m36-smoke m47_smoke:m47-smoke m48_smoke:m48-smoke  m50_smoke:m50-smoke m51_smoke:m51-smoke  m57_smoke:m57-smoke m75_smoke:m75-smoke; do \
		src=$${b%%:*}; dst=$${b##*:}; \
		[ "$$(readlink $(ROOTFS)/bin/$$dst 2>/dev/null)" = "$$src" ] || \
			ln -sfn $$src $(ROOTFS)/bin/$$dst; \
	done
	@# M95: the module utilities are BusyBox applets (the /bin symlinks come
	@# from the manifest loop above). /sbin carries the customary aliases, and
	@# the old /bin/kmod multicall is cleared out of an incrementally built
	@# rootfs so no stale copy shadows the applet.
	@rm -f $(ROOTFS)/bin/kmod
	@mkdir -p $(ROOTFS)/sbin
	@for b in insmod rmmod lsmod modprobe depmod; do \
		[ "$$(readlink $(ROOTFS)/sbin/$$b 2>/dev/null)" = "/opt/busybox/bin/busybox" ] || \
			ln -sfn /opt/busybox/bin/busybox $(ROOTFS)/sbin/$$b; \
	done
	@mkdir -p $(ROOTFS)/home
	$(CIC) hello_native.c $(ROOTFS)/home/
	@if [ -f ../build/$(B1NIX_ARCH)/ports/musl/install/usr/lib/Scrt1.o ]; then \
		$(CIC) ../build/$(B1NIX_ARCH)/ports/musl/install/usr/lib/Scrt1.o $(ROOTFS)/lib/crt0.o ; \
		$(CIC) ../build/$(B1NIX_ARCH)/ports/musl/install/usr/lib/Scrt1.o $(ROOTFS)/lib/crt1.o ; \
	fi
	@if [ -f ../build/$(B1NIX_ARCH)/ports/musl/install/usr/lib/libc.a ]; then \
		$(CIC) ../build/$(B1NIX_ARCH)/ports/musl/install/usr/lib/libc.a $(ROOTFS)/lib/libc.a ; \
		$(CIC) ../build/$(B1NIX_ARCH)/ports/musl/install/usr/lib/libc.a $(ROOTFS)/lib/libb1nix.a ; \
	fi
	@# b1cc's native link path reads its start file from /lib/b1cc (the default
	@# compiled in under -Db1nix). Stage musl's Scrt1.o so an on-device
	@# `b1cc -fPIC -pie foo.c -o foo` links with no environment set: the result is
	@# a musl PIE with PT_INTERP=/lib/ld-musl-x86_64.so.1 and DT_NEEDED=libc.so,
	@# exactly like every other binary in the image.
	@#
	@# Deliberately no static crt0.o/libb1nix.a here. That is the retired model
	@# (fixed-base ET_EXEC against a static b1nix libc), and musl's libc.a needs
	@# thread-local storage that b1cc's static linker does not yet emit a PT_TLS
	@# for — so a static link would produce a binary that faults instead of one
	@# that fails to link.
	@mkdir -p $(ROOTFS)/lib/b1cc
	@if [ -f ../build/$(B1NIX_ARCH)/ports/musl/install/lib/Scrt1.o ]; then \
		$(CIC) ../build/$(B1NIX_ARCH)/ports/musl/install/lib/Scrt1.o $(ROOTFS)/lib/b1cc/crt0-dynamic.o ; \
	fi
	@mkdir -p $(ROOTFS)/etc
	@mkdir -p $(ETCSTAGE)
	@printf "root:x:0:0:root:/root:/bin/zsh\nuser:x:1000:1000:b1nix user:/home/user:/bin/zsh\npamtest:x:1001:1001:M104 PAM smoke test account:/home/pamtest:/bin/sh\n" > $(ETCSTAGE)/passwd
	@# M108: three accounts for the BusyBox su/passwd smoke
	@# (userspace/bin/smoke/m108_smoke.c), kept apart from root/user/pamtest so the
	@# test never touches a credential another test depends on.
	@#   m108user  the *caller* in the unprivileged su test. Its login shell is
	@#             /bin/m108shell, a small ELF that prints its own argv[0] — that
	@#             is how the test proves su ran THE ACCOUNT'S shell, with the
	@#             login '-' prefix, rather than whatever shell it inherited.
	@#   m108peer  the su *target*.
	@#   m108pw    the account whose password `passwd` rewrites.
	@printf "m108user:x:1002:1002:M108 su caller:/home/m108user:/bin/m108shell\nm108peer:x:1003:1003:M108 su target:/home/m108peer:/bin/sh\nm108pw:x:1004:1004:M108 passwd target:/home/m108pw:/bin/sh\n" >> $(ETCSTAGE)/passwd
	@mkdir -p $(ROOTFS)/home/m108user $(ROOTFS)/home/m108peer $(ROOTFS)/home/m108pw
	@# M108: four accounts whose passwords are rewritten SIMULTANEOUSLY, to
	@# prove /etc/shadow locking. Separate from m108pw so the concurrent run and
	@# the single-writer passwd test never touch the same record.
	@printf "m108c0:x:1010:1010:M108 concurrent passwd 0:/home/m108c0:/bin/sh\nm108c1:x:1011:1011:M108 concurrent passwd 1:/home/m108c1:/bin/sh\nm108c2:x:1012:1012:M108 concurrent passwd 2:/home/m108c2:/bin/sh\nm108c3:x:1013:1013:M108 concurrent passwd 3:/home/m108c3:/bin/sh\n" >> $(ETCSTAGE)/passwd
	@mkdir -p $(ROOTFS)/home/m108c0 $(ROOTFS)/home/m108c1 $(ROOTFS)/home/m108c2 $(ROOTFS)/home/m108c3
	@printf 'root:$$6$$rootsalt$$DMaW/SWhAWN4kO5JlNx8ozBbTvstA7SEpK23VKJsNNwPfGKtVkA28Khht9Dlhody/oJANee.ewEebcJXbvL.T1:20000:0:99999:7:::\nuser:$$6$$usersalt$$Yj4u6k0ECzcsaZL01tgtZmlWMRHoHrJR8mVBtKmTUJb5zwclLqVeQi6QTcE9t/R7ZtGHS74rVqEELrrKpsaNj0:20000:0:99999:7:::\n' > $(ETCSTAGE)/shadow
	@# M104: dedicated /etc/shadow entry for the PAM smoke test
	@# (userspace/bin/smoke/m104_pam_smoke.c) — a real SHA-512 "$$6$$" hash of the
	@# known plaintext "PamSmoke123!" (generated on the host with
	@# `openssl passwd -6 -salt pamtestsalt 'PamSmoke123!'`), kept separate
	@# from root/user so this test never depends on — or risks perturbing —
	@# their credentials.
	@printf 'pamtest:$$6$$pamtestsalt$$.8vbtO9GTrzzFPO1mFgs22hz1YDCsqQnz/AucCx/9DkM5GoSWTqA78BSIVE4IESMY2ZYdF7Ywln/2p2LVLdd20:20000:0:99999:7:::\n' >> $(ETCSTAGE)/shadow
	@# M108: SHA-512 "$$6$$" hashes for the BusyBox su/passwd accounts. Same
	@# scheme, same file and same crypt(3) comparison pam_unix.so uses, which is
	@# the whole point of the milestone: one shadow format for the applet and
	@# the PAM path. Plaintexts (host: `openssl passwd -6 -salt <salt> <pw>`):
	@#   m108user = M108user!   m108peer = M108peer!   m108pw = M108old!
	@# m108pw's password is the one m108_smoke rewrites with `passwd`.
	@printf 'm108user:$$6$$m108usersalt$$U7uuAPJyfx58AT26qAudu8PvVKsmO6jTB5EiK/pag6OV8oGNxzS9DPtQlEhTAc4URuN2oMFLgarPW2Utx3DGA1:20000:0:99999:7:::\n' >> $(ETCSTAGE)/shadow
	@printf 'm108peer:$$6$$m108peersalt$$TCl.o4K5Iw7SlNXrk9dNp4NHfvT0JPCUmyk9otIuGpjRuq8d0trvLR6DDBava0iQY5MBTlU6kRnSNF4XOZXPF/:20000:0:99999:7:::\n' >> $(ETCSTAGE)/shadow
	@printf 'm108pw:$$6$$m108pwsalt$$YvQPFIRKzF9l53ejsICs/u66sJBq6NfeOai7reDJDsOGog0q.k7t7bHAfGgVrOs3Ni0rrwKmaEwEWvZv3Tl9V1:20000:0:99999:7:::\n' >> $(ETCSTAGE)/shadow
	@# The four concurrent-passwd accounts. Plaintexts (host:
	@# `openssl passwd -6 -salt m108cNsalt 'M108concN!'`):
	@#   m108c0 = M108conc0!   m108c1 = M108conc1!
	@#   m108c2 = M108conc2!   m108c3 = M108conc3!
	@printf 'm108c0:$$6$$m108c0salt$$HuyPuTI9qFa/FoKggrJeCwtFkHsDDJlaoFORwM3kqCOFoGZc95vIxua/wd6tNJcFpVnh72HFmSPBF8.dxCdMe0:20000:0:99999:7:::\n' >> $(ETCSTAGE)/shadow
	@printf 'm108c1:$$6$$m108c1salt$$V7SApmUI/.YLU4jynPO7G0q4cD6Vfy2RXg.lKfo7M4ME8Xh5WJ1Zr0R1YuU.dyIZA.DHfHFJzj.aqXlb9JN5I0:20000:0:99999:7:::\n' >> $(ETCSTAGE)/shadow
	@printf 'm108c2:$$6$$m108c2salt$$yeLeTxwP6LfUNDPRWwuqy4bN1qaSxaFIBljpDoNwzqgBiBfxzuWgL2dF9dtUjKqxFDG.wZIp1w1uQqOcyeByN/:20000:0:99999:7:::\n' >> $(ETCSTAGE)/shadow
	@printf 'm108c3:$$6$$m108c3salt$$k3HI/VMOXhcyVakp9WNAJfC0k9a1FvB2LdRLDC64bMRgkeQkSnxa.MF0p/ZCY/4tUzpNeM5Ecd6uyn6ngxCB.1:20000:0:99999:7:::\n' >> $(ETCSTAGE)/shadow
	@printf "root:x:0:\ndaemon:x:1:\ntty:x:5:\ndisk:x:6:\nnet:x:7:\nwheel:x:10:root\nuucp:x:14:\nusers:x:1000:user\npamtest:x:1001:pamtest\nm108user:x:1002:\nm108peer:x:1003:\nm108pw:x:1004:\nm108c0:x:1010:\nm108c1:x:1011:\nm108c2:x:1012:\nm108c3:x:1013:\n" > $(ETCSTAGE)/group
	@printf "/bin/sh\n/bin/zsh\n/bin/ash\n/bin/m108shell\n/opt/busybox/bin/busybox\n" > $(ETCSTAGE)/shells
	@$(CIC) $(ETCSTAGE)/passwd $(ETCSTAGE)/shadow $(ETCSTAGE)/group $(ETCSTAGE)/shells $(ROOTFS)/etc/
	@# rootfs-overlay: /etc/rc, bpkg (script+fixtures), posix/zsh/bpkg-smoke.sh,
	@# sshd init.d, bb-w6/bb-w2b fixtures — real files (not embedded kernel C
	@# strings) copied verbatim onto the ext4 root. cp -R preserves the tree
	@# structure and creates /etc, /bin, /pkgs subdirs as needed.
	@# --skip-if-in: a path the package staging root also provides is left to
	@# the merge in root-image, which runs after this and wins anyway. Writing
	@# ours here first only for it to be overwritten a moment later put a fresh
	@# timestamp on the tree every build (etc/fonts/fonts.conf, etc/bpkg.conf).
	@$(CIC) --skip-if-in ../build/$(B1NIX_ARCH)/pkgroot -r rootfs-overlay/. $(ROOTFS)/
	@# BusyBox applet symlinks: every command in applet-manifest.conf (M42) is
	@# "upstream" (native in-kernel dispatch was retired with the Ring 3 init
	@# rewrite), so they become /bin/<cmd> -> /opt/busybox/bin/busybox.
	@# 'sh'/'ash' aren't in the manifest (it only covers coreutils) but every
	@# script here execve()s /bin/sh directly, so add them explicitly.
	@# M108: there is no exception list any more. Every name the manifest claims
	@# for BusyBox really resolves to the multicall ELF — the dedicated ELFs that
	@# used to shadow chmod/chown/id/whoami/groups/halt/setfattr are deleted, so
	@# the symlink can no longer race cp or have cp write a binary's bytes
	@# through it into the busybox ELF. The rm below clears any stale copy left
	@# in an incrementally built rootfs -- a stale COPY, which is what it is
	@# there for, so a name that is already the right link is left alone. It was
	@# deleting and recreating eleven links on every build, and the image was
	@# repacked for it.
	@for n in id whoami groups chmod chown halt setfattr meminfo \
	          useradd userdel groupadd; do \
		case "$$(readlink $(ROOTFS)/bin/$$n 2>/dev/null)" in \
		/opt/busybox/bin/busybox*) ;; \
		*) rm -f $(ROOTFS)/bin/$$n ;; \
		esac; \
	done
	@mkdir -p $(ROOTFS)/bin
	@awk -F'=' '/^[[:space:]]*[^#]/ { gsub(/^[[:space:]]+|[[:space:]]+$$/, "", $$1); gsub(/^[[:space:]]+|[[:space:]]+$$/, "", $$2); if ($$2 == "upstream") print $$1; }' ../tools/configs/applet-manifest.conf | \
		grep -vE '^(su|passwd|login)$$' | \
		while read -r cmd; do \
			[ "$$(readlink "$(ROOTFS)/bin/$$cmd" 2>/dev/null)" = "/opt/busybox/bin/busybox" ] || \
				ln -sfn /opt/busybox/bin/busybox "$(ROOTFS)/bin/$$cmd"; \
		done
	@$(call link_if_changed,/opt/busybox/bin/busybox,$(ROOTFS)/bin/sh)
	@$(call link_if_changed,/opt/busybox/bin/busybox,$(ROOTFS)/bin/ash)
	@# /usr/bin/env: every third-party script starts "#!/usr/bin/env something",
	@# and the kernel resolves that path literally — without this entry the
	@# script fails with ENOENT and the shell reports "not found", which reads
	@# like the script itself is missing. Alpine (and every distro) ships it.
	@mkdir -p $(ROOTFS)/usr/bin
	@$(call link_if_changed,/opt/busybox/bin/busybox,$(ROOTFS)/usr/bin/env)
	@# M108: su, passwd and login are BusyBox's three BB_SUID_REQUIRE applets —
	@# they need euid 0 to read /etc/shadow and to change identity, so they get
	@# the setuid copy of the multicall ELF (build-busybox.sh installs
	@# busybox-suid mode 4755 root; the top-level Makefile stamps that mode onto
	@# the ext4 image). Everything else above keeps the plain, non-setuid copy,
	@# so the setuid inode is reachable under exactly three names. Combined with
	@# CONFIG_FEATURE_SUID=y (libbb check_suid() drops euid for every applet
	@# that is not BB_SUID_REQUIRE), even a hand-made symlink from `sh` to
	@# busybox-suid yields no privilege.
	@$(call link_if_changed,/opt/busybox/bin/busybox-suid,$(ROOTFS)/bin/su)
	@$(call link_if_changed,/opt/busybox/bin/busybox-suid,$(ROOTFS)/bin/passwd)
	@$(call link_if_changed,/opt/busybox/bin/busybox-suid,$(ROOTFS)/bin/login)


clean:
	rm -rf $(BUILD)
	-$(MAKE) -C $(B1CC_DIR) clean

-include $(DEPS)
