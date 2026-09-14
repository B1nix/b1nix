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

## Open

- **Compositor on iris.** sway on the gles2/iris renderer was last seen getting
  `-ENOSPC` from its third `EXECBUFFER2` (earlier batches in the same context
  succeed) — `eb_reserve` failing to bind softpin addresses, not submission
  itself. Offsets are dumped under `b1nix.i915-execbuf`.
- **Bare metal on Gen8** (HP Pavilion), logs over netconsole, ISO from USB.
