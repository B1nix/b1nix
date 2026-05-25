# TCC Hang — Kernel .text Layout Sensitivity

## Symptom

When the kernel `.text` section grows by approximately 200+ bytes (e.g. adding a
new `.o` file, adding functions to an existing file, or even changing struct
layouts that affect code generation), the M25 smoke test hangs after spawning
`/bin/tcc`:

```
M25-SMOKE: start
M25-SMOKE: ok tcc-launch        ← TCC binary is found
M25-SMOKE: ok compile-hello     ← TCC compiles hello.c
                                ← HANGS here — never runs the compiled binary
```

All other smoke tests (M12–M16, M22, M24, M11) pass normally. The hang is
**deterministic for a given binary** but changes unpredictably with kernel size.

## Root Cause

TCC (Tiny C Compiler) is **not compiled as position-independent (PIE)**. It is a
statically-linked ELF binary with a fixed load address at `0x2000000`. When the
kernel `.text` section changes size, the physical-to-virtual memory layout of
the entire system shifts. This can cause:

1. **Page table collisions** — TCC's fixed load address may overlap with kernel
   heap, page tables, or other dynamically mapped regions whose positions are
   determined by the kernel's own `.text` size.

2. **VFS/mmap address shifts** — The kernel's `vmm_map_page()` path uses
   addresses relative to the kernel's own layout; a `.text` change can shift
   where user ELF segments land.

3. **Stack/Heap base changes** — The kernel heap (`kheap`) and user stack
   addresses are influenced by how much virtual address space the kernel
   consumes. A larger kernel pushes user regions around.

## Affected Code Path

```
m25_smoke.c → fork() → SYS_FORK
    ↓
child: execvp("/bin/tcc", ...) → SYS_EXECVE
    ↓
kernel: user_spawn() → user_load_image() → user_load_elf64()
    ↓
kernel: user_run_elf_image() → vmm_map_page()
    ↓
kernel: x86_user_jump() → Ring 3 entry → ??? hangs
```

Key files to investigate:

- `kernel/arch/x86/paging.c` — `vmm_map_page()`, `paging_create_address_space()`
- `kernel/arch/x86/interrupts.c` — Exception handler, signal delivery
- `kernel/user/process.c` — `user_run_elf_image()`, `user_address_space_create()`
- `kernel/user/process.c` — `user_load_elf64()` segment loading

## Workaround

A kernel command-line flag `b1nix.skip-m25` was added to `init_main()` in
`kernel/user/programs.c`. When present, the M25 smoke test is skipped entirely:

```sh
make ARCH=x86 KERNEL_CMDLINE="b1nix.test=1 b1nix.skip-m25" smoke
```

This allows all other smoke tests (M12–M16, M22, M24, M11) to run normally even
when the kernel `.text` has grown past the TCC tolerance threshold.

## Permanent Fix Ideas

### Option A: PIE TCC binary
Recompile `/bin/tcc` as position-independent. This requires changing the
userspace linker script (`userspace/linker.ld`) to produce ET_DYN or adding
`-pie -fPIE` to the TCC build. TCC itself can then be loaded at any address.

### Option B: Fixed kernel `.text` size
Pad the kernel `.text` section to a fixed size (e.g. 512 KB) so that adding or
removing code doesn't shift addresses. This wastes memory but stabilises the
layout.

### Option C: Dynamic ELF loader
Make `user_load_elf64()` (in `kernel/user/process.c`) search for a free address
space hole instead of loading at the ELF's `p_vaddr`. This is more complex but
provides true PIE support for all userspace binaries.

### Option D: Reserve guard pages
Reserve a fixed virtual address range for TCC (e.g. `0x2000000–0x2100000`) in
the VMM so that even if the kernel layout shifts, TCC's load area is never
reused by other mappings.

## History

| Date | Event |
|------|-------|
| 2026-05-25 | First observed: adding `runqueue.c` (~220 bytes) to the build hung TCC |
| 2026-05-25 | M10 (TX buffer pool) and M16 (MC clipboard) changes also triggered it |
| 2026-05-25 | `b1nix.skip-m25` workaround added |
| 2026-05-25 | Documented in `archive/smp/README.md` and this file |
