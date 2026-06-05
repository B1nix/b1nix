# M37 — Real-Hardware Device Drivers (NIC + USB keyboard)

Two drivers added toward booting b1nix on a real x86-64 PC, alongside the
existing ACPI/MADT de-hardcoding work tracked in the M37 roadmap entry.

The target machine (the development host) is an ASRock/Coffee-Lake box:

| Function | Device | Status |
|----------|--------|--------|
| Ethernet | Intel I219-V `8086:15b8` (Linux `e1000e`) | driver attaches via the e1000 family path |
| Wi-Fi    | Intel Wireless 7260 `8086:08b1` (`iwlwifi`) | out of scope (proprietary firmware + mac80211) |
| SATA     | Intel 200-series AHCI `8086:a282` | existing `ahci.c` (spec-compliant) |
| NVMe     | Silicon Motion SM2263 `126f:2263` | existing `nvme.c` (spec-compliant) |
| USB      | Intel + Renesas xHCI | new `usb_xhci.c` (HID keyboard) |
| Display  | AMD Navi 23 | framebuffer from GRUB/Multiboot2 (no native driver) |

## Generic netdev model

`kernel/include/b1nix/netdev.h` defines `struct netdev` — `{mac, irq,
transmit(hdr, payload), poll, irq_ack}` — with a first-wins registry
(`netdev_register` / `netdev_active`) in `kernel/net/net.c`. The protocol stack
is now driver-agnostic: `net_send_ethernet` builds the 14-byte ethernet header
and hands it (plus the payload) to `nd->transmit`; `net_poll` pumps `nd->poll`;
the IRQ/MAC/link plumbing reads the active device. The header and payload are
passed separately so no large frame buffer is needed on a deep send stack.

virtio-net moved verbatim into `kernel/dev/virtio_net.c` implementing this
interface; behaviour is unchanged.

## e1000 / e1000e / I219 (`kernel/dev/e1000.c`)

A legacy (8254x-style) descriptor-ring driver. The whole Intel gigabit family
exposes the same MMIO register layout, so one driver covers the QEMU-emulated
82540EM (`-device e1000`) and 82574L (`-device e1000e`) and the PCH-integrated
I217/I218/I219 parts including the host's I219-V (`8086:15b8`).

- BAR0 mapped arch-guarded (direct map on x86-64, `vmm_map_mmio` on 32-bit).
- Reset, MAC from RAL/RAH (EEPROM fallback), set-link-up, multicast clear,
  RX/TX legacy descriptor rings, RCTL/TCTL/TIPG. All hardware waits are bounded.
- `net_init` probes virtio-net first (so it stays active under QEMU) then e1000
  unconditionally; on real hardware virtio is absent and e1000 becomes active.

## USB xHCI + HID keyboard (`kernel/dev/usb_xhci.c`)

A minimal polling xHCI driver — controller bring-up (DCBAA, command ring, event
ring + ERST), port reset, Enable Slot, Address Device, EP0 control transfers
(device + config descriptors, SET_CONFIGURATION, SET_PROTOCOL boot, SET_IDLE),
Configure Endpoint for the interrupt-IN endpoint. `usb_kbd_poll()` runs from the
BSP timer tick, drains interrupt-IN transfer events, and translates each 8-byte
HID boot report to PS/2 set-1 make/break scancodes fed through
`ps2_kbd_handle_byte()` — so USB keys reuse the existing shift/ctrl/signal and
line-discipline handling. Scope: one controller, one keyboard, no hubs, no
interrupts.

## Verification

`tests/smoke.sh` attaches a second NIC (`-device e1000` on `net1`, override with
`E1000_MODEL=e1000e`) and a `qemu-xhci` + `usb-kbd`, and checks the self-test
markers emitted by the M37 test section:

```
M37-E1000: ok init / mac / link / tx / rx-arp     (ARP exchange over a 2nd SLIRP backend, read straight off the ring)
M37-USB:   ok xhci-init / port-reset / slot-enabled / device-addressed
           ok descriptors vid=0x0627 pid=0x0001 / hid-config ep=0x81 / hid-translate
```

Both run with the boot still reaching `B1NIX-TEST: done` and the existing
virtio-net / PS/2 paths intact.

## What is NOT verified from QEMU (needs on-metal testing)

- **I219-V link/PHY bring-up.** The driver attaches (device ID in the table, MAC
  read, ring setup) but the I219's PCH MAC↔PHY interface has real-hardware
  quirks (ULP disable, PHY power/config, the FWSM/SWSM semaphore for NVM/PHY
  access) the legacy path does not perform. Expect to add quirks after a first
  boot on the metal.
- **Live USB keypresses.** The interrupt-endpoint → `usb_kbd_poll` →
  `ps2_kbd_handle_byte` path is wired and the HID→scancode translation is
  verified with a synthetic report, but real keystrokes can't be injected
  headlessly. On real hardware (or QEMU with the monitor `sendkey`) the
  keyboard should type into the console.
- On-metal **runtime** display mode-setting (changing resolution after boot)
  remains deferred — see below; the boot-time framebuffer works on both BIOS
  and UEFI.

## Booting on real hardware (USB stick, BIOS or UEFI)

`make ARCH=x86_64 iso` produces a **hybrid** image with `grub2-mkrescue`. The
ISO's El-Torito catalog carries two boot images — a BIOS one
(`i386-pc/eltorito.img`, with the isohybrid boot-info-table) and a UEFI one
(`/efi.img`, an EFI System Partition holding GRUB's `BOOTX64.EFI`) — and the
image starts with a DOS/MBR boot sector, so it is *isohybrid*: the very same
file boots from an optical drive **and** when written raw to a USB stick, on a
legacy-BIOS or a UEFI PC. No separate bootloader is needed — GRUB's EFI image is
the UEFI bootloader.

Flash it (replace `sdX` with your stick; this erases the stick):

```sh
make ARCH=x86_64 iso
sudo dd if=build/x86_64/b1nix.iso of=/dev/sdX bs=4M conv=fsync status=progress
```

Then boot the target machine from USB. On a UEFI box, pick the USB entry in the
firmware boot menu (Secure Boot must be off — GRUB's image is unsigned). GRUB
shows the b1nix menu; the default entry boots the kernel.

### Graphics

`boot/grub/grub.cfg` does `insmod all_video` + `set gfxmode=1024x768x32,…` +
`set gfxpayload=keep`, and the Multiboot2 header (`kernel/arch/x86_64/boot.S`)
requests a 1024×768×32 framebuffer. GRUB programs that mode via **VBE** on BIOS
or **GOP** on UEFI and hands the linear framebuffer to the kernel, which picks
it up from `bootinfo_get()->framebuffer` (exactly the path QEMU exercises). So
boot-time mode-setting works on real hardware of either firmware type. True
*in-kernel runtime* mode-setting (resolution changes after boot) would need UEFI
GOP runtime services or a v86 INT-10h emulator and is deferred — a console/TUI
OS does not need it.

### Input

Real machines have no PS/2 controller, so console input comes from the USB HID
keyboard via the xHCI driver (above). The `ps2_kbd` path is kept for QEMU and
the rare board that still exposes an i8042.

## True On-Demand Live CD Booting from USB

To avoid copying the entire `rootfs.img` into RAM, b1nix implements a Unix-like USB on-demand boot sequence:
1. Probes xHCI controllers and registers USB mass storage devices.
2. Mounts the ISO9660 filesystem on the USB device at `/mnt/iso`.
3. Registers a loopback block device `loop0` backed by the file `/boot/rootfs.img` inside the ISO.
4. Mounts the loopback device as the primary `ext4` rootfs at `/` on normal boots.
   In `b1nix.test=1` smoke mode it mounts at `/mnt/root` instead, preserving the initramfs so the test harness can keep running.
5. After a normal root switch, remounts the ISO at the new root's `/mnt/iso`; the loop device keeps its original backing node open, so the rootfs remains readable throughout the switch.

### Kernel Command Line Options

To boot using this method on real hardware, specify the following parameters in `grub.cfg` or the GRUB command line:
- `root=liveiso`: Requests the loopback mount flow.
- `b1nix.xhci.run` (or `b1nix.xhci.enum`): Forces the native xHCI host controller driver to initialize and probe connected USB devices. Without this flag, xHCI is left uninitialized to prevent bios-handoff issues from breaking BIOS keyboard emulation.

### Whole-Disk ISO vs Partition Probing

Depending on how the USB drive is partitioned or written (e.g. raw hybrid ISO written via `dd` versus written to a specific partition), the kernel might see the ISO9660 filesystem on the whole disk (e.g. `usb0`) or on a partition (e.g. `usb0p1`).
To handle this dynamically, the boot flow iterates through all registered block devices starting with `"usb"` (e.g., `usb0`, `usb0p1`, `usb1`) and attempts to mount each as `iso9660` at `/mnt/iso`. Probing succeeds at the first device that mounts successfully and contains `/boot/rootfs.img`.

### Timing and Retries

USB mass storage devices and controllers (especially on real hardware) require time to initialize, reset the port, assign slots, read descriptors, and register the storage device.
The boot process handles this asynchronously:
- The kernel loops up to 50 times, sleeping for 10 scheduler ticks (100ms) per iteration (total up to 5 seconds), waiting for the first block device starting with `"usb"` to be registered.
- Once at least one USB block device is discovered, the kernel initiates the partition/disk probing sequence.
- If no USB devices are registered within the timeout, or if mounting the ISO/loopback fails, the boot process automatically falls back to the standard `ram0` ramdisk module boot so the system remains bootable.
