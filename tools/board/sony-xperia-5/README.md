# B1NIX on Sony Xperia 5 (`bahamut`, J9210)

Boot image packing and flash scripts for running the `aarch64` kernel on the
Sony Xperia 5: Qualcomm Snapdragon 855 (SM8150, platform Kumano), 6 GB RAM,
1080x2520 OLED, GICv3, Qualcomm ABL bootloader (boot image header v2).

| File | Purpose |
|---|---|
| `mkramdisk_bahamut.sh` | Builds the ext4 ramdisk carrying the board's userspace (trimmed to fit the boot image) |
| `mkrootfs_bahamut.sh` | Builds the full rootfs image for the phone's UFS (`make bahamut-ufs`) |
| `flash_ufs_rootfs.sh` | Flashes that rootfs to `system_a` and the kernel to `boot_a` |
| `mkbootimg_bahamut.py` | Packs kernel, ramdisk and DTB into an Android boot image v2 |
| `flash_slot_a.sh` | Flashes and boots slot A over fastboot |
| `restore_slot_b.sh` | Switches back to slot B (stock Android) |
| `android_device_tree.tar.gz` | LineageOS device trees (`device/sony/bahamut`, `device/sony/sm8150-common`, `vendor/sony`) |
| `../dts/sm8150-sony-bahamut.dts` | Extracted device tree (source and `.dtb`) |

Unpack the device trees when needed:

```sh
tar -xzf tools/board/sony-xperia-5/android_device_tree.tar.gz -C .
```

The unpacked copy accumulates the vendor's proprietary blobs (~810 MiB), which
are not ours to redistribute, so it is gitignored.

## Build and boot

```sh
make bahamut          # clean kernel build (linked at 0x80080000), ramdisk, boot image
make bahamut-fast     # same, incremental
make bahamut-test     # boots the smoke suite (lane BAHAMUT_SMOKE_LANE, default sys)
make bahamut-ufs      # rootfs on UFS system_a + USB Ethernet gadget, see docs/xperia5-ufs-usb.md

# Phone in fastboot mode (Vol Up + USB cable, blue LED)
./tools/board/sony-xperia-5/flash_slot_a.sh
./tools/board/sony-xperia-5/flash_ufs_rootfs.sh # after make bahamut-ufs: system_a + boot_a
./tools/board/sony-xperia-5/restore_slot_b.sh   # roll back to Android
```

The panel is the console (continuous-splash framebuffer at `0x9C000000`), so
smoke results are read off the screen. The kernel link address, cmdline and font
scale are set by the `BAHAMUT_*` variables in the top-level `Makefile`.
