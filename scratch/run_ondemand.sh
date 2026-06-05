#!/bin/sh
set -e

ARCH=x86_64
PROJECT_DIR="/Users/dmytrom/Documents/GitHub/b1nix"

echo "Building iso-live-ondemand..."
make -C "$PROJECT_DIR" ARCH="$ARCH" iso-live-ondemand

mkdir -p "$PROJECT_DIR/smoke_run"
SATA_IMG="$PROJECT_DIR/smoke_run/sata-ondemand-test.img"
NVME_IMG="$PROJECT_DIR/smoke_run/nvme-ondemand-test.img"
SWAP_IMG="$PROJECT_DIR/smoke_run/swap-ondemand-test.img"

dd if=/dev/zero of="$SATA_IMG" bs=1M count=4 2>/dev/null
dd if=/dev/zero of="$NVME_IMG" bs=1M count=4 2>/dev/null
dd if=/dev/zero of="$SWAP_IMG" bs=1M count=2 2>/dev/null

MKE2FS="/opt/homebrew/opt/e2fsprogs/sbin/mke2fs"
if [ ! -x "$MKE2FS" ]; then
    MKE2FS=$(command -v mke2fs 2>/dev/null || echo "/sbin/mke2fs")
fi

"$MKE2FS" -F -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q "$SATA_IMG" 2>/dev/null || true
"$MKE2FS" -F -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q "$NVME_IMG" 2>/dev/null || true

LOG_FILE="$PROJECT_DIR/smoke_run/b1nix-smoke-ondemand.log"
rm -f "$LOG_FILE"

echo "Running QEMU..."
qemu-system-x86_64 \
    -cdrom "$PROJECT_DIR/build/$ARCH/b1nix-live-ondemand.iso" \
    -serial stdio -display none -monitor none -no-reboot \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -netdev user,id=net0,restrict=on -device virtio-net-pci,netdev=net0 \
    -netdev user,id=net1,restrict=on -device e1000,netdev=net1 \
    -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 \
    -device ich9-ahci,id=ahci \
    -drive file="$SATA_IMG",if=none,id=satadrive,format=raw \
    -device ide-hd,drive=satadrive,bus=ahci.0 \
    -drive file="$SWAP_IMG",if=none,id=swapdrive,format=raw \
    -device ide-hd,drive=swapdrive,bus=ahci.1 \
    -drive file="$NVME_IMG",if=none,id=nvmedrive,format=raw \
    -device nvme,serial=deadbeef,drive=nvmedrive \
    -drive file="$PROJECT_DIR/build/$ARCH/b1nix-live-ondemand.iso",if=none,id=usbdisk,format=raw,readonly=on \
    -device usb-storage,bus=xhci.0,drive=usbdisk > "$LOG_FILE" 2>&1 &
QEMU_PID=$!

echo "Waiting 6 seconds for boot sequence..."
sleep 6
kill -9 "$QEMU_PID" 2>/dev/null || true

echo "=== LOG OUTPUT ==="
cat "$LOG_FILE"

rm -f "$SATA_IMG" "$NVME_IMG" "$SWAP_IMG"
