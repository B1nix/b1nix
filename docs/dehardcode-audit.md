# De-Hardcode / Hardware-Adaptive Audit

Goal (maintainer's stated direction, 2026-05-29): **move away from hardcoded
constants and make the system adapt to the actual hardware and runtime needs.**
This document is the prioritized backlog for that effort. It extends roadmap
**M37** ("Replace hardcoded system limits with dynamic ACPI table discovery").

Status legend: `P0` highest leverage, `P1` valuable, `P2` nice-to-have.
Each item: current state → target → risk/effort → file refs.

## ✅ Done (2026-05-29 session)

| Item | Status | Commit |
|------|--------|--------|
| **A1** ACPI MADT CPU enum + IOAPIC IRQ routing | ✅ done | `514d46f`, `4d58e9d` |
| **A2** LAPIC timer calibrated against PIT      | ✅ done | `c08faf7` |
| **A3** Graphics mode discovery                  | ✅ already runtime via Multiboot2; real mode-set needs UEFI/v86 (out of scope) |
| **B1** Direct map sized to actual RAM           | ✅ done | `8ce35bb` |
| **B2** Caches scaled to RAM                     | ✅ done | `7b8303c` |
| **B3** Swap + eviction tables to RAM            | ✅ done | `489f4c7` |
| **C1** Growable task table (chunked, lazy)     | ✅ done | `7dd5b23` |
| **C2** Growable program registry                | ✅ done | `eb809fa` |
| **C3** MAX_CPUS 16 → 64 ceiling + runtime count | ✅ done | `643212a` |

Roadmap M37 ("Replace hardcoded system limits with dynamic ACPI table
discovery") now spans CPU topology, IRQ routing, direct map sizing, cache
sizing, task scaling, and SMP-CPU sizing — effectively closed for the
limits the audit named. Remaining future work is captured under
"Follow-ups" at the end of this document.

---

## A. Hardware discovery (the biggest levers)

### A1. `P0` Parse ACPI (RSDP → RSDT/XSDT → MADT) for CPU + interrupt topology
- **Now:** CPU count comes from CPUID leaf 0x0B (`kernel/arch/x86/lapic.c:381`),
  capped at `MAX_CPUS 16`. There is **no ACPI parsing** at all (only the enum
  `BOOT_MEMORY_ACPI_RECLAIMABLE` and a shutdown-port comment in
  `kernel/syscall/syscall.c:1850`). IRQ routing is legacy PIC/PIT
  (`kernel/arch/x86/interrupts.c`) + LAPIC timer; no IOAPIC.
- **Target:** Find the RSDP, walk the MADT for the real Local-APIC list (CPU
  count + APIC IDs) and IOAPIC/interrupt-source-override entries; drive AP
  startup and IRQ routing from that instead of CPUID-only + legacy PIC.
- **Risk/effort:** Medium-high, self-contained new module. Foundation for true
  multi-socket/odd-topology support and for dropping legacy assumptions.
- **Refs:** new `kernel/dev/acpi.c`; `kernel/arch/x86/lapic.c`,
  `kernel/arch/x86/interrupts.c`.

### A2. `P1` Calibrate the timer instead of assuming the PIT input clock
- **Now:** `PIT_FREQUENCY 1193182`, `TIMER_HZ 100` hardcoded
  (`kernel/arch/x86/interrupts.c:22-23,232`). LAPIC-timer tick rate is not
  calibrated against a reference.
- **Target:** Calibrate LAPIC timer against the PIT (or ACPI PM timer/TSC), so
  the scheduler tick is correct on hardware where assumptions differ.
- **Risk/effort:** Low-medium, localized.

### A3. `P2` Dynamic graphics mode (VBE/GOP) instead of GRUB-only framebuffer
- **Now:** The boot framebuffer (resolution/bpp) is whatever GRUB set; `bpp`
  is read dynamically (`kernel/dev/video.c:145`) but mode-setting is not done.
- **Target:** Query/set a VBE (BIOS) or GOP (UEFI) mode at startup.
- **Risk/effort:** Medium; lower priority (text/serial is the first-class path).

---

## B. Memory-derived limits (size to actual RAM, not a constant)

### B1. `P0` `DIRECT_MAP_SIZE` is a fixed 8 GB (`kernel/include/b1nix/mm.h`)
- **Now:** `8ULL*1024^3` fixed; the pmm drops RAM above it
  (`kernel/mm/pmm.c:208-256`) and the direct map is sized at compile time, not
  to the machine. Wastes page tables on small-RAM boxes; loses RAM on >8 GB.
- **Target:** Size the direct map (and its page tables) to the Multiboot2
  memory map's top of usable RAM, clamped to a sane ceiling.
- **Risk/effort:** Medium (touches early paging bring-up). High value for the
  "adapt to hardware" goal.
- **Refs:** `kernel/arch/x86/paging.c`, `kernel/mm/pmm.c`,
  `kernel/bootinfo/multiboot2.c`.

### B2. `P1` Cache sizes fixed regardless of RAM
- **Now:** block cache `CACHE_ENTRIES 256`, `DCACHE_SIZE 256` /
  `MAX_DCACHE_ENTRIES 512`, `ICACHE_SIZE 128` / `MAX_ICACHE_ENTRIES 256`,
  page-cache thresholds — all fixed.
- **Target:** Scale cache capacities from total RAM (e.g. a fraction of usable
  frames), with floors/ceilings.
- **Risk/effort:** Low-medium per cache; do one at a time.

### B3. `P1` `MAX_USER_PAGES 65536`, `MAX_SWAP_SLOTS 65536` fixed
- **Now:** Fixed (already bumped once and swap clamped to device in M26).
- **Target:** Derive user-page accounting from RAM; swap slots already track
  the device — verify both follow RAM/device rather than a constant.
- **Refs:** `kernel/mm/swap.c`, `kernel/mm/eviction.c`.

---

## C. Fixed global tables that should grow on demand

(Same pattern the M33 shell work used: heap-backed, grow ×2.)

### C1. `P0` `MAX_TASKS 64` — global static task table
- **Now:** `static struct task tasks[MAX_TASKS]` (`kernel/sched/scheduler.c:47`)
  + `g_tasks_lock`. Hard ceiling of 64 processes.
- **Target:** Growable task table (or slab), preserving the SMP-safe slot
  alloc/free and `find_unused_task` semantics.
- **Risk/effort:** Medium (concurrency-sensitive; coordinate with the
  `g_tasks_lock`). High value.

### C2. `P1` Other fixed registries
- `MAX_PROGRAMS 96` built-in program table (`kernel/user/process.c:60`),
  `MAX_MOUNTS 16` (`kernel/fs/vfs.c:65`), `MAX_VFS_NODES 4096`, `MAX_FILES 128`,
  `MAX_USERS 16`, `MQ_MAX_QUEUES 16`, `MAX_TCP_CONNS 16`, `ARP_TABLE_SIZE 16`,
  `MAX_UDP_BINDINGS 64`, `MAX_FILE_LOCKS 64`, `MAX_BLK_PARTITIONS 32`.
- **Target:** Convert the ones that bound real workloads to growable structures;
  leave small protocol tables fixed unless they actually constrain use.

### C3. `P2` `MAX_CPUS 16` per-CPU arrays
- `tasks`-adjacent per-CPU arrays (`g_ap_idle_tasks`, `x86_tss_arr`,
  `ap_cpu_data`) are `[MAX_CPUS]`. Once A1 (ACPI) lands, size these to the
  discovered CPU count. Low priority until >16-CPU targets matter.

---

## D. Must STAY fixed (contracts, not hardcoding — do NOT "fix")

- Userspace ELF load base `0x02000000` and AP trampoline phys `0x8000` — ABI /
  boot contracts.
- `PAGE_SIZE 4096`, `IDT_ENTRY_COUNT 256`, alignment boundaries, `KHEAP_HEADER_SIZE`.
- `KERNEL_STACK_SIZE 32 KB` — a tuned safety value (see M26 stack-overflow fix);
  could be revisited but is not "arbitrary".
- `$1..$9` shell positionals — POSIX model, not a cap.
- `VFS_MAX_SYMLINK_DEPTH 16`, `MAX_BACKTRACE_FRAMES 32` — spec/safety bounds.

---

## E. Follow-ups — concrete backlog (for the next M-milestone passes)

Cross-referenced to the milestone they slot into so a future session can
pick one off without re-deriving scope. Each entry lists the concrete
files / call sites to touch, the prerequisite for tackling it, and the
risk profile.

---

### E1. Drivers off `phys + DIRECT_MAP_BASE`, onto `vmm_map_mmio()`  *(M28 / M37)*

**Why it matters:** B1 sized the direct map to actual RAM but had to
floor it at 4 GiB because PCI MMIO BARs (AHCI ABAR, NVMe BAR0, virtio
config space, etc.) live in the 32-bit PCI hole around `0xFE000000` and
several drivers reach them as `phys + DIRECT_MAP_BASE`. Below 4 GiB those
loads page-fault. Migrating each driver to `vmm_map_mmio(phys, size,
flags)` would let `DIRECT_MAP_MIN` drop to ~64 MiB (or even just
top-of-RAM aligned), saving ~16 KiB of page tables on a 128 MiB guest and
making MMIO accesses go through a real, audited mapping.

**Files / call sites (exact line numbers as of `dc7cabe`):**
- `kernel/dev/ahci.c:257, 263, 268, 408` — ABAR and per-port cmd/FIS tables
- `kernel/dev/nvme.c:226, 317, 346, 351, 356, 361, 367, 368` — controller
  regs + admin/IO SQ/CQ + identify buffer
- `kernel/dev/virtio.c:97` — generic virtio config-space mapping
- `kernel/dev/virtio_gpu.c:184, 229, 449, 637` — GPU regs + scanout/cursor
  surfaces
- `kernel/dev/virtio_blk.c` — uses helpers in virtio.c, audit when touched
- `kernel/dev/compositor.c:374` — backbuffer (this one is RAM, NOT MMIO;
  if the backbuffer lives in real RAM it's fine to stay; verify before
  changing)
- `kernel/dev/ioapic.c:146` — IOAPIC MMIO (we added this in A1-irq;
  small, easy to migrate)
- `kernel/net/net.c:243, 268` — tx/rx ring buffers (RAM, fine to stay)
- `kernel/dev/acpi.c:35` — ACPI table reads (BIOS ROM region, always
  below 1 MiB, can stay)

**Method:** for each MMIO site, replace
```c
void *virt = (void *)(phys + vmm_direct_map_base());
```
with
```c
void *virt = vmm_map_mmio(phys, size, VMM_PRESENT | VMM_WRITABLE | VMM_PCD);
```
and store the result in driver state. Don't unmap on shutdown — kernel
lifetime.

**Prerequisite for shrinking DIRECT_MAP_MIN:** all four AHCI/NVMe/virtio
families migrated; verify with QEMU `-m 256M` boot that no MMIO load
faults.

**Risk:** medium — touches every PCI driver. Phase by driver family
(AHCI, then NVMe, then virtio_blk+virtio_gpu+virtio_net).

---

### E2. LAPIC timer as primary tick source  *(M28)*

**Why it matters:** A2 already calibrated the LAPIC timer
(`lapic_ticks_per_ms()` at commit `c08faf7`). Today the PIT on the BSP is
still the only timer-tick source — APs only run cooperatively yielded
work. Switching to per-CPU LAPIC ticks is the prerequisite for **real
preemptive SMP scheduling** (M28's first bullet) and would let each AP
drive its own quantum without relying on BSP-side timing.

**Where to change:**
- `kernel/arch/x86/lapic.c` — add `lapic_timer_arm_periodic(u32 hz)` that
  reuses `g_lapic_ticks_per_ms` to compute the init count and call
  `lapic_timer_start()`. Wire into `ap_main()` after the work-stealing
  phase ends.
- `kernel/arch/x86/interrupts.c:24-25, 243-247, 277` — `TIMER_HZ` and the
  PIT init logic; the PIT can stay as a wall-clock reference but should
  no longer be the scheduler tick.
- `kernel/arch/x86/interrupts.c:270-279` — IRQ handler vector 32 (PIT
  IRQ); add a parallel branch for LAPIC_TIMER_VECTOR (0x40) that calls
  `scheduler_on_timer_tick()` + `lapic_eoi()`.
- `kernel/sched/scheduler.c:861` `scheduler_on_timer_tick()` — needs to
  be SMP-safe under per-CPU calls (currently called from one CPU only).

**Risk:** high — touches the scheduler's heartbeat. Stage behind a
kernel cmdline flag (`b1nix.lapic-tick=1`) so the PIT path stays
fallback during bring-up, then flip the default once stable.

---

### E3. Heap-backed TSS array — truly remove `MAX_CPUS`  *(M28)*

**Why it matters:** C3 raised `MAX_CPUS` 16→64 and added runtime
`g_max_cpus`, but two static structures still cap us at compile time:
`x86_tss_arr[MAX_CPUS]` (BSS) and the GDT `gdt64_tss` slot reservation
in `boot.S` (assembly). A heap-backed TSS would close that.

**Files:**
- `kernel/arch/x86/arch.c:30` `static struct x86_tss x86_tss_arr[MAX_CPUS]`
  — convert to `struct x86_tss **x86_tss_arr` with lazy alloc in
  `x86_tss_init_cpu()`.
- `kernel/arch/x86/boot.S:136-142` `gdt64_tss: .fill 128, 8, 0` — the
  hard part. Options:
  - **Easier:** raise to a much higher ceiling (e.g. `.fill 1024`,
    enough for 512 CPUs) and live with the GDT size growth (~8 KiB).
  - **Cleaner:** defer GDT setup to C — allocate a runtime GDT in
    kheap once `acpi_cpu_count()` is known, then `lgdt` to it from
    the BSP and have APs use the same descriptor. Touches all `ltr`
    sites because the selector arithmetic stays the same. AP
    bring-up order needs care.
- `kernel/arch/x86/lapic.c:212` `ap_cpu_data[MAX_CPUS]` — trivial to
  convert to heap once the count is known.

**Risk:** low for the C-side, medium for boot.S — assembly changes
should be tested at single-CPU and `-smp 4` immediately.

---

### E4. VBE / GOP graphics mode-setting at runtime  *(M37)*

**Why it matters:** Framebuffer parameters are already runtime-discovered
from Multiboot2 (`bootinfo_get()->framebuffer`, see comment block at
`kernel/dev/video.c:27`). What's missing is **changing** the mode after
boot — e.g., switching resolution from a userspace request.

**Blocker:** there is no way to call BIOS INT 10h from long mode without
a v86 emulator (large engineering project). UEFI GOP is the right path,
and it depends on **M37's first bullet: UEFI bootloader**. Until then,
this stays at `partial`.

**Possible interim:** carry a *list* of pre-set modes in Multiboot2
(if GRUB can be configured to advertise alternates via the VBE info tag,
type 7) and let userspace pick from those without actual mode-setting.
Low priority.

---

### E5. Small fixed tables NOT currently bottlenecks  *(no milestone yet)*

The audit explicitly said "leave small protocol tables fixed unless they
actually constrain use". These are the surviving fixed caps; promote
each one to a discrete task only when a real workload hits the wall.
Listed here so future-me knows where to look:

| Cap | Where | Spec/protocol justification |
|-----|-------|------------------------------|
| `MAX_BLK_DEVICES 32`         | `kernel/dev/blk.c:9`         | rare to exceed; trivial grow if needed |
| `MAX_BLK_PARTITIONS 32`      | `kernel/dev/blk.c:10`        | one disk = one row; bump if multi-disk persists |
| `MAX_MOUNTS 16`              | `kernel/include/b1nix/vfs.h:233` + `kernel/fs/vfs.c:65` | / + a handful of partitions |
| `MAX_USERS 16` / `MAX_GROUPS 8` | `kernel/sched/uidgid.c:7, 10` | tiny `/etc/passwd` for now |
| `MAX_TCP_CONNS 16`           | `kernel/net/tcp.c:86`        | minimal TCP, M32 will revisit |
| `MAX_UDP_BINDINGS 64` / `MAX_UDP_HANDLERS 8` | `kernel/net/udp.c:27` | DHCP + DNS + a few sockets |
| `ARP_TABLE_SIZE 16`          | `kernel/net/arp.c`           | tiny LAN |
| `MAX_FILE_LOCKS 64`          | `kernel/fs/filelock.c:10`    | per-system |
| `MAX_JBD_BLOCKS_PER_TX 32`, `MAX_JOURNALS 4`, `MAX_JOURNAL_HANDLES 16` | `kernel/fs/journal.c:40-41` | per ext4 design choice |
| `MAX_VFS_PIPES 64`           | `kernel/fs/pipe.c`           | pipes per system |
| `MAX_VIRTIO_BLK 8`           | `kernel/dev/virtio_blk.c:25` | rare to exceed |
| `MAX_TRACKED_BLOCKS 1024`    | `kernel/mm/kheap.c:177`      | debug instrumentation only |
| `MAX_PROC_ATTACH 16`         | `kernel/ipc/shm.c:21`        | SysV shm attachments per proc |
| `MAX_SYMBOLS 128`            | `kernel/lib/klog.c:27`       | kernel debug symtab |
| `MAX_VFS_NODES 4096`         | `kernel/include/b1nix/vfs.h:231` | **already unused / vestigial** — the slab allocates on demand. Remove the macro in a cleanup commit. |
| `BOOTINFO_MAX_MEMORY_REGIONS 32` | `kernel/include/b1nix/bootinfo.h:6` | QEMU emits ≤10; OK |
| `ACL_MAX_ENTRIES 8`          | `kernel/fs/...`              | POSIX ACL guidance |
| `MAX_EXEC_ARGS 256`, `MAX_EXEC_ARG_LEN 4096` | exec path | Linux defaults, fine |

---

### E6. UEFI bootloader + GRUB-USB image  *(M37, first bullet)*

Independent of de-hardcoding but listed for completeness — it gates E4
(real mode-setting) and would also let us discover the ACPI tables via
EFI System Table instead of the BIOS scan in `kernel/dev/acpi.c:75-95`.
After this lands, replace the EBDA + 0xE0000-0xFFFFF scan with the EFI
configuration-table lookup.

---

### E7. Layout-sensitivity safety net for TCC  *(operational, not de-hardcode)*

`docs/tcc-hang.md` documents that the M25 TCC binary's fixed load
address is sensitive to kernel `.text` size. B3 raised the linker pad
from 256K to 512K (in `kernel/arch/x86/linker.ld`) because the audit's
cumulative `.text` growth crossed the previous boundary. Two follow-ups:
- Add a build-time `make check-layout` that compares `nm`-extracted
  kernel size against the pad and fails fast if the next commit would
  cross the boundary.
- Long-term: relocate TCC to use position-independent loading via the
  ELF loader's `PT_LOAD` handling so its address is no longer fixed.
  (Touches `userspace/tcc/` and the kernel ELF loader.)

---

## Quick start for the next session

When you come back, the highest-leverage single step is **E1.1 (AHCI
ABAR via `vmm_map_mmio()`)** — it's the smallest concrete migration that
proves the pattern, after which the other PCI drivers follow the same
template. Once E1 is fully done, `DIRECT_MAP_MIN` can finally drop.

Build:
```sh
make ARCH=x86 LD=ld.lld KERNEL_CMDLINE="b1nix.test=1" iso
```

Smoke (always run both):
```sh
bash smoke_run/qrun.sh 60           # single-CPU
sh tests/smoke.sh x86               # full suite incl. -smp 4
```

Smoke baseline at the end of this session: **250 / 0 / 0**.
