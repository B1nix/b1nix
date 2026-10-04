#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# INSTALL-SMOKE: Calamares installs b1nix from the live medium onto a blank
# disk, unattended, and the installed disk boots to a login prompt -- once with
# the network attached and once without.
#
#   sh tests/install-smoke.sh
#
# Phase D of docs/distro/roadmap.md. The in-guest driver is b1nix-installer-
# config's b1nix-install-smoke, gated on b1nix.install-smoke. The two runs
# differ in more than the network on purpose: the first installs from a BIOS
# boot (GPT with a BIOS boot partition, Limine's BIOS stages) and boots the
# disk through BIOS; the second installs from UEFI and boots through OVMF.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LANE="INSTALL-SMOKE"
OUT_DIR="$ROOT_DIR/smoke_run"
LOG="$OUT_DIR/install-smoke.log"
ISO="$ROOT_DIR/build/x86_64/b1nix-install-smoke.iso"
INSTALL_TIMEOUT="${INSTALL_TIMEOUT:-1800}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-300}"
DISK_GIB="${DISK_GIB:-16}"
SKIP_BUILD="${SKIP_BUILD:-0}"

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; NC='\033[0m'
pass=0
fail=0
ok()   { printf "${GREEN}%s: ok %s${NC}\n" "$LANE" "$1"; pass=$((pass + 1)); }
bad()  { printf "${RED}%s: FAIL %s${NC} -- %s\n" "$LANE" "$1" "$2"; fail=$((fail + 1)); }
info() { printf "${YELLOW}%s${NC}\n" "$*"; }
stage_die() { printf "${RED}%s: STAGE-FAILED %s${NC} -- see %s\n" "$LANE" "$1" "$LOG"; exit 2; }

mkdir -p "$OUT_DIR"
: >"$LOG"
clean_log() { sed 's/\x1b\[[0-9;]*[A-Za-z]//g; s/\r$//' "$1"; }
marker() { clean_log "$1" | grep -a "INSTALL-SMOKE: $2" | tail -1; }

ovmf() { # code|vars
	for d in /usr/share/edk2/x64 /usr/share/OVMF /usr/share/ovmf/x64 /usr/share/qemu; do
		for f in "OVMF_$(echo "$1" | tr a-z A-Z).4m.fd" "OVMF_$(echo "$1" | tr a-z A-Z).fd"; do
			[ -f "$d/$f" ] && { echo "$d/$f"; return 0; }
		done
	done
	return 1
}

# Runs QEMU until it powers off, the deadline passes, or (when a pattern is
# given) the serial console shows it. -no-reboot: the installed system's
# reboot request ends the run instead of looping.
run_qemu() { # tag firmware net(on|off) deadline pattern qemu-args...
	tag=$1 fw=$2 net=$3 deadline=$4 pattern=$5
	shift 5
	serial="$OUT_DIR/install-smoke-$tag.log"
	fw_args=""
	if [ "$fw" = uefi ]; then
		code=$(ovmf code) && vars=$(ovmf vars) || return 1
		[ -f "$OUT_DIR/install-smoke-$tag.vars.fd" ] || cp -f "$vars" "$OUT_DIR/install-smoke-$tag.vars.fd"
		fw_args="-drive if=pflash,format=raw,readonly=on,file=$code -drive if=pflash,format=raw,file=$OUT_DIR/install-smoke-$tag.vars.fd"
	fi
	net_args="-nic none"
	[ "$net" = off ] || net_args="-nic user,model=virtio-net-pci"
	accel=""
	[ ! -w /dev/kvm ] || accel="-accel kvm -cpu host,+invtsc"
	: >"$serial"
	# shellcheck disable=SC2086
	qemu-system-x86_64 -machine q35 $accel $fw_args -m 3072 -smp 2 $net_args \
		-display none -serial file:"$serial" -no-reboot "$@" >>"$LOG" 2>&1 &
	qpid=$!
	t=0
	while kill -0 "$qpid" 2>/dev/null && [ "$t" -lt "$deadline" ]; do
		if [ -n "$pattern" ] && clean_log "$serial" | grep -aq "$pattern"; then
			break
		fi
		sleep 5
		t=$((t + 5))
	done
	kill "$qpid" 2>/dev/null || true
	wait "$qpid" 2>/dev/null || true
	return 0
}

# ── stage 1: build ──────────────────────────────────────────────────────────
if [ "$SKIP_BUILD" = 1 ]; then
	info "SKIP_BUILD=1 -- reusing $ISO"
	[ -f "$ISO" ] || stage_die no-iso
else
	info "building the packages and the installer medium"
	sh "$ROOT_DIR/tools/deb/build-deb.sh" >>"$LOG" 2>&1 || stage_die build-packages
	sh "$ROOT_DIR/tools/deb/publish-repo.sh" >>"$LOG" 2>&1 || stage_die publish-repo
	ISO="$ISO" CMDLINE_EXTRA="console=ttyS0 b1nix.install-smoke" \
		sh "$ROOT_DIR/tools/image/mk-b1nix-iso.sh" >>"$LOG" 2>&1 || stage_die build-iso
fi

# ── the two installs ────────────────────────────────────────────────────────
for run in bios-online uefi-offline; do
	fw=${run%-*}
	net=on
	[ "${run#*-}" = offline ] && net=off
	disk="$OUT_DIR/install-smoke-$run.qcow2"
	share="$OUT_DIR/install-smoke-$run"
	rm -rf "$disk" "$share" "$OUT_DIR/install-smoke-$run"*.vars.fd
	mkdir -p "$share"
	qemu-img create -q -f qcow2 "$disk" "${DISK_GIB}G" || stage_die "disk-$run"

	info "$run: installing from the medium onto a blank ${DISK_GIB} GiB disk"
	run_qemu "$run-install" "$fw" "$net" "$INSTALL_TIMEOUT" "INSTALL-SMOKE: done" \
		-cdrom "$ISO" \
		-drive file="$disk",format=qcow2,if=virtio \
		-virtfs local,path="$share",mount_tag=installsmoke,security_model=none ||
		{ bad "$run-install" "no OVMF firmware"; continue; }
	ilog="$OUT_DIR/install-smoke-$run-install.log"
	if ! clean_log "$ilog" | grep -aq "INSTALL-SMOKE: done"; then
		bad "$run-install" "the installer never finished -- see $ilog and $share"
		continue
	fi
	case "$(marker "$ilog" "result=")" in
	*result=ok) ok "$run-install" ;;
	*) bad "$run-install" "$(marker "$ilog" "result=" | sed 's/.*result=//') -- see $share/calamares-session.log"
	   continue ;;
	esac

	# The UEFI install keeps its variable store: the firmware boots the disk
	# from the removable-media path either way, as a new machine would.
	info "$run: booting the installed disk"
	cp -f "$OUT_DIR/install-smoke-$run-install.vars.fd" "$OUT_DIR/install-smoke-$run-boot.vars.fd" 2>/dev/null || true
	run_qemu "$run-boot" "$fw" "$net" "$BOOT_TIMEOUT" "login:" \
		-drive file="$disk",format=qcow2,if=virtio || true
	blog="$OUT_DIR/install-smoke-$run-boot.log"
	if clean_log "$blog" | grep -aq "^Limine"; then
		ok "$run-bootloader"
	else
		bad "$run-bootloader" "Limine did not start from the installed disk -- see $blog"
		continue
	fi
	if clean_log "$blog" | grep -aq "b1nix-installed login:"; then
		ok "$run-login"
	else
		bad "$run-login" "no login prompt from the installed system -- see $blog"
	fi
done

printf '\n%s: %d passed, %d failed (log: %s)\n' "$LANE" "$pass" "$fail" "$LOG"
[ "$fail" -eq 0 ] || exit 1
