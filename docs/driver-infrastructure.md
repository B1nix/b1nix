# M98–M100: driver infrastructure, linuxkpi, DRM core

Implementation notes for the three milestones that turn b1nix's hand-rolled
device code into something a ported Linux GPU driver can sit on. The roadmap
entries are the status summary; this is the detail.

The plan these implement is `docs/gpu-drm-plan.md` (written when the milestones
were numbered M99–M101; the roadmap numbering M98/M99/M100 is authoritative).

---

## M98 — driver infrastructure

### netconsole (`kernel/dev/netconsole.c`)

`tests/smoke.sh` reads COM1. The bring-up laptop has no serial port, so a
bare-metal boot there has no log channel at all — which makes netconsole a hard
prerequisite for every hardware milestone after this one, not a convenience.

- Configured only from the kernel command line: `b1nix.netconsole=<ip>:<port>`.
  The parser is strict (four octets in range, a non-zero port, nothing else) —
  a half-parsed address would ship the boot log to an arbitrary host.
- A kernel thread drains the klog ring every 20 ms through a new cursor API
  (`klog_cursor_now` / `klog_drain` in `kernel/lib/klog.c`) and sends each chunk
  with `udp_send`. The cursor is separate from the one `dmesg` uses, which is
  deliberately non-destructive.
- **The send does not happen inside `console_write`.** That function takes
  `console_lock` with interrupts off and is called from interrupt context; a
  synchronous send would re-enter the NIC driver underneath the console lock and
  deadlock as soon as the driver logged anything. `netconsole_flush()` exists so
  a future panic-path flush can reuse the same drain from the caller's context.
- Payloads are capped at 1024 bytes so a datagram never needs IP fragmentation.
- Host side: `tools/netconsole-collect.sh <port>`.

Delivery is best effort. If the writer laps the drain cursor, the lapped bytes
are dropped rather than resent — the alternative is blocking the kernel on the
network, which is exactly what a log channel must not do.

### Memory typing (`kernel/arch/x86_64/memtype.c`)

`IA32_PAT` is a per-CPU MSR of eight memory types selected by
`(PAT << 2) | (PCD << 1) | PWT` from the page-table entry. b1nix rewrites
**one** slot:

| slot | reset | b1nix |
|---|---|---|
| 0 | WB | WB |
| 1 | WT | WT |
| 2 | UC- | UC- |
| 3 | UC | UC |
| 4 | WB | WB |
| 5 | WT | **WC** |
| 6 | UC- | UC- |
| 7 | UC | UC |

Slots 0–3 are what every existing mapping selects, because nothing in the tree
sets the PTE PAT bit (bit 7). Changing only slot 5 therefore cannot change the
meaning of a live mapping and needs no cache/TLB flush dance.

`VMM_WC` (in `<b1nix/mm.h>`) is `VMM_PAT | VMM_PWT`, i.e. slot 5. It is only
valid in a 4 KiB leaf PTE — bit 7 in a directory entry is PS (huge page), so it
must never reach one.

`pat_init_cpu()` runs from `arch_init` on the BSP and from `x86_ap_arch_init` on
every AP. A CPU that skipped it would read a WC PTE as write-through. On a CPU
without PAT support the flag degrades to write-through: slow, not wrong.

Also here: `mem_clflush` / `mem_wbinvd` / `mem_mfence` / `mem_sfence` and
`cache_flush_range()`, which is what pushes a write-back buffer out to memory
for a display engine that does not snoop the LLC.

### PCI (`kernel/dev/pci.c`)

Before this the file had config read/write and two find helpers.

- **BAR enumeration and sizing** (`pci_bar_read`, `pci_bar_enumerate`).
  Sizing writes all-ones and reads the decode mask back, so the function
  disables the device's memory/IO decode for the duration and restores both the
  BAR and the command register before returning. 64-bit BARs consume two
  registers and the upper half is reported as not-a-BAR so a caller iterating
  0..5 never double-counts.
- **Bus mastering / decode enables** (`pci_enable_bus_master`,
  `pci_enable_decode`) return the register read back from the device rather than
  what was written, because a function may hard-wire bits it does not implement.
- **Capability walk** (`pci_find_capability`), bounded and cycle-proof.
- **PCIe extended capabilities** (`pci_find_ext_capability`) need memory-mapped
  ECAM, which needs the ACPI MCFG table — so `acpi_find_table()` was added to
  `kernel/dev/acpi.c` (a generalisation of the existing MADT lookup) and ECAM
  windows are mapped lazily, one 1 MiB bus at a time.
- **MSI and MSI-X.** b1nix's IDT carries device gates for vectors 32..47 only,
  and the entry path turns a vector into `irq = vector - 32` before calling
  `irq_dispatch(irq)`. MSI is therefore programmed to deliver vector `32 + irq`
  to the BSP's local APIC and a driver registers through the ordinary
  `irq_register_handler(irq, ...)`. Nothing is routed through the IOAPIC for an
  MSI, so a driver switching to MSI should mask its legacy line (the code sets
  `PCI_CMD_INTX_DISABLE` for it).
- **Intel stolen memory** (`pci_intel_stolen_read`) reads GGC `0x50`, BDSM
  `0x5C` and BGSM `0x70` from the **host bridge at 00:00.0**, not from the GPU,
  and decodes GMS/GGMS into byte sizes.

#### What the M98 tests can and cannot prove under QEMU

- BAR sizing, restoration, the capability walk and bus mastering are verified
  against the device's own config registers, re-read after the operation. These
  are real hardware round trips.
- MSI and MSI-X programming is verified by reading the address/data/vector-control
  the device now holds and comparing against the LAPIC id and vector the code was
  asked for. **Interrupt *delivery* through MSI is not exercised**: doing so
  would mean reprogramming a device the smoke suite is actively driving. The
  first consumer will be the i915/amdgpu bring-up in M102a/M102b, which is where
  delivery gets proven on hardware.
- Intel stolen memory: QEMU's 440FX and Q35 host bridges implement neither BDSM
  nor BGSM, so under QEMU the function correctly reports *absence*, and that is
  what the test asserts. The decode arithmetic runs for real only on Intel
  graphics hardware.

---

## M99 — linuxkpi (`kernel/include/lkpi/`, `kernel/lkpi/`)

### The licensing constraint that shapes the layer

b1nix is MIT. The DRM core and the `i915` / `amdgpu` drivers are MIT and can be
ported close to verbatim. Linux's headers under `include/linux` are **GPL-2.0**,
so copying `workqueue.h` and friends would relicense the tree. Every header here
is written from scratch to the same API *shape* — the names and signatures a
driver calls — backed by b1nix's own kheap, scheduler, VFS and paging.

### What is in it

| header | backed by |
|---|---|
| `idr.h` | flat growable pointer table + rotating free hint |
| `completion.h` | `scheduler_wait_prepare/commit/cancel` |
| `workqueue.h` | one `kthread_create` thread per queue, FIFO |
| `scatterlist.h` | run-coalescing page lists |
| `firmware.h` | VFS reads under `/lib/firmware`, `/usr/lib/firmware` |
| `io.h` | `vmm_map_mmio` (+ `VMM_WC` for `ioremap_wc`) |
| `dma-mapping.h` | `pmm_alloc_frames` + the direct map + `cache_flush_range` |
| `lock.h` | b1nix spinlocks; a sleeping mutex on wait channels |

### Design decisions worth knowing

- **`idr` is a flat array, not a radix tree.** The callers are GEM handle tables
  and object-id spaces: tens to low thousands of live ids. A flat table gives
  O(1) lookup and amortised O(1) alloc; the radix tree buys memory density
  nothing here needs. `idr_for_each` runs its callback outside the lock so a
  callback may sleep.
- **`completion` counts.** A `complete()` that lands before anyone waits is
  consumed by the next wait, so a signaller that runs early does not hang a
  waiter that arrives late.
- **The two lock types are not interchangeable, on purpose.** `lkpi_spinlock` is
  a b1nix spinlock: IRQ-saving, holder must not sleep. `lkpi_mutex` parks on a
  wait channel: may be held across a sleep, must not be taken from an interrupt
  handler. Driver code that guards a firmware load maps onto the mutex; code
  that guards a ring index shared with an ISR maps onto the spinlock.
- **`iounmap` is a no-op.** The MMIO virtual window is a bump allocator that is
  never reclaimed. Unmapping without a VA allocator would leave a hole nothing
  can reuse and risk tearing down a mapping another driver still holds; a leaked
  virtual range costs one page-table entry per device BAR for the life of the
  kernel.
- **dma addresses are physical addresses.** There is no IOMMU. Drivers still go
  through the API because that is the single place an IOMMU would be inserted,
  and because it is where the cache maintenance for a non-snooping device lives.
  `dma_map_single` refuses a buffer outside the direct map rather than silently
  bounce-buffering.

---

## M100 — DRM core

### dma-fence (`kernel/drm/dma_fence.c`)

One-shot, refcounted, with callbacks and error propagation. Signalling is safe
from an interrupt handler; waiting parks the caller. `dma_fence_signal` on an
already-signalled fence returns `-EINVAL` rather than absorbing it — that is a
driver bug and hiding it makes the next one harder to find.

The callback list is detached under the fence lock before the callbacks run, so
`dma_fence_add_callback` racing `dma_fence_signal` either lands on the list (and
is run by the signaller) or observes `signaled` and runs itself; never both,
never neither.

### GPU scheduler (`kernel/drm/gpu_scheduler.c`)

One thread owns the hardware ring; submitters push jobs and get a fence.

- Per-entity FIFOs with round-robin between entities, so one busy client cannot
  starve another — the property a single global queue cannot provide.
- Job dependencies are waited out **in the scheduler thread**, so a submitter
  never blocks on another client's work.
- `run_job` returns `DRM_SCHED_RUN_DONE` (the scheduler signals the fence),
  `DRM_SCHED_RUN_ASYNC` (the driver signals it from its completion path), or a
  negative errno (the scheduler signals the error).

### virtio-gpu conversion

`vgpu_submit_stream` used to run inline in the caller's context and then sit
inside `virtio_gpu_wait_used()` — a TSC-bounded busy-spin on the virtqueue used
index — with `vgpu_udev_lock` held and interrupts disabled for the whole device
round trip.

Now `B1NIX_VIRGL_SUBMIT` takes no lock at all: it builds a `vgpu_submit_job`,
hands it to the scheduler entity, and waits on the job's fence, which parks the
task. The scheduler thread takes `vgpu_udev_lock` around the actual device round
trip and is the single writer of the control queue for these submissions.

The synchronous path survives as `vgpu_submit_stream_locked` and is used only
before the scheduler thread exists (or if `kthread_create` failed). Dropping
work silently in that window would be worse than briefly spinning.

### GEM with scatter-gather buffer objects (`kernel/dev/drm.c`)

Buffer objects were one physically contiguous `pmm_alloc_frames` run. That is
the first thing to fail on a fragmented system, and no real GPU driver needs it
— it needs a page list.

- Pages are allocated **one at a time** and described by an `sg_table`.
- Each object slot has a fixed 64 MiB kernel virtual window at
  `DRM_VMAP_BASE`, into which its pages are mapped contiguously. That gives the
  scanout path (`virtio_gpu_present`, which reads a linear buffer) the linear
  view it needs without a VA allocator: slot *i* is always at the same address.
- Userspace mappings resolve **one page at a time** through a new inode hook,
  `mmap_handle_page_phys_cb`. The pre-existing `mmap_handle_phys_cb` answers
  with a single base and `sys_mmap` extrapolates the rest of the range from it,
  which is exactly the assumption a scatter-gather object breaks.
- Handles moved from a bare `client->next_handle++` onto an `idr` based at 1, so
  a freed handle is reused instead of the id space growing without bound.
- `DRM_IOCTL_B1NIX_GEM_INFO` (b1nix-specific, read-only) reports `nents`,
  `npages` and `contiguous` so a test can assert the object really is
  scatter-gather backed rather than a contiguous allocation that happens to
  work.

### `drm_ioctl` split

The single switch was cyclomatic 56 / cognitive 161. It is now one static
handler per command with the switch reduced to dispatch. No behaviour changed;
the handlers are the previous case bodies verbatim apart from taking their
arguments explicitly.

---

## Test coverage and its limits

All markers are listed in the roadmap entries and asserted in `tests/smoke.sh`.

Verified end to end under QEMU:

- netconsole through the real UDP stack over 127.0.0.1 to a registered handler,
  matching a unique magic string written into the klog ring.
- PAT slot decoding read back from the MSR; WC PTE bits read back from the page
  tables; WC/direct-map coherency.
- PCI BAR sizing/restoration, capability walk, bus mastering, MSI/MSI-X register
  programming — all against device config-space read-back.
- Every linuxkpi primitive against independently known values (pointers the test
  allocated, frames from the page allocator, bytes written through the VFS).
- Fence signalling, ordering and error propagation; scheduler dependency
  ordering and round-robin fairness (gated on one unsignalled fence so the
  interleaving is a property, not a race).
- GEM objects through the real `/dev/dri/card0` ABI, including a second
  independent mapping used to read back what the first one wrote.

Not verified under QEMU, and why:

- **MSI interrupt delivery** — would require reprogramming a device the suite is
  driving. Proven when the first MSI-using driver lands.
- **Intel stolen memory decode** — QEMU's host bridges do not implement the
  registers; the test asserts the correct "absent" answer instead.
- **`wbinvd`** — exercised only as the CLFLUSH-less fallback path, which no
  x86-64 CPU QEMU emulates actually takes.
