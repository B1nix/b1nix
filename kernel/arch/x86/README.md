# x86 32-bit port scaffold

This directory is reserved for the future 32-bit IA-32 kernel port.

The existing implementation was moved to `kernel/arch/x86_64` because it boots
and links as a long-mode `x86_64-elf` kernel. `ARCH=x86` is intentionally kept
non-buildable until the 32-bit boot path, ABI, paging, interrupt frame, and
userspace/toolchain choices are implemented.

Suggested first milestones:

- Add a 32-bit boot path and linker script.
- Define the 32-bit interrupt/trap frame and syscall ABI.
- Split or share common x86 device helpers with `kernel/arch/x86_64`.
- Add `ARCH=x86` Makefile sources once the port can at least link a kernel.
