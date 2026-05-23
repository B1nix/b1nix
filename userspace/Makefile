# B1NIX Userspace Makefile
#
# Builds the userspace C library (libb1nix.a) and example programs.
# All binaries are ELF executables loadable by the B1NIX kernel.

CC       := clang
TARGET   := x86_64-unknown-elf
LD       := $(shell command -v ld.lld 2>/dev/null || echo /opt/homebrew/bin/ld.lld)
AR       := $(shell command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)
CFLAGS   := --target=$(TARGET) -ffreestanding -fno-builtin -fno-stack-protector -nostdinc -isystem include -I include -Wall -Wextra -O2 -msoft-float
LDFLAGS  := -m elf_x86_64
BUILD    := build
LIB      := $(BUILD)/libb1nix.a
CRT0     := $(BUILD)/crt/crt0.o
LINKER   := linker.ld

LIBC_OBJS := \
	$(BUILD)/libc/string.o \
	$(BUILD)/libc/stdio.o \
	$(BUILD)/libc/stdlib.o \
	$(BUILD)/libc/unistd.o

BINARIES := \
	$(BUILD)/bin/hello \
	$(BUILD)/bin/native_smoke \
	$(BUILD)/bin/m12_smoke \
	$(BUILD)/bin/m13_smoke \
	$(BUILD)/bin/m14_smoke \
	$(BUILD)/bin/m15_smoke \
	$(BUILD)/bin/tcc \
	$(BUILD)/bin/test_sync

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
$(BUILD)/bin/%: bin/%.c $(LIB) $(CRT0)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $(BUILD)/bin/$*.o
	$(LD) $(LDFLAGS) -T $(LINKER) -o $@ $(CRT0) $(BUILD)/bin/$*.o --whole-archive $(LIB) --no-whole-archive

$(BUILD)/bin/native_smoke: bin/native_smoke.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $(BUILD)/bin/native_smoke.o
	$(LD) $(LDFLAGS) -T $(LINKER) -o $@ $(BUILD)/bin/native_smoke.o

$(BUILD)/bin/tcc: tcc/tcc.c $(LIB) $(CRT0)
	@mkdir -p $(dir $@)
	cd tcc && clang -DC2STR conftest.c -o c2str.exe && ./c2str.exe include/tccdefs.h tccdefs_.h
	../tools/b1nix-cc tcc/tcc.c -o $@ -DTCC_TARGET_X86_64 -DCONFIG_TCCDIR="\"/lib/tcc\""

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
	@mkdir -p ../build/x86/rootfs/home
	cp hello_native.c ../build/x86/rootfs/home/
	@mkdir -p ../build/x86/rootfs/lib/tcc/include
	cp tcc/include/*.h ../build/x86/rootfs/lib/tcc/include/

clean:
	rm -rf $(BUILD)
