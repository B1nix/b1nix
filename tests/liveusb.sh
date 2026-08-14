#!/bin/sh
# liveusb.sh — boot with root=liveiso and a live ISO plugged in as a USB stick,
# and check that the kernel finds the boot medium.
#
# Not part of the parallel smoke suite: this boot ends on a shell rather than at
# `B1NIX-TEST: done`, so the suite would score it as a wedged instance. Run it
# by hand (or from CI) when touching the live-boot path.
#
# What it pins down is the half that used to depend on a name. The medium was
# found with strncmp(dev->name, "usb", 3), which cannot work now that USB mass
# storage is an ordinary sd* disk: it is found by mounting candidates and
# looking for the boot image, removable media first.
#
# KNOWN, PRE-EXISTING: the root switch that follows does NOT complete, and the
# cause has nothing to do with naming. loop_init() already registered loop0..7
# as empty devices, so loop_register_file(..., "loop0") registers a SECOND block
# device also called loop0. blk_get("loop0") returns the first match — the empty
# one — so mounting it reads a device with no backing file
# ("blk_read_cached: read_blocks failed for loop0 lba=2") and the kernel falls
# back to ram0. The fix belongs in loop_register_file (bind into the existing
# slot instead of registering a duplicate), which is why this script checks
# medium selection and reports the root switch separately instead of asserting
# it.
set -e

PROJECT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ARCH=${ARCH:-x86_64}
ISO="$PROJECT_DIR/build/$ARCH/b1nix-live.iso"
LOG="$PROJECT_DIR/smoke_run/b1nix-liveusb-$ARCH.log"
TIMEOUT=${LIVEUSB_TIMEOUT:-150}

mkdir -p "$PROJECT_DIR/smoke_run"

if [ ! -f "$ISO" ] || [ "${LIVEUSB_REBUILD:-0}" = "1" ]; then
	echo "[BUILD] iso-live with root=liveiso"
	make -C "$PROJECT_DIR" KERNEL_CMDLINE="root=liveiso b1nix.test=1" iso-live
fi

echo "[RUN] booting $ISO with itself attached as USB mass storage"
timeout "$TIMEOUT" qemu-system-x86_64 \
	$([ -w /dev/kvm ] && echo -enable-kvm) \
	-m "${LIVEUSB_MEM_MB:-3072}" -smp 2 \
	-cdrom "$ISO" \
	-serial stdio -display none -monitor none -no-reboot \
	-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	-device qemu-xhci,id=xhci \
	-drive file="$ISO",if=none,id=usbstick,format=raw,readonly=on \
	-device usb-storage,bus=xhci.0,drive=usbstick \
	-nic none > "$LOG" 2>&1 || true

fail=0
check() {  # check <pattern> <description>
	if grep -aq "$1" "$LOG"; then
		echo "  PASS  $2"
	else
		echo "  FAIL  $2 (missing: $1)"
		fail=1
	fi
}

echo "=== Results ==="
# The stick is the only disk here, so it takes the first name in the sd
# sequence — the same sequence AHCI draws from.
check "usb: registered sd" "USB mass storage registers as an sd* disk"
check "rootfs: liveiso mount requested" "the live-ISO path runs"
check "isofs: mounted sd" "the boot medium is found by content and mounted at /mnt/iso"
check "loop: loop0 backing /boot/rootfs.img" "the boot image on it is found and attached to loop0"

if grep -aq "M37-LIVEISO: ok isofs-loop-root" "$LOG"; then
	echo "  NOTE  the root switch completed too (the loop/isofs read bug is gone)"
else
	echo "  NOTE  root switch did NOT complete — known loop-over-isofs read failure:"
	grep -a "read_blocks failed for loop0" "$LOG" | head -2 | sed 's/^/        /'
fi

echo "log: $LOG"
exit "$fail"
