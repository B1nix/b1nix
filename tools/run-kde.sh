#!/bin/sh
# run-kde.sh — boot the KDE image and photograph its scanout from the host.
#
# The picture is taken with the QEMU monitor's `screendump`, which reads the
# framebuffer the guest programmed into the virtual GPU. Nothing inside the
# guest is involved in producing it, so nothing inside the guest can fake it,
# and it shows exactly what a monitor plugged into that card would show.
#
# The guest brackets the interesting window with two markers on the serial
# line -- KDE: SCANOUT-READY and KDE: SCANOUT-END -- and this script dumps a
# frame every few seconds in between.
#
# Usage: sh tools/run-kde.sh [tag]
set -e
DIR=$(cd "$(dirname "$0")/.." && pwd)
TAG=${1:-kde}
LOG=$DIR/smoke_run/$TAG.log
MON=$DIR/smoke_run/$TAG-mon.sock
OUT=$DIR/smoke_run/$TAG-frames
RUN_SECONDS=${RUN_SECONDS:-420}

rm -rf "$OUT"; mkdir -p "$OUT"
rm -f "$MON" "$LOG"

ACCEL=
[ -w /dev/kvm ] && ACCEL="-accel kvm -cpu host,+invtsc"

# shellcheck disable=SC2086
qemu-system-x86_64 $ACCEL \
	-m "${KDE_MEM_MB:-4096}" -smp "${KDE_SMP:-4}" \
	-cdrom "$DIR/build/x86_64/${KDE_ISO:-b1nix.iso}" \
	-device virtio-gpu-pci \
	-netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
	-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 \
	-device virtio-tablet-pci,id=vtablet \
	-display none -no-reboot \
	-monitor "unix:$MON,server,nowait" \
	-serial "file:$LOG" -serial null &
QPID=$!
trap 'kill $QPID 2>/dev/null || true' EXIT INT TERM

mon() {
	printf '%s\n' "$1" | timeout 20 socat - "UNIX-CONNECT:$MON" >/dev/null 2>&1 || return 1
}

i=0
shot=0
ready=0
while [ "$i" -lt "$RUN_SECONDS" ]; do
	kill -0 $QPID 2>/dev/null || break
	if [ "$ready" = 0 ] && grep -aq "SCANOUT-READY" "$LOG" 2>/dev/null; then
		ready=1
		echo "[run-kde] scanout ready at t=${i}s"
	fi
	# Frames are taken from the moment the compositor is up, not only after
	# the desktop reports itself: a run that dies early still leaves evidence.
	if [ "$ready" = 1 ] || grep -aq "ok drm-card\|ok nested-socket" "$LOG" 2>/dev/null; then
		if [ $((i % 5)) -eq 0 ]; then
			shot=$((shot + 1))
			mon "screendump $OUT/frame-$(printf %03d $shot).ppm" || true
		fi
	fi
	grep -aq "SCANOUT-END\|KDE: done" "$LOG" 2>/dev/null && break
	i=$((i + 1))
	sleep 1
done

mon "quit" || kill $QPID 2>/dev/null || true
wait $QPID 2>/dev/null || true
echo "[run-kde] log: $LOG"
echo "[run-kde] frames: $(ls -1 "$OUT" 2>/dev/null | wc -l) in $OUT"
