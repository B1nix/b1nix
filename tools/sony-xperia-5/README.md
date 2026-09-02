# B1NIX on Sony Xperia 5 (`bahamut` / `J9210`)

This directory contains device tree definitions, boot image pack scripts, and flash utilities for running B1NIX natively on **Sony Xperia 5 (Qualcomm Snapdragon 855 / SM8150 / platform Kumano)**.

---

## 📱 Hardware Architecture

| Parameter | Specification |
|---|---|
| **SoC** | Qualcomm Snapdragon 855 (`SM8150`) |
| **CPU** | 8-core Kryo 485 (1x Cortex-A76 @ 2.84 GHz + 3x Cortex-A76 @ 2.42 GHz + 4x Cortex-A55 @ 1.78 GHz) |
| **RAM** | 6 GB LPDDR4X |
| **Display** | 1080 x 2520 OLED 21:9 CinemaWide (32bpp ARGB) |
| **Continuous Splash FB** | `0x9C000000` (Qualcomm MDP / DPU framebuffer) |
| **Interrupt Controller** | ARM GICv3 (`0x17A00000` / `0x17A60000`) |
| **Bootloader** | Qualcomm ABL (Header v2 with AVB metadata) |

---

## 📂 Project Layout

```
b1nix/
├── tools/
│   ├── dts/
│   │   ├── sm8150-sony-bahamut.dtb     # Extracted Snapdragon 855 device tree blob
│   │   └── sm8150-sony-bahamut.dts     # Decompiled human-readable Device Tree Source
│   └── sony-xperia-5/
│       ├── mkbootimg_bahamut.py        # Android Boot Image v2 Packer
│       ├── flash_slot_a.sh             # Fastboot flash & boot slot A
│       ├── restore_slot_b.sh           # Safe rollback to slot B (Android)
│       └── android_device_tree.tar.gz  # LineageOS 23/24 device trees (unpack to use)
│           ├── device/sony/bahamut/
│           ├── device/sony/sm8150-common/
│           └── vendor/sony/
```

---

The device trees are shipped packed. Unpack them where they are read from:

```sh
tar -xzf tools/sony-xperia-5/android_device_tree.tar.gz -C .
```

Only the trees themselves are in the tarball. An unpacked working copy also
accumulates the vendor's proprietary blobs (~810 MiB), which are not ours to
redistribute, so the unpacked directory is gitignored.

## 🚀 Quick Start

### 1. Build & Pack B1NIX Boot Image:
```bash
# Build AArch64 Image
ARCH=aarch64 make build/aarch64/Image

# Pack into Android Boot Image v2
python3 tools/sony-xperia-5/mkbootimg_bahamut.py
```

### 2. Test Boot on Slot A (Safe, Non-Destructive):
```bash
# Connect phone in Fastboot mode (Vol Up + USB cable -> Blue LED)
./tools/sony-xperia-5/flash_slot_a.sh
```

### 3. Rollback to Normal Android (Slot B):
```bash
./tools/sony-xperia-5/restore_slot_b.sh
```
