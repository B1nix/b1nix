# B1NIX Userspace Makefile
#
# Builds the userspace example/smoke ELFs and GUI archive against musl.
# All binaries are ELF executables loadable by the B1NIX kernel.

B1NIX_ARCH ?= x86_64
ifeq ($(B1NIX_ARCH),x86)
TARGET   := i686-unknown-elf
LDFLAGS  := -m elf_i386
# Host triplet for the per-triplet ported-library install trees (must match
# tools/toolchain/env.sh: x86 -> i686-b1nix, x86_64 -> x86_64-b1nix).
B1NIX_TRIPLET := i686-b1nix
else
TARGET   := x86_64-unknown-elf
LDFLAGS  := -m elf_x86_64
B1NIX_TRIPLET := x86_64-b1nix
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
B1CC_SYSROOT_INCLUDE := $(realpath include)
B1CC_DIR := b1cc
B1CC := $(B1CC_DIR)/build/b1cc
B1CC_SELF_SRCS := $(filter-out b1cc/src/b1cc_token_lexer.c, $(wildcard b1cc/src/*.c))
B1CC_BINS := b1cc_argv b1cc_better_c b1cc_file_write b1cc_hello b1cc_m34 b1cc_m34_corpus b1cc_stderr_exit
LIBM_DIR := ../build/$(B1NIX_ARCH)/ports/openlibm/install
LIBS_DIR := ../build/$(B1NIX_ARCH)/libs
# ── musl paths (must be before CFLAGS so $(MUSL_INCLUDE) expands) ───────
MUSL_INSTALL := ../build/$(B1NIX_ARCH)/ports/musl/install
MUSL_INCLUDE := $(MUSL_INSTALL)/include
MUSL_LIB     := $(MUSL_INSTALL)/lib
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
                   ../build/$(B1NIX_ARCH)/toolchain/sysroot/usr/lib/libcompiler_rt.a \
                   ../build/$(B1NIX_ARCH)/toolchain/llvm-runtimes-build/install/lib/libcompiler_rt.a \
                   ../build/$(B1NIX_ARCH)/toolchain/cross/$(B1NIX_TRIPLET)/lib/libcompiler_rt.a))

# NOTE: -MD (not -MMD). Our headers live under `include/`, pulled in via
# `-isystem include`, which makes them *system* headers — and -MMD omits system
# headers from the generated .d files. With -MMD, editing a header in
# userspace/include/ would NOT trigger a recompile (observed: a struct-size
# change to <unistd.h> left a stale statvfs.o, crashing BusyBox df on i686).
# -MD records every header so header edits rebuild the affected objects.
CFLAGS   := --target=$(TARGET) -ffreestanding -fno-builtin -fno-stack-protector -nostdinc -isystem include -I include -Wall -Wextra -O2 -msoft-float -mno-implicit-float -Db1nix -fno-pic -fno-pie -fdirect-access-external-data -MD -MP
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
# Strong utmp/utmpx overrides for musl's weak no-op stubs (real login accounting).
UTMP_OBJ := $(BUILD)/compat/utmp.o
GUI_LIB  := $(BUILD)/libb1gui.a
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
GEN_LIBDEP     := $(MUSL_LIBSO) $(UTMP_OBJ)
GEN_LINK_EXTRA := -pie -z norelro --hash-style=sysv \
                  --dynamic-linker /lib/ld-musl-x86_64.so.1 \
                  -L$(MUSL_LIB)
GEN_LINK_LIBS  := $(MUSL_CRTI) $(UTMP_OBJ) -lc $(MUSL_CRTN) $(BUILTINS_LIB)
GEN_MUSL_POSTLINK := 1

# Bespoke ported-library / GUI binaries: non-PIE dynamic ET_EXEC against musl.
# Same shape as the generic musl link (PIC/PIE, no linker script) so the program
# headers stay inside the first PT_LOAD where AT_PHDR can find them.
BESPOKE_CRT0   := $(MUSL_CRT1)
BESPOKE_CXX_EXTRA := --eh-frame-hdr
BESPOKE_EXTRA  := -pie -z norelro --hash-style=sysv \
                  --dynamic-linker /lib/ld-musl-x86_64.so.1 \
                  -L$(MUSL_LIB)
BESPOKE_LIBC   := $(MUSL_CRTI) $(UTMP_OBJ) -lc $(MUSL_CRTN) $(BUILTINS_LIB)
BESPOKE_LIBCXX_EXTRA := $(BUILD)/libcxx_compat.o
BESPOKE_LINKER :=
BESPOKE_LIBDEP := $(MUSL_LIBSO) $(MUSL_CRT1) $(UTMP_OBJ)
BESPOKE_PIC    := -fPIC
BESPOKE_MUSL_POSTLINK := 1
# Same reasoning as CLANG_BIN/CC above: prefer the real Homebrew LLVM clang++.
CLANGXX_BIN    := $(shell command -v /opt/homebrew/opt/llvm/bin/clang++ 2>/dev/null || command -v clang++ 2>/dev/null || echo clang++)
CXX            := $(if $(CCACHE),$(CCACHE) $(CLANGXX_BIN),$(CLANGXX_BIN))
CXXFLAGS       := --target=x86_64-unknown-elf -ffreestanding -nostdinc++ \
                  -isystem $(LIBCXX_INCLUDE) -nostdinc -isystem $(MUSL_INCLUDE) \
                  -isystem $(CLANG_RES_DIR)/include -idirafter include -Wall -Wextra -O2 -msoft-float -mno-implicit-float -Db1nix \
                  -fPIC -MD -MP -D__linux__ -D__b1nix__ -D_GNU_SOURCE

GUI_OBJS := $(BUILD)/libgui/gui.o
GUI_BINARIES := $(BUILD)/bin/gclock $(BUILD)/bin/gterm $(BUILD)/bin/gpaint \
	$(BUILD)/bin/gdesktop $(BUILD)/bin/gabout $(BUILD)/bin/m49_smoke \
	$(BUILD)/bin/displayd


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
	$(BUILD)/bin/m53_zlib_smoke \
	$(BUILD)/bin/m53_libpng_smoke \
	$(BUILD)/bin/m53_libjpeg_smoke \
	$(BUILD)/bin/m53_libwebp_smoke \
	$(BUILD)/bin/m53_libvpx_smoke \
	$(BUILD)/bin/m53_wapcaplet_smoke \
	$(BUILD)/bin/m53_parserutils_smoke \
	$(BUILD)/bin/m53_hubbub_smoke \
	$(BUILD)/bin/m53_libcss_smoke \
	$(BUILD)/bin/m53_libdom_smoke \
	$(BUILD)/bin/m53_nslibs_smoke \
	$(BUILD)/bin/m53_virgl_smoke \
	$(BUILD)/bin/m34_smoke \
	$(BUILD)/bin/m35_smoke \
	$(BUILD)/bin/m36_smoke \
	$(BUILD)/bin/diskbench \
	$(BUILD)/bin/su \
	$(BUILD)/bin/passwd \
	$(BUILD)/bin/id \
	$(BUILD)/bin/whoami \
	$(BUILD)/bin/groups \
	$(BUILD)/bin/useradd \
	$(BUILD)/bin/userdel \
	$(BUILD)/bin/groupadd \
	$(BUILD)/bin/chown \
	$(BUILD)/bin/chmod \
	$(BUILD)/bin/telinit \
	$(BUILD)/bin/m30_pie \
	$(BUILD)/bin/m38_sound \
	$(BUILD)/bin/m79_sound \
	$(BUILD)/bin/m39_smoke \
	$(BUILD)/bin/m77_smoke \
	$(BUILD)/bin/m47_smoke
BINARIES += $(BUILD)/bin/m48_smoke
BINARIES += $(BUILD)/bin/m49_libwayland
BINARIES += $(BUILD)/bin/m49_libwayland_server
BINARIES += $(BUILD)/bin/m50_smoke
BINARIES += $(BUILD)/bin/m51_smoke
# M51 ported-library smokes. These used to reach the guest only through the
# xxd/.inc initramfs path, which became bootstrap-only after the ext4-root
# migration — install them like every other binary so /bin/init's discovery
# loop finds them in the rootfs.
BINARIES += $(BUILD)/bin/m51_pixman_smoke
BINARIES += $(BUILD)/bin/m51_freetype_smoke
BINARIES += $(BUILD)/bin/m51_cairo_smoke
BINARIES += $(BUILD)/bin/m51_cairo_wayland
BINARIES += $(BUILD)/bin/m51_xkb_smoke
BINARIES += $(BUILD)/bin/m51_harfbuzz_smoke
BINARIES += $(BUILD)/bin/m51_fontconfig_smoke
BINARIES += $(BUILD)/bin/m52_gl_smoke
BINARIES += $(BUILD)/bin/cxx_smoke
BINARIES += $(BUILD)/bin/m55_iostream
BINARIES += $(BUILD)/bin/m64_clang_smoke
BINARIES += $(BUILD)/bin/m57_smoke
BINARIES += $(BUILD)/bin/m75_smoke
BINARIES += $(BUILD)/bin/m73_smoke
# M104: OpenPAM smoke — links libpam.so.2 (tools/ports/build-openpam.sh)
BINARIES += $(BUILD)/bin/m104_pam_smoke
# M80: ptrace register sets, /proc/<pid>/task|auxv|mem, crash capture.
BINARIES += $(BUILD)/bin/m80_smoke
# M86: per-thread CPU accounting, tkill/tgkill, pthread_exit return values.
BINARIES += $(BUILD)/bin/m86_smoke
# M95/M96: the module utilities (multi-call: insmod/rmmod/lsmod/modinfo/modprobe)
# and the two loadable-kernel-module smoke tests.
BINARIES += $(BUILD)/bin/kmod
BINARIES += $(BUILD)/bin/m95_smoke
BINARIES += $(BUILD)/bin/m96_smoke
# M98: GNU-free build tools (bmake as /bin/make, samurai as /bin/samu|ninja).
BINARIES += $(BUILD)/bin/m98_smoke
BINARIES += $(BUILD)/bin/m63_smoke
BINARIES += $(BUILD)/bin/m71_aslr
BINARIES += $(BUILD)/bin/m42_w5pre_smoke
BINARIES += $(BUILD)/bin/m46_smoke
BINARIES += $(BUILD)/bin/lock_smoke
BINARIES += $(BUILD)/bin/ext_stress
BINARIES += $(BUILD)/bin/m24_stress
BINARIES += $(BUILD)/bin/m27_cmdline
BINARIES += $(BUILD)/bin/m11_pipe_smoke
BINARIES += $(BUILD)/bin/m22_smoke
BINARIES += $(BUILD)/bin/net_smoke
BINARIES += $(BUILD)/bin/m51_clipboard_smoke
BINARIES += $(BUILD)/bin/m53_httpd
# M53 loopback HTTPS server + the driver that runs the NetSurf render tests.
BINARIES += $(BUILD)/bin/m53_httpsd
BINARIES += $(BUILD)/bin/m53_smoke
BINARIES += $(BUILD)/bin/halt
BINARIES += $(BUILD)/bin/setfattr
BINARIES += $(BUILD)/bin/b1fetch
BINARIES += $(BUILD)/bin/gpuinfo
BINARIES += $(BUILD)/bin/meminfo
BINARIES += $(BUILD)/bin/mc
BINARIES += $(BUILD)/bin/ne
# M16 driver: runs mc/ne out-of-process and covers the rest of the TUI surface.
BINARIES += $(BUILD)/bin/m16_smoke
# M58: standalone JavaScript interpreter (embeds Duktape) + its smoke test.
BINARIES += $(BUILD)/bin/js
BINARIES += $(BUILD)/bin/m58_smoke
# POSIX memory/signal primitives: madvise, MAP_NORESERVE, sigaltstack
BINARIES += $(BUILD)/bin/m_posixmm_smoke
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
# b1cc (standalone "better C" compiler + its M5/M7/M32-M34 smoke binaries)
BINARIES += $(addprefix $(BUILD)/bin/,$(B1CC_BINS)) $(BUILD)/bin/b1cc $(BUILD)/bin/b1cc_selfsmoke

ifeq ($(B1NIX_ARCH),x86_64)
BINARIES += $(BUILD)/bin/m30_dynamic
endif

# /bin/js has a custom multi-source rule (duktape.c + js.c) — exclude it from
# the generic single-source BIN_OBJS pattern.
BIN_OBJS := $(patsubst $(BUILD)/bin/%,$(BUILD)/bin/%.o,$(filter-out $(BUILD)/bin/native_smoke $(BUILD)/bin/m30_pie $(BUILD)/bin/m30_dynamic $(BUILD)/bin/js $(BUILD)/bin/b1cc $(BUILD)/bin/b1cc_selfsmoke $(addprefix $(BUILD)/bin/,$(B1CC_BINS)) $(BUILD)/bin/mc $(BUILD)/bin/ne $(BUILD)/bin/m16_smoke $(BUILD)/bin/m57_smoke $(BUILD)/bin/m53_httpsd $(BUILD)/bin/m104_pam_smoke,$(BINARIES)))
DEPS := $(BIN_OBJS:.o=.d)

.PHONY: all clean install install-headers-libs
.SECONDARY:

# musl provides its own libc/crt; the b1nix-native libc/gui archives are not
# linked into the ELFs, so `all` just builds the binaries.
all: $(BINARIES) $(GUI_BINARIES)

# utmp/utmpx overrides for musl's weak stubs (real login accounting).
$(UTMP_OBJ): compat/utmp.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GEN_PIC) -c $< -o $@

$(BUILD)/libgui/%.o: libgui/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

# ── GUI library ──
# Always available: built on-demand by ports (e.g. netsurf-fb, displayd) even
# under LINK=musl, since the source uses only POSIX + <b1nix/gui.h>.
$(GUI_LIB): $(GUI_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $(GUI_OBJS)
	@mkdir -p ../build/$(B1NIX_ARCH)/libs
	cp -f $@ ../build/$(B1NIX_ARCH)/libs/libb1gui.a

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
B1CC_HOST_CFLAGS ?= -std=c23 -D_DEFAULT_SOURCE -Wall -Wextra -O3 -flto -march=native

$(B1CC): $(B1CC_DIR)/Makefile $(wildcard $(B1CC_DIR)/src/*.c)
	$(MAKE) -C $(B1CC_DIR) CC="$(HOST_CC)" CFLAGS="$(B1CC_HOST_CFLAGS)"

# The host b1cc links through the external musl driver (b1nix-musl-cc → musl
# crt1.o + libc). B1CC_INTERNAL_LD must be UNSET to select the external linker:
# the host build treats the variable's mere presence (any value, including 0) as
# "use the internal b1nix-crt0/libb1nix.a linker", so leave it unset for musl.
$(addprefix $(BUILD)/bin/,$(B1CC_BINS)): $(B1CC)
$(addprefix $(BUILD)/bin/,$(B1CC_BINS)): $(BUILD)/bin/%: bin/%.c
	@mkdir -p $(dir $@)
	B1NIX_CC=$(B1CC_BINS_CC) B1CC_SYSROOT_INCLUDE=$(B1CC_SYSROOT_INCLUDE) $(B1CC) --target=$(B1NIX_ARCH)-b1nix bin/$*.c -o $@

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
	B1NIX_ARCH=$(B1NIX_ARCH) $(MUSL_CC) -std=gnu2x -Db1nix $(B1CC_SELF_SRCS) -o $@

# M32: /bin/b1cc-selfsmoke — the on-device self-host driver, statically linked
# against musl's libc.a (the minimal initramfs ships no shared libc.so).
$(BUILD)/bin/b1cc_selfsmoke: bin/b1cc_selfsmoke.c $(MUSL_LIB)/libc.a $(MUSL_CRT1)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $(BUILD)/bin/b1cc_selfsmoke.o
	$(LD) $(LDFLAGS) -static -o $@ $(MUSL_CRT1) $(MUSL_CRTI) $(BUILD)/bin/b1cc_selfsmoke.o \
	  $(MUSL_LIB)/libc.a $(MUSL_CRTN) $(BUILTINS_LIB)
	python3 -c " \
	import sys; f=sys.argv[1]; \
	fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true

$(BUILD)/src/tui_common.o: src/tui_common.c include/tui.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GEN_PIC) -c $< -o $@

$(BUILD)/bin/mc: bin/mc.c $(BUILD)/src/tui_common.o $(GEN_LIBDEP) $(GEN_CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GEN_PIC) -c bin/mc.c -o $(BUILD)/bin/mc.o
	$(LD) $(LDFLAGS) $(GEN_LINK_EXTRA) -o $@ $(GEN_CRT0) $(BUILD)/bin/mc.o $(BUILD)/src/tui_common.o $(GEN_LINK_LIBS)
ifeq ($(GEN_MUSL_POSTLINK),1)
	python3 -c "\
import sys; f=sys.argv[1]; \
d=open(f,'rb').read(); \
d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
open(f,'wb').write(d); \
fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true
endif

$(BUILD)/bin/ne: bin/ne.c $(BUILD)/src/tui_common.o $(GEN_LIBDEP) $(GEN_CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GEN_PIC) -c bin/ne.c -o $(BUILD)/bin/ne.o
	$(LD) $(LDFLAGS) $(GEN_LINK_EXTRA) -o $@ $(GEN_CRT0) $(BUILD)/bin/ne.o $(BUILD)/src/tui_common.o $(GEN_LINK_LIBS)
ifeq ($(GEN_MUSL_POSTLINK),1)
	python3 -c "\
import sys; f=sys.argv[1]; \
d=open(f,'rb').read(); \
d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
open(f,'wb').write(d); \
fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true
endif

# M16 driver links tui_common.o for the shared key-sequence decoder it checks.
$(BUILD)/bin/m16_smoke: bin/m16_smoke.c $(BUILD)/src/tui_common.o $(GEN_LIBDEP) $(GEN_CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GEN_PIC) -c bin/m16_smoke.c -o $(BUILD)/bin/m16_smoke.o
	$(LD) $(LDFLAGS) $(GEN_LINK_EXTRA) -o $@ $(GEN_CRT0) $(BUILD)/bin/m16_smoke.o $(BUILD)/src/tui_common.o $(GEN_LINK_LIBS)
ifeq ($(GEN_MUSL_POSTLINK),1)
	python3 -c "\
import sys; f=sys.argv[1]; \
d=open(f,'rb').read(); \
d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
open(f,'wb').write(d); \
fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true
endif

$(BUILD)/src/mojo_core.o: src/mojo_core.c include/mojo/mojo.h include/mojo.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GEN_PIC) -c $< -o $@

$(BUILD)/bin/m57_smoke: bin/m57_smoke.c $(BUILD)/src/mojo_core.o $(GEN_LIBDEP) $(GEN_CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GEN_PIC) -c bin/m57_smoke.c -o $(BUILD)/bin/m57_smoke.o
	$(LD) $(LDFLAGS) $(GEN_LINK_EXTRA) -o $@ $(GEN_CRT0) $(BUILD)/bin/m57_smoke.o $(BUILD)/src/mojo_core.o $(GEN_LINK_LIBS)
ifeq ($(GEN_MUSL_POSTLINK),1)
	python3 -c "\
import sys; f=sys.argv[1]; \
d=open(f,'rb').read(); \
d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
open(f,'wb').write(d); \
fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true
endif


$(BUILD)/bin/%: bin/%.c $(GEN_LIBDEP) $(GEN_CRT0) $(GEN_LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GEN_PIC) -I ../kernel/arch/x86_64 -c $< -o $(BUILD)/bin/$*.o
	$(LD) $(LDFLAGS) $(GEN_LINK_EXTRA) $(if $(GEN_LINKER),-T $(GEN_LINKER)) -o $@ $(GEN_CRT0) $(BUILD)/bin/$*.o $(GEN_LINK_LIBS)
ifeq ($(GEN_MUSL_POSTLINK),1)
	python3 -c "\
import sys; f=sys.argv[1]; \
d=open(f,'rb').read(); \
d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
open(f,'wb').write(d); \
fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true
endif

DISPLAYD_DIR := displayd
DISPLAYD_SRCS := $(DISPLAYD_DIR)/displayd_main.c $(DISPLAYD_DIR)/displayd_protocol.c \
	$(DISPLAYD_DIR)/displayd_window.c $(DISPLAYD_DIR)/displayd_render.c \
	$(DISPLAYD_DIR)/displayd_menu.c $(DISPLAYD_DIR)/displayd_input.c \
	$(DISPLAYD_DIR)/displayd_dispatch.c
DISPLAYD_HDRS := $(DISPLAYD_DIR)/displayd.h $(DISPLAYD_DIR)/font8x8.h $(DISPLAYD_DIR)/xkb_keymap_us.h
DISPLAYD_OBJS := $(patsubst $(DISPLAYD_DIR)/%.c,$(BUILD)/displayd/%.o,$(DISPLAYD_SRCS))

$(BUILD)/displayd/%.o: $(DISPLAYD_DIR)/%.c $(DISPLAYD_HDRS)
	@mkdir -p $(BUILD)/displayd
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I$(DISPLAYD_DIR) -I ../kernel/arch/x86_64 -c $< -o $@

$(BUILD)/bin/displayd: $(DISPLAYD_OBJS) $(GUI_LIB) $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(DISPLAYD_OBJS) \
		--whole-archive $(GUI_LIB) --no-whole-archive $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  python3 -c "\
	  import sys; f=sys.argv[1]; \
	  d=open(f,'rb').read(); \
	  d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	  open(f,'wb').write(d); \
	  fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true \
	)

$(filter-out $(BUILD)/bin/displayd,$(GUI_BINARIES)): $(BUILD)/bin/%: bin/%.c $(GUI_LIB) $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I ../kernel/arch/x86_64 -c $< -o $(BUILD)/bin/$*.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/$*.o \
		--whole-archive $(GUI_LIB) --no-whole-archive $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  python3 -c "\
	  import sys; f=sys.argv[1]; \
	  d=open(f,'rb').read(); \
	  d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	  open(f,'wb').write(d); \
	  fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true \
	)

# M49: upstream libwayland-client, with libffi for protocol dispatch.
WAYLAND_DIR := ../build/$(B1NIX_ARCH)/ports/wayland/install
$(WAYLAND_DIR)/lib/libwayland-client.a: ../tools/ports/build-wayland.sh ../tools/ports/build-libffi.sh \
		../tools/wayland/os.c ../tools/wayland/compat.c ../tools/wayland/compat.h
	B1NIX_ARCH=$(B1NIX_ARCH) ../tools/ports/build-wayland.sh >/dev/null
$(WAYLAND_DIR)/lib/libwayland-server.a: $(WAYLAND_DIR)/lib/libwayland-client.a

$(BUILD)/bin/m49_libwayland: bin/m49_libwayland.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER)) \
		$(WAYLAND_DIR)/lib/libwayland-client.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(WAYLAND_DIR)/include -c $< -o $(BUILD)/bin/m49_libwayland.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m49_libwayland.o \
		$(WAYLAND_DIR)/lib/libwayland-client.a $(WAYLAND_DIR)/lib/libffi.a \
		$(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  python3 -c "\
	  import sys; f=sys.argv[1]; \
	  d=open(f,'rb').read(); \
	  d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	  open(f,'wb').write(d); \
	  fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true \
	)

$(BUILD)/bin/m49_libwayland_server: bin/m49_libwayland_server.c $(BESPOKE_LIBDEP) \
		$(if $(BESPOKE_LINKER),$(BESPOKE_LINKER)) $(WAYLAND_DIR)/lib/libwayland-server.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(WAYLAND_DIR)/include -c $< -o $(BUILD)/bin/m49_libwayland_server.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m49_libwayland_server.o \
		$(WAYLAND_DIR)/lib/libwayland-server.a $(WAYLAND_DIR)/lib/libffi.a \
		$(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  python3 -c "\
	  import sys; f=sys.argv[1]; \
	  d=open(f,'rb').read(); \
	  d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	  open(f,'wb').write(d); \
	  fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true \
	)

$(BUILD)/bin/native_smoke: bin/native_smoke.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $(BUILD)/bin/native_smoke.o
	$(LD) $(LDFLAGS) $(if $(LINKER),-T $(LINKER)) -o $@ $(BUILD)/bin/native_smoke.o

# M104: OpenPAM smoke — the one binary in this tree that links a THIRD-PARTY
# shared object (libpam.so.2, DT_NEEDED) rather than only musl's libc.so.
# OPENPAM_DIR is tools/ports/build-openpam.sh's install dir; libpam.so.2 also
# has to be present in the rootfs at runtime (see the Makefile's root-image
# target) for ld-musl to resolve it.
OPENPAM_DIR := ../build/$(B1NIX_ARCH)/ports/openpam/install
$(BUILD)/bin/m104_pam_smoke: bin/m104_pam_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER)) $(OPENPAM_DIR)/lib/libpam.so.2
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(OPENPAM_DIR)/include -c $< -o $(BUILD)/bin/m104_pam_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -L $(OPENPAM_DIR)/lib -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m104_pam_smoke.o \
	  -l:libpam.so.2 $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  python3 -c "\
	  import sys; f=sys.argv[1]; \
	  d=open(f,'rb').read(); \
	  d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	  open(f,'wb').write(d); \
	  fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true \
	)

# m32_nettool gains an mbedTLS loopback HTTPS server when the static mbedTLS
# archives are present (built by tools/ports/build-curl.sh with B1NIX_TLS=mbedtls).
# Without them it builds HTTP-only so a `B1NIX_TLS=none` build still works.
MBEDTLS_DIR := ../build/$(B1NIX_ARCH)/ports/mbedtls/install
$(BUILD)/bin/m32_nettool: bin/m32_nettool.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER)) $(wildcard $(MBEDTLS_DIR)/lib/libmbed*.a)
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
	  python3 -c "\
	  import sys; f=sys.argv[1]; \
	  d=open(f,'rb').read(); \
	  d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	  open(f,'wb').write(d); \
	  fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true \
	)
	ln -sf m32_nettool $(BUILD)/bin/m32-nettool

# m53_httpsd: loopback HTTPS server (mbedTLS) for the M53 NetSurf web-access smoke.
$(BUILD)/bin/m53_httpsd: bin/m53_httpsd.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER)) $(wildcard $(MBEDTLS_DIR)/lib/libmbed*.a)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(MBEDTLS_DIR)/include -c $< -o $(BUILD)/bin/m53_httpsd.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_httpsd.o \
	  --start-group \
	  $(MBEDTLS_DIR)/lib/libmbedtls.a $(MBEDTLS_DIR)/lib/libmbedx509.a $(MBEDTLS_DIR)/lib/libmbedcrypto.a \
	  $(BESPOKE_LIBC) \
	  --end-group
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  python3 -c "\
	  import sys; f=sys.argv[1]; \
	  d=open(f,'rb').read(); \
	  d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	  open(f,'wb').write(d); \
	  fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true \
	)


# M58: /bin/js — standalone JavaScript interpreter embedding Duktape 2.7.0.
# Duktape is a single-file ANSI C amalgamation; it compiles fine with the same
# freestanding clang path as the rest of userspace. It needs a handful of libm
# functions (pow/floor/ceil/fmod/...), supplied by the ported openlibm. The
# duktape.c object is large (~600KB) and slow-ish to build, so it is compiled
# once into its own .o and linked into js.
DUK_DIR := duktape
$(BUILD)/duktape/duktape.o: $(DUK_DIR)/duktape.c $(DUK_DIR)/duktape.h $(DUK_DIR)/duk_config.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -Wno-unused-parameter -Wno-unused-but-set-variable \
	  -Wno-tautological-constant-out-of-range-compare -I $(DUK_DIR) -c $< -o $@

$(BUILD)/bin/js: bin/js.c $(BUILD)/duktape/duktape.o $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(DUK_DIR) -c $< -o $(BUILD)/bin/js.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/js.o \
	  $(BUILD)/duktape/duktape.o $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  python3 -c "\
	  import sys; f=sys.argv[1]; \
	  d=open(f,'rb').read(); \
	  d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	  open(f,'wb').write(d); \
	  fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true \
	)

# M51: m51_smoke links the ported static libm (openlibm).
$(BUILD)/bin/m51_smoke: bin/m51_smoke.c $(BESPOKE_LIBDEP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(LIBM_DIR)/include -c $< -o $(BUILD)/bin/m51_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_smoke.o \
	  $(LIBM_DIR)/lib/libm.a $(BESPOKE_LIBC)

# M51: m51_pixman_smoke links the ported static pixman (+libm).
PIXMAN_DIR := ../build/$(B1NIX_ARCH)/ports/pixman/install
$(BUILD)/bin/m51_pixman_smoke: bin/m51_pixman_smoke.c $(BESPOKE_LIBDEP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(PIXMAN_DIR)/include/pixman-1 -c $< -o $(BUILD)/bin/m51_pixman_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_pixman_smoke.o \
	  $(PIXMAN_DIR)/lib/libpixman-1.a $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC)

# M51: m51_freetype_smoke links the ported static FreeType (+libm).
FT_DIR := ../build/$(B1NIX_ARCH)/ports/freetype/install
$(BUILD)/bin/m51_freetype_smoke: bin/m51_freetype_smoke.c $(BESPOKE_LIBDEP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(FT_DIR)/include -c $< -o $(BUILD)/bin/m51_freetype_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_freetype_smoke.o \
	  $(FT_DIR)/lib/libfreetype.a $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC)

# M51: m51_cairo_smoke links the full stack (Cairo + FreeType + pixman + libm).
CAIRO_DIR := ../build/$(B1NIX_ARCH)/ports/cairo/install
$(BUILD)/bin/m51_cairo_smoke: bin/m51_cairo_smoke.c $(BESPOKE_LIBDEP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(CAIRO_DIR)/include/cairo -I $(FT_DIR)/include \
	  -c $< -o $(BUILD)/bin/m51_cairo_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_cairo_smoke.o \
	  $(CAIRO_DIR)/lib/libcairo.a $(FT_DIR)/lib/libfreetype.a \
	  $(PIXMAN_DIR)/lib/libpixman-1.a $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC)

# M51 end-to-end: Cairo + libb1gui (Wayland client) -> displayd.
$(BUILD)/bin/m51_cairo_wayland: bin/m51_cairo_wayland.c $(GUI_LIB) $(BESPOKE_LIBDEP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(CAIRO_DIR)/include/cairo -I $(FT_DIR)/include \
	  -c $< -o $(BUILD)/bin/m51_cairo_wayland.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_cairo_wayland.o \
	  $(CAIRO_DIR)/lib/libcairo.a $(FT_DIR)/lib/libfreetype.a \
	  $(PIXMAN_DIR)/lib/libpixman-1.a $(LIBM_DIR)/lib/libm.a \
	  --whole-archive $(GUI_LIB) --no-whole-archive $(BESPOKE_LIBC)

# M52 end-to-end: EGL + TinyGL (software GL) + libb1gui (Wayland client) -> displayd.
TINYGL_DIR := ../build/$(B1NIX_ARCH)/ports/tinygl/install
$(TINYGL_DIR)/lib/libEGL.a: ../tools/ports/build-tinygl.sh libegl/b1egl.c \
		include/EGL/egl.h
	B1NIX_ARCH=$(B1NIX_ARCH) ../tools/ports/build-tinygl.sh >/dev/null
$(BUILD)/bin/m52_gl_smoke: bin/m52_gl_smoke.c $(GUI_LIB) $(BESPOKE_LIBDEP) \
		$(TINYGL_DIR)/lib/libEGL.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(TINYGL_DIR)/include -c $< -o $(BUILD)/bin/m52_gl_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m52_gl_smoke.o \
	  $(TINYGL_DIR)/lib/libEGL.a $(TINYGL_DIR)/lib/libTinyGL.a \
	  $(LIBM_DIR)/lib/libm.a \
	  --whole-archive $(GUI_LIB) --no-whole-archive $(BESPOKE_LIBC)

# Hosted C++ smoke: built against LLVM libc++ (musl-linked shared objects). The
# triplet-named cross compiler is only a compatibility shim; GCC/libstdc++ are
# not part of the active toolchain.
$(BUILD)/libcxx_compat.o: libcxx_compat.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -c $< -o $@

$(BUILD)/bin/cxx_smoke: bin/cxx_smoke.cpp $(BESPOKE_LIBDEP) $(BESPOKE_LIBCXX_EXTRA)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $(BUILD)/bin/cxx_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(BESPOKE_CXX_EXTRA) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/cxx_smoke.o \
	  -l:libc++.so.1 -l:libc++abi.so.1 $(BESPOKE_LIBCXX_EXTRA) $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  python3 -c "\
	  import sys; f=sys.argv[1]; \
	  d=open(f,'rb').read(); \
	  d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	  open(f,'wb').write(d); \
	  fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true \
	)

$(BUILD)/bin/m64_clang_smoke: bin/m64_clang_smoke.cpp $(BESPOKE_LIBDEP) $(BESPOKE_LIBCXX_EXTRA)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $(BUILD)/bin/m64_clang_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(BESPOKE_CXX_EXTRA) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m64_clang_smoke.o \
	  -l:libc++.so.1 -l:libc++abi.so.1 $(BESPOKE_LIBCXX_EXTRA) $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  python3 -c "\
	  import sys; f=sys.argv[1]; \
	  d=open(f,'rb').read(); \
	  d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	  open(f,'wb').write(d); \
	  fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true \
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
$(BUILD)/bin/m55_iostream: bin/m55_iostream.cpp $(BESPOKE_LIBDEP) $(BESPOKE_LIBCXX_EXTRA)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -U__STDC_HOSTED__ -D__STDC_HOSTED__=1 -c $< -o $(BUILD)/bin/m55_iostream.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(BESPOKE_CXX_EXTRA) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m55_iostream.o \
	  -l:libc++.so.1 -l:libc++abi.so.1 $(BESPOKE_LIBCXX_EXTRA) $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  python3 -c "\
	  import sys; f=sys.argv[1]; \
	  d=open(f,'rb').read(); \
	  d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	  open(f,'wb').write(d); \
	  fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true \
	)

# M51: m51_harfbuzz_smoke links the ported static HarfBuzz (+libm +compiler-rt
# for integer builtins). HarfBuzz is C++ but -fno-exceptions/-rtti keep it off
# libc++.
HB_DIR := ../build/$(B1NIX_ARCH)/ports/harfbuzz/install
LLVM_RT := $(firstword $(wildcard ../build/$(B1NIX_ARCH)/toolchain/$(B1NIX_TRIPLET)/sysroot/usr/lib/libcompiler_rt.a ../build/$(B1NIX_ARCH)/toolchain/$(B1NIX_TRIPLET)/llvm-runtimes-build/install/lib/libcompiler_rt.a ../build/$(B1NIX_ARCH)/toolchain/$(B1NIX_TRIPLET)/cross/$(B1NIX_TRIPLET)/lib/libcompiler_rt.a))
$(BUILD)/bin/m51_harfbuzz_smoke: bin/m51_harfbuzz_smoke.c $(BESPOKE_LIBDEP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(HB_DIR)/include -c $< -o $(BUILD)/bin/m51_harfbuzz_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_harfbuzz_smoke.o \
	  $(HB_DIR)/lib/libharfbuzz.a $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC) $(LLVM_RT)

# M51: m51_fontconfig_smoke links Fontconfig + expat + FreeType + libm.
FC_DIR := ../build/$(B1NIX_ARCH)/ports/fontconfig/install
EXPAT_DIR := ../build/$(B1NIX_ARCH)/ports/expat/install
$(BUILD)/bin/m51_fontconfig_smoke: bin/m51_fontconfig_smoke.c $(BESPOKE_LIBDEP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(FC_DIR)/include -c $< -o $(BUILD)/bin/m51_fontconfig_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_fontconfig_smoke.o \
	  $(FC_DIR)/lib/libfontconfig.a $(EXPAT_DIR)/lib/libexpat.a \
	  $(FT_DIR)/lib/libfreetype.a $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC)

# M51: m51_xkb_smoke links the ported static xkbcommon.
XKB_DIR := ../build/$(B1NIX_ARCH)/ports/xkbcommon/install
XKB_INC := $(wildcard $(XKB_DIR)/include/xkbcommon/xkbcommon.h)
XKB_REAL := $(if $(XKB_INC),$(XKB_DIR)/include,$(XKB_DIR)/include/include)
$(BUILD)/bin/m51_xkb_smoke: bin/m51_xkb_smoke.c $(BESPOKE_LIBDEP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(XKB_REAL) -c $< -o $(BUILD)/bin/m51_xkb_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_xkb_smoke.o \
	  $(XKB_DIR)/lib/libxkbcommon.a $(BESPOKE_LIBC)

# m32_pcre2_smoke links the ported static PCRE2 (8-bit) library.
PCRE2_DIR := ../build/$(B1NIX_ARCH)/ports/pcre2/install
$(BUILD)/bin/m32_pcre2_smoke: bin/m32_pcre2_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -DPCRE2_STATIC -I $(PCRE2_DIR)/include -c $< -o $(BUILD)/bin/m32_pcre2_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m32_pcre2_smoke.o \
	  $(PCRE2_DIR)/lib/libpcre2-8.a $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  python3 -c "\
	  import sys; f=sys.argv[1]; \
	  d=open(f,'rb').read(); \
	  d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	  open(f,'wb').write(d); \
	  fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true \
	)

# m53_zlib_smoke links the ported static zlib (image-codec dependency, M53).
ZLIB_DIR := ../build/$(B1NIX_ARCH)/ports/zlib/install
$(BUILD)/bin/m53_zlib_smoke: bin/m53_zlib_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(ZLIB_DIR)/include -c $< -o $(BUILD)/bin/m53_zlib_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_zlib_smoke.o \
	  $(ZLIB_DIR)/lib/libz.a $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  python3 -c "\
	  import sys; f=sys.argv[1]; \
	  d=open(f,'rb').read(); \
	  d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	  open(f,'wb').write(d); \
	  fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true \
	)

# m53_libpng_smoke links the ported libpng over zlib (M53 image codec).
LIBPNG_DIR := ../build/$(B1NIX_ARCH)/ports/libpng/install
$(BUILD)/bin/m53_libpng_smoke: bin/m53_libpng_smoke.c $(GEN_LIBDEP) $(GEN_LINKER)
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
LIBJPEG_DIR := ../build/$(B1NIX_ARCH)/ports/libjpeg/install
$(BUILD)/bin/m53_libjpeg_smoke: bin/m53_libjpeg_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(LIBJPEG_DIR)/include -c $< -o $(BUILD)/bin/m53_libjpeg_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_libjpeg_smoke.o \
	  $(LIBJPEG_DIR)/lib/libjpeg.a $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  python3 -c "\
	  import sys; f=sys.argv[1]; \
	  d=open(f,'rb').read(); \
	  d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	  open(f,'wb').write(d); \
	  fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true \
	)

# m53_libwebp_smoke links the ported static libwebp (M53 image/video codec).
LIBWEBP_DIR := ../build/$(B1NIX_ARCH)/ports/libwebp/install
$(BUILD)/bin/m53_libwebp_smoke: bin/m53_libwebp_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(LIBWEBP_DIR)/include -c $< -o $(BUILD)/bin/m53_libwebp_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_libwebp_smoke.o \
	  $(LIBWEBP_DIR)/lib/libwebp.a $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  python3 -c "\
	  import sys; f=sys.argv[1]; \
	  d=open(f,'rb').read(); \
	  d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	  open(f,'wb').write(d); \
	  fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true \
	)

# m53_libvpx_smoke links the ported libvpx (VP8 decode) + libwebp (VP8 keyframe
# source) + libm. M53 full-motion video codec.
LIBVPX_DIR := ../build/$(B1NIX_ARCH)/ports/libvpx/install
$(BUILD)/bin/m53_libvpx_smoke: bin/m53_libvpx_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(LIBVPX_DIR)/include -I $(LIBWEBP_DIR)/include -c $< -o $(BUILD)/bin/m53_libvpx_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_libvpx_smoke.o \
	  $(LIBVPX_DIR)/lib/libvpx.a $(LIBWEBP_DIR)/lib/libwebp.a $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  python3 -c "\
	  import sys; f=sys.argv[1]; \
	  d=open(f,'rb').read(); \
	  d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	  open(f,'wb').write(d); \
	  fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true \
	)

# m53_wapcaplet_smoke links the ported NetSurf libwapcaplet (string internment)
# — the lowest layer of the NetSurf browser library chain (M53).
LWC_DIR := ../build/$(B1NIX_ARCH)/ports/libwapcaplet/install
$(BUILD)/bin/m53_wapcaplet_smoke: bin/m53_wapcaplet_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(LWC_DIR)/include -c $< -o $(BUILD)/bin/m53_wapcaplet_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_wapcaplet_smoke.o \
	  $(LWC_DIR)/lib/liblwc.a $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  python3 -c "\
	  import sys; f=sys.argv[1]; \
	  d=open(f,'rb').read(); \
	  d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	  open(f,'wb').write(d); \
	  fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true \
	)

# m53_parserutils_smoke links the ported NetSurf libparserutils (input streams +
# bundled charset codecs) — browser lib chain step 2 (M53).
PU_DIR := ../build/$(B1NIX_ARCH)/ports/libparserutils/install
$(BUILD)/bin/m53_parserutils_smoke: bin/m53_parserutils_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(PU_DIR)/include -c $< -o $(BUILD)/bin/m53_parserutils_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_parserutils_smoke.o \
	  $(PU_DIR)/lib/libparserutils.a $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  python3 -c "\
	  import sys; f=sys.argv[1]; \
	  d=open(f,'rb').read(); \
	  d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	  open(f,'wb').write(d); \
	  fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true \
	)

# m53_hubbub_smoke links the ported NetSurf libhubbub (HTML5 tokeniser) over
# libparserutils — browser lib chain step 3 (M53).
HUBBUB_DIR := ../build/$(B1NIX_ARCH)/ports/libhubbub/install
$(BUILD)/bin/m53_hubbub_smoke: bin/m53_hubbub_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(HUBBUB_DIR)/include -I $(PU_DIR)/include -c $< -o $(BUILD)/bin/m53_hubbub_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_hubbub_smoke.o \
	  $(HUBBUB_DIR)/lib/libhubbub.a $(PU_DIR)/lib/libparserutils.a \
	  $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  python3 -c "\
	  import sys; f=sys.argv[1]; \
	  d=open(f,'rb').read(); \
	  d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	  open(f,'wb').write(d); \
	  fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true \
	)

# m53_libcss_smoke links the ported NetSurf libcss (CSS parser + selection) over
# libwapcaplet + libparserutils — browser lib chain step 4 (M53).
LIBCSS_DIR := ../build/$(B1NIX_ARCH)/ports/libcss/install
$(BUILD)/bin/m53_libcss_smoke: bin/m53_libcss_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(LIBCSS_DIR)/include -I $(LWC_DIR)/include -I $(PU_DIR)/include -c $< -o $(BUILD)/bin/m53_libcss_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_libcss_smoke.o \
	  $(LIBCSS_DIR)/lib/libcss.a $(LWC_DIR)/lib/liblwc.a $(PU_DIR)/lib/libparserutils.a \
	  $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  python3 -c "\
	  import sys; f=sys.argv[1]; \
	  d=open(f,'rb').read(); \
	  d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	  open(f,'wb').write(d); \
	  fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true \
	)

# m53_libdom_smoke links the ported NetSurf libdom (DOM) + its hubbub binding
# over libhubbub + libparserutils + libwapcaplet — browser lib chain step 5.
LIBDOM_DIR := ../build/$(B1NIX_ARCH)/ports/libdom/install
$(BUILD)/bin/m53_libdom_smoke: bin/m53_libdom_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(LIBDOM_DIR)/include -I $(HUBBUB_DIR)/include -I $(PU_DIR)/include -I $(LWC_DIR)/include -c $< -o $(BUILD)/bin/m53_libdom_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_libdom_smoke.o \
	  $(LIBDOM_DIR)/lib/libdom.a $(HUBBUB_DIR)/lib/libhubbub.a \
	  $(PU_DIR)/lib/libparserutils.a $(LWC_DIR)/lib/liblwc.a \
	  $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  python3 -c "\
	  import sys; f=sys.argv[1]; \
	  d=open(f,'rb').read(); \
	  d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	  open(f,'wb').write(d); \
	  fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true \
	)

# m53_nslibs_smoke links the ported NetSurf helper/decoder libraries: libnsutils
# + libnsgif + libnsbmp + libnslog — browser lib chain helpers (step 6, M53).
NSUTILS_DIR := ../build/$(B1NIX_ARCH)/ports/libnsutils/install
NSGIF_DIR := ../build/$(B1NIX_ARCH)/ports/libnsgif/install
NSBMP_DIR := ../build/$(B1NIX_ARCH)/ports/libnsbmp/install
NSLOG_DIR := ../build/$(B1NIX_ARCH)/ports/libnslog/install
$(BUILD)/bin/m53_nslibs_smoke: bin/m53_nslibs_smoke.c $(BESPOKE_LIBDEP) $(if $(BESPOKE_LINKER),$(BESPOKE_LINKER))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BESPOKE_PIC) -I $(NSUTILS_DIR)/include -I $(NSGIF_DIR)/include -I $(NSBMP_DIR)/include -I $(NSLOG_DIR)/include -c $< -o $(BUILD)/bin/m53_nslibs_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) $(if $(BESPOKE_LINKER),-T $(BESPOKE_LINKER)) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_nslibs_smoke.o \
	  $(NSUTILS_DIR)/lib/libnsutils.a $(NSGIF_DIR)/lib/libnsgif.a \
	  $(NSBMP_DIR)/lib/libnsbmp.a $(NSLOG_DIR)/lib/libnslog.a \
	  $(BESPOKE_LIBC)
	$(if $(BESPOKE_MUSL_POSTLINK), \
	  python3 -c "\
	  import sys; f=sys.argv[1]; \
	  d=open(f,'rb').read(); \
	  d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	  open(f,'wb').write(d); \
	  fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true \
	)

# M30: PIE smoke binary. Compiled with -fPIC and linked with -pie so the
# linker emits ET_DYN with R_X86_64_RELATIVE relocations. No crt0 or
# libb1nix.a — the body is self-contained inline-asm syscalls; the
# kernel's PIE-aware loader takes care of base relocation.
# m30_pie: raw no-interp PIE (no libc, no ld.so) — the kernel loads it directly
# and applies its R_X86_64_RELATIVE relocations. --no-dynamic-linker keeps it
# interp-less; the linker's default layout puts the ELF header in the first
# PT_LOAD (entry at a non-zero vaddr) with no custom script.
$(BUILD)/bin/m30_pie: bin/m30_pie.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c $< -o $(BUILD)/bin/m30_pie.o
	$(LD) $(LDFLAGS) -pie --no-dynamic-linker -z norelro -o $@ $(BUILD)/bin/m30_pie.o

$(BUILD)/bin/m30_dynamic: bin/m30_dynamic.c $(MUSL_LIBSO) $(MUSL_CRT1)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c $< -o $(BUILD)/bin/m30_dynamic.o
	$(LD) $(LDFLAGS) -pie -z norelro --hash-style=sysv --dynamic-linker /lib/ld-musl-x86_64.so.1 \
		-o $@ $(MUSL_CRT1) $(MUSL_CRTI) $(BUILD)/bin/m30_dynamic.o -L$(MUSL_LIB) -l:libc.so $(BUILTINS_LIB) $(MUSL_CRTN)
	python3 -c "\
	import sys; f=sys.argv[1]; \
	d=open(f,'rb').read(); \
	d=d.replace(b'\x00ld-musl-x86_64.so.1\x00',b'\x00libc.so\x00'+b'\x00'*12); \
	open(f,'wb').write(d); \
	fh=open(f,'r+b'); fh.seek(7); fh.write(bytes([3])); fh.close()" $@ 2>/dev/null || true

# M69: a runtime-dlopen'd shared object, linked against musl's libc.so. Uses the
# linker's default shared-object layout (no custom script): that keeps the ELF
# header + program headers inside the first PT_LOAD (so AT_PHDR resolves) and,
# crucially, starts .text at a non-zero vaddr. A custom "= 0" script put the
# first exported function at st_value 0, which musl's dlsym (find_sym2) rejects
# as undefined — dlopen would succeed but dlsym("m69_plugin_add") return NULL.
$(BUILD)/bin/m69_plugin.so: bin/m69_plugin.c $(MUSL_LIBSO)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c $< -o $(BUILD)/bin/m69_plugin.o
	$(LD) $(LDFLAGS) -shared -z norelro --hash-style=gnu -soname m69_plugin.so \
		-o $@ $(BUILD)/bin/m69_plugin.o -L$(MUSL_LIB) -l:libc.so

# ── Install to rootfs ── musl provides its own libc/ld.so, so only headers ship.
install-headers-libs:
	@echo "Installing musl userspace headers..."
	@mkdir -p $(ROOTFS)/include
	@mkdir -p $(ROOTFS)/lib
	cp -r include/* $(ROOTFS)/include/
	@# musl owns every libc header in the image: in-guest compilers (b1cc,
	@# native clang) must see the same declarations the binaries were built
	@# against. The b1nix-only extras copied above (EGL/, b1nix/, tui.h, ...)
	@# stay; anything musl also ships is overwritten by musl's version. Skip
	@# c++/v1 (16 MB of libc++ headers) — the C toolchains don't need it.
	@for d in $$(cd $(MUSL_INCLUDE) && find . -type d -not -path './c++*'); do \
		mkdir -p $(ROOTFS)/include/$$d; \
	done
	@cd $(MUSL_INCLUDE) && find . -type f -name '*.h' -not -path './c++/*' \
		-exec cp {} $(abspath $(ROOTFS))/include/{} \;
	@mkdir -p $(ROOTFS)/usr
	ln -sfn ../include $(ROOTFS)/usr/include
	ln -sfn ../lib $(ROOTFS)/usr/lib

install: all install-headers-libs
	@echo "Installing userspace binaries..."
	@mkdir -p $(ROOTFS)/bin
	cp $(BINARIES) $(ROOTFS)/bin/
ifneq ($(GUI_BINARIES),)
	cp $(GUI_BINARIES) $(ROOTFS)/bin/
endif
	@chmod 4755 $(ROOTFS)/bin/m31_setuid 2>/dev/null || true
	@chmod 4755 $(ROOTFS)/bin/su 2>/dev/null || true
	@chmod 4755 $(ROOTFS)/bin/passwd 2>/dev/null || true
	@for b in m12_smoke:m12-smoke m13_smoke:m13-smoke m13_job_control:m13-job-control m14_smoke:m14-smoke m15_smoke:m15-smoke m17_smoke:m17-smoke m24b_smoke:m24b-smoke m25_smoke:m25-smoke m26_smoke:m26-smoke m27_smoke:m27-smoke m29_smoke:m29-smoke m31_smoke:m31-smoke m31_setuid:m31-setuid m32_smoke:m32-smoke m34_smoke:m34-smoke m35_smoke:m35-smoke m36_smoke:m36-smoke m47_smoke:m47-smoke m48_smoke:m48-smoke m49_smoke:m49-smoke m50_smoke:m50-smoke m51_smoke:m51-smoke m52_gl_smoke:m52-gl-smoke m57_smoke:m57-smoke m75_smoke:m75-smoke; do \
		src=$${b%%:*}; dst=$${b##*:}; \
		ln -sfn $$src $(ROOTFS)/bin/$$dst; \
	done
	@# M95: kmod is a multi-call binary; each utility is a name for it.
	@mkdir -p $(ROOTFS)/sbin
	@for b in insmod rmmod lsmod modinfo modprobe; do \
		ln -sfn kmod $(ROOTFS)/bin/$$b; \
	done
	@ln -sfn /bin/kmod $(ROOTFS)/sbin/modprobe
	@ln -sfn /bin/kmod $(ROOTFS)/sbin/insmod
	@ln -sfn /bin/kmod $(ROOTFS)/sbin/rmmod
	@mkdir -p $(ROOTFS)/home
	cp hello_native.c $(ROOTFS)/home/
	@if [ -f ../build/$(B1NIX_ARCH)/ports/musl/install/usr/lib/Scrt1.o ]; then \
		cp ../build/$(B1NIX_ARCH)/ports/musl/install/usr/lib/Scrt1.o $(ROOTFS)/lib/crt0.o ; \
		cp ../build/$(B1NIX_ARCH)/ports/musl/install/usr/lib/Scrt1.o $(ROOTFS)/lib/crt1.o ; \
	fi
	@if [ -f ../build/$(B1NIX_ARCH)/ports/musl/install/usr/lib/libc.a ]; then \
		cp ../build/$(B1NIX_ARCH)/ports/musl/install/usr/lib/libc.a $(ROOTFS)/lib/libc.a ; \
		cp ../build/$(B1NIX_ARCH)/ports/musl/install/usr/lib/libc.a $(ROOTFS)/lib/libb1nix.a ; \
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
		cp ../build/$(B1NIX_ARCH)/ports/musl/install/lib/Scrt1.o $(ROOTFS)/lib/b1cc/crt0-dynamic.o ; \
	fi
	@mkdir -p $(ROOTFS)/etc
	@printf "root:x:0:0:root:/root:/bin/zsh\nuser:x:1000:1000:b1nix user:/home/user:/bin/zsh\npamtest:x:1001:1001:M104 PAM smoke test account:/home/pamtest:/bin/sh\n" > $(ROOTFS)/etc/passwd
	@printf 'root:$$6$$rootsalt$$DMaW/SWhAWN4kO5JlNx8ozBbTvstA7SEpK23VKJsNNwPfGKtVkA28Khht9Dlhody/oJANee.ewEebcJXbvL.T1:0:0:99999:7:::\nuser:$$6$$usersalt$$Yj4u6k0ECzcsaZL01tgtZmlWMRHoHrJR8mVBtKmTUJb5zwclLqVeQi6QTcE9t/R7ZtGHS74rVqEELrrKpsaNj0:0:0:99999:7:::\n' > $(ROOTFS)/etc/shadow
	@# M104: dedicated /etc/shadow entry for the OpenPAM smoke test
	@# (userspace/bin/m104_pam_smoke.c) — a real SHA-512 "$$6$$" hash of the
	@# known plaintext "PamSmoke123!" (generated on the host with
	@# `openssl passwd -6 -salt pamtestsalt 'PamSmoke123!'`), kept separate
	@# from root/user so this test never depends on — or risks perturbing —
	@# their credentials.
	@printf 'pamtest:$$6$$pamtestsalt$$.8vbtO9GTrzzFPO1mFgs22hz1YDCsqQnz/AucCx/9DkM5GoSWTqA78BSIVE4IESMY2ZYdF7Ywln/2p2LVLdd20:0:0:99999:7:::\n' >> $(ROOTFS)/etc/shadow
	@printf "root:x:0:\ndaemon:x:1:\ntty:x:5:\ndisk:x:6:\nnet:x:7:\nwheel:x:10:root\nuucp:x:14:\nusers:x:1000:user\npamtest:x:1001:pamtest\n" > $(ROOTFS)/etc/group
	@printf "/bin/sh\n/bin/zsh\n/bin/ash\n/opt/busybox/bin/busybox\n" > $(ROOTFS)/etc/shells
	@# rootfs-overlay: /etc/rc, bpkg (script+fixtures), posix/zsh/bpkg-smoke.sh,
	@# sshd init.d, bb-w6/bb-w2b fixtures — real files (not embedded kernel C
	@# strings) copied verbatim onto the ext4 root. cp -R preserves the tree
	@# structure and creates /etc, /bin, /pkgs subdirs as needed.
	@cp -R rootfs-overlay/. $(ROOTFS)/
	@# BusyBox applet symlinks: every command in applet-manifest.conf (M42) is
	@# "upstream" (native in-kernel dispatch was retired with the Ring 3 init
	@# rewrite), so they become /bin/<cmd> -> /opt/busybox/bin/busybox — except
	@# id/whoami/chown/chmod, which BINARIES already installs as dedicated real
	@# ELFs; symlinking those too would race cp (dangling-symlink refusal) or,
	@# worse, have cp write a real binary's bytes through the symlink into the
	@# busybox ELF itself. 'sh'/'ash' aren't in the manifest (it only covers
	@# coreutils) but every script here execve()s /bin/sh directly, so add them
	@# explicitly.
	@mkdir -p $(ROOTFS)/bin
	@awk -F'=' '/^[[:space:]]*[^#]/ { gsub(/^[[:space:]]+|[[:space:]]+$$/, "", $$1); gsub(/^[[:space:]]+|[[:space:]]+$$/, "", $$2); if ($$2 == "upstream") print $$1; }' ../tools/configs/applet-manifest.conf | \
		grep -vE '^(id|whoami|chown|chmod)$$' | \
		while read -r cmd; do ln -sfn /opt/busybox/bin/busybox "$(ROOTFS)/bin/$$cmd"; done
	@ln -sfn /opt/busybox/bin/busybox $(ROOTFS)/bin/sh
	@ln -sfn /opt/busybox/bin/busybox $(ROOTFS)/bin/ash


clean:
	rm -rf $(BUILD)
	-$(MAKE) -C $(B1CC_DIR) clean

-include $(DEPS) $(GUI_OBJS:.o=.d)
