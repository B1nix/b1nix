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
# Usage: sh tools/run/run-kde.sh [tag]
#   KDE_KERNEL=path  boot another kernel against the same root image
set -e
DIR=$(cd "$(dirname "$0")/../.." && pwd)
TAG=${1:-kde}
LOG=$DIR/smoke_run/$TAG.log
MON=$DIR/smoke_run/$TAG-mon.sock
OUT=$DIR/smoke_run/$TAG-frames
RUN_SECONDS=${RUN_SECONDS:-420}

rm -rf "$OUT"; mkdir -p "$OUT"
rm -f "$MON" "$LOG"

#
# The root filesystem is a disk, not a boot module.
#
# The image's ISO carries root.img as a Multiboot2 module: the bootloader
# copies all of it into memory before the kernel starts, and the KDE root is
# 2.5 GB. Off the emulated CD that took 160 of the 194 seconds between QEMU
# starting and the desktop reporting itself; off a virtio disk, 28. Here the
# kernel is booted from a module-less ISO built on the spot from the same
# kernel and command line, and root.img is attached as a virtio disk, which
# the kernel mounts by its label (b1nix-root): the bootloader has 50 MB to
# read and the guest gets its memory back. snapshot=on keeps the build's
# image unchanged. KDE_ROOT=module keeps the old shape, with the ISO on a
# virtio disk (KDE_BOOT=cdrom for the CD).
#
ISO=$DIR/build/x86_64/${KDE_ISO:-b1nix.iso}
if [ "${KDE_ROOT:-disk}" = disk ]; then
	STAGE=$DIR/build/x86_64/${KDE_ISO:-b1nix.iso}
	STAGE=${STAGE%.iso}
	[ "$STAGE" = "$DIR/build/x86_64/b1nix" ] && STAGE=$DIR/build/x86_64/iso
	CMDLINE=$(sed -n 's/^ *cmdline: //p' "$STAGE/boot/limine/limine.conf" | head -1)
	# KDE_CMDLINE replaces the whole line, so the KDE loop needs no ISO build
	# at all: `B1NIX_KDE=1 make root-image` for the root and this for the boot.
	[ -n "${KDE_CMDLINE:-}" ] && CMDLINE="$KDE_CMDLINE"
	CMDLINE="$CMDLINE${KDE_EXTRA_CMDLINE:+ $KDE_EXTRA_CMDLINE}"
	sh "$DIR/tools/images/mkiso.sh" --stage "$DIR/build/x86_64/kde-run-iso" \
		--out "$DIR/build/x86_64/b1nix-kde-run.iso" --arch x86_64 \
		--kernel "${KDE_KERNEL:-$DIR/build/x86_64/kernel.elf}" --timeout 0 \
		--cmdline "$CMDLINE" > /dev/null
	# The KDE group packs its own image (Makefile: ROOT_IMAGE); an older tree
	# packed it under the shared name.
	ROOT=$DIR/build/x86_64/root-kde.img
	[ -f "$ROOT" ] || ROOT=$DIR/build/x86_64/root.img
	BOOT_MEDIA="-cdrom $DIR/build/x86_64/b1nix-kde-run.iso \
		-drive file=$ROOT,if=virtio,format=raw,snapshot=on"
elif [ "${KDE_BOOT:-virtio}" = cdrom ]; then
	BOOT_MEDIA="-cdrom $ISO"
else
	BOOT_MEDIA="-drive file=$ISO,if=virtio,format=raw,readonly=on"
fi
ACCEL=
[ -w /dev/kvm ] && ACCEL="-accel kvm -cpu host,+invtsc"

# shellcheck disable=SC2086
qemu-system-x86_64 $ACCEL \
	-m "${KDE_MEM_MB:-4096}" -smp "${KDE_SMP:-4}" \
	$BOOT_MEDIA \
	-device virtio-gpu-pci,id=vgpu \
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
		if [ $((i % ${KDE_SHOT_EVERY:-5})) -eq 0 ]; then
			shot=$((shot + 1))
			#
			# Name the device, because there is more than one.
			#
			# QEMU adds a standard VGA adapter of its own alongside the
			# virtio-gpu asked for here, and a bare `screendump` takes the
			# first one -- so every picture was of the VGA console the
			# compositor does not draw on, while kwin was modesetting the
			# virtio-gpu. Frames came back 94% black with one other colour and
			# looked exactly like a desktop that never painted. Ask for the
			# device kwin actually programmed.
			mon "screendump $OUT/frame-$(printf %03d $shot).ppm vgpu" ||
				mon "screendump $OUT/frame-$(printf %03d $shot).ppm" || true
		fi
	fi
	grep -aq "SCANOUT-END\|KDE: done" "$LOG" 2>/dev/null && break
	i=$((i + 1))
	sleep 1
done

mon "quit" || kill $QPID 2>/dev/null || true
wait $QPID 2>/dev/null || true
# The monitor accepted "quit" and the shell's child is gone, and a QEMU was
# still running afterwards -- twice in one afternoon. Whatever the reason,
# the process that writes this run's serial log has no business outliving
# the run: name it by that log and make sure.
pkill -9 -f -- "-serial file:$LOG" 2>/dev/null || true
echo "[run-kde] log: $LOG"
echo "[run-kde] frames: $(ls -1 "$OUT" 2>/dev/null | wc -l) in $OUT"
