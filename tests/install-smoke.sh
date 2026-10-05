#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# INSTALL-SMOKE: b1nix-install installs b1nix from the live medium onto a blank
# disk, unattended, and the installed disk boots to a login prompt -- once with
# the network attached and once without. The two runs go side by side.
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
STALL_SECS="${STALL_SECS:-120}"
SKIP_BUILD="${SKIP_BUILD:-0}"
# The size budget of the install medium: a console live system and the root it
# installs, no desktop -- that comes from the network after the install.
ISO_MAX_MB="${ISO_MAX_MB:-256}"

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
	quiet=0
	last_size=0
	while kill -0 "$qpid" 2>/dev/null && [ "$t" -lt "$deadline" ]; do
		if [ -n "$pattern" ] && clean_log "$serial" | grep -aq "$pattern"; then
			break
		fi
		# A guest whose console has said nothing for STALL_SECS is wedged
		# (a firmware or loader hang shows up this way too): stop it and let
		# the verdict below say what was missing, instead of waiting out the
		# whole deadline.
		size=$(stat -c %s "$serial" 2>/dev/null || echo 0)
		if [ "$size" = "$last_size" ]; then
			quiet=$((quiet + 5))
		else
			quiet=0
			last_size=$size
		fi
		if [ "$quiet" -ge "$STALL_SECS" ]; then
			echo "$tag: console silent for ${STALL_SECS}s -- stopped" >>"$LOG"
			info "$tag: console silent for ${STALL_SECS}s -- stopped"
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

iso_mb=$(( $(stat -c %s "$ISO") / 1048576 ))
if [ "$iso_mb" -le "$ISO_MAX_MB" ]; then
	ok "iso-size ${iso_mb}MB"
else
	bad "iso-size" "the medium is ${iso_mb} MB, over the ${ISO_MAX_MB} MB budget"
fi

# ── the two installs ────────────────────────────────────────────────────────
# One run: install, then boot the installed disk. A run writes its verdicts to
# its own file -- the runs are separate processes -- and the totals are counted
# from those once both have finished.
one_run() { # bios-online | uefi-offline
	run=$1
	fw=${run%-*}
	net=on
	[ "${run#*-}" = offline ] && net=off
	disk="$OUT_DIR/install-smoke-$run.qcow2"
	share="$OUT_DIR/install-smoke-$run"
	res="$OUT_DIR/install-smoke-$run.result"
	rm -rf "$disk" "$share" "$OUT_DIR/install-smoke-$run"*.vars.fd
	mkdir -p "$share"
	: >"$res"
	r_ok() { echo "ok $1" >>"$res"; }
	r_bad() { echo "bad $1 $2" >>"$res"; }
	qemu-img create -q -f qcow2 "$disk" "${DISK_GIB}G" || { r_bad "$run-disk" "qemu-img failed"; return; }

	info "$run: installing from the medium onto a blank ${DISK_GIB} GiB disk"
	# cache=unsafe: the disk is thrown away after the run, and flushing every
	# write through to the host's own disk was most of the install's time.
	run_qemu "$run-install" "$fw" "$net" "$INSTALL_TIMEOUT" "INSTALL-SMOKE: done" \
		-cdrom "$ISO" \
		-drive file="$disk",format=qcow2,if=virtio,cache=unsafe \
		-virtfs local,path="$share",mount_tag=installsmoke,security_model=none ||
		{ r_bad "$run-install" "no OVMF firmware"; return; }
	ilog="$OUT_DIR/install-smoke-$run-install.log"
	clean_log "$ilog" | grep -a "b1nix-install: step .* done in" |
		sed "s/.*b1nix-install: step /  $run: /" >"$OUT_DIR/install-smoke-$run.steps"
	if ! clean_log "$ilog" | grep -aq "INSTALL-SMOKE: done"; then
		r_bad "$run-install" "the installer never finished -- see $ilog"
		return
	fi
	case "$(marker "$ilog" "result=")" in
	*result=ok) r_ok "$run-install" ;;
	*) r_bad "$run-install" "$(marker "$ilog" "result=" | sed 's/.*result=//') -- see $share/install.log"
	   return ;;
	esac

	# The UEFI install keeps its variable store: the firmware boots the disk
	# from the removable-media path either way, as a new machine would.
	info "$run: booting the installed disk"
	cp -f "$OUT_DIR/install-smoke-$run-install.vars.fd" "$OUT_DIR/install-smoke-$run-boot.vars.fd" 2>/dev/null || true
	run_qemu "$run-boot" "$fw" "$net" "$BOOT_TIMEOUT" "login:" \
		-drive file="$disk",format=qcow2,if=virtio,cache=unsafe || true
	blog="$OUT_DIR/install-smoke-$run-boot.log"
	if clean_log "$blog" | grep -aq "^Limine"; then
		r_ok "$run-bootloader"
	else
		r_bad "$run-bootloader" "Limine did not start from the installed disk -- see $blog"
		return
	fi
	if clean_log "$blog" | grep -aq "b1nix-installed login:"; then
		r_ok "$run-login"
	else
		r_bad "$run-login" "no login prompt from the installed system -- see $blog"
	fi
}

RUNS="${RUNS:-bios-online uefi-offline}"
t0=$(date +%s)
pids=""
for run in $RUNS; do
	one_run "$run" &
	pids="$pids $!"
done
# shellcheck disable=SC2086
wait $pids || true

for run in $RUNS; do
	res="$OUT_DIR/install-smoke-$run.result"
	[ -s "$res" ] || { bad "$run" "the run left no verdict"; continue; }
	while read -r verdict name reason; do
		if [ "$verdict" = ok ]; then ok "$name"; else bad "$name" "$reason"; fi
	done <"$res"
	[ ! -s "$OUT_DIR/install-smoke-$run.steps" ] || cat "$OUT_DIR/install-smoke-$run.steps"
done

printf '\n%s: %d passed, %d failed in %ds (log: %s)\n' "$LANE" "$pass" "$fail" \
	"$(($(date +%s) - t0))" "$LOG"
[ "$fail" -eq 0 ] || exit 1
