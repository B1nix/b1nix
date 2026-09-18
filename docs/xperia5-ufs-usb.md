# Xperia 5: rootfs on UFS, network over the USB cable

The Xperia 5 boot image is 64 MiB. The kernel takes about 8.5 MiB of it, and the
staged rootfs is 168 MiB, so what fit was a trimmed shell environment in the
ramdisk. Loading the image into RAM with `fastboot boot` does not work on this
phone. Two drivers take the rootfs out of the boot image instead.

| | UFS (`kernel/dev/ufs.c`) | USB gadget (`kernel/dev/dwc3_gadget.c`) |
|---|---|---|
| What it gives | The whole rootfs on the phone's own flash, in `system_a` | `usb0`, a CDC-ECM Ethernet link to the computer the cable goes to |
| Enabled by | `b1nix.ufs` (the PCI controller QEMU emulates is always probed) | `b1nix.usb-gadget` |
| Verified | QEMU `-device ufs` (UFS-SMOKE, gfx lane), plus a boot to login with `/` on a GPT partition | Builds only: QEMU has no DWC3 device model |
| On the phone | Not run yet | Not run yet |

## Build and flash

```sh
make bahamut-ufs                                  # kernel + rescue ramdisk + 1 GiB rootfs image
./tools/boards/sony-xperia-5/flash_ufs_rootfs.sh  # system_a + boot_a, set slot A, reboot
./tools/boards/sony-xperia-5/restore_slot_b.sh    # back to Android, as before
```

`flash_ufs_rootfs.sh` compares the image size with the size fastboot reports for
`system_a` and asks before writing. `system_a` is slot A's system partition. On
this phone it is also slot A's retrofit "super". Android on slot B does not use
it, but an Android OTA would overwrite it. Size the image with
`BAHAMUT_ROOTFS_MB`; fastboot sparses it on the way.

The boot image still carries the trimmed ramdisk as a rescue system, labelled
`b1nix-rescue`. The kernel mounts the first device labelled `b1nix-root` (the
UFS partition). When UFS does not come up, it falls back to `ram0` by name.

`make bahamut-ufs BAHAMUT_USB_GADGET=` builds without the USB gadget, in case its
bring-up is what stops the boot. The gadget probes after UFS and before the root
mount.

## UFS

- **Keeps the bootloader's link.** ABL reads the boot image from this
  controller, so on entry the link is up at a high-speed gear with a calibrated
  PHY. If the controller reports itself enabled, with a device present and both
  lists ready, the driver clears run-stop, re-points both list bases at kernel
  memory and sends a NOP. Only if that fails (or with `b1nix.ufs-reset`) does it
  run the full sequence: HCE reset, UIC link startup, `fDeviceInit`. The full
  sequence on Qualcomm adds only the two quirks that need no clock or PHY
  driver (`QUNIPRO_SEL`, TX LCC off), and it leaves the link in PWM gear 1.
- **Power and clocks first.** Before the first register read, the driver clears
  `ufs_phy_gdsc`'s collapse bit and sets the UFS branch clocks' enable bits in
  GCC. It reports the GDSC and the AXI clock register values. "Powered" on
  SM8150 is `POWER_UP_COMPLETE` in the CFG register (GDSCR + 4), not `PWR_ON`.
- **4 KiB logical blocks.** A LUN is registered as `sd*` with 512-byte sectors,
  and `block_device.lb_size` records the medium's own block size. The GPT
  scanner multiplies every LBA by it, and partitions now carry their GPT names
  (`blk_partition_label`). A write that covers part of a logical block is read,
  patched and written back. The registry grew to 255 devices and 200
  partitions: a phone has six LUNs and about 90 partitions.
- **Writes are fenced.** A LUN refuses every write outside a partition that is
  named in `b1nix.ufs-rw=<gpt name>[,…]` or already holds an ext filesystem
  labelled `b1nix-root`. On a phone the other partitions hold the bootloaders
  and modem firmware, and one stray write there bricks the handset rather than
  corrupting a filesystem.
- **One request at a time.** Slot 0, polled, through a 256 KiB bounce buffer
  below 4 GiB. That is enough for a root filesystem; speed was not a goal.

What prints on the panel, in order: `ufs: qcom,ufshc at …`, the GDSC/clock
values, `ufs0: cap= ver= hce= hcs= qcom-hw=`, then either `kept the
bootloader's link` or `link started from reset`, or the step that failed
(`HCE did not set`, `link startup failed, HCS …`, `device did not answer a
NOP`). Then one line per LUN and one line per writable partition.

## USB gadget

Bring-up, each step printed:

1. GCC: `usb30_prim_gdsc` on; block resets of the controller
   (`GCC_USB30_PRIM_BCR`) and the HS PHY (`GCC_QUSB2PHY_PRIM_BCR`); master, sleep,
   mock-UTMI, AXI and clkref clocks on.
2. The Synopsys femto HS PHY (`hsphy@88e2000`), programmed with the init
   sequence from Linux's `phy-qcom-snps-femto-v2` plus the board's override
   `<0x43 0x70>`. Its regulators are RPMh LDOs that nothing here controls, so
   they are assumed to be on.
3. QSCRATCH: the UTMI clock replaces the PIPE clock (the SuperSpeed PHY is never
   brought up, so the link is USB 2.0 only), and VBUS-valid is forced (VBUS
   sensing lives in the PMIC).
4. DWC3 in device mode, `GSNPSID` checked, one event buffer, polled from the
   network stack's 100 Hz poll hook.

The USB side has the standard control requests and one configuration: a CDC
ECM control interface with an interrupt IN endpoint for link notifications,
and a data interface whose alternate setting 1 carries a bulk IN/OUT pair. It
uses the Linux "Ethernet Gadget" ids 0525:a4a1, so hosts bind their stock
driver. When the host selects alternate setting 1, `usb0` takes
`b1nix.usb-ip` (default 172.16.42.1/24) and the link goes up.

On the Mac:

```sh
sudo ifconfig en7 172.16.42.2 netmask 255.255.255.0
```

```sh
ssh root@172.16.42.1
```

The interface name (`en7` above; `en8` on the Mac this was brought up on) is whatever `ifconfig` lists for the new
"b1nix USB Ethernet" device. The password is `root`.

## If it does not work

- **Screen stops at a `ufs:` line.** A register read did not return: the block
  is unclocked or unpowered. Note the GDSC/CFG values printed just before it.
- **`link startup failed`** on the full path. ABL powered the PHY down on exit,
  so the PHY needs its calibration tables. That is the next piece of work, and
  it needs the values on the panel.
- **No `usb-gadget: connected`** after plugging in. The PHY did not come up:
  check that `GSNPSID` printed, then suspect the PHY regulators.
- **`rootfs: staying on initramfs`** or `ram0 mounted`. No partition labelled
  `b1nix-root` was found: check the `lun N: … partitions` lines.

## What running it on the phone found (2026-09-14)

- **UFS works.** The kernel keeps ABL's link and mounts `/` from `system_a` (`sda44`, label `b1nix-root`), booting to `localhost login:`. The phone has 79 partitions on LUN 0 and 2 each on LUNs 1 and 2.
- **GCC starts at 1 MiB,** inside the first 2 MiB the identity map leaves unmapped. It is reached through `vmm_map_mmio`; a direct read took a translation fault.
- **Device DMA is fenced by the hypervisor, not the SMMU.** The UFS stream's context bank has translation off (`SCTLR.M=0`). A NOP whose descriptor sat at `0xe9000000`–`0xffffd000` was answered; at `0xc0000000` or in the page allocator's gigabyte (`0xa9000000`–`0xe9000000`) the phone reset. UFS DMA now lives at `0xf0000000` and the gadget's at `0xf1000000`.
- **USB's SMMU stream (0x140) has no stream-match entry,** and unmatched streams fault. The gadget copies UFS's S2CR into a free group; the read-back shows it takes.
- **The gadget enumerates on macOS** ("b1nix USB Ethernet", 480 Mb/s, `en8`). Ping, SSH (`ssh root@172.16.42.1`) and netconsole (UDP to 172.16.42.2:6666) work since the RAM fix below (2026-09-18). Steady-state ping is 40–150 ms: the gadget is polled from net_task's backoff. ARP replies from the Mac arrive and are cached (checked with an AF_PACKET sniffer on the phone). The "endless ARP" seen earlier was net_task frozen by the hang below, plus requests sent before the Mac had configured `en8`. ARP probes from 0.0.0.0 are no longer cached. One resolution still sends a broadcast every 10 ms until the reply is processed (`ipv4_send`'s wait loop).
- **The page allocator's gigabyte had a hole in it.** SM8150 used a fixed window, 0xa9000000–0xe9000000. ABL's `/memory` (Android `/proc/iomem`) has RAM at `a8800000-bcbfffff` and `c0000000-ffafffff`: the 52 MiB between belong to the hypervisor. The first page handed out from there hung the CPU on its first access, with no fault and no interrupt. That happened at the same point of every boot, a `fork` in `/etc/i915-sway.sh` just after OpenRC. With the CPU went the panel, the log mirror and the gadget's polling, so macOS stopped half-way through enumeration. QEMU has no hole, so it never reproduced there. `bootinfo.c` now clips the window to ABL's banks (972 MiB).
- **Logs without a serial port:**
  - The console is copied into the ramoops console zone (`0xffc80000`, Android reads it as `/sys/fs/pstore/console-ramoops-0` after a warm reset).
  - The same zone is mirrored every 2 s to `system_a` at MiB 1024, readable from Android with `dd ... skip=1024 count=1`.
  - Panics paint a full-screen stop screen.
