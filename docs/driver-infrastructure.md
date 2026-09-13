# Driver infrastructure (M98–M100)

Reference for the low-level pieces ported GPU drivers stand on: netconsole,
memory typing, PCI, the linuxkpi primitives, and b1nix's DRM core. Status lives
in `docs/roadmap.md`; markers (`M98-DRV-SMOKE:` etc.) are asserted in
`tests/smoke.sh`.

## netconsole — `kernel/dev/netconsole.c`

- Enabled only by `b1nix.netconsole=<ip>:<port>` (strict parser; a malformed
  value disables it). Needed on machines without a serial port.
- A kernel thread drains the klog ring every 20 ms via its own cursor
  (`klog_cursor_now` / `klog_drain` in `kernel/lib/klog.c`, independent of
  `dmesg`) and sends ≤1024-byte UDP datagrams (no fragmentation).
- Never sends from `console_write` (IRQ-off, console lock held). Lapped bytes
  are dropped, never blocked on. `netconsole_flush()` drains from the caller's
  context.
- Host collector: `tools/debug/netconsole-collect.sh <port>`.

## Memory typing — `kernel/arch/x86_64/memtype.c`

- `pat_init_cpu()` (BSP in `arch_init`, each AP in `x86_ap_arch_init`) rewrites
  only `IA32_PAT` slot 5 (WT → WC). Slots 0–3 are unchanged and nothing else sets
  the PTE PAT bit, so live mappings keep their meaning.
- `VMM_WC` = `VMM_PAT | VMM_PWT` (`<b1nix/mm.h>`) — **4 KiB leaf PTEs only**
  (bit 7 is PS in directory entries). Without PAT it degrades to write-through.
- Cache helpers: `mem_clflush`, `mem_wbinvd`, `mem_mfence`, `mem_sfence`,
  `cache_flush_range()`. Slot layout: `kernel/include/b1nix/memtype.h`.

## PCI — `kernel/dev/pci.c`

| API | Notes |
|---|---|
| `pci_bar_read`, `pci_bar_enumerate` | sizes BARs with decode disabled, restores BAR + command register; upper half of a 64-bit BAR reports not-a-BAR |
| `pci_enable_bus_master`, `pci_enable_decode` | return the register value read back |
| `pci_find_capability` | bounded, cycle-proof walk |
| `pci_find_ext_capability` | via ECAM from ACPI MCFG (`acpi_find_table()`), mapped lazily per bus |
| `pci_msi_enable`, `pci_msix_enable`, `pci_msi_readback` | vector from `msi_alloc_vector()` (`<b1nix/irq.h>`, vectors 48..63, own IDT gates, one owner each); set `PCI_CMD_INTX_DISABLE` |
| `pci_intel_stolen_read` | GGC/BDSM/BGSM from the host bridge 00:00.0; reports absent under QEMU |

## linuxkpi primitives — `kernel/include/lkpi/`, `kernel/lkpi/`

API-shaped reimplementations of Linux kernel interfaces, backed by b1nix's own
heap, scheduler, VFS and paging. Imported Linux code must only see `linux/*`
and `lkpi/*` headers; `<lkpi/env.h>` is the boundary.

| Header | Backed by / behaviour |
|---|---|
| `idr.h` | flat growable array + rotating free hint; O(1) lookup; own spinlock, IRQ-safe |
| `completion.h` | counting; an early `complete()` satisfies a later wait |
| `workqueue.h` | one kthread per queue, FIFO |
| `scatterlist.h` | run-coalescing page lists |
| `firmware.h` | VFS reads from `/lib/firmware`, `/usr/lib/firmware` |
| `io.h` | `vmm_map_mmio` (+ `VMM_WC` for `ioremap_wc`); `iounmap` is a no-op (MMIO VA window is never reclaimed) |
| `dma-mapping.h` | see below |
| `lock.h` | `lkpi_spinlock` (IRQ-saving, no sleep) vs `lkpi_mutex` (sleeps, not from IRQ) — not interchangeable |
| also | `kref`, `wait`, `ww_mutex`, `rbtree`, `interval_tree`, `xarray`, `kthread_worker`, `rcu`, `rwsem`, `page`, `device` |

**DMA.** Without an IOMMU a DMA address is the physical address:
`dma_map_single` refuses buffers outside the direct map;
`dma_map_single_masked` bounces buffers above the device mask, from a boot-time
pool sized by `b1nix.bounce-pool=<KiB>` (0 = allocate per mapping). With an
active IOMMU (`kernel/dev/iommu.c` VT-d, `kernel/dev/amdvi.c` AMD-Vi),
`dma_device_attach` gives the device a translated domain and
`dma_map_single_dev` maps IOVAs. Cache maintenance for non-snooping devices is
done in the map/sync calls.

## DRM core

- **dma-fence** (`kernel/drm/dma_fence.c`): one-shot, refcounted, callbacks,
  error propagation. Signal is IRQ-safe; wait parks. Double signal returns
  `-EINVAL`. Callback list is detached under the lock, so add-vs-signal runs a
  callback exactly once.
- **GPU scheduler** (`kernel/drm/gpu_scheduler.c`, `<b1nix/gpu_scheduler.h>`):
  one thread owns the ring; per-entity FIFOs, round-robin between entities;
  dependencies are awaited in the scheduler thread. `run_job` returns
  `DRM_SCHED_RUN_DONE`, `DRM_SCHED_RUN_ASYNC` (driver signals later) or `-errno`.
- **virtio-gpu** (`kernel/dev/virtio_gpu.c`): `B1NIX_VIRGL_SUBMIT` queues a
  `vgpu_submit_job` and waits on its fence; the scheduler thread holds
  `vgpu_udev_lock` for the device round trip. `vgpu_submit_stream_locked` is the
  synchronous fallback used only before the scheduler thread exists.
- **GEM** (`kernel/dev/drm.c`): buffer objects are page-at-a-time allocations
  described by an `sg_table`; each object slot has a fixed
  `DRM_MAP_STRIDE` (64 MiB) kernel window at `DRM_VMAP_BASE` for linear scanout
  access. Userspace mmap resolves per page via `mmap_handle_page_phys_cb`.
  Handles come from an `idr` based at 1. `DRM_IOCTL_B1NIX_GEM_INFO` reports
  `nents`/`npages`/`contiguous` for tests.
- `drm_ioctl` dispatches to one static handler per command.

## Not verifiable under QEMU

- MSI delivery for the smoke devices (register programming is verified by read-back).
- Intel stolen-memory decode (QEMU host bridges lack the registers; test asserts "absent").
- `wbinvd` path (only taken on CPUs without CLFLUSH).
