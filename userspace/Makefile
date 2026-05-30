# B1NIX Userspace Makefile
#
# Builds the userspace C library (libb1nix.a) and example programs.
# All binaries are ELF executables loadable by the B1NIX kernel.

CC       := clang
TARGET   := x86_64-unknown-elf
LD       := $(shell command -v ld.lld 2>/dev/null || echo /opt/homebrew/bin/ld.lld)
AR       := $(shell command -v llvm-ar 2>/dev/null || command -v /opt/homebrew/opt/llvm/bin/llvm-ar 2>/dev/null || command -v ar 2>/dev/null || echo ar)
CFLAGS   := --target=$(TARGET) -ffreestanding -fno-builtin -fno-stack-protector -nostdinc -isystem include -I include -Wall -Wextra -O2 -msoft-float -mno-implicit-float -Db1nix -MMD -MP
LDFLAGS  := -m elf_x86_64
BUILD    := build
LIB      := $(BUILD)/libb1nix.a
CRT0     := $(BUILD)/crt/crt0.o
LINKER   := linker.ld

LIBC_OBJS := \
	$(BUILD)/libc/string.o \
	$(BUILD)/libc/stdio.o \
	$(BUILD)/libc/stdlib.o \
	$(BUILD)/libc/unistd.o \
	$(BUILD)/libc/ctype.o \
	$(BUILD)/libc/dirent.o \
	$(BUILD)/libc/pwd.o \
	$(BUILD)/libc/pthread.o

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
	$(BUILD)/bin/m30_pie

BIN_OBJS := $(patsubst $(BUILD)/bin/%,$(BUILD)/bin/%.o,$(filter-out $(BUILD)/bin/native_smoke $(BUILD)/bin/tcc $(BUILD)/bin/m30_pie,$(BINARIES)))
DEPS := $(LIBC_OBJS:.o=.d) $(BIN_OBJS:.o=.d) $(CRT0:.o=.d)

BIN_OBJS := $(patsubst $(BUILD)/bin/%,$(BUILD)/bin/%.o,$(filter-out $(BUILD)/bin/native_smoke $(BUILD)/bin/tcc $(BUILD)/bin/m30_pie,$(BINARIES)))
DEPS := $(LIBC_OBJS:.o=.d) $(BIN_OBJS:.o=.d) $(CRT0:.o=.d)

.PHONY: all clean install

all: $(LIB) $(BINARIES)

# ── crt0 ──
$(BUILD)/crt/crt0.o: crt/crt0.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ── libc objects ──
$(BUILD)/libc/%.o: libc/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ── libb1nix.a ──
$(LIB): $(LIBC_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(LIBC_OBJS)

# ── User programs ──
$(BUILD)/bin/%: bin/%.c $(LIB) $(CRT0) $(LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $(BUILD)/bin/$*.o
	$(LD) $(LDFLAGS) -T $(LINKER) -o $@ $(CRT0) $(BUILD)/bin/$*.o --whole-archive $(LIB) --no-whole-archive

$(BUILD)/bin/native_smoke: bin/native_smoke.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $(BUILD)/bin/native_smoke.o
	$(LD) $(LDFLAGS) -T $(LINKER) -o $@ $(BUILD)/bin/native_smoke.o

# M30: PIE smoke binary. Compiled with -fPIC and linked with -pie so the
# linker emits ET_DYN with R_X86_64_RELATIVE relocations. No crt0 or
# libb1nix.a — the body is self-contained inline-asm syscalls; the
# kernel's PIE-aware loader takes care of base relocation.
$(BUILD)/bin/m30_pie: bin/m30_pie.c linker_pie.ld
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c $< -o $(BUILD)/bin/m30_pie.o
	$(LD) $(LDFLAGS) -pie -T linker_pie.ld -o $@ $(BUILD)/bin/m30_pie.o

$(BUILD)/bin/tcc: tcc/tcc.c $(LIB) $(CRT0)
	@mkdir -p $(dir $@)
	cd tcc && clang -DC2STR conftest.c -o c2str.exe && ./c2str.exe include/tccdefs.h tccdefs_.h
	../tools/b1nix-cc tcc/tcc.c -o $@ -DTCC_TARGET_X86_64 -DCONFIG_TCCDIR="\"/lib/tcc\"" -DCONFIG_TCC_SYSINCLUDEPATHS="\"/include:/lib/tcc/include\"" -DCONFIG_TCC_LIBPATHS="\"/lib\"" -DCONFIG_TCC_CRTPREFIX="\"/lib\""

# ── Install to rootfs ──
install: all
	@echo "Installing userspace binaries..."
	@mkdir -p ../build/x86/rootfs/bin
	@mkdir -p ../build/x86/rootfs/include
	@mkdir -p ../build/x86/rootfs/lib
	cp $(BINARIES) ../build/x86/rootfs/bin/
	cp -r include/* ../build/x86/rootfs/include/
	cp $(LIB) ../build/x86/rootfs/lib/
	cp $(CRT0) ../build/x86/rootfs/lib/
	ln -sf libb1nix.a ../build/x86/rootfs/lib/libc.a
	ln -sf libb1nix.a ../build/x86/rootfs/lib/libm.a
	@mkdir -p ../build/x86/rootfs/usr
	ln -sfn ../include ../build/x86/rootfs/usr/include
	ln -sfn ../lib ../build/x86/rootfs/usr/lib
	@mkdir -p ../build/x86/rootfs/home
	cp hello_native.c ../build/x86/rootfs/home/
	@mkdir -p ../build/x86/rootfs/lib/tcc/include
	cp tcc/include/*.h ../build/x86/rootfs/lib/tcc/include/

clean:
	rm -rf $(BUILD)

-include $(DEPS)
