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

## Permanent Fix (Applied)

### ✅ Option B: Fixed kernel `.text` size (256 KB alignment)
Added `. = ALIGN(256K)` after the `.text` section in `kernel/arch/x86/linker.ld`.
This pads the `.text` section to the nearest 256 KB boundary, so adding or
removing up to 256 KB of code never shifts subsequent sections. No memory is
wasted inside the kernel binary — the alignment adds zero-fill padding that the
ELF loader handles naturally.

**Result:** M25 passes, B1NIX-TEST: done, no more intermittent crashes at M22-POLISH.

## History

| Date | Event |
|------|-------|
| 2026-05-25 | First observed: adding `runqueue.c` (~220 bytes) to the build hung TCC |
| 2026-05-25 | M10 (TX buffer pool) and M16 (MC clipboard) changes also triggered it |
| 2026-05-25 | `b1nix.skip-m25` workaround added |
| 2026-05-26 | **Fixed:** `. = ALIGN(256K)` in `kernel/arch/x86/linker.ld` — stabilises .text layout, TCC passes, B1NIX-TEST: done |
| 2026-05-26 | Fixed: changed userspace load address from `0x2000000` to `0x400000` in `userspace/linker.ld`. The old address conflicted with kernel page table allocations when kernel `.text` grew past a certain threshold. `0x400000` is the standard Linux userspace base and avoids the collision zone. |
