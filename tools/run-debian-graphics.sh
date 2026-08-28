#!/bin/sh
# run-debian-graphics.sh — boot the Debian graphics image on this kernel and
# photograph its scanout from the host.
#
# The picture is taken with the QEMU monitor's `screendump`, which reads the
# framebuffer the guest programmed into the virtual GPU. Nothing inside the
# guest is involved in producing the file, so nothing inside the guest can fake
# it, and it shows what a monitor plugged into that card would show.
#
# The guest brackets the interesting window with two markers on the serial
# line — GFX: SCANOUT-READY and GFX: SCANOUT-END — and this script dumps a
# frame every few seconds in between.
#
# Usage: sh tools/run-debian-graphics.sh [tag]
set -eu
DIR=$(cd "$(dirname "$0")/.." && pwd)
TAG=${1:-debian-graphics}
LOG=$DIR/smoke_run/$TAG.log
MON=$DIR/smoke_run/$TAG-mon.sock
OUT=$DIR/smoke_run/$TAG-frames
IMG=$DIR/build/x86_64/debian-graphics.ext4
RUN_IMG=$DIR/smoke_run/$TAG-root.img
ISO=$DIR/build/x86_64/${GFX_ISO:-b1nix-graphics.iso}
RUN_SECONDS=${RUN_SECONDS:-420}

[ -f "$IMG" ] || {
	echo "[run-debian-graphics] no $IMG — run PROFILE=graphics sh tools/images/mk-debian-image.sh" >&2
	exit 1
}
[ -f "$ISO" ] || {
	echo "[run-debian-graphics] no $ISO" >&2
	exit 1
}

rm -rf "$OUT"
mkdir -p "$OUT"
rm -f "$MON" "$LOG"
# A scratch copy, so a run that writes to its root never changes the image the
# next run starts from.
cp "$IMG" "$RUN_IMG"

ACCEL=
[ -w /dev/kvm ] && ACCEL="-accel kvm -cpu host,+invtsc"

# Four CPUs. This used to default to one, because the workload intermittently
# killed a user process with SIGILL on a valid instruction -- which turned out
# to be CR4.PGE set on the APs and not the BSP, making bit 8 of a leaf PTE the
# GLOBAL bit on some cores while this kernel used it as a software flag (M116).
# Fixed and measured: 0 ring-3 faults in 10 runs here, against 4 in 5 before.

# shellcheck disable=SC2086
qemu-system-x86_64 $ACCEL \
	-m "${GFX_MEM_MB:-3072}" -smp "${GFX_SMP:-4}" \
	-cdrom "$ISO" \
	-drive file="$RUN_IMG",if=none,id=sdroot,format=raw \
	-device virtio-blk-pci,drive=sdroot \
	-device virtio-gpu-pci,id=vgpu \
	-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 \
	-device virtio-tablet-pci,id=vtablet \
	-display none -no-reboot \
	-monitor "unix:$MON,server,nowait" \
	-serial "file:$LOG" -serial null &
QPID=$!
trap 'kill -9 $QPID 2>/dev/null || true; rm -f "$RUN_IMG"' EXIT INT TERM

mon() {
	printf '%s\n' "$1" | timeout 20 socat - "UNIX-CONNECT:$MON" > /dev/null 2>&1 || return 1
}

i=0
shot=0
ready=0
while [ "$i" -lt "$RUN_SECONDS" ]; do
	kill -0 $QPID 2> /dev/null || break
	if [ "$ready" = 0 ] && grep -aq "SCANOUT-READY" "$LOG" 2> /dev/null; then
		ready=1
		echo "[run-debian-graphics] scanout ready at t=${i}s"
	fi
	# Frames are taken from the moment the compositor has a socket, not only
	# once it reports itself ready: a run that dies early still leaves
	# evidence of what was on the screen when it did.
	if [ "$ready" = 1 ] || grep -aq "GFX-SMOKE: ok weston-socket" "$LOG" 2> /dev/null; then
		if [ $((i % 5)) -eq 0 ]; then
			shot=$((shot + 1))
			#
			# Name the device, because there is more than one.
			#
			# QEMU adds a standard VGA adapter of its own alongside the
			# virtio-gpu asked for here, and a bare `screendump` takes the
			# first one — so the picture is of the VGA console the compositor
			# does not draw on, and it comes back nearly black, which reads
			# exactly like a desktop that never painted.
			mon "screendump $OUT/frame-$(printf %03d $shot).ppm vgpu" ||
				mon "screendump $OUT/frame-$(printf %03d $shot).ppm" || true
		fi
	fi
	grep -aq "SCANOUT-END\|GFX-SMOKE: done" "$LOG" 2> /dev/null && break
	i=$((i + 1))
	sleep 1
done

mon "quit" || kill -9 $QPID 2> /dev/null || true
wait $QPID 2> /dev/null || true
QPID=
rm -f "$RUN_IMG"
echo "[run-debian-graphics] log: $LOG"
echo "[run-debian-graphics] frames: $(ls -1 "$OUT" 2> /dev/null | wc -l) in $OUT"
