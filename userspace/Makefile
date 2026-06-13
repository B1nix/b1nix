# B1NIX Userspace Makefile
#
# Builds the userspace C library (libb1nix.a) and example programs.
# All binaries are ELF executables loadable by the B1NIX kernel.

B1NIX_ARCH ?= x86_64
ifeq ($(B1NIX_ARCH),x86)
TARGET   := i686-unknown-elf
LDFLAGS  := -m elf_i386
# Host triplet for the per-triplet ported-library install trees (must match
# tools/toolchain-env.sh: x86 -> i686-b1nix, x86_64 -> x86_64-b1nix).
B1NIX_TRIPLET := i686-b1nix
else
TARGET   := x86_64-unknown-elf
LDFLAGS  := -m elf_x86_64
B1NIX_TRIPLET := x86_64-b1nix
endif
CC       := clang
LD       := $(shell command -v ld.lld 2>/dev/null || echo /opt/homebrew/bin/ld.lld)
AR       := $(shell command -v llvm-ar 2>/dev/null || command -v /opt/homebrew/opt/llvm/bin/llvm-ar 2>/dev/null || command -v ar 2>/dev/null || echo ar)
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

LIBC_OBJS := \
	$(BUILD)/libc/string.o \
	$(BUILD)/libc/stdio.o \
	$(BUILD)/libc/stdlib.o \
	$(BUILD)/libc/unistd.o \
	$(BUILD)/libc/ctype.o \
	$(BUILD)/libc/dirent.o \
	$(BUILD)/libc/pwd.o \
	$(BUILD)/libc/pty.o \
	$(BUILD)/libc/crypt.o \
	$(BUILD)/libc/grp.o \
	$(BUILD)/libc/pthread.o \
	$(BUILD)/libc/netdb.o \
	$(BUILD)/libc/udivti3.o \
	$(BUILD)/libc/wctype.o \
	$(BUILD)/libc/wchar.o \
	$(BUILD)/libc/libgen.o \
	$(BUILD)/libc/getopt.o \
	$(BUILD)/libc/fnmatch.o \
	$(BUILD)/libc/regex.o \
	$(BUILD)/libc/mntent.o \
	$(BUILD)/libc/statvfs.o \
	$(BUILD)/libc/div64.o
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

ifeq ($(B1NIX_ARCH),x86_64)
BINARIES += $(BUILD)/bin/m30_dynamic
DYNAMIC_TARGETS := $(SHARED_LIBC)
endif

BIN_OBJS := $(patsubst $(BUILD)/bin/%,$(BUILD)/bin/%.o,$(filter-out $(BUILD)/bin/native_smoke $(BUILD)/bin/tcc $(BUILD)/bin/m30_pie $(BUILD)/bin/m30_dynamic,$(BINARIES)))
DEPS := $(LIBC_OBJS:.o=.d) $(BIN_OBJS:.o=.d) $(CRT0:.o=.d)

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
	$(AR) rcs $@ $(LIBC_OBJS)

$(GUI_LIB): $(GUI_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(GUI_OBJS)

$(SHARED_LIBC): $(PIC_LIBC_OBJS) linker_shared.ld
	$(LD) $(LDFLAGS) -shared -z norelro --hash-style=sysv -soname libc.so.1 \
		-T linker_shared.ld -o $@ $(PIC_LIBC_OBJS)

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
$(BUILD)/bin/%: bin/%.c $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I ../kernel/arch/x86_64 -c $< -o $(BUILD)/bin/$*.o
	$(LD) $(LDFLAGS) -T $(LINKER) -o $@ $(CRT0) $(BUILD)/bin/$*.o --whole-archive $(LIB) --no-whole-archive

$(GUI_BINARIES): $(BUILD)/bin/%: bin/%.c $(GUI_LIB) $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I ../kernel/arch/x86_64 -c $< -o $(BUILD)/bin/$*.o
	$(LD) $(LDFLAGS) -T $(LINKER) -o $@ $(CRT0) $(BUILD)/bin/$*.o \
		--whole-archive $(GUI_LIB) $(LIB) --no-whole-archive

# M49: upstream libwayland-client, with libffi for protocol dispatch.
WAYLAND_DIR := ../build/wayland-b1nix/$(B1NIX_TRIPLET)/install
$(WAYLAND_DIR)/lib/libwayland-client.a: ../tools/build-wayland.sh ../tools/build-libffi.sh \
		../tools/wayland/os.c ../tools/wayland/compat.c ../tools/wayland/compat.h
	B1NIX_ARCH=$(B1NIX_ARCH) ../tools/build-wayland.sh >/dev/null
$(WAYLAND_DIR)/lib/libwayland-server.a: $(WAYLAND_DIR)/lib/libwayland-client.a

$(BUILD)/bin/m49_libwayland: bin/m49_libwayland.c $(LIB) $(CRT0) $(LINKER) \
		$(WAYLAND_DIR)/lib/libwayland-client.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(WAYLAND_DIR)/include -c $< -o $(BUILD)/bin/m49_libwayland.o
	$(LD) $(LDFLAGS) -T $(LINKER) -o $@ $(CRT0) $(BUILD)/bin/m49_libwayland.o \
		$(WAYLAND_DIR)/lib/libwayland-client.a $(WAYLAND_DIR)/lib/libffi.a \
		--whole-archive $(LIB) --no-whole-archive

$(BUILD)/bin/m49_libwayland_server: bin/m49_libwayland_server.c $(LIB) $(CRT0) \
		$(LINKER) $(WAYLAND_DIR)/lib/libwayland-server.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(WAYLAND_DIR)/include -c $< -o $(BUILD)/bin/m49_libwayland_server.o
	$(LD) $(LDFLAGS) -T $(LINKER) -o $@ $(CRT0) $(BUILD)/bin/m49_libwayland_server.o \
		$(WAYLAND_DIR)/lib/libwayland-server.a $(WAYLAND_DIR)/lib/libffi.a \
		--whole-archive $(LIB) --no-whole-archive

$(BUILD)/bin/native_smoke: bin/native_smoke.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $(BUILD)/bin/native_smoke.o
	$(LD) $(LDFLAGS) -T $(LINKER) -o $@ $(BUILD)/bin/native_smoke.o

# m32_nettool gains an mbedTLS loopback HTTPS server when the static mbedTLS
# archives are present (built by tools/build-curl.sh with B1NIX_TLS=mbedtls).
# Without them it builds HTTP-only so a `B1NIX_TLS=none` build still works.
MBEDTLS_DIR := ../build/mbedtls-b1nix/$(B1NIX_TRIPLET)/install
$(BUILD)/bin/m32_nettool: bin/m32_nettool.c $(LIB) $(CRT0) $(LINKER) $(wildcard $(MBEDTLS_DIR)/lib/libmbed*.a)
	@mkdir -p $(dir $@)
	@if [ -f $(MBEDTLS_DIR)/lib/libmbedtls.a ]; then \
	  echo "  CC+TLS  m32_nettool"; \
	  $(CC) $(CFLAGS) -DB1NIX_HAVE_MBEDTLS -I $(MBEDTLS_DIR)/include -c $< -o $(BUILD)/bin/m32_nettool.o; \
	  $(LD) $(LDFLAGS) -T $(LINKER) -o $@ $(CRT0) $(BUILD)/bin/m32_nettool.o \
	    $(MBEDTLS_DIR)/lib/libmbedtls.a $(MBEDTLS_DIR)/lib/libmbedx509.a $(MBEDTLS_DIR)/lib/libmbedcrypto.a \
	    --whole-archive $(LIB) --no-whole-archive; \
	else \
	  echo "  CC      m32_nettool (HTTP-only, no mbedTLS)"; \
	  $(CC) $(CFLAGS) -c $< -o $(BUILD)/bin/m32_nettool.o; \
	  $(LD) $(LDFLAGS) -T $(LINKER) -o $@ $(CRT0) $(BUILD)/bin/m32_nettool.o --whole-archive $(LIB) --no-whole-archive; \
	fi


# m32_pcre2_smoke links the ported static PCRE2 (8-bit) library.
PCRE2_DIR := ../build/pcre2-b1nix/$(B1NIX_TRIPLET)/install
$(BUILD)/bin/m32_pcre2_smoke: bin/m32_pcre2_smoke.c $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DPCRE2_STATIC -I $(PCRE2_DIR)/include -c $< -o $(BUILD)/bin/m32_pcre2_smoke.o
	$(LD) $(LDFLAGS) -T $(LINKER) -o $@ $(CRT0) $(BUILD)/bin/m32_pcre2_smoke.o \
	  $(PCRE2_DIR)/lib/libpcre2-8.a --whole-archive $(LIB) --no-whole-archive

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

$(BUILD)/bin/tcc: tcc/tcc.c $(LIB) $(CRT0)
	@mkdir -p $(dir $@)
	cd tcc && test -x c2str.exe && ./c2str.exe include/tccdefs.h tccdefs_.h
ifeq ($(B1NIX_ARCH),x86)
	B1NIX_ARCH=$(B1NIX_ARCH) ../tools/b1nix-cc tcc/tcc.c -o $@ -DTCC_TARGET_I386 -DCONFIG_TCCDIR="\"/lib/tcc\"" -DCONFIG_TCC_SYSINCLUDEPATHS="\"/include:/lib/tcc/include\"" -DCONFIG_TCC_LIBPATHS="\"/lib\"" -DCONFIG_TCC_CRTPREFIX="\"/lib\""
else
	B1NIX_ARCH=$(B1NIX_ARCH) ../tools/b1nix-cc tcc/tcc.c -o $@ -DTCC_TARGET_X86_64 -DCONFIG_TCCDIR="\"/lib/tcc\"" -DCONFIG_TCC_SYSINCLUDEPATHS="\"/include:/lib/tcc/include\"" -DCONFIG_TCC_LIBPATHS="\"/lib\"" -DCONFIG_TCC_CRTPREFIX="\"/lib\""
endif

# ── Install to rootfs ──
install-headers-libs: $(LIB) $(GUI_LIB) $(CRT0)
	@echo "Installing userspace headers and libraries..."
	@mkdir -p $(ROOTFS)/include
	@mkdir -p $(ROOTFS)/lib
	cp -r include/* $(ROOTFS)/include/
	cp $(LIB) $(ROOTFS)/lib/
	cp $(GUI_LIB) $(ROOTFS)/lib/
	cp $(CRT0) $(ROOTFS)/lib/
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
