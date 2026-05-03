# AArch64 Port Placeholder

The first implementation target is x86 via Multiboot2. A minimal AArch64 QEMU
`virt` boot path exists and writes to the PL011 UART:

```sh
make ARCH=aarch64
make run-aarch64
```

The full AArch64 kernel port should keep the same public architecture boundary as x86:

- early boot entry;
- exception vector setup;
- generic timer;
- GIC interrupt controller;
- MMU setup;
- context switching.

The shared kernel should not depend on x86-specific concepts such as VGA, GDT,
IDT, or port I/O.
