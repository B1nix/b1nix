# B1NIX Userspace Makefile
#
# Builds the userspace C library (libb1nix.a) and example programs.
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
CCACHE   := $(shell command -v ccache 2>/dev/null)
CC       := $(if $(CCACHE),$(CCACHE) clang,clang)
LD       := $(shell command -v ld.lld 2>/dev/null || echo /opt/homebrew/bin/ld.lld)
AR       := $(shell command -v llvm-ar 2>/dev/null || command -v /opt/homebrew/opt/llvm/bin/llvm-ar 2>/dev/null || command -v ar 2>/dev/null || echo ar)
HOST_CC  ?= cc
B1NIX_CXX ?= ../tools/toolchain/bin/b1nix-c++
LIBM_DIR := ../build/openlibm-b1nix/$(B1NIX_TRIPLET)/install
# NOTE: -MD (not -MMD). Our headers live under `include/`, pulled in via
# `-isystem include`, which makes them *system* headers — and -MMD omits system
# headers from the generated .d files. With -MMD, editing a header in
# userspace/include/ would NOT trigger a recompile (observed: a struct-size
# change to <unistd.h> left a stale statvfs.o, crashing BusyBox df on i686).
# -MD records every header so header edits rebuild the affected objects.
CFLAGS   := --target=$(TARGET) -ffreestanding -fno-builtin -fno-stack-protector -nostdinc -isystem include -I include -Wall -Wextra -O2 -msoft-float -mno-implicit-float -Db1nix -MD -MP
BUILD    := build/$(B1NIX_ARCH)
ROOTFS   := ../build/$(B1NIX_ARCH)/rootfs
LIB      := $(BUILD)/libb1nix.a
GUI_LIB  := $(BUILD)/libb1gui.a
SHARED_LIBC := $(BUILD)/libc.so.1
CRT0     := $(BUILD)/crt/crt0.o
DYNAMIC_CRT0 := $(BUILD)/crt/crt0-dynamic.o
LINKER   := linker.ld

# ── Link model (dynamic by default) ───────────────────────────────────────
# The base userspace links DYNAMICALLY against the shared libc.so.1 and is
# loaded through the in-kernel M69 dynamic loader on every spawn. `LINK=static`
# restores fully static ET_EXEC linking (the historical fallback). Only the
# generic single-source binary rule honours this switch; ported-library and
# C++/GUI binaries keep their bespoke static recipes (explicit residual debt).
LINK ?= dynamic
ifeq ($(LINK),static)
GEN_PIC        :=
GEN_CRT0       := $(CRT0)
GEN_LINKER     := linker.ld
GEN_LIBDEP     := $(LIB)
GEN_LINK_EXTRA :=
GEN_LINK_LIBS  := --whole-archive $(LIB) --no-whole-archive
else
GEN_PIC        := -fPIC
GEN_CRT0       := $(DYNAMIC_CRT0)
GEN_LINKER     := linker_pie.ld
GEN_LIBDEP     := $(LIB) $(SHARED_LIBC)
GEN_LINK_EXTRA := -pie -z norelro --hash-style=sysv --dynamic-linker /lib/ld-b1nix.so
GEN_LINK_LIBS  := $(SHARED_LIBC)
endif

# Bespoke ported-library / GUI binaries fold NON-PIC static archives (Mesa,
# libb1gui, mbedTLS, the NetSurf stack), so they cannot be built -pie. They link
# as a NON-PIE dynamic ET_EXEC against the shared libc.so.1 (the d8 / autotools-
# port model): the bundled archives stay statically folded, only libc becomes a
# shared dependency. LINK=static restores the historical whole-archive static link.
ifeq ($(LINK),static)
BESPOKE_CRT0  := $(CRT0)
BESPOKE_EXTRA :=
BESPOKE_LIBC  := --whole-archive $(LIB) --no-whole-archive
else
BESPOKE_CRT0  := $(DYNAMIC_CRT0)
BESPOKE_EXTRA := -z norelro --hash-style=sysv --dynamic-linker /lib/ld-b1nix.so
BESPOKE_LIBC  := $(SHARED_LIBC)
endif

LIBC_OBJS := \
	$(BUILD)/libc/string.o \
	$(BUILD)/libc/atomic.o \
	$(BUILD)/libc/stdio.o \
	$(BUILD)/libc/stdlib.o \
	$(BUILD)/libc/cxa_guard.o \
	$(BUILD)/libc/unistd.o \
	$(BUILD)/libc/ctype.o \
	$(BUILD)/libc/dirent.o \
	$(BUILD)/libc/pwd.o \
	$(BUILD)/libc/pty.o \
	$(BUILD)/libc/crypt.o \
	$(BUILD)/libc/grp.o \
	$(BUILD)/libc/pthread.o \
	$(BUILD)/libc/utmp.o \
	$(BUILD)/libc/pam.o \
	$(BUILD)/libc/locale.o \
	$(BUILD)/libc/iconv.o \
	$(BUILD)/libc/netdb.o \
	$(BUILD)/libc/udivti3.o \
	$(BUILD)/libc/wctype.o \
	$(BUILD)/libc/wchar.o \
	$(BUILD)/libc/uchar.o \
	$(BUILD)/libc/libgen.o \
	$(BUILD)/libc/getopt.o \
	$(BUILD)/libc/fnmatch.o \
	$(BUILD)/libc/regex.o \
	$(BUILD)/libc/mntent.o \
	$(BUILD)/libc/statvfs.o \
	$(BUILD)/libc/eventpoll.o \
	$(BUILD)/libc/dlfcn.o \
	$(BUILD)/libc/div64.o \
	$(BUILD)/libc/posix_compat.o \
	$(BUILD)/libc/rust_compat.o \
	$(BUILD)/libc/openlibm_compat.o
GUI_OBJS := $(BUILD)/libgui/gui.o
GUI_BINARIES := $(BUILD)/bin/gclock $(BUILD)/bin/gterm $(BUILD)/bin/gpaint \
	$(BUILD)/bin/gdesktop $(BUILD)/bin/gabout $(BUILD)/bin/m49_smoke

PIC_LIBC_OBJS := $(patsubst $(BUILD)/libc/%.o,$(BUILD)/libc-pic/%.o,$(LIBC_OBJS))

BINARIES := \
	$(BUILD)/bin/hello \
	$(BUILD)/bin/native_smoke \
	$(BUILD)/bin/m12_smoke \
	$(BUILD)/bin/m13_smoke \
	$(BUILD)/bin/m13_job_control \
	$(BUILD)/bin/m14_smoke \
	$(BUILD)/bin/m15_smoke \
	$(BUILD)/bin/m56_smoke \
	$(BUILD)/bin/m8_aio_test \
	$(BUILD)/bin/tcc \
	$(BUILD)/bin/test_sync \
	$(BUILD)/bin/m25_smoke \
	$(BUILD)/bin/m26_smoke \
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
	$(BUILD)/bin/m38_sound
BINARIES += $(BUILD)/bin/m48_smoke
BINARIES += $(BUILD)/bin/m49_libwayland
BINARIES += $(BUILD)/bin/m49_libwayland_server
BINARIES += $(BUILD)/bin/m57_smoke
BINARIES += $(BUILD)/bin/m73_smoke
# M58: standalone JavaScript interpreter (embeds Duktape) + its smoke test.
BINARIES += $(BUILD)/bin/js
BINARIES += $(BUILD)/bin/m58_smoke
# POSIX memory/signal primitives: madvise, MAP_NORESERVE, sigaltstack
BINARIES += $(BUILD)/bin/m_posixmm_smoke
# b1cc: the standalone "better C" compiler (separate repo, not in this tree). It
# compiles the M5/M7 self-host smoke programs below. Detect it; override with
# B1CC=/path/to/b1cc. If absent (e.g. a host where the b1cc repo isn't checked
# out), skip those targets rather than hard-fail the whole userspace build.
B1CC ?= $(or $(wildcard $(HOME)/Documents/GitHub/b1cc/build/b1cc),$(shell command -v b1cc 2>/dev/null))
B1CC_BINS := return_42 b1cc_hello b1cc_argv b1cc_file_write b1cc_stderr_exit b1cc_better_c
ifneq ($(strip $(B1CC)),)
BINARIES += $(addprefix $(BUILD)/bin/,$(B1CC_BINS))
else
$(warning b1cc compiler not found - skipping M5/M7 b1cc smoke binaries ($(B1CC_BINS)). Set B1CC=/path/to/b1cc to include them.)
endif

ifeq ($(B1NIX_ARCH),x86_64)
BINARIES += $(BUILD)/bin/m30_dynamic
DYNAMIC_TARGETS := $(SHARED_LIBC)
endif

# /bin/js has a custom multi-source rule (duktape.c + js.c) — exclude it from
# the generic single-source BIN_OBJS pattern.
BIN_OBJS := $(patsubst $(BUILD)/bin/%,$(BUILD)/bin/%.o,$(filter-out $(BUILD)/bin/native_smoke $(BUILD)/bin/tcc $(BUILD)/bin/m30_pie $(BUILD)/bin/m30_dynamic $(BUILD)/bin/js,$(BINARIES)))
# Include the PIC libc objects' .d files too: libc.so.1 is built from
# $(PIC_LIBC_OBJS), and without their header dependencies a change to a shared
# header (e.g. the struct dirent layout in include/dirent.h) rebuilds the static
# libb1nix.a but leaves the shared libc.so.1 stale — so dynamically linked base
# programs run against an out-of-date libc ABI (observed: readdir d_name offset
# skew breaking every dynamic readdir consumer while the static ones were fine).
DEPS := $(LIBC_OBJS:.o=.d) $(PIC_LIBC_OBJS:.o=.d) $(BIN_OBJS:.o=.d) $(CRT0:.o=.d) $(DYNAMIC_CRT0:.o=.d)

.PHONY: all clean install install-headers-libs
.SECONDARY:

all: $(LIB) $(GUI_LIB) $(DYNAMIC_TARGETS) $(BINARIES) $(GUI_BINARIES)

# ── crt0 ──
$(BUILD)/crt/crt0.o: crt/crt0.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(DYNAMIC_CRT0): crt/crt0.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -DB1NIX_DYNAMIC -c $< -o $@

# ── libc objects ──
$(BUILD)/libc/%.o: libc/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/libc-pic/%.o: libc/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(BUILD)/libgui/%.o: libgui/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ── libb1nix.a ──
$(LIB): $(LIBC_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $(LIBC_OBJS)

$(GUI_LIB): $(GUI_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $(GUI_OBJS)

# libc.so.1 folds the PIC openlibm (musl-style: libm IS libc) so the shared
# library exports the math funcs (log1p/cosh/tan/...) too. Dynamic consumers
# (e.g. the rust libLLVM.so / librustc_driver.so built against the shared libc)
# then import math from libc.so.1 instead of statically folding openlibm.
$(LIBM_DIR)/lib/libm.a: ../tools/ports/build-openlibm.sh
	B1NIX_ARCH=$(B1NIX_ARCH) ../tools/ports/build-openlibm.sh >/dev/null

$(SHARED_LIBC): $(PIC_LIBC_OBJS) linker_shared.ld $(LIBM_DIR)/lib/libm.a
	$(LD) $(LDFLAGS) -shared -z norelro --hash-style=sysv -soname libc.so.1 \
		-T linker_shared.ld -o $@ $(PIC_LIBC_OBJS) \
		--whole-archive $(LIBM_DIR)/lib/libm.a --no-whole-archive

build/libb1nix.a: $(LIB)
	@mkdir -p build
	ln -sf $(B1NIX_ARCH)/libb1nix.a $@

build/crt/crt0.o: $(CRT0)
	@mkdir -p build/crt
	ln -sf ../$(B1NIX_ARCH)/crt/crt0.o $@

build/bin/%: build/$(B1NIX_ARCH)/bin/%
	@mkdir -p build/bin
	ln -sf ../$(B1NIX_ARCH)/bin/$* $@



# ── User programs ──
# Dynamic by default (links against libc.so.1 via /lib/ld-b1nix.so + the
# in-kernel loader); LINK=static restores the historical static recipe. The
# mode is selected by the GEN_* variables defined near the top of this file.
$(addprefix $(BUILD)/bin/,$(B1CC_BINS)): $(BUILD)/bin/%: bin/%.c
	@mkdir -p $(dir $@)
	B1NIX_CC=$(realpath ../tools/toolchain/bin/b1nix-cc) $(B1CC) --target=$(B1NIX_ARCH)-b1nix bin/$*.c -o $@


$(BUILD)/bin/%: bin/%.c $(GEN_LIBDEP) $(GEN_CRT0) $(GEN_LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GEN_PIC) -I ../kernel/arch/x86_64 -c $< -o $(BUILD)/bin/$*.o
	$(LD) $(LDFLAGS) $(GEN_LINK_EXTRA) -T $(GEN_LINKER) -o $@ $(GEN_CRT0) $(BUILD)/bin/$*.o $(GEN_LINK_LIBS)

$(GUI_BINARIES): $(BUILD)/bin/%: bin/%.c $(GUI_LIB) $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I ../kernel/arch/x86_64 -c $< -o $(BUILD)/bin/$*.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/$*.o \
		--whole-archive $(GUI_LIB) --no-whole-archive $(BESPOKE_LIBC)

# M49: upstream libwayland-client, with libffi for protocol dispatch.
WAYLAND_DIR := ../build/wayland-b1nix/$(B1NIX_TRIPLET)/install
$(WAYLAND_DIR)/lib/libwayland-client.a: ../tools/ports/build-wayland.sh ../tools/ports/build-libffi.sh \
		../tools/wayland/os.c ../tools/wayland/compat.c ../tools/wayland/compat.h
	B1NIX_ARCH=$(B1NIX_ARCH) ../tools/ports/build-wayland.sh >/dev/null
$(WAYLAND_DIR)/lib/libwayland-server.a: $(WAYLAND_DIR)/lib/libwayland-client.a

$(BUILD)/bin/m49_libwayland: bin/m49_libwayland.c $(LIB) $(CRT0) $(LINKER) \
		$(WAYLAND_DIR)/lib/libwayland-client.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(WAYLAND_DIR)/include -c $< -o $(BUILD)/bin/m49_libwayland.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m49_libwayland.o \
		$(WAYLAND_DIR)/lib/libwayland-client.a $(WAYLAND_DIR)/lib/libffi.a \
		$(BESPOKE_LIBC)

$(BUILD)/bin/m49_libwayland_server: bin/m49_libwayland_server.c $(LIB) $(CRT0) \
		$(LINKER) $(WAYLAND_DIR)/lib/libwayland-server.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(WAYLAND_DIR)/include -c $< -o $(BUILD)/bin/m49_libwayland_server.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m49_libwayland_server.o \
		$(WAYLAND_DIR)/lib/libwayland-server.a $(WAYLAND_DIR)/lib/libffi.a \
		$(BESPOKE_LIBC)

$(BUILD)/bin/native_smoke: bin/native_smoke.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $(BUILD)/bin/native_smoke.o
	$(LD) $(LDFLAGS) -T $(LINKER) -o $@ $(BUILD)/bin/native_smoke.o

# m32_nettool gains an mbedTLS loopback HTTPS server when the static mbedTLS
# archives are present (built by tools/ports/build-curl.sh with B1NIX_TLS=mbedtls).
# Without them it builds HTTP-only so a `B1NIX_TLS=none` build still works.
MBEDTLS_DIR := ../build/mbedtls-b1nix/$(B1NIX_TRIPLET)/install
$(BUILD)/bin/m32_nettool: bin/m32_nettool.c $(LIB) $(CRT0) $(LINKER) $(wildcard $(MBEDTLS_DIR)/lib/libmbed*.a)
	@mkdir -p $(dir $@)
	@if [ -f $(MBEDTLS_DIR)/lib/libmbedtls.a ]; then \
	  echo "  CC+TLS  m32_nettool"; \
	  $(CC) $(CFLAGS) -DB1NIX_HAVE_MBEDTLS -I $(MBEDTLS_DIR)/include -c $< -o $(BUILD)/bin/m32_nettool.o; \
	  $(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m32_nettool.o \
	    $(MBEDTLS_DIR)/lib/libmbedtls.a $(MBEDTLS_DIR)/lib/libmbedx509.a $(MBEDTLS_DIR)/lib/libmbedcrypto.a \
	    $(BESPOKE_LIBC); \
	else \
	  echo "  CC      m32_nettool (HTTP-only, no mbedTLS)"; \
	  $(CC) $(CFLAGS) -c $< -o $(BUILD)/bin/m32_nettool.o; \
	  $(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m32_nettool.o $(BESPOKE_LIBC); \
	fi

# m53_httpsd: loopback HTTPS server (mbedTLS) for the M53 NetSurf web-access smoke.
$(BUILD)/bin/m53_httpsd: bin/m53_httpsd.c $(LIB) $(CRT0) $(LINKER) $(wildcard $(MBEDTLS_DIR)/lib/libmbed*.a)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(MBEDTLS_DIR)/include -c $< -o $(BUILD)/bin/m53_httpsd.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_httpsd.o \
	  $(MBEDTLS_DIR)/lib/libmbedtls.a $(MBEDTLS_DIR)/lib/libmbedx509.a $(MBEDTLS_DIR)/lib/libmbedcrypto.a \
	  $(BESPOKE_LIBC)


# M58: /bin/js — standalone JavaScript interpreter embedding Duktape 2.7.0.
# Duktape is a single-file ANSI C amalgamation; it compiles fine with the same
# freestanding clang path as the rest of userspace. It needs a handful of libm
# functions (pow/floor/ceil/fmod/...), supplied by the ported openlibm. The
# duktape.c object is large (~600KB) and slow-ish to build, so it is compiled
# once into its own .o and linked into js.
DUK_DIR := duktape
$(BUILD)/duktape/duktape.o: $(DUK_DIR)/duktape.c $(DUK_DIR)/duktape.h $(DUK_DIR)/duk_config.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-unused-parameter -Wno-unused-but-set-variable \
	  -Wno-tautological-constant-out-of-range-compare -I $(DUK_DIR) -c $< -o $@

$(BUILD)/bin/js: bin/js.c $(BUILD)/duktape/duktape.o $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(DUK_DIR) -c $< -o $(BUILD)/bin/js.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/js.o \
	  $(BUILD)/duktape/duktape.o $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC)

# M51: m51_smoke links the ported static libm (openlibm).
$(BUILD)/bin/m51_smoke: bin/m51_smoke.c $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(LIBM_DIR)/include -c $< -o $(BUILD)/bin/m51_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_smoke.o \
	  $(LIBM_DIR)/lib/libm.a $(BESPOKE_LIBC)

# M51: m51_pixman_smoke links the ported static pixman (+libm).
PIXMAN_DIR := ../build/pixman-b1nix/$(B1NIX_TRIPLET)/install
$(BUILD)/bin/m51_pixman_smoke: bin/m51_pixman_smoke.c $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(PIXMAN_DIR)/include/pixman-1 -c $< -o $(BUILD)/bin/m51_pixman_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_pixman_smoke.o \
	  $(PIXMAN_DIR)/lib/libpixman-1.a $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC)

# M51: m51_freetype_smoke links the ported static FreeType (+libm).
FT_DIR := ../build/freetype-b1nix/$(B1NIX_TRIPLET)/install
$(BUILD)/bin/m51_freetype_smoke: bin/m51_freetype_smoke.c $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(FT_DIR)/include -c $< -o $(BUILD)/bin/m51_freetype_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_freetype_smoke.o \
	  $(FT_DIR)/lib/libfreetype.a $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC)

# M51: m51_cairo_smoke links the full stack (Cairo + FreeType + pixman + libm).
CAIRO_DIR := ../build/cairo-b1nix/$(B1NIX_TRIPLET)/install
$(BUILD)/bin/m51_cairo_smoke: bin/m51_cairo_smoke.c $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(CAIRO_DIR)/include/cairo -I $(FT_DIR)/include \
	  -c $< -o $(BUILD)/bin/m51_cairo_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_cairo_smoke.o \
	  $(CAIRO_DIR)/lib/libcairo.a $(FT_DIR)/lib/libfreetype.a \
	  $(PIXMAN_DIR)/lib/libpixman-1.a $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC)

# M51 end-to-end: Cairo + libb1gui (Wayland client) -> displayd.
$(BUILD)/bin/m51_cairo_wayland: bin/m51_cairo_wayland.c $(GUI_LIB) $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(CAIRO_DIR)/include/cairo -I $(FT_DIR)/include \
	  -c $< -o $(BUILD)/bin/m51_cairo_wayland.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_cairo_wayland.o \
	  $(CAIRO_DIR)/lib/libcairo.a $(FT_DIR)/lib/libfreetype.a \
	  $(PIXMAN_DIR)/lib/libpixman-1.a $(LIBM_DIR)/lib/libm.a \
	  --whole-archive $(GUI_LIB) --no-whole-archive $(BESPOKE_LIBC)

# M52 end-to-end: EGL + TinyGL (software GL) + libb1gui (Wayland client) -> displayd.
TINYGL_DIR := ../build/tinygl-b1nix/$(B1NIX_TRIPLET)/install
$(TINYGL_DIR)/lib/libEGL.a: ../tools/ports/build-tinygl.sh libegl/b1egl.c \
		include/EGL/egl.h
	B1NIX_ARCH=$(B1NIX_ARCH) ../tools/ports/build-tinygl.sh >/dev/null
$(BUILD)/bin/m52_gl_smoke: bin/m52_gl_smoke.c $(GUI_LIB) $(LIB) $(CRT0) $(LINKER) \
		$(TINYGL_DIR)/lib/libEGL.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(TINYGL_DIR)/include -c $< -o $(BUILD)/bin/m52_gl_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m52_gl_smoke.o \
	  $(TINYGL_DIR)/lib/libEGL.a $(TINYGL_DIR)/lib/libTinyGL.a \
	  $(LIBM_DIR)/lib/libm.a \
	  --whole-archive $(GUI_LIB) --no-whole-archive $(BESPOKE_LIBC)

# Hosted C++ smoke: built with the cross GCC's libstdc++ via tools/toolchain/bin/b1nix-c++
# (NOT clang $(CC)). Requires the C++-enabled toolchain (enable-cxx-toolchain.sh).
$(BUILD)/bin/cxx_smoke: bin/cxx_smoke.cpp $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	B1NIX_ARCH=$(B1NIX_ARCH) $(B1NIX_CXX) $< -o $@

$(BUILD)/bin/m64_clang_smoke: bin/m64_clang_smoke.cpp $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	B1NIX_ARCH=$(B1NIX_ARCH) ../tools/toolchain/bin/b1nix-clang++ $< -o $@

# M55: std::iostream (std::cout/cerr + locale facets) + std::filesystem over the
# b1nix VFS, built against libstdc++ via tools/toolchain/bin/b1nix-c++.
$(BUILD)/bin/m55_iostream: bin/m55_iostream.cpp $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	B1NIX_ARCH=$(B1NIX_ARCH) $(B1NIX_CXX) $< -o $@

# M51: m51_harfbuzz_smoke links the ported static HarfBuzz (+libm +libgcc for
# __popcountdi2). HarfBuzz is C++ but -fno-exceptions/-rtti keep it off libstdc++.
HB_DIR := ../build/harfbuzz-b1nix/$(B1NIX_TRIPLET)/install
LIBGCC := ../build/toolchain_build/$(B1NIX_TRIPLET)/cross/lib/gcc/$(B1NIX_TRIPLET)/13.2.0/libgcc.a
$(BUILD)/bin/m51_harfbuzz_smoke: bin/m51_harfbuzz_smoke.c $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(HB_DIR)/include -c $< -o $(BUILD)/bin/m51_harfbuzz_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_harfbuzz_smoke.o \
	  $(HB_DIR)/lib/libharfbuzz.a $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC) $(LIBGCC)

# M51: m51_fontconfig_smoke links Fontconfig + expat + FreeType + libm.
FC_DIR := ../build/fontconfig-b1nix/$(B1NIX_TRIPLET)/install
EXPAT_DIR := ../build/expat-b1nix/$(B1NIX_TRIPLET)/install
$(BUILD)/bin/m51_fontconfig_smoke: bin/m51_fontconfig_smoke.c $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(FC_DIR)/include -c $< -o $(BUILD)/bin/m51_fontconfig_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_fontconfig_smoke.o \
	  $(FC_DIR)/lib/libfontconfig.a $(EXPAT_DIR)/lib/libexpat.a \
	  $(FT_DIR)/lib/libfreetype.a $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC)

# M51: m51_xkb_smoke links the ported static xkbcommon.
XKB_DIR := ../build/xkbcommon-b1nix/$(B1NIX_TRIPLET)/install
XKB_INC := $(wildcard $(XKB_DIR)/include/xkbcommon/xkbcommon.h)
XKB_REAL := $(if $(XKB_INC),$(XKB_DIR)/include,$(XKB_DIR)/include/include)
$(BUILD)/bin/m51_xkb_smoke: bin/m51_xkb_smoke.c $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(XKB_REAL) -c $< -o $(BUILD)/bin/m51_xkb_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m51_xkb_smoke.o \
	  $(XKB_DIR)/lib/libxkbcommon.a $(BESPOKE_LIBC)

# m32_pcre2_smoke links the ported static PCRE2 (8-bit) library.
PCRE2_DIR := ../build/pcre2-b1nix/$(B1NIX_TRIPLET)/install
$(BUILD)/bin/m32_pcre2_smoke: bin/m32_pcre2_smoke.c $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DPCRE2_STATIC -I $(PCRE2_DIR)/include -c $< -o $(BUILD)/bin/m32_pcre2_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m32_pcre2_smoke.o \
	  $(PCRE2_DIR)/lib/libpcre2-8.a $(BESPOKE_LIBC)

# m53_zlib_smoke links the ported static zlib (image-codec dependency, M53).
ZLIB_DIR := ../build/zlib-b1nix/$(B1NIX_TRIPLET)/install
$(BUILD)/bin/m53_zlib_smoke: bin/m53_zlib_smoke.c $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(ZLIB_DIR)/include -c $< -o $(BUILD)/bin/m53_zlib_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_zlib_smoke.o \
	  $(ZLIB_DIR)/lib/libz.a $(BESPOKE_LIBC)

# m53_libpng_smoke links the ported static libpng over zlib (M53 image codec).
LIBPNG_DIR := ../build/libpng-b1nix/$(B1NIX_TRIPLET)/install
$(BUILD)/bin/m53_libpng_smoke: bin/m53_libpng_smoke.c $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(LIBPNG_DIR)/include -I $(ZLIB_DIR)/include -c $< -o $(BUILD)/bin/m53_libpng_smoke.o
	# Stays STATIC (residual debt): libpng's setjmp/longjmp error path takes the
	# absolute address of `longjmp`, emitting R_X86_64_32 relocs a non-PIE dynamic
	# link cannot form against an imported (libc.so.1) symbol.
	$(LD) $(LDFLAGS) -T $(LINKER) -o $@ $(CRT0) $(BUILD)/bin/m53_libpng_smoke.o \
	  $(LIBPNG_DIR)/lib/libpng16.a $(ZLIB_DIR)/lib/libz.a $(LIBM_DIR)/lib/libm.a \
	  --whole-archive $(LIB) --no-whole-archive

# m53_libjpeg_smoke links the ported static IJG libjpeg (M53 image codec).
LIBJPEG_DIR := ../build/libjpeg-b1nix/$(B1NIX_TRIPLET)/install
$(BUILD)/bin/m53_libjpeg_smoke: bin/m53_libjpeg_smoke.c $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(LIBJPEG_DIR)/include -c $< -o $(BUILD)/bin/m53_libjpeg_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_libjpeg_smoke.o \
	  $(LIBJPEG_DIR)/lib/libjpeg.a $(BESPOKE_LIBC)

# m53_libwebp_smoke links the ported static libwebp (M53 image/video codec).
LIBWEBP_DIR := ../build/libwebp-b1nix/$(B1NIX_TRIPLET)/install
$(BUILD)/bin/m53_libwebp_smoke: bin/m53_libwebp_smoke.c $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(LIBWEBP_DIR)/include -c $< -o $(BUILD)/bin/m53_libwebp_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_libwebp_smoke.o \
	  $(LIBWEBP_DIR)/lib/libwebp.a $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC)

# m53_libvpx_smoke links the ported libvpx (VP8 decode) + libwebp (VP8 keyframe
# source) + libm. M53 full-motion video codec.
LIBVPX_DIR := ../build/libvpx-b1nix/$(B1NIX_TRIPLET)/install
$(BUILD)/bin/m53_libvpx_smoke: bin/m53_libvpx_smoke.c $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(LIBVPX_DIR)/include -I $(LIBWEBP_DIR)/include -c $< -o $(BUILD)/bin/m53_libvpx_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_libvpx_smoke.o \
	  $(LIBVPX_DIR)/lib/libvpx.a $(LIBWEBP_DIR)/lib/libwebp.a $(LIBM_DIR)/lib/libm.a \
	  $(BESPOKE_LIBC)

# m53_wapcaplet_smoke links the ported NetSurf libwapcaplet (string internment)
# — the lowest layer of the NetSurf browser library chain (M53).
LWC_DIR := ../build/libwapcaplet-b1nix/$(B1NIX_TRIPLET)/install
$(BUILD)/bin/m53_wapcaplet_smoke: bin/m53_wapcaplet_smoke.c $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(LWC_DIR)/include -c $< -o $(BUILD)/bin/m53_wapcaplet_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_wapcaplet_smoke.o \
	  $(LWC_DIR)/lib/liblwc.a $(BESPOKE_LIBC)

# m53_parserutils_smoke links the ported NetSurf libparserutils (input streams +
# bundled charset codecs) — browser lib chain step 2 (M53).
PU_DIR := ../build/libparserutils-b1nix/$(B1NIX_TRIPLET)/install
$(BUILD)/bin/m53_parserutils_smoke: bin/m53_parserutils_smoke.c $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(PU_DIR)/include -c $< -o $(BUILD)/bin/m53_parserutils_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_parserutils_smoke.o \
	  $(PU_DIR)/lib/libparserutils.a $(BESPOKE_LIBC)

# m53_hubbub_smoke links the ported NetSurf libhubbub (HTML5 tokeniser) over
# libparserutils — browser lib chain step 3 (M53).
HUBBUB_DIR := ../build/libhubbub-b1nix/$(B1NIX_TRIPLET)/install
$(BUILD)/bin/m53_hubbub_smoke: bin/m53_hubbub_smoke.c $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(HUBBUB_DIR)/include -I $(PU_DIR)/include -c $< -o $(BUILD)/bin/m53_hubbub_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_hubbub_smoke.o \
	  $(HUBBUB_DIR)/lib/libhubbub.a $(PU_DIR)/lib/libparserutils.a \
	  $(BESPOKE_LIBC)

# m53_libcss_smoke links the ported NetSurf libcss (CSS parser + selection) over
# libwapcaplet + libparserutils — browser lib chain step 4 (M53).
LIBCSS_DIR := ../build/libcss-b1nix/$(B1NIX_TRIPLET)/install
$(BUILD)/bin/m53_libcss_smoke: bin/m53_libcss_smoke.c $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(LIBCSS_DIR)/include -I $(LWC_DIR)/include -I $(PU_DIR)/include -c $< -o $(BUILD)/bin/m53_libcss_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_libcss_smoke.o \
	  $(LIBCSS_DIR)/lib/libcss.a $(LWC_DIR)/lib/liblwc.a $(PU_DIR)/lib/libparserutils.a \
	  $(BESPOKE_LIBC)

# m53_libdom_smoke links the ported NetSurf libdom (DOM) + its hubbub binding
# over libhubbub + libparserutils + libwapcaplet — browser lib chain step 5.
LIBDOM_DIR := ../build/libdom-b1nix/$(B1NIX_TRIPLET)/install
$(BUILD)/bin/m53_libdom_smoke: bin/m53_libdom_smoke.c $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(LIBDOM_DIR)/include -I $(HUBBUB_DIR)/include -I $(PU_DIR)/include -I $(LWC_DIR)/include -c $< -o $(BUILD)/bin/m53_libdom_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_libdom_smoke.o \
	  $(LIBDOM_DIR)/lib/libdom.a $(HUBBUB_DIR)/lib/libhubbub.a \
	  $(PU_DIR)/lib/libparserutils.a $(LWC_DIR)/lib/liblwc.a \
	  $(BESPOKE_LIBC)

# m53_nslibs_smoke links the ported NetSurf helper/decoder libraries: libnsutils
# + libnsgif + libnsbmp + libnslog — browser lib chain helpers (step 6, M53).
NSUTILS_DIR := ../build/libnsutils-b1nix/$(B1NIX_TRIPLET)/install
NSGIF_DIR := ../build/libnsgif-b1nix/$(B1NIX_TRIPLET)/install
NSBMP_DIR := ../build/libnsbmp-b1nix/$(B1NIX_TRIPLET)/install
NSLOG_DIR := ../build/libnslog-b1nix/$(B1NIX_TRIPLET)/install
$(BUILD)/bin/m53_nslibs_smoke: bin/m53_nslibs_smoke.c $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(NSUTILS_DIR)/include -I $(NSGIF_DIR)/include -I $(NSBMP_DIR)/include -I $(NSLOG_DIR)/include -c $< -o $(BUILD)/bin/m53_nslibs_smoke.o
	$(LD) $(LDFLAGS) $(BESPOKE_EXTRA) -T $(LINKER) -o $@ $(BESPOKE_CRT0) $(BUILD)/bin/m53_nslibs_smoke.o \
	  $(NSUTILS_DIR)/lib/libnsutils.a $(NSGIF_DIR)/lib/libnsgif.a \
	  $(NSBMP_DIR)/lib/libnsbmp.a $(NSLOG_DIR)/lib/libnslog.a \
	  $(BESPOKE_LIBC)

# M30: PIE smoke binary. Compiled with -fPIC and linked with -pie so the
# linker emits ET_DYN with R_X86_64_RELATIVE relocations. No crt0 or
# libb1nix.a — the body is self-contained inline-asm syscalls; the
# kernel's PIE-aware loader takes care of base relocation.
$(BUILD)/bin/m30_pie: bin/m30_pie.c linker_pie.ld
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c $< -o $(BUILD)/bin/m30_pie.o
	$(LD) $(LDFLAGS) -pie -T linker_pie.ld -o $@ $(BUILD)/bin/m30_pie.o

$(BUILD)/bin/m30_dynamic: bin/m30_dynamic.c $(SHARED_LIBC) $(DYNAMIC_CRT0) linker_pie.ld
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c $< -o $(BUILD)/bin/m30_dynamic.o
	$(LD) $(LDFLAGS) -pie -z norelro --hash-style=sysv --dynamic-linker /lib/ld-b1nix.so \
		-T linker_pie.ld -o $@ $(DYNAMIC_CRT0) $(BUILD)/bin/m30_dynamic.o $(SHARED_LIBC)

# M69: a runtime-dlopen'd shared object. Linked against libc.so.1 so its write()
# stays an undefined import resolved by the userspace loader (dlfcn.c) at dlopen.
$(BUILD)/bin/m69_plugin.so: bin/m69_plugin.c $(SHARED_LIBC) linker_shared.ld
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c $< -o $(BUILD)/bin/m69_plugin.o
	$(LD) $(LDFLAGS) -shared -z norelro --hash-style=sysv -soname m69_plugin.so \
		-T linker_shared.ld -o $@ $(BUILD)/bin/m69_plugin.o $(SHARED_LIBC)

$(BUILD)/host/c2str: tcc/conftest.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -O2 -DC2STR=1 $< -o $@

tcc/config.h:
	@printf '%s\n' '/* Generated by userspace/Makefile for the b1nix TCC build. */' > $@
	@printf '%s\n' '#define TCC_VERSION "0.9.28rc"' >> $@
	@printf '%s\n' '#define CC_NAME CC_clang' >> $@
	@printf '%s\n' '#define GCC_MAJOR 0' >> $@
	@printf '%s\n' '#define GCC_MINOR 0' >> $@
	@printf '%s\n' '#define CONFIG_TCC_PREDEFS 1' >> $@

$(BUILD)/bin/tcc: tcc/tcc.c $(LIB) $(CRT0) $(BUILD)/host/c2str tcc/config.h
	@mkdir -p $(dir $@)
	$(BUILD)/host/c2str tcc/include/tccdefs.h tcc/tccdefs_.h
ifeq ($(B1NIX_ARCH),x86)
	B1NIX_ARCH=$(B1NIX_ARCH) ../tools/toolchain/bin/b1nix-cc tcc/tcc.c -o $@ -DTCC_TARGET_I386 -DCONFIG_TCCDIR="\"/lib/tcc\"" -DCONFIG_TCC_SYSINCLUDEPATHS="\"/include:/lib/tcc/include\"" -DCONFIG_TCC_LIBPATHS="\"/lib\"" -DCONFIG_TCC_CRTPREFIX="\"/lib\""
else
	B1NIX_ARCH=$(B1NIX_ARCH) ../tools/toolchain/bin/b1nix-cc tcc/tcc.c -o $@ -DTCC_TARGET_X86_64 -DCONFIG_TCCDIR="\"/lib/tcc\"" -DCONFIG_TCC_SYSINCLUDEPATHS="\"/include:/lib/tcc/include\"" -DCONFIG_TCC_LIBPATHS="\"/lib\"" -DCONFIG_TCC_CRTPREFIX="\"/lib\""
endif

# ── Install to rootfs ──
install-headers-libs: $(LIB) $(GUI_LIB) $(CRT0) $(SHARED_LIBC)
	@echo "Installing userspace headers and libraries..."
	@mkdir -p $(ROOTFS)/include
	@mkdir -p $(ROOTFS)/lib
	cp -r include/* $(ROOTFS)/include/
	cp $(LIB) $(ROOTFS)/lib/
	cp $(GUI_LIB) $(ROOTFS)/lib/
	cp $(CRT0) $(ROOTFS)/lib/
	cp $(SHARED_LIBC) $(ROOTFS)/lib/
	ln -sf libc.so.1 $(ROOTFS)/lib/libc.so
	ln -sf libb1nix.a $(ROOTFS)/lib/libc.a
	ln -sf libb1nix.a $(ROOTFS)/lib/libm.a
	@mkdir -p $(ROOTFS)/usr
	ln -sfn ../include $(ROOTFS)/usr/include
	ln -sfn ../lib $(ROOTFS)/usr/lib

install: all install-headers-libs
	@echo "Installing userspace binaries..."
	@mkdir -p $(ROOTFS)/bin
	cp $(BINARIES) $(ROOTFS)/bin/
	cp $(GUI_BINARIES) $(ROOTFS)/bin/
	@mkdir -p $(ROOTFS)/home
	cp hello_native.c $(ROOTFS)/home/
	@mkdir -p $(ROOTFS)/lib/tcc/include
	cp tcc/include/*.h $(ROOTFS)/lib/tcc/include/


clean:
	rm -rf $(BUILD)

-include $(DEPS) $(GUI_OBJS:.o=.d)
