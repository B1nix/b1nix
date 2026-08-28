# i915 on Gen8/Gen9.5 — display, submission and Mesa iris

Milestone **M102a**. The driver is upstream's `i915`, imported unmodified and
cut to the Gen8/Gen9.5 paths (no firmware is needed on these parts). Everything
it stands on is the shim described in [`lkpi-primitives.md`](lkpi-primitives.md);
a fault seen inside the driver is a defect there, and is fixed there.

The development machine passes its UHD 630 through with VFIO; the guest drives
the physical HDMI panel.

## Host setup for the passthrough

`intel_iommu=on` on the host command line, the iGPU bound to `vfio-pci`, and
the host not driving it (`i915.modeset=0`, or blacklist the module) — which
makes the host headless, so drive it over SSH. The UHD 630 must sit alone in
its IOMMU group; it does here (group 0). QEMU needs `memlock` unlimited or the
device's memory cannot be pinned.

GVT-d (the whole device) rather than GVT-g (a mediated vGPU): mediation puts a
layer between the driver and the hardware, and during bring-up every "is this
mine or the mediator's" costs days. Render-only work can skip OpRegion/VBIOS
handling entirely.

The earlier plan recorded this route as blocked — the desktop BIOS was thought
to hide the iGPU, and the laptop SKUs (Pentium 3825U, Celeron N3050) genuinely
have VT-x without VT-d, so no IOMMU and no VFIO there. The desktop turned out
to work, and M102a was closed through passthrough rather than bare metal.

## Display

A Wayland compositor (cage, then sway, on wlroots with pixman) drives the
physical monitor through the passed-through UHD 630 — atomic modeset, page
flips, and a photograph of the panel matching the guest's own screenshot.
sway runs at the EDID's preferred 1920x1080 with swaybg and a foot terminal:
[`images/m102a-sway-on-monitor.jpg`](images/m102a-sway-on-monitor.jpg).

Two kernel faults were behind the failures on the way there, neither in the
driver:

- `schedule()` in the shim was mapped to a yield, which left every blocking
  atomic commit parked with nothing able to wake it.
- Threads carried private copies of the break and of the mapping-list head, so
  one thread mapped fresh zero pages over another thread's live heap. That was
  the real cause of both the 720x400 fallback and the crashes behind it.

## Surviving client churn

sway keeps its display, its IPC and its window tree across clients starting,
being asked to quit, and being killed outright — six runs out of six on six
CPUs. The workload is in `tools/soak/`. Three kernel faults were behind the
stalls:

- `sigsuspend` restored the caller's mask *before* delivery.
- A waker cleared the wait channel of a task that had already blocked again.
- `epoll_pwait`/`ppoll`/`pselect` ignored their mask argument.

## GT: contexts, execlists and interrupt-driven signalling

GTT/PPGTT, contexts and execlists submission run on the hardware: four engines
(rcs0, bcs0, vcs0, vecs0), all execlists, 4 GiB of GGTT and full 48-bit PPGTT,
and an empty request submitted to each engine's kernel context executes and
retires. A waiter with nobody polling on its behalf is woken by the completion
interrupt.

`b1nix.i915-gt-probe` reports **three claims separately** — execution,
signalling, and *unprompted* signalling — because they fail apart, and a probe
that polls its own fence will report success on a machine whose interrupts never
arrive.

Six shim defects were behind it, again none in the driver:

| Defect | Effect |
|---|---|
| jiffies counted as scheduler ticks | every driver timeout was wrong by the tick ratio |
| `wake_up_process` was a stub | a woken waiter stayed asleep |
| `schedule_timeout` slept its whole timeout and reported expiry either way | no caller could distinguish a wake from a timeout |
| `current` was shared per CPU rather than per task | a task read another task's identity |
| `irq_work` re-initialised while queued | the queue was corrupted under load |
| dma-fence never asked the driver — neither to arm signalling nor whether the work was already done | fences that the hardware had completed never signalled |

## EXECBUFFER2 and the ABI

The ioctl is served, and the ABI is the pinned Mesa's own: nothing in the shim
touches the argument. `lkpi_drm_ioctl` hands it to the imported `drm_ioctl`,
which does every copy itself, so the structures are the ones in the import
tree's uapi header rather than a copy that could drift.

Counted at the crossing under `b1nix.i915-execbuf`, iris is softpin-only exactly
as claimed: every object carries `EXEC_OBJECT_PINNED`, every batch sets
`I915_EXEC_NO_RELOC`, and the total relocation count over a run is zero — the
relocation path is never entered.

## Mesa iris renders, and pixels say so

`/bin/gl_probe` (`b1nix.glprobe`) brings up EGL on the DRM device itself, clears
an off-screen target to a colour with three different channels, rasterises a
triangle through a compiled shader, then reads both back and checks them apart:

```
renderer Mesa Intel(R) UHD Graphics 630 (CFL GT2)
clear pixel      rgba=64,128,191,255  -> ok
triangle pixel   rgba=255,0,128,255   -> ok
background pixel rgba=64,128,191,255  -> ok
```

No LLVM was rebuilt — the shaders go through iris's own NIR backend. The
renderer string is part of the result on purpose: llvmpipe passes every pixel
check and says nothing about the GPU.

## Open

- **A compositor's submissions still fail.** sway on the gles2/iris renderer
  gets `-ENOSPC` from its third `EXECBUFFER2` after 6.8 s and aborts, while the
  same context's earlier batches succeed. `-ENOSPC` there is `eb_reserve` giving
  up after evicting the whole address space, so it is the binding path — softpin
  addresses that cannot be honoured — and not the submission itself. The offsets
  of a failing batch are dumped under `b1nix.i915-execbuf`.
- **Bare metal on the Pavilion's Gen8**, with logs over netconsole (the
  guaranteed path — no OS on the laptop, the ISO boots from USB).
