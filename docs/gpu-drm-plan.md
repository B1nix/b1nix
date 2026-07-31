# M99–M103: GPU/DRM — plan

Goal: a reusable, MIT-clean Linux-driver compatibility layer, proven first on
`virtio_gpu`, then carrying real hardware GPU drivers.

Split into five milestones. M99–M101 are hardware-independent and are not wasted
under any outcome; M102 and M103 are the two hardware targets and can be
reordered or dropped independently.

| milestone | scope | est. |
|---|---|---|
| M99 | Driver infrastructure: netconsole, PAT/WC, PCI BARs/caps/MSI | ~1 month |
| M100 | linuxkpi compatibility layer (own MIT headers) | ~1 month |
| M101 | DRM core: `dma-fence`, scheduler, GEM — proven on virtio-gpu | ~2 months |
| M102 | Intel `i915` (Gen8/Gen9.5) + Mesa `iris` | ~3 months |
| M103 | `amdgpu` on RX 6600, render-only + radeonsi | ~6–9 months |

## Strategy

**Port Linux drivers through an own compat shim; do not hand-write drivers.**

A hand-written driver from vendor PRMs yields exactly one driver and no reusable
surface — the next chip starts from zero. The shim (the FreeBSD `drm-kmod`
pattern) is the actual deliverable: it carries `i915`, `amdgpu`, and also non-GPU
Linux drivers, where b1nix currently hand-rolls `e1000.c`, `r8169.c`, `nvme.c`,
`ahci.c`.

### Licensing constraint (load-bearing)

B1NIX is MIT since `3a36129`. This survives the port only if the shim is written
from scratch:

| source | licence | usable |
|---|---|---|
| `drivers/gpu/drm/i915/` | MIT | yes, near-verbatim |
| `drivers/gpu/drm/amd/` (incl. `display/dc`) | MIT | yes, near-verbatim |
| DRM core (`drivers/gpu/drm/drm_*.c`) | MIT | yes |
| **`include/linux/*.h`** | **GPL** | **no — write our own** |

Copying `linux/workqueue.h` et al. pulls the whole tree into GPL. Every shim
header under `kernel/include/lkpi/` must be independently written to the same
API shape.

## Hardware and how each is reached

| target | GPU | access | role |
|---|---|---|---|
| QEMU | virtio-gpu | native | shim development, smoke suite |
| HP Pavilion 15-ac007ag | Gen8 (Broadwell/Braswell) — *confirm exact model* | **bare metal, USB boot** | primary `i915` target; `r8169` NIC confirmed working |
| desktop dGPU | RX 6600 (Navi 23, GFX10.3) | bare metal | `amdgpu`; GOP scanout already proven without drivers |
| desktop iGPU | UHD 630 (Gen9.5, Coffee Lake) | KVM + VFIO — *currently blocked* | would be the better dev loop, see below |

Gen8 and Gen9.5 are the same driver and the same Mesa driver (`iris` covers
Gen8+), so M102 serves whichever is reachable.

**Neither machine compiles anything.** Builds happen on the dev host; the
Pavilion has no OS installed and only boots the b1nix ISO from USB. Its
dual-core Pentium is not a build machine and is never asked to be one.

### KVM + VFIO passthrough: nice-to-have, not the plan's foundation

Under KVM the guest keeps a virtual COM1, so `tests/smoke.sh` and every existing
marker-based test would work **unchanged** against a real GPU — plus snapshots and
no reboot cycle. Materially better than bare metal, which is why it is worth
re-checking periodically. But both routes to it are currently blocked:

- **Desktop UHD 630** — BIOS hides it with no toggle. Nothing can be passed
  through that does not enumerate on the host PCI bus.
- **Laptop Gen8, with a minimal Linux host on the laptop** — viable in principle
  (the host would only run qemu; the ISO arrives over the network, so the slow
  CPU does not matter). The blocker is **VT-d**: mobile Pentium/Celeron SKUs
  such as the Pentium 3825U and Celeron N3050 ship with VT-x but no VT-d. No
  IOMMU means no VFIO. Verify the exact CPU on Intel ARK before investing in
  this path.

If passthrough ever becomes available: `intel_iommu=on`, iGPU bound to
`vfio-pci`, host not driving it (`i915.modeset=0` or blacklist) and therefore
headless over SSH. Prefer GVT-d (full device) over GVT-g (mediated vGPU) —
mediation inserts a layer between driver and hardware, and during bring-up every
"is this bug mine or the mediator's" costs days. For render-only work,
OpRegion/VBIOS handling can be skipped entirely.

**Consequence for the plan:** bare metal is the guaranteed path on both cards,
so M99's netconsole is a hard prerequisite for all hardware work, not a
convenience. It is scheduled first for that reason.

If the laptop turns out to be Gen7 (HD 4000 class) rather than Gen8: Mesa uses
`crocus`, not `iris`, and the kernel must process execbuffer **relocations**
instead of softpin. Add ~1 month to M102.

## Current gaps (graph-verified, 2026-07-31)

A search across the whole kernel for
`msi|ioremap|firmware|fence|scatter|workqueue|completion` returns **only**
`kthread_create*`. Missing entirely: `dma-fence`, `drm_gpu_scheduler`, GEM/TTM
BO manager, GPUVM, `scatterlist`, `request_firmware`, workqueue, `completion`,
`idr`/`xarray`, MSI/MSI-X, PAT/MTRR/write-combining, `clflush`/`wbinvd`/`mfence`,
netconsole.

`pci.c` has config read/write plus `pci_find_device`/`pci_find_class` and
nothing else — no BAR enumeration, no bus-master enable, no capability walk.

`kernel/dev/drm.c` is a KMS shim bound to virtio-gpu: 8 ioctls, hardcoded single
`DRM_CRTC_ID`/`DRM_CONNECTOR_ID`/`DRM_ENCODER_ID`, `connector_type = VIRTUAL`,
mode from `virtio_gpu_get_mode()`, BOs in a static `DRM_MAX_OBJECTS` array
allocated **physically contiguous** via `pmm_alloc_frames`, GEM handle is a bare
`client->next_handle++`. `drm_ioctl` is cyclomatic 56 / cognitive 161 in one
switch and will not absorb ~30 more ioctls.

Already present and reusable: `vmm_map_mmio`, `irq_register_handler`,
`kthread_create*`, `scheduler_wait_prepare/commit/cancel`, `scheduler_wake_all`,
FPU save/restore (`fpu.S`, `fxsave64`/`fxrstor64`), a full UDP stack
(`udp_send`, `udp_register_handler`), and — importantly — `virtio_gpu.c` already
implements a command-submission ABI (`vgpu_ctx_create`, `vgpu_res_create_attach`,
`vgpu_submit_stream`, `vgpu_transfer_to_host`), structurally the same shape as
amdgpu CS / i915 execbuffer.

---

## M99: Driver infrastructure

### T1 — netconsole · ~1 week

`tests/smoke.sh` reads COM1 and nothing else. That is fine under QEMU/KVM, but
the Pavilion has no serial port, so bare-metal bring-up there has no log channel
at all. Ship `kernel/dev/netconsole.c`: klog ring buffer drained by a kthread and
shipped as UDP datagrams via the existing `udp_send`, enabled by
`b1nix.netconsole=<ip>:<port>`, plus a host-side collector script.

Draining from a kthread rather than sending inside `console_write` avoids
re-entrancy: `console_write` takes `console_lock` with IRQs off and is called
from interrupt context, so a synchronous send would deadlock against the NIC
driver. A best-effort synchronous flush on the panic path can follow later —
`console_bust_lock()` already establishes that pattern.

Marker: `M99-NETCON: ok udp-log`

### T2 — Memory typing · ~1 week

`IA32_PAT` MSR programming, a `VMM_WC` flag threaded through `vmm_map_page` /
`vmm_map_mmio`, and `clflush` / `wbinvd` / `mfence` primitives. Mandatory for any
GPU: GTT and scanout buffers mapped UC read catastrophically slowly, and the
display engine does not snoop LLC — framebuffers must be flushed to memory before
scanout. Also speeds up the existing `fb.c` path on its own.

Marker: `M99-MM: ok pat-wc`

### T3 — PCI modernisation · 1–2 weeks

BAR enumeration and sizing, bus-master enable, capability walk, PCIe extended
caps, MSI and MSI-X. Navi additionally requires PCIe atomics
(`pci_enable_atomic_ops_to_root`). Intel additionally needs stolen memory
(DSM/GSM) read from **host-bridge** config `0x5C`/`0x70`, not from the GPU.

Markers: `M99-PCI: ok bar-enum`, `M99-PCI: ok msi`

---

## M100: linuxkpi compatibility layer · 3–4 weeks

Own MIT headers under `kernel/include/lkpi/`: `idr`, `completion`, workqueue over
`kthread_create` + `scheduler_wait_*`, `scatterlist`, `request_firmware` over VFS,
mutex/rwsem/spinlock wrappers over the existing primitives, `ioremap` over
`vmm_map_mmio`, dma-mapping.

M95/M96 (loadable kernel modules) pair naturally here — Linux ships GPU drivers
as modules, and `.ko` support would make driver iteration far cheaper. Not a
dependency, but worth sequencing before M102 if M95 is close.

Marker: `M100-LKPI: ok selftest`

---

## M101: DRM core, proven on virtio-gpu · ~2 months

### T1 — dma-fence + scheduler

Implement `dma-fence` and a minimal `drm_gpu_scheduler`, then convert
`vgpu_submit_stream` off `virtio_gpu_wait_used` onto them. This is the point of
the milestone: the shim gets exercised under QEMU in the smoke suite, where
failures are reproducible, rather than debugging shim bugs and unfamiliar
hardware simultaneously six months from now. FreeBSD shipped `i915` before
`amdgpu` for the same reason.

Markers: `M101-FENCE: ok signal`, `M101-SCHED: ok submit`; M50–M52 and M91 stay green.

### T2 — GEM with discontiguous BOs

sg-backed buffer objects (the current contiguous-only `pmm_alloc_frames` is the
first thing real drivers break), a real handle table on `idr`, and `drm_ioctl`
split out of its single switch.

Marker: `M101-GEM: ok sg-bo`

---

## M102: Intel i915 + Mesa iris · ~3 months

GTT/PPGTT, stolen memory, execlists submission, contexts. No firmware is
required on Gen8/Gen9 — GuC/HuC submission is optional and off by default, DMC
only affects display power states. **Inherit the firmware-configured mode**; no
modesetting initially, which skips panel power sequencing and backlight PWM.

`iris` uses softpin exclusively (`EXEC_OBJECT_PINNED`, 48-bit PPGTT), so the
kernel needs no relocation processing, and its shader compiler is Intel's own
NIR backend — **no LLVM rebuild required**.

Markers: `M102-I915: ok ring-submit`, `M102-I915: ok triangle`
(under KVM passthrough first, then repeated on the Pavilion bare metal via netconsole)

---

## M103: amdgpu on RX 6600, render-only · 6–9 months

Reuses M99–M101 unchanged. Build without DC: no KMS, scanout stays on the GOP
framebuffer (already proven working on this card), render offscreen and blit.
This drops `display/dc` entirely and with it its double-precision DML, which
would otherwise need per-TU SSE flags and a `kernel_fpu_begin` on top of the
existing `fpu.S`.

Still required: PSP (`psp_v11_0` — ring, TMR, firmware load protocol; mandatory
on Navi, unlike Polaris where microcode loads over MMIO), SMU 11 PMFW mailbox,
GFX10.3 KIQ/MQD, GPUVM, and visible/invisible VRAM windowing (8 GB VRAM behind a
256 MB BAR without ReBAR). Finally `libdrm_amdgpu` plus a `libLLVM.so` rebuilt
with the AMDGPU target (currently X86 only) for radeonsi.

Markers: `M103-AMDGPU: ok gfx-ring`, `M103-AMDGPU: ok triangle`

---

## Notes

- Builds happen on the dev host; the Pavilion boots the ISO from USB. Its
  dual-core Pentium is not a build machine.
- M101 onward must keep the existing graphics smoke tests (M50–M52, M91) green;
  they are the regression net for the shim.
- Cumulative: shim proven under QEMU ~4 months, accelerated triangle on Intel
  ~7 months, accelerated triangle on RX 6600 ~15 months.
