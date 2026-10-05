#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# LIVE-SMOKE: the b1nix live medium boots under BIOS and UEFI into a writable
# live session, and stays inside its size budget.
#
#   sh tests/live-smoke.sh
#
# Phase D of docs/distro/roadmap.md, to the contract in docs/distro/lanes.md.
# The in-guest half is b1nix-installer-config's b1nix-live-smoke, gated on
# b1nix.live-smoke, so the medium a lane boots is the medium people download.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LANE="LIVE-SMOKE"
OUT_DIR="$ROOT_DIR/smoke_run"
LOG="$OUT_DIR/live-smoke.log"
ISO="$ROOT_DIR/build/x86_64/b1nix-live.iso"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-300}"
SKIP_BUILD="${SKIP_BUILD:-0}"
# The size budget, in MiB: the plan's ceiling for a medium that carries the
# base system and the installer, with the desktop pulled over the network.
ISO_BUDGET_MIB="${ISO_BUDGET_MIB:-256}"

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
marker() { clean_log "$1" | grep -a "LIVE-SMOKE: $2" | head -1; }

ovmf() { # code|vars
	for d in /usr/share/edk2/x64 /usr/share/OVMF /usr/share/ovmf/x64 /usr/share/qemu; do
		for f in "OVMF_$(echo "$1" | tr a-z A-Z).4m.fd" "OVMF_$(echo "$1" | tr a-z A-Z).fd"; do
			[ -f "$d/$f" ] && { echo "$d/$f"; return 0; }
		done
	done
	return 1
}

# q35: its ICH9 AHCI carries the CD drive, which is where a person's machine
# has it too. The guest powers off when its checks are done.
boot_iso() { # firmware serial-log
	fwargs=""
	if [ "$1" = uefi ]; then
		code=$(ovmf code) && vars=$(ovmf vars) || return 1
		cp -f "$vars" "$2.vars.fd"
		fwargs="-drive if=pflash,format=raw,readonly=on,file=$code -drive if=pflash,format=raw,file=$2.vars.fd"
	fi
	accel=""
	[ ! -w /dev/kvm ] || accel="-accel kvm -cpu host,+invtsc"
	# shellcheck disable=SC2086
	timeout "$BOOT_TIMEOUT" qemu-system-x86_64 -machine q35 $accel $fwargs -m 2048 -smp 2 \
		-cdrom "$ISO" -display none -serial file:"$2" -no-reboot >>"$LOG" 2>&1 || true
}

# ── stage 1: build ──────────────────────────────────────────────────────────
if [ "$SKIP_BUILD" = 1 ]; then
	info "SKIP_BUILD=1 -- reusing $ISO"
	[ -f "$ISO" ] || stage_die no-iso
else
	info "building the packages and the live medium"
	sh "$ROOT_DIR/tools/deb/build-deb.sh" >>"$LOG" 2>&1 || stage_die build-packages
	sh "$ROOT_DIR/tools/deb/publish-repo.sh" >>"$LOG" 2>&1 || stage_die publish-repo
	CMDLINE_EXTRA="console=ttyS0 b1nix.live-smoke" sh "$ROOT_DIR/tools/image/mk-b1nix-iso.sh" \
		>>"$LOG" 2>&1 || stage_die build-iso
fi

size_mib=$(( $(stat -c %s "$ISO") / 1048576 ))
info "medium: $ISO, $size_mib MiB (budget $ISO_BUDGET_MIB MiB)"
if [ "$size_mib" -le "$ISO_BUDGET_MIB" ]; then
	ok "size-budget"
else
	bad "size-budget" "$size_mib MiB is over the $ISO_BUDGET_MIB MiB budget"
fi

# ── stage 2: boot it both ways ──────────────────────────────────────────────
for fw in bios uefi; do
	blog="$OUT_DIR/live-smoke-$fw.log"
	info "booting the medium under $fw"
	if ! boot_iso "$fw" "$blog"; then
		bad "$fw-boot" "no OVMF firmware on this host"
		continue
	fi
	if clean_log "$blog" | grep -aq "LIVE-SMOKE: done"; then
		ok "$fw-reaches-multi-user"
	else
		bad "$fw-reaches-multi-user" "the live checks never ran to the end -- see $blog"
		clean_log "$blog" | grep -a "b1nix-live:" | head -3 | sed 's/^/    /'
		continue
	fi
	# Which path Limine took, from its own banner: the kernel has no
	# /sys/firmware/efi to ask (docs/kernel/abi-gaps.md).
	want=BIOS
	[ "$fw" = bios ] || want=UEFI
	if clean_log "$blog" | grep -aq "Limine .*(x86-64, $want)"; then
		ok "$fw-firmware"
	else
		bad "$fw-firmware" "Limine's banner does not say $want"
	fi
	case "$(marker "$blog" "root=")" in
	*"root=btrfs"*) ok "$fw-root-is-the-seed" ;;
	*) bad "$fw-root-is-the-seed" "/ is '$(marker "$blog" "root=" | sed 's/.*root=//')', not the btrfs seed" ;;
	esac
	case "$(marker "$blog" "seed=")" in
	*"seed=2 devices") ok "$fw-sprouted-onto-zram" ;;
	*) bad "$fw-sprouted-onto-zram" "btrfs reports '$(marker "$blog" "seed=" | sed 's/.*seed=//')'" ;;
	esac
	case "$(marker "$blog" "root-writable=")" in
	*root-writable=yes) ok "$fw-root-writable" ;;
	*) bad "$fw-root-writable" "a write to /var/tmp did not read back" ;;
	esac
	case "$(marker "$blog" "medium=")" in
	*medium=iso9660) ok "$fw-medium-mounted" ;;
	*) bad "$fw-medium-mounted" "the medium is not mounted at /run/live/medium" ;;
	esac
	failed=$(clean_log "$blog" | sed -n 's/.*LIVE-SMOKE: failed-unit=//p' | sort -u | tr '\n' ' ')
	if [ -z "$failed" ]; then
		ok "$fw-no-failed-units"
	else
		bad "$fw-no-failed-units" "$failed"
	fi
done

printf '\n%s: %d passed, %d failed (log: %s)\n' "$LANE" "$pass" "$fail" "$LOG"
[ "$fail" -eq 0 ] || exit 1
