# x86 32-bit Architecture Port (`ARCH=x86`)

This document describes all changes made to port b1nix from x86_64-only to also support genuine 32-bit x86 (`ARCH=x86`), including kernel, userspace, and boot infrastructure.

---

## Overview

The goal was to create a fully functional 32-bit kernel and userspace that:
- Builds with `make ARCH=x86`
- Boots in QEMU (`qemu-system-x86_64` with a 32-bit ISO)
- Runs all builtin programs via the kernel's builtin scheduler
- Passes the full `tests/smoke.sh x86` smoke test suite

The port is organized into separate `kernel/arch/x86/` and `userspace/` directories, with conditional compilation guarded by `__x86_64__` vs the absence of that macro.

---

## Architecture Decisions

### Calling Convention
- 32-bit x86 uses the **SysV i386 ABI**: arguments are passed **on the stack**, not in registers.
- Syscalls use `int $0x80` with: `eax`=number, `ebx`=arg0, `ecx`=arg1, `edx`=arg2, `esi`=arg3, `edi`=arg4, `ebp`=arg5.
- `syscall_dispatch_impl` still takes 64-bit arguments; on 32-bit, we push zero-extended 64-bit pairs onto the stack.

### Address Space Layout (32-bit)
| Region             | Range                      |
|--------------------|----------------------------|
| User space         | `0x00000000 – 0xBFFFFFFF`  |
| Direct map (kernel)| `0x80000000 – 0x90000000`  |
| Kernel heap        | `0xC0000000 – ...`         |
| MMIO map           | `0xD0000000 – 0xDFFFFFFF`  |

### Page Tables
- 32-bit uses **2-level paging** (PD + PT), each with 1024 × 4-byte entries.
- 4MB PSE huge pages cover the direct-map and identity-map regions.
- `kernel_pml4_virt` is aliased to the 32-bit Page Directory.

---

## New Files (`kernel/arch/x86/`)

All files were written from scratch for the 32-bit target:

| File | Purpose |
|------|---------|
| `boot.S` | Multiboot2 header, boot stack, initial PD setup, PSE+paging enable, GDT load, higher-half jump |
| `linker.ld` | 32-bit kernel link script (`.text`, `.rodata`, `.data`, `.bss` at 1 MiB) |
| `arch.c` | GDT setup, TSS, percpu FS segment, arch init entry point |
| `console.c` | VGA text-mode console (80×25), with fallback to framebuffer |
| `fb_console.c` | Framebuffer console write (same as x86_64 version, re-implemented for 32-bit) |
| `serial.c` | COM1 UART serial output |
| `interrupts.c` | IDT setup, exception/IRQ handlers, PIC masking, signal delivery |
| `isr.S` | ISR stubs for all 256 vectors; saves full interrupt_frame; reloads/restores segment registers |
| `syscall_entry.S` | `int $0x80` handler; zero-extends register args to 64-bit pairs; calls `syscall_dispatch_impl` |
| `user_jump.S` | `x86_user_jump(entry, stack, argc, argv)`: sets up iret frame, loads user segments (0x1B/0x23/0x33), clears GPRs, executes iret |
| `context_switch.S` | `context_switch_asm(from_ctx, to_ctx)`: saves/restores `ebp,ebx,esi,edi,esp` |
| `paging.c` | VMM: 2-level PD/PT, huge-page direct map, `vmm_init`, `vmm_map_page`, `paging_create_address_space`, COW fork |
| `lapic.c` | Local APIC init, LAPIC timer calibration against PIT, IPI send |
| `io.c` | `inb/outb/inw/outw/inl/outl` |
| `rtc.c` | RTC read (CMOS) |
| `signal.c` | Signal frame construction on user stack; `sys_sigreturn` |
| `tlb.c` | TLB shootdown via IPI |
| `coredump.c` | ELF core dump writer (ET_CORE, PT_NOTE with prstatus) |
| `gdbstub.c` | GDB RSP stub: register file read/write, memory read, stop reply |
| `ap_trampoline.S` | Application Processor 16-bit → 32-bit trampoline (INIT-SIPI sequence) |
| `fpu.S` | FPU/SSE state init (`arch_fpu_init_current`) |

---

## Modified Files

### Build System

**`Makefile`**
- Added `ARCH=x86` target: sets `CC_TARGET=i686-elf`, `LD_EMUL=elf_i386`, `-m32 -mno-sse -mno-mmx` flags.
- Selects `kernel/arch/x86/` source files when `ARCH=x86`.
- Pre-builds `userspace/lib/libc.a` and `crt0.o` sequentially before parallel make (fixes autotools race condition).

**`userspace/Makefile`**
- Supports `B1NIX_ARCH=x86`: selects `i686-unknown-elf` target, `-m elf_i386` linker flag.
- Generates 32-bit `crt0.o`, `libc.a`, and ELF32 binaries.

**`tools/b1nix-cc`**
- Passes `--target=i686-elf -m32` when `B1NIX_ARCH=x86`.

**Per-architecture toolchain & port directories**
- `tools/toolchain-env.sh` (new) is the single source of truth mapping
  `B1NIX_ARCH` → host triplet (`x86` → `i686-b1nix`, `x86_64` → `x86_64-b1nix`)
  and per-triplet build paths. Every toolchain/port build script sources it.
- Cross + native toolchains build under `build/toolchain_build/<triplet>/`
  (`cross`, `native_root`, `native_build`, `src`, `sysroot`); source tarballs are
  cached once in the shared `build/toolchain_build/dist/`. The cross compilers are
  necessarily per-triplet (different target backends, `--disable-multilib`).
- Ported programs (curl, wget, pcre2, openssl, mbedtls, dropbear, libidn2,
  libunistring) build under `build/<prog>-{src,b1nix}/<triplet>/` so x86 and
  x86_64 never share objects. `tools/patch-gcc.py` now emits both the
  `x86_64-*-b1nix*` and `i[34567]86-*-b1nix*` GCC target cases.
- See `docs/toolchain.md` → "Per-architecture toolchain & port layout".

---

### Core Headers

**`kernel/include/b1nix/arch.h`**
- `usize` = `u32` on 32-bit, `u64` on 64-bit.
- `isize` = `i32` on 32-bit.
- `get_percpu()` uses `FS` segment on both architectures.

**`kernel/include/b1nix/mm.h`**
```c
#ifdef __x86_64__
#define DIRECT_MAP_BASE 0xffff800000000000ULL
#define KHEAP_START     0xffff900000000000ULL
#else
#define DIRECT_MAP_BASE 0x80000000ULL
#define DIRECT_MAP_MAX  (1024ULL * 1024ULL * 1024ULL)   // 1 GB
#define KHEAP_START     0xc0000000ULL
#endif
```

**`kernel/include/b1nix/user.h`**
```c
#ifdef __x86_64__
#define USER_SPACE_LIMIT 0x0000800000000000ULL
#define USER_STACK_TOP   0x0000800000000000ULL
#else
#define USER_SPACE_LIMIT 0xC0000000ULL
#define USER_STACK_TOP   0xC0000000ULL
#endif
```

**`kernel/include/b1nix/sched.h`**
- `struct cpu_context` has `{rsp,rbp,...}` on 64-bit and `{esp,ebp,ebx,esi,edi}` on 32-bit.
- `task->pml4_phys` is `u64` on 64-bit, `u32` on 32-bit.

**`kernel/include/b1nix/lapic.h`**
- Guard for 64-bit-only TLS helpers (`get_fs_base` / `set_fs_base` via `rdfsbase`/`wrfsbase`).

**`kernel/include/b1nix/arch_x86.h`**
- `struct interrupt_frame` for 32-bit: all registers are `u32`, with `union { u32 eax; u32 rax; }` aliases for shared code compatibility.

---

### Scheduler & Process Management

**`kernel/sched/scheduler.c`**
- `kthread_create_impl`: sets `initial_rsp = stack_top - 8` (32-bit) vs `stack_top - 16` (64-bit) for correct ABI stack alignment.
- Pushes trampoline address as `u32` on 32-bit, `u64` on 64-bit.
- Context saved/restored as `esp/ebp/ebx/esi/edi` on 32-bit.
- `clone_thread_impl` (fork path): same `initial_rsp - 8` alignment.
- Added debug logs in `scheduler_set_user_image` (temporary, for debugging).

**`kernel/user/process.c`**

Key changes:
1. **`x86_user_jump` signature**: uses `usize` (32-bit: `u32`) instead of `u64`.
2. **`PIE_LOAD_BASE`**: `0x0000500000000000` on 64-bit, `0x40000000` on 32-bit.
3. **`user_stack_push`**: pushes `usize`-sized values (4 bytes on 32-bit, 8 bytes on 64-bit) when building the initial user stack layout.
4. **`user_build_initial_stack`**: `argc`, `argv[]`, `envp[]`, `auxv[]` entries are all pushed as `usize` (pointer-sized), matching what `crt0.S` expects.
5. **`user_load_elf32`** (new): Full ELF32 (i386) loader, supporting `ET_EXEC` and `ET_DYN`. Validates `ELF_CLASS_32`, `EM_386`, and applies `R_386_RELATIVE` relocations for PIE binaries.
6. **`user_load_image`**: Falls back from ELF64 → ELF32 → BUILTIN.
7. **`ap_runnable`**: ELF32 processes set `ap_runnable = 1` (same as ELF64); builtins stay on BSP.
8. Address space limit checks use `USER_SPACE_LIMIT` instead of the hardcoded `0x00007FFFFFFFFFFFULL`.

**`kernel/user/programs.c`**
- Added `#include <b1nix/console.h>` for direct `console_write` access in `init_main`.
- Added temporary debug prints in `init_main` and `user_process_thread` to trace startup.

---

### Paging / VMM (`kernel/arch/x86/paging.c`)

**`paging_create_address_space`**
- Allocates a new Page Directory.
- Clones kernel-half entries (PD index 512–1023, i.e., virtual `0x80000000` and above).
- **Also clones the kernel's identity map entries** (PD index 0 to `DIRECT_MAP_SIZE >> 22`), so that the kernel remains accessible after loading the process's PD into CR3. Without this clone, writing CR3 immediately caused a triple-fault because the kernel's code/data (at physical 1 MiB, mapped via identity pages) was no longer accessible.

---

### Syscall Layer

**`kernel/syscall/syscall.c`**
- `is_canonical(addr)`: on 32-bit, checks `(addr >> 32) == 0` instead of the 47-bit sign-extension check.
- Memory range checks use `USER_SPACE_LIMIT` constant.

**`kernel/mm/kheap.c`**
- `is_canonical_addr`: same fix as above.

---

### ISR / Syscall Entry Assembly

**`kernel/arch/x86/isr.S`**
- Each ISR stub pushes `error_code` (0 or real) and `vector`.
- Saves full GPR set (`eax, ebx, ecx, edx, ebp, edi, esi`).
- Reloads kernel segment registers (`ds/es/gs = 0x10`, `fs = 0x38` for percpu).
- Calls `x86_irq_handler(frame)` or `x86_exception_handler(frame)`.
- On return: checks RPL of saved CS; if 3, restores user segments (`ds/es/fs = 0x23`, `gs = 0x33`); then `iret`.

**`kernel/arch/x86/syscall_entry.S`**
- `x86_syscall_entry`: saves registers, reloads kernel segments, zero-extends 32-bit register values to 64-bit pairs on the stack, calls `syscall_dispatch_impl`, restores registers.
- `x86_syscall_return`: restores saved EAX (return value), restores user segments on ring-3 return, executes `iret`.
- `x86_fork_child_trampoline` / `x86_fork_kernel_trampoline`: fork child return paths.

---

### Userspace (`userspace/`)

**`userspace/crt/crt0.S`**
- 32-bit `_start`: reads `argc`/`argv[]`/`envp[]` from the stack as 32-bit words.
- Calls `__init_array` constructors with 16-byte stack alignment (sub $12, %esp before calling `*(%ecx)`).
- Tail-calls `main(argc, argv, envp)` via `call`.
- Falls through to `exit(eax)`.

**`userspace/include/syscall.h`**
```c
#ifdef __i386__
  register long ebx __asm__("ebx") = a0;
  // ...
  __asm__ volatile("int $0x80" : "=a"(ret) : ... : "memory");
#else
  // x86_64 syscall path
#endif
```

**`userspace/bin/native_smoke.S`**
- 32-bit `int $0x80`-based SYS_WRITE + SYS_EXIT calls.

**`userspace/bin/m29_smoke.c`**
- Inline asm for `int $0x80` on 32-bit for TLS (`SYS_SET_TLS`).

**`userspace/bin/m30_pie.c`**
- 32-bit inline asm for PIE smoke test.

---

### Misc Library Fixes

**`kernel/mm/pmm.c`**
- `pmm_init`: uses `usize` for frame counts/bitmap sizes to avoid overflow on 32-bit.

**`kernel/net/tcp.c`**
- Sequence number arithmetic uses `u32` not `usize`.

**`kernel/lib/string.c`**, **`stdlib.c`**
- `memset`/`memcpy` use `unsigned char *` / `usize` byte counters.

---

## Key Bug Fixes

### Triple Fault After `paging_switch_address_space`

**Root Cause**: `paging_create_address_space` only cloned the kernel's high-half entries (PD index 512–1023, addresses ≥ `0x80000000`). After loading the new PD into CR3, execution continued at the kernel's physical address (≈1 MiB = PD index 0), which was now unmapped.

**Fix**: Also clone PD index 0 through `DIRECT_MAP_SIZE >> 22` (the identity-mapped region) into the new address space.

### Builtin Programs Calling Kernel-Mode `syscall_dispatch`

**Observation**: Builtin programs (like `init_main`) call `syscall_dispatch(SYS_CLEAR, ...)` directly as C function calls. On 32-bit, arguments larger than `u32` must be split into 64-bit pairs on the stack. The `syscall_entry.S` correctly handles this for `int $0x80`, and `syscall.h`'s `SYSCALL_DISPATCH_X` macro works correctly for direct C calls.

### Stack Alignment for 32-bit Kernel Threads

**Root Cause**: `kthread_create_impl` placed `initial_rsp = stack_top - 16` (matching 64-bit ABI). On 32-bit, the return address is 4 bytes, so `initial_rsp = stack_top - 8` gives the correct 16-byte alignment after the `ret` instruction.

### Segment Register Corruption

**Root Cause**: When the ISR stub or syscall handler ran in kernel mode, it used the percpu `FS` segment (selector `0x38`). If returning to user mode with the wrong segment selectors, later `get_percpu()` calls from timer IRQs would fault.

**Fix**: `isr.S` and `syscall_entry.S` both save the callee's segment registers, forcibly reload kernel segments on entry, and conditionally restore user segments (only when CS RPL = 3) before `iret`.

---

## Testing

Run with:
```sh
sh tests/smoke.sh x86
```

The smoke test script was already `x86`-aware and uses `qemu-system-x86_64` for both `x86` and `x86_64` targets.

### Current Status

| Category | Status |
|----------|--------|
| Kernel boots (Multiboot2 + PSE paging) | ✅ |
| PMM / KHeap / VMM | ✅ |
| Scheduler + context switch | ✅ |
| GDB stub | ✅ |
| ftrace / kallsyms | ✅ |
| M35/M36 diagnostics | ✅ |
| virtio-net + DHCP | ✅ |
| procfs / sysfs mounted | ✅ |
| Builtin `/bin/init` starts | ✅ |
| rc script runs | ✅ |
| `/bin/sh` spawns | ✅ |
| SMP (4 cores) | ⚠️ (single CPU in test env) |
| Native ELF32 binaries | 🔧 (loader added, testing in progress) |
| Full smoke suite pass | 🔧 (in progress) |

---

## Branch

All changes are on the `x86-32bit-port` branch.
