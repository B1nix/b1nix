#!/bin/bash
# Manual QEMU Boot Script for exFAT Live USB Root Probing (M37)
# Creates a partitioned exFAT image containing rootfs.img, builds b1nix, and runs QEMU.

set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_DIR"

EXFAT_DMG="smoke_run/exfat-manual-test.dmg"
EXFAT_RAW="smoke_run/exfat-manual-test.raw"

echo "=== exFAT Live Boot QEMU manual test script ==="

# 1. Rebuild the root ext4 image and kernel ELF
echo "[1/4] Rebuilding userspace and root ext4 image..."
make ARCH=x86_64 root-image

# 2. Rebuild the ISO image with the appropriate cmdline
echo "[2/4] Rebuilding ISO with exfat parameters..."
make ARCH=x86_64 KERNEL_CMDLINE="b1nix.test=1 root=liveiso b1nix.xhci.run" iso

# 3. Create the exFAT image
echo "[3/4] Creating partitioned exFAT USB drive image..."
mkdir -p smoke_run
rm -f "$EXFAT_DMG" "$EXFAT_RAW"

if ! command -v hdiutil >/dev/null 2>&1; then
    echo "Error: hdiutil not found on the host (macOS is required for image creation)."
    exit 1
fi

hdiutil create -size 530m -fs exfat -volname B1NIX "$EXFAT_DMG" >/dev/null
mount_info=$(hdiutil attach "$EXFAT_DMG")
volume_path=$(echo "$mount_info" | grep "/Volumes/" | awk -F'/Volumes/' '{print "/Volumes/"$2}')

echo "  Writing /boot/rootfs.img into the exFAT volume..."
mkdir -p "$volume_path/boot"
cp build/x86_64/root.ext4 "$volume_path/boot/rootfs.img"

echo "  Detaching and converting image to raw format..."
hdiutil detach "$volume_path" >/dev/null
hdiutil convert "$EXFAT_DMG" -format UDTO -o "$EXFAT_RAW" >/dev/null
mv "${EXFAT_RAW}.cdr" "$EXFAT_RAW"
rm -f "$EXFAT_DMG"

# 4. Launch QEMU
echo "[4/4] Launching QEMU with exFAT USB storage..."
echo "      (Press Ctrl+A then X to exit QEMU)"
echo ""

qemu-system-x86_64 \
    -cdrom build/x86_64/b1nix.iso \
    -serial stdio -display none -monitor none -no-reboot \
    -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 \
    -drive file="$EXFAT_RAW",if=none,id=usbexfat,format=raw \
    -device usb-storage,bus=xhci.0,drive=usbexfat
