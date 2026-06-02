# tinyunix

A small Unix-like monolithic kernel experiment in C.

The current target is a portable kernel core with architecture-specific boot
and CPU code isolated under `kernel/arch`. The first boot path is x86_64 via a
Multiboot2-compatible loader. The old AArch64 experiment is archived under
`archive/kernel/arch/aarch64` and is not part of the active build.

## Goals

- Monolithic kernel in C11 with small assembly entry points.
- Portable core for memory, scheduling, files, networking, and syscalls.
- x86_64 first.
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
build/x86_64/kernel.elf
```

## Run

The run target expects `qemu-system-x86_64` and `grub-mkrescue` to be installed:

```sh
make run-x86_64
```

If those tools are missing, the kernel can still be built and inspected.
See `docs/toolchain.md` for macOS setup notes.

For a headless smoke test, this is the QEMU shape used during development:

```sh
qemu-system-x86_64 -cdrom build/x86_64/tinyunix.iso -serial stdio -display none -monitor none -no-reboot
```

## Layout

```text
kernel/
  arch/
    x86/       # reserved 32-bit scaffold
    x86_64/    # active 64-bit port
  include/
  lib/
  main.c
archive/
  kernel/
    arch/
      aarch64/
docs/
  roadmap.md
```
