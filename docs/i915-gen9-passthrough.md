# i915 on Gen8/Gen9.5 — display, submission and Mesa iris

Milestone **M102a**. The driver is upstream's `i915`, imported unmodified and
cut to the Gen8/Gen9.5 paths (no firmware needed). It runs on the linuxkpi shim
in `kernel/lkpi/`; a fault seen inside the driver is a shim defect and is fixed
there. Built by default on x86_64 (`B1NIX_I915 ?= 1`).

The reference machine passes a UHD 630 through with VFIO (GVT-d, the whole
device) and the guest drives the physical HDMI panel.

## Host setup

- `intel_iommu=on`; the iGPU bound to `vfio-pci`; the host not driving it
  (`i915.modeset=0` or blacklisted), so the host is headless — use SSH.
- The iGPU must be alone in its IOMMU group.
- QEMU needs `memlock` unlimited.

`sh tools/run/run-i915-passthrough.sh --preflight` prints the missing steps for
this machine; without arguments it runs the guest (env: `IGD_BDF`, `MEM_MB`,
`TIMEOUT`, `MACHINE=legacy|q35`, `ISO`; log in `smoke_run/i915-passthrough.log`).

## Images and flags

The passthrough ISOs (`make iso-pass`, `iso-pass-sway`, `iso-pass-probe`,
`iso-pass-headless`, `iso-pass-bright`) differ only in the cmdline
(`SMOKE_CMDLINE_pass*` in the `Makefile`). `/etc/i915-sway.sh` runs from
inittab under `b1nix.i915sway`.

| Flag | Effect |
|---|---|
| `b1nix.i915sway` | run the compositor-on-panel script |
| `b1nix.i915-gt-probe` | GT report: engines, submission method, GGTT/PPGTT, one empty request per engine to retirement; reports execution, signalling and *unprompted* (interrupt-driven) signalling separately |
| `b1nix.i915-execbuf` | count `EXECBUFFER2` objects/pins/relocations; dump object offsets of refused batches |
| `b1nix.glprobe` | `/bin/gl_probe` on iris (`=virtio_gpu` probes virgl instead) |

## What works

- **Display.** sway (wlroots, pixman) at the EDID's 1920x1080 with swaybg and
  foot, atomic modeset and page flips:
  [`images/m102a-sway-on-monitor.jpg`](images/m102a-sway-on-monitor.jpg).
  Survives client start/quit/kill churn (workload in `tools/run/soak/`).
- **GT.** Four engines (rcs0, bcs0, vcs0, vecs0) on execlists, 4 GiB GGTT, full
  48-bit PPGTT; requests execute and retire, waiters are woken by the completion
  interrupt.
- **EXECBUFFER2.** `lkpi_drm_ioctl` hands the argument untouched to the imported
  `drm_ioctl`, so the ABI is the import tree's uapi header. iris is softpin-only:
  every object `EXEC_OBJECT_PINNED`, every batch `I915_EXEC_NO_RELOC`, zero
  relocations.
- **Mesa iris.** `gl_probe` brings up EGL on the DRM device, clears to a
  three-channel colour, draws a shader triangle and checks clear, triangle and
  background pixels separately, with renderer `Mesa Intel(R) UHD Graphics 630
  (CFL GT2)` (the renderer string is checked so llvmpipe cannot pass).

- **Compositor on iris.** sway on gles2/iris composes on the panel
  (`b1nix.i915gl`, image built with `B1NIX_GPU_DRV=1`, run with
  `NO_VIRTIO_GPU=1` so wlroots does not take the virtio card for EGL). Two shim
  bugs stood in the way: a dma-buf `lseek(SEEK_END)` answered 0, so iris
  softpinned an imported 8 MiB BO over its neighbours (`-ENOSPC`); and a
  `MAX_SCHEDULE_TIMEOUT` sleep wrapped to an immediate timeout, so waiting for
  a scanout buffer still being rendered failed the modeset with `-ETIME`.
  `MACHINE=q35` works too with `IGD_DEV_EXTRA=addr=02.0,x-igd-opregion=on`.

## Open

- **Bare metal on Gen8** is still untested. A Kaby Lake R laptop (UHD 620)
  runs sway on iris on its panel: boot it over PXE with
  `tools/run/pxe-serve.sh <iso-stage>` and collect the log with
  `b1nix.netconsole=<host>:<port>`. A root module has to fit below 4 GiB for
  Limine, so the live root is packed tight (ext4, ~340 MiB).

## The uncore->lock probe lockup (unreproducible)

Roughly one passthrough boot in five used to die with `SPINLOCK LOCKUP` on
`uncore->lock` early in probe. On 2026-09-16 it did not reproduce in 30
probe-only boots on the current kernel, nor in 30 on a kernel from before that
day's work, so no fix can be claimed for it and there is nothing to test
against. The loop that produced those numbers: a probe-only ISO
(`b1nix.i915sway b1nix.drm-probe-only b1nix.drm-debug console=hvc0`) booted
through `tools/run/run-i915-passthrough.sh`, graded on `I915-SWAY: probe only,
done` against `lockup|spin_lock_stuck|KERNEL PANIC`.
