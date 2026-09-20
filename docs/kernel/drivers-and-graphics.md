# Drivers, modules and graphics

Milestones M7, M9, M38, M47, M49–M50, M70, M75–M76, M79, M95–M96, M98–M101 and
M102a.

## Devices

The kernel's own drivers cover:

- **virtio:** block, network, GPU, input, console and 9p;
- **storage:** AHCI (with ATAPI) and NVMe;
- **USB:** xHCI with HID and mass storage (M76);
- **input:** the PS/2 keyboard and mouse, delivered to userspace as evdev;
- **network:** e1000/e1000e and r8169;
- **display:** a framebuffer console and `/dev/fb0` with virtual terminals
  (M7, M47);
- **audio:** Intel HDA and AC'97 behind a mixer, `/dev/dsp` and an ALSA shim
  (M38, M79);
- **clocks and buses:** the RTC, a software `/dev/watchdog`, and SMBus/I2C.

Device completions arrive through interrupts that wake the waiting task, not
through busy polling (M70). Wayland compositors run on these devices
unmodified (M49). Where no GPU driver applies, llvmpipe renders on the CPU
through `libLLVM.so` (M75).

## Loadable modules (M95, M96)

Modules are relocatable ELF objects built from kernel sources with `-DMODULE`.
The kernel stays monolithic; modules are how it stays modular.

- **Declaration.** A module declares its name, license, aliases, dependencies
  and parameters with `MODULE_*` macros. The build fails if a module needs a
  symbol that nothing exports (`tools/toolchain/kernel/check-module-syms.sh`).
- **Loading.** The loader copies the sections, resolves symbols against the
  kernel and against live modules, applies x86_64 or AArch64 relocations,
  switches text to read-execute (W^X) and calls the init function. A module
  whose vermagic does not match is refused.
- **Unloading.** Unload refuses a module that is still referenced. The VFS pins
  a filesystem's module for each mount.
- **Where they live.** Modules sit in a 128 MiB region at the top of the
  x86_64 address space, or in 16 MiB just past the kernel on AArch64.
- **Userspace interface.** `init_module`, `finit_module` and `delete_module`,
  `/proc/modules`, `/sys/module/*/parameters`, and `modules.dep` and
  `modules.alias` for BusyBox's `modprobe`. `request_module` loads a module and
  its dependencies on demand.

## Driver infrastructure (M98–M100)

- **netconsole** (`kernel/dev/netconsole.c`). Enabled only by
  `b1nix.netconsole=<ip>:<port>`, it sends the kernel log ring as UDP from its
  own thread. It never sends from inside `console_write`; when the ring laps it,
  the lost bytes are dropped rather than waited for.
- **Memory typing.** On x86_64 the PAT's slot 5 is rewritten to write-combining,
  and 4 KiB leaves that ask for WC get it. AArch64 uses MAIR slots and `DC
  CIVAC` for the same purposes.
- **PCI.** BARs are sized with decode disabled. Capabilities and extended
  capabilities (through ECAM from ACPI MCFG) are walked safely. MSI and MSI-X
  vectors each have one owner, and Intel stolen memory is decoded from the host
  bridge.
- **DMA.** With an IOMMU (VT-d with interrupt remapping, AMD-Vi, or SMMUv3 on
  AArch64) each device gets its own translated domain. Without one, buffers
  above a device's mask are bounced through a pool reserved at boot
  (`b1nix.bounce-pool=<KiB>`).

## linuxkpi and the imported DRM core (M99–M101)

`kernel/include/linux`, `kernel/include/lkpi` and `kernel/lkpi/` reimplement
Linux kernel interfaces on top of b1nix's heap, scheduler, VFS and paging: idr,
xarray, workqueues, completions, ww_mutex, RCU, scatterlists, firmware loading,
`ioremap`, the DMA API and the device model. Imported code sees only `linux/*`
and `lkpi/*` headers, and `<lkpi/env.h>` is the boundary. The rule for every
import is the same: upstream source is compiled unmodified, and a fault inside
it is a defect of the shim.

The DRM core from Linux 6.18.51 runs this way. It provides `/dev/dri/card1`
with atomic commits and master leases, dma-fence, a GPU scheduler and GEM
objects backed by scatterlists (M100, M101).

On virtio-gpu, an adapter serves Mesa's `DRM_IOCTL_VIRTGPU_*` ioctls with
upstream's layouts on top of the kernel's VirGL transport. GLES therefore runs
on the host GPU through virglrenderer. Acceleration is claimed only when a
render node opens, a Mesa DRI driver is present and `gl_probe` draws a shader
triangle and reads it back; otherwise compositors run on pixman. The
render-smoke checks prove each path by reading pixels back through
wlr-screencopy:

- `software-frame`: pixman;
- `accel-frame`: GLES on the render node;
- `fallback-engaged`: acceleration forced off, and the compositor still paints;
- `selection`: which of the paths was chosen at runtime.

The ordinary image has no DRI driver, so `accel-frame` is reported as a skip.
The accelerated path needs `make B1NIX_GPU_DRV=1 iso-gfx` and a
`virtio-gpu-gl-pci` device.

## Intel i915 on Gen8/Gen9.5 (M102a)

Upstream's i915 is imported unmodified and cut to the Gen8/Gen9.5 paths, which
need no firmware. It is built by default on x86_64.

The reference machine passes a UHD 630 through with VFIO. The host needs
`intel_iommu=on`, the iGPU bound to `vfio-pci` and alone in its IOMMU group, and
unlimited `memlock`. `sh tools/run/run-i915-passthrough.sh --preflight` lists
whatever is still missing.

On that GPU:

- the GT runs four engines on execlists with a 4 GiB GGTT and 48-bit PPGTT, and
  requests are woken by the completion interrupt;
- Mesa's iris is softpin-only and draws, and `gl_probe` checks the renderer
  string so that llvmpipe cannot pass;
- sway composes on iris at the monitor's EDID mode with atomic page flips, and
  KDE runs on it too.

A laptop with a UHD 620 runs the same stack on its own panel, booted over PXE.
Photos are in `docs/images/`.

Two things the passed-through card needed that the driver does not do on its
own. Userspace maps a dumb buffer write-combining (`vfs_inode.mmap_wc`, as
Linux does for i915): with a cacheable mapping the compositor's frame stayed in
the CPU cache and the display engine read DRAM, which looked like tearing while
a window was dragged; `b1nix.drm-mmap-wc=0` restores the old mapping for
comparison. And a monitor waking from deep sleep answers its DDC late while its
hotplug line never moves, so a probe that finds no display switches on i915's
port polling (`intel_hpd_poll_enable`): the ports are re-detected every ten
seconds and the hotplug reaches both the console and the compositor;
`b1nix.i915-hpd-poll` forces it, `b1nix.i915-edid-raw` prints the EDID bytes as
they come off the wire.

The instruments that settled those, all command-line flags, are in
`kernel/lkpi/i915_display_probe.c`: `b1nix.drm-framecap=N` keeps the last N
frames as they were at the moment the display latched them, with a hash taken
again at the end of the same frame (a difference is a write into the buffer
being scanned out) and the bounding box of that write; `b1nix.drm-eventwatch`
compares the armed and the live surface at every flip completion handed to the
compositor; `b1nix.drm-cadence` counts the frames each image was held. Their
verdict on the tearing was: pages consistent (`b1nix.pageaudit`), events never
early, the surface changing under the display -- the cache, not the driver.

An `uncore->lock` lockup during probe used to hit about one boot in five. It
did not reproduce in 30 probe-only boots, before or after the latest changes,
so no fix can be claimed for it. The loop that measures it boots
`b1nix.drm-probe-only` through the passthrough runner and grades on
`I915-SWAY: probe only, done`.
