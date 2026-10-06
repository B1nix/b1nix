#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Boot the b1nix disk image and print what the guest reported.
#
#   sh tools/run/run-distro.sh [IMAGE] [-- extra qemu args...]
#
# Three things make this faster than a hand-written qemu line, and the first
# two are worth more than everything else in the loop:
#
#   KVM. The distribution boot runs a whole Debian userspace; under pure
#   emulation that is minutes of wall time per attempt. Every other run script
#   in this tree already asks for it.
#
#   snapshot=on. The image is never written, so a boot cannot leave the disk in
#   a state that changes the next one, several boots can run at once, and
#   nothing has to be rebuilt between attempts.
#
#   A stuck-console watchdog. A wedged boot used to cost the whole timeout --
#   ten minutes of waiting to learn nothing. The console is watched instead,
#   and not only for silence: the guest that wasted most of today was not quiet
#   at all, it was a process retrying access("/run/systemd/journal/flushed")
#   thousands of times a second. So what is watched is the last DISTINCT line.
#   Unchanged for SILENCE seconds means stuck, whether the guest is saying
#   nothing or saying the same thing for ever.
#
# -machine pc is the lane's reference shape. q35 boots as well since the AHCI
# probe stopped hanging on an empty ATAPI port (M133); the q35 lane covers it.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${ARCH:-x86_64}"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/$ARCH}"
IMG="${1:-$BUILD_DIR/b1nix-disk.img}"
[ $# -eq 0 ] || shift
REPO="${REPO:-$ROOT_DIR/build/packages/repo}"
LOG="${LOG:-$ROOT_DIR/smoke_run/distro-run.log}"
DEADLINE="${DEADLINE:-420}"
SILENCE="${SILENCE:-45}"
MEM="${MEM:-2048}"
CPUS="${CPUS:-4}"
# A caller that needs the disk to remember one boot in the next -- the boot
# counting lane, whose whole subject is what the last boot wrote -- passes its
# own writable copy (usually a qcow2 overlay) with SNAPSHOT=off.
IMG_FORMAT="${IMG_FORMAT:-raw}"
SNAPSHOT="${SNAPSHOT:-on}"
# DISK_CACHE=unsafe for a throwaway writable copy: a guest flush then costs no
# host fsync. Under host I/O pressure that fsync held one boot-count write for
# eighteen seconds, and the lane read the stall as a hung boot.
DISK_CACHE="${DISK_CACHE:-}"
# FIRMWARE=uefi boots through OVMF and Limine's EFI loader instead of SeaBIOS
# and Limine's BIOS stages. The image carries both, and an installed machine
# boots whichever its firmware is.
FIRMWARE="${FIRMWARE:-bios}"

log() { printf '\033[1;34m[run-distro]\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31m[run-distro] %s\033[0m\n' "$*" >&2; exit 1; }

[ -f "$IMG" ] || die "no image at $IMG"
mkdir -p "$(dirname "$LOG")"
: >"$LOG"

ACCEL=""
if [ -w /dev/kvm ]; then
	ACCEL="-accel kvm -cpu host,+invtsc"
else
	log "no writable /dev/kvm -- falling back to emulation, which is minutes slower"
fi

FW=""
if [ "$FIRMWARE" = "uefi" ]; then
	OVMF_CODE=""
	OVMF_VARS=""
	for d in /usr/share/edk2/x64 /usr/share/OVMF /usr/share/ovmf/x64 /usr/share/qemu; do
		for c in OVMF_CODE.4m.fd OVMF_CODE.fd OVMF_CODE_4M.fd; do
			[ -f "$d/$c" ] && { OVMF_CODE="$d/$c"; break; }
		done
		for v in OVMF_VARS.4m.fd OVMF_VARS.fd OVMF_VARS_4M.fd; do
			[ -f "$d/$v" ] && { OVMF_VARS="$d/$v"; break; }
		done
		[ -n "$OVMF_CODE" ] && [ -n "$OVMF_VARS" ] && break
	done
	[ -n "$OVMF_CODE" ] && [ -n "$OVMF_VARS" ] || die "FIRMWARE=uefi but this host has no OVMF firmware"
	# The variable store is written by the firmware: one private copy per boot.
	cp -f "$OVMF_VARS" "$LOG.vars.fd"
	FW="-drive if=pflash,format=raw,readonly=on,file=$OVMF_CODE -drive if=pflash,format=raw,file=$LOG.vars.fd"
elif [ "$FIRMWARE" != "bios" ]; then
	die "FIRMWARE must be bios or uefi, not '$FIRMWARE'"
fi

VIRTFS=""
[ ! -d "$REPO" ] ||
	VIRTFS="-virtfs local,path=$REPO,mount_tag=b1nixrepo,security_model=none,readonly=on"

log "booting $(basename "$IMG") under $FIRMWARE (deadline ${DEADLINE}s, silence ${SILENCE}s)"
# shellcheck disable=SC2086
qemu-system-x86_64 -machine pc $ACCEL $FW -m "$MEM" -smp "$CPUS" \
	-drive file="$IMG",format="$IMG_FORMAT",if=virtio,snapshot="$SNAPSHOT"${DISK_CACHE:+,cache=$DISK_CACHE} \
	$VIRTFS -display none -serial file:"$LOG" -no-reboot "$@" &
QEMU=$!
trap 'kill "$QEMU" 2>/dev/null || true' EXIT INT TERM

started=$(date +%s)
last_line=""
last_change=$started
while kill -0 "$QEMU" 2>/dev/null; do
	sleep 3
	now=$(date +%s)
	# Without the timestamp. Every line carries one, so comparing raw text
	# says "still changing" about a guest that has been repeating the same
	# message a thousand times a second -- which is exactly the failure this
	# watchdog exists to catch.
	line=$(tail -c 4096 "$LOG" 2>/dev/null | tail -1 | sed 's/^\[[0-9.]*\] //')
	if [ "$line" != "$last_line" ]; then
		last_line="$line"
		last_change="$now"
	fi
	# The guest says "done" when its checks have finished; there is nothing to
	# wait for after that.
	if grep -aq "DISTRO-SMOKE: done" "$LOG" 2>/dev/null; then
		log "guest finished its checks"
		break
	fi
	# Nor after init has exited: nothing on the machine can make progress
	# again, and waiting for the silence watchdog cost every failing boot of
	# the fallback stage its full SILENCE.
	if grep -aq "INIT-EXIT: init exited" "$LOG" 2>/dev/null; then
		log "init exited -- the boot is over"
		break
	fi
	if [ $((now - last_change)) -ge "$SILENCE" ]; then
		log "console stuck for ${SILENCE}s (same line repeating, or nothing at all) -- killing the guest"
		break
	fi
	if [ $((now - started)) -ge "$DEADLINE" ]; then
		log "deadline reached"
		break
	fi
done
kill "$QEMU" 2>/dev/null || true
wait "$QEMU" 2>/dev/null || true
trap - EXIT INT TERM

elapsed=$(( $(date +%s) - started ))
log "boot took ${elapsed}s; log in $LOG"

clean() { sed 's/\x1b\[[0-9;]*[A-Za-z]//g' "$LOG"; }
if clean | grep -aq "DISTRO-SMOKE:"; then
	clean | grep -a "DISTRO-SMOKE:" | sed 's/^/  /'
else
	log "the guest printed no checks; the last thing it said was:"
	clean | grep -a -v "^\[[0-9.]*\] *$" | tail -5 | sed 's/^/  /'
fi
