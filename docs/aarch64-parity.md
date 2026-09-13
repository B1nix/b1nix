# AArch64 parity with x86_64

**This is not a milestone.** AArch64 is a second target of the *same* kernel, so
each gap belongs to the milestone that owns the mechanism (a fault-handler case
is an MM item, a missing package is a ports item). This document is the
inventory of what differs between the two targets and the procedure for closing
a gap.

## Targets and how to run them

| Target | How |
|---|---|
| QEMU `virt` (the smoke suite) | `sh tests/smoke.sh aarch64` — boots `build/aarch64/Image` directly (no GRUB ISO), root on virtio-blk over virtio-mmio; `make run-aarch64` for a bare boot |
| Raspberry Pi 4 (QEMU `raspi4b`) | `make run-rpi4` / `run-rpi4-test`; in the suite with `SMOKE_RASPI_LANE=1` (SD card root, spin-table SMP, `M109-RPI` firmware/GPIO/timer checks) |
| Sony Xperia 5 (bahamut) | `make bahamut` (normal boot), `bahamut-test` (smoke lane `BAHAMUT_SMOKE_LANE`, results on the panel with `b1nix.keep-running`); tools in `tools/sony-xperia-5/` |

Suite layout on `virt`: the same lanes as x86_64 with AHCI, NVMe, xHCI, e1000,
virtio-gpu and virtio-tablet reached over PCIe ECAM. The `sys` lane gets 2 CPUs
(the M101 RCU check needs a second core). The `smp` lane runs 4 CPUs on
`virt,gic-version=3,iommu=smmuv3` and carries the GICv3/ITS (`M98-ITS`) and
SMMUv3 (`M100E`) checks; it ends at `M24B-SMP: ok work-stealing` plus a settle
period, because userspace does not run on secondaries (below).

## Gap inventory

### Memory management

`kernel/arch/aarch64/paging.c` implements copy-on-write fork, swap-in,
access-flag reference bits (dirty is conservative), per-page `tlbi vaae1is`
invalidation, `mremap` leaf moves (page at a time), and file-backed demand
paging. The exception vectors enter with IRQs masked, so the fault handler never
blocks: it returns `PF_NEEDS_FILE_FILL` and `file_fill_fault` reads outside the
page-table lock, the same shape as swap-in. `mmap` of a file is lazy on both
arches.

| Differs from x86_64 | Status |
|---|---|
| Heap tail page-return | **disabled** (`KHEAP_ENABLE_PAGE_RETURN 0` in `kheap.c`) after live kernel stacks were found in returned ranges; heap high-water only grows |
| Break-before-make on block split | deliberately violated in `ensure_child` — read its comment before "fixing" it |
| MMIO | real mappings in a 32 GiB bump-allocated window at 416 GiB (never reclaimed). `VMM_PCD\|VMM_PWT` selects Device-nGnRE; `ioremap_wc` gets Normal-NC |
| Memory typing / cache flush | `kernel/arch/aarch64/memtype.c`: MAIR slots instead of PAT, `DC CIVAC` using `CTR_EL0.DminLine` |
| Address-space teardown | skips `L0[0]`, the kernel half every process shares by pointer |

**Kernel VA rule:** every new kernel virtual address must stay out of the boot
identity map `[0x40000000, DIRECT_MAP_MAX)` and under 512 GiB (inside `L0[0]`).
Heap is at 64 GiB, the large-allocation arena 128–320 GiB. The module region is
identity-mapped reserved RAM derived from `__kernel_end` (`module_region_base()`).

### SMP

Secondaries start over PSCI `CPU_ON` or a spin-table (per the device tree's
`enable-method`) in `kernel/arch/aarch64/smp.c`, take their own timer interrupts
and run stealable kernel workers. Per-CPU data is found through `MPIDR_EL1`
(`TPIDR_EL1` is the vectors' scratch register). TLB shootdown needs no IPI —
`tlbi …is` is broadcast by the hardware.

**Userspace on secondaries is off** unless `b1nix.ap-userspace` is passed. With
it on, the suite loses hundreds of checks with wide variance: a secondary was
caught running with SP inside another task's kernel stack. Root cause not
found. The corresponding checks are reported as skips.

### Interrupts and devices

- GICv2 or GICv3 (`gicv3.c`), with an ITS for MSI/MSI-X (`gicv3_its.c`; MSI
  delivery is only exercised on the GICv3 `smp` lane, other lanes skip it).
- PCI INTx is routed from the host bridge's device-tree `interrupt-map`
  (`pci_intx_line()`), since no firmware fills config offset 0x3C.
- SMMUv3 driver in `kernel/dev/smmuv3.c`; VT-d/AMD-Vi checks are skipped.
- Block and network use virtio-mmio (the kernel's virtio-PCI driver is the
  legacy port-I/O one). Root is found by `mount_first_virtio_root()`.
- PL011 console (one UART), PL031 RTC, `CNTVCT_EL0` monotonic clock.
- Framebuffer console via `fb_panel.c` (virtio-gpu frames, or a bootloader
  splash framebuffer on phones).
- No x86 pieces: port I/O, CMOS, XSAVE/AVX, PAT MSR, pflash (MTD), second UART.

### ABI

- Linux syscall numbers are asm-generic (`LINUX_NR_*` in
  `kernel/include/b1nix/linux_abi.h`). **Flag values differ too**:
  `O_DIRECTORY`/`O_NOFOLLOW`/`O_DIRECT`/`O_LARGEFILE` are renumbered
  (`linux_open_flags_to_b1nix`).
- `struct epoll_event` is packed on x86_64 only.
- Signal frames build a real Linux `ucontext_t` with an `fpsimd_context`
  record; `sigreturn` restores from b1nix's own frame, so edits to
  `uc_mcontext` are not obeyed (same as x86_64).
- ptrace: `NT_PRFPREG` is `user_fpsimd_state` (528 bytes, FPSR/FPCR saved on
  context switch), `NT_ARM_TLS` supported; `NT_X86_XSTATE` checks skipped.
- ASLR is opt-in on both arches (`b1nix.aslr`).

### Userspace and ports

Both arches take packages from the same Alpine set
(`tools/packages/alpine-ports.map`, pinned by `tools/packages/alpine.lock`) with
one `install-ports` recipe. Still excluded on aarch64:

| Excluded | Where | Why |
|---|---|---|
| `m64_clang_smoke` | `userspace/Makefile` | native in-guest Clang is x86_64-only |
| `m53_virgl_smoke` | `userspace/Makefile` | x86_64-only |
| `chromium` | `alpine.lock` | no aarch64 hash recorded (`ALPINE_LOCK_UPDATE=1` adds it) |
| i915 | `Makefile` (`B1NIX_I915 ?= 0`) | x86 GPU |

### Shared code

Many shared files carry `__aarch64__` conditionals. Two traps:

- `#ifdef __x86_64__ / #else` historically meant "32-bit". That port is gone, so
  a surviving `#else` branch is silently the aarch64 branch.
- Changes made "for aarch64" in shared files ship to x86_64 and must be
  regression-tested there.

## Procedure for porting a mechanism

1. Query the knowledge graph for the x86_64 implementation and its callers.
2. Split it into genuinely architectural, incidentally architectural (move to
   shared code) and shared logic.
3. If user-visible, check number **and flag** tables before the code.
4. Implement within this arch's constraints: kernel VA rule above, no FP in the
   kernel, `-mgeneral-regs-only` (V-register code lives in `.S`, e.g. `fpu.S`).
5. Register new files in the top-level `Makefile`; grep for the symbol first
   (duplicate symbols only show at link).
6. Unblock a smoke check: prefer removing an entry from the
   `ifneq ($(B1NIX_ARCH),aarch64)` blocks in `userspace/Makefile`.
7. Run one lane, then the suite; read the logs (`grep -a`), not just the count.
8. Regression-test x86_64 if a shared file changed.

## Gotchas

- Never enable interrupts or block inside the aarch64 fault handler.
- A lazy marker means "reserved", not "zero".
- Don't donate page-cache frames to a process without a reference: teardown
  frees `SW_USER` leaves. Use `VMM_SHARED` + `pmm_ref_frame`.
- Module relocations are per-arch (`R_AARCH64_*` in `kernel/module/module.c`).
- `__attribute__((aligned(N)))` on a member type overrides `packed` on the
  struct; add `_Static_assert`s on wire structs.
- Mismatched cacheability aliases need `DC CIVAC` before reading through the
  other view.
- `bootinfo_get_kv` returns 1 on a match, not 0.
- SIG_IGN is inherited across execve: the runner's `trap '' … TERM` reaches
  every test, so cleanup must use SIGKILL.
- Default-action termination proves nothing about signal delivery; only a
  handler exercises the return-to-user path.
- A failing check is not automatically a kernel gap — check what the lane gives
  the guest (e.g. the e1000 lane's subnet is `b1nix.e1000-subnet`) and whether an
  `#if defined(__x86_64__)` guard is stale.
- `pgrep -f "smoke.sh aarch64"` matches its own wrapper.

## Open, in suggested order

1. **Userspace on secondaries** — find the stack corruption behind
   `b1nix.ap-userspace`; fault-path locking must be reviewed with it.
2. **Heap page-return** — re-enable once kernel stacks can no longer end up in
   a returned range.
3. **Missing packages** — `chromium` aarch64 lock entry.
4. **Audio** — `virtio-snd` if audio is wanted on `virt`.
