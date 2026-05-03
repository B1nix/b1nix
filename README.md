# tinyunix

A small Unix-like monolithic kernel experiment in C.

The current target is a portable kernel core with architecture-specific boot
and CPU code isolated under `kernel/arch`. The first boot path is x86_64 via a
Multiboot2-compatible loader, with AArch64 reserved in the tree for the next
port.

## Goals

- Monolithic kernel in C11 with small assembly entry points.
- Portable core for memory, scheduling, files, networking, and syscalls.
- x86_64 first, AArch64 next.
- QEMU-first development, real hardware later.
- VirtIO-first drivers for disk, network, and eventually GPU.

## Current Status

- Freestanding C kernel scaffold.
- x86_64 Multiboot2 header and early bootstrap assembly.
- VGA text console and serial logging.
- Panic/assert/log basics.
- Roadmap in `docs/roadmap.md`.

## Build

```sh
make
```

This produces:

```text
build/x86/kernel.elf
```

## Run

The run target expects `qemu-system-x86_64` and `grub-mkrescue` to be installed:

```sh
make run-x86
```

If those tools are missing, the kernel can still be built and inspected.
See `docs/toolchain.md` for macOS setup notes.

For a headless smoke test, this is the QEMU shape used during development:

```sh
qemu-system-x86_64 -cdrom build/x86/tinyunix.iso -serial stdio -display none -monitor none -no-reboot
```

The minimal AArch64 QEMU `virt` boot path can be built and run with:

```sh
make ARCH=aarch64
make run-aarch64
```

## Layout

```text
kernel/
  arch/
    x86/
    aarch64/
  include/
  lib/
  main.c
docs/
  roadmap.md
```
