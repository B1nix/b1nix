# B1NIX Userspace ELF ABI

## Target Triple

```
x86_64-b1nix-elf
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

- PT_LOAD segments with p_vaddr starting at 0x400000
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
2. **VFS-loaded ELF**: Full ELF64 executables loaded from the filesystem through
   `user_load_elf64()` — used for externally compiled programs.

The kernel tries ELF loading first; if it fails, it falls back to built-in dispatch.

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
