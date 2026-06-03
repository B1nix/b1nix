# B1NIX Userspace ELF ABI

b1nix has two userspace ABIs, selected by `ARCH`. The bulk of this document
describes **x86_64-b1nix** (`ARCH=x86_64`); the **i686-b1nix** (`ARCH=x86`)
32-bit variant is summarized in its own section below and detailed in
[`x86-32bit-port.md`](x86-32bit-port.md).

## Target Triple

```
x86_64-b1nix-elf   (ARCH=x86_64)
i686-b1nix-elf     (ARCH=x86)
```

## Calling Convention

Standard System V AMD64 ABI (same as Linux x86_64):
- Integer args: RDI, RSI, RDX, RCX, R8, R9
- Return value: RAX
- Callee-saved: RBX, RBP, R12-R15
- Stack aligned to 16 bytes before call

## Syscall ABI

Uses the `syscall` instruction (not `int $0x80`):
- Number: RAX (same numbers as kernel SYS_* enum)
- Args: RDI, RSI, RDX, R10 (not RCX!)
- Return: RAX
- Clobbers: RCX, R11 (standard x86_64 syscall semantics)

Syscall numbers are defined in `userspace/include/syscall.h` and
mirror the kernel's `kernel/include/b1nix/syscall.h` SYS_* enum.

## ELF Format

- Class: ELF64
- Data: Little-endian
- Type: ET_EXEC (2)
- Machine: EM_X86_64 (0x3E)
- Entry: `_start` (from crt0.S)

## Load Segments

- PT_LOAD segments with p_vaddr starting at `0x02000000` (32 MiB; set in
  `userspace/linker.ld`). PIE/ET_DYN binaries are relocated to `PIE_LOAD_BASE`
  instead (M30).
- Standard sections: .text, .rodata, .data, .bss
- No dynamic linking (static only)
- No .eh_frame or .comment sections (discarded)

## Stack Layout at _start Entry

RSP points to:
```
[auxv entries]     ← high addresses
[0]                ← AT_NULL
[AT_ENTRY, entry]  ← auxiliary vector entry
[AT_PHDR, phdr]
[0]                ← envp terminator
[envp[n-1]]        ← environment string pointers
...
[envp[0]]
[0]                ← argv terminator
[argv[argc-1]]     ← argument string pointers
...
[argv[0]]
[argc]             ← integer argument count  ← RSP points here
```

## Registered Programs vs ELF Loading

B1NIX supports two execution models:
1. **Built-in programs**: Function pointers registered via `user_register_program()`
   in the kernel and dispatched directly — used for shell, busybox, and core utils.
2. **VFS-loaded ELF**: Full ELF executables loaded from the filesystem through
   `user_load_elf64()` / `user_load_elf32()` — used for externally compiled
   programs.

The kernel tries the ELF dispatch first (elf64, then elf32); if both reject the
file, it falls back to built-in dispatch.

## i686-b1nix (32-bit, `ARCH=x86`) variant

Same ELF load base, stack layout, two-execution-models, and syscall *numbers* as
above; the differences are the calling convention and syscall mechanism:

- **Calling convention:** cdecl / SysV i386 — integer args passed on the stack;
  return value in EAX; callee-saved EBX, ESI, EDI, EBP; `%esp & 0xF == 4` at
  `_start` (i.e. 16-byte aligned *after* the implicit return-address push).
- **Syscall ABI:** the `int $0x80` instruction (not `syscall`) — number in EAX;
  args EBX, ECX, EDX, ESI, EDI, EBP. 64-bit values are passed as explicit lo/hi
  register pairs. The libc `syscall()` wrapper uses an EBP trick (a constant-0
  sixth argument) so the frame pointer stays usable.
- **ELF:** Class ELF32, Machine EM_386 (0x03), Type ET_EXEC. User segment
  selectors: CS=0x1B, SS=0x23, FS=0x38 (per-CPU), GS=0x33 (TLS).

See [`x86-32bit-port.md`](x86-32bit-port.md) for the interrupt-frame offsets and
the entry-assembly details.

## External Cross-Compilation

Use the `tools/b1nix-cc` wrapper or clang directly:

```sh
# Using the wrapper
tools/b1nix-cc hello.c -o hello

# Using clang directly
clang --target=x86_64-unknown-elf \
  -ffreestanding -fno-builtin -fno-stack-protector \
  -nostdlib -nostdinc -isystem userspace/include \
  userspace/build/crt/crt0.o hello.c \
  userspace/build/libb1nix.a \
  -T userspace/linker.ld -static -nostdlib
```

## In-Guest Compilation Status

M25 closes the early TinyCC/TCC path for the QEMU/dev baseline. `/bin/tcc`,
userspace headers, `crt0.o`, and `libb1nix.a` are packaged into initramfs, and
the smoke suite compiles and runs native programs inside B1NIX. Current coverage
includes hello output, argc/argv propagation, stderr redirection, file output,
and non-zero exit status propagation.

M26 remains the full toolchain and self-hosting milestone for Binutils, GCC,
larger programs, native `make`, and rebuilding the kernel from inside B1NIX.
