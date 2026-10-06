#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# DISTRO-SMOKE: an installed b1nix boots Debian's systemd, apt works, and a
# kernel that cannot bring userspace up is fallen back from.
#
#   sh tests/distro-smoke.sh
#
# Phase B of docs/distro/roadmap.md, to the contract in docs/distro/lanes.md.
# The in-guest half is /usr/local/sbin/b1nix-smoke, installed in every image
# and gated on b1nix.smoke, so what runs here is the image a person installs.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ARCH="${ARCH:-x86_64}"
LANE="DISTRO-SMOKE"
OUT_DIR="$ROOT_DIR/smoke_run"
LOG="$OUT_DIR/distro-smoke.log"
BOOT_LOG="$OUT_DIR/distro-smoke-boot.log"
REPO="$ROOT_DIR/build/packages/repo"
IMG="$ROOT_DIR/build/$ARCH/b1nix-disk.img"
BROKEN_IMG="$ROOT_DIR/build/$ARCH/b1nix-disk-broken.img"
KNOWN_DEGRADED="$ROOT_DIR/tests/support/known-degraded.txt"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-240}"
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

# One boot path for everything: tools/run/run-distro.sh asks for KVM, boots
# from a snapshot so the image is never written, and kills a guest whose
# console has stopped saying anything new. Before it existed, each caller wrote
# its own qemu line, and a wedged boot cost the full timeout -- seven minutes
# of waiting to learn nothing.
qemu_boot() { # image serial-log
	LOG="$2" REPO="$REPO" DEADLINE="$BOOT_TIMEOUT" SILENCE="${SILENCE:-45}" \
		IMG_FORMAT="${IMG_FORMAT:-raw}" SNAPSHOT="${SNAPSHOT:-on}" \
		FIRMWARE="${FIRMWARE:-bios}" \
		sh "$ROOT_DIR/tools/run/run-distro.sh" "$1" >>"$LOG.run" 2>&1 || true
}

# The guest's console ends lines with CR as well: left in, "apt-update=ok\r"
# matched no `*=ok)` pattern and a passing check was reported as failed.
clean_log() { sed 's/\x1b\[[0-9;]*[A-Za-z]//g; s/\r$//' "$1"; }
marker() { clean_log "$1" | grep -a "DISTRO-SMOKE: $2" | head -1; }

# ── stage 1: build ──────────────────────────────────────────────────────────
if [ "$SKIP_BUILD" = "1" ]; then
	info "SKIP_BUILD=1 -- reusing $IMG (never do this for a release run)"
	[ -f "$IMG" ] || stage_die no-image
else
	info "building the overlay packages and the disk image"
	sh "$ROOT_DIR/tools/deb/build-deb.sh" >>"$LOG" 2>&1 || stage_die build-packages
	sh "$ROOT_DIR/tools/deb/publish-repo.sh" >>"$LOG" 2>&1 || stage_die publish-repo
	CMDLINE_EXTRA="console=ttyS0 b1nix.smoke ${DISTRO_EXTRA_CMDLINE:-}" sh "$ROOT_DIR/tools/image/mk-b1nix-image.sh" \
		>>"$LOG" 2>&1 || stage_die build-image
fi
# The image's age is printed so that a stale one is visible in the log rather
# than mistaken for a fresh result.
info "image: $IMG ($(date -r "$IMG" '+%Y-%m-%d %H:%M:%S'))"

# ── stage 2: boot ───────────────────────────────────────────────────────────
info "booting the installed system"
qemu_boot "$IMG" "$BOOT_LOG"

if clean_log "$BOOT_LOG" | grep -aq "b1nix kernel starting"; then
	ok "kernel-boots"
else
	bad "kernel-boots" "the kernel printed nothing -- see $BOOT_LOG"
	printf '\n%s: %d passed, %d failed\n' "$LANE" "$pass" "$fail"
	exit 1
fi

if clean_log "$BOOT_LOG" | grep -aq "DISTRO-SMOKE: done"; then
	ok "userspace-reaches-multi-user"
else
	bad "userspace-reaches-multi-user" "the in-guest checks never ran to the end"
fi

# The console carries carriage returns; a state of "starting\r" matches no
# pattern below and lands in the catch-all, which is how a boot that had only
# the checks' own job left was reported as if systemd had said something
# unexpected.
_state=$(marker "$BOOT_LOG" "state=" | sed 's/.*state=//' | tr -d '\r' |
	awk '{print $1}')
case "$_state" in
running) ok "system-running" ;;
degraded) ok "system-running" ;;  # graded below, unit by unit
# "starting" with nothing left but the checks' own unit: systemd counts the job
# it is running us from, so this is as far as a boot can get while a unit is
# asking. The in-guest side says so explicitly before it goes on.
starting)
	if clean_log "$BOOT_LOG" | grep -qa "with only this unit left"; then
		ok "system-running"
	else
		bad "system-running" "still starting, with jobs other than the checks' own"
	fi
	;;
*) bad "system-running" "systemctl is-system-running said '${_state:-nothing}'" ;;
esac

# Every failed unit must be on the list, and every unit on the list must still
# be failing.
_failed_file="$OUT_DIR/failed-units.txt"
_known_file="$OUT_DIR/known-degraded-units.txt"
clean_log "$BOOT_LOG" | sed -n 's/.*DISTRO-SMOKE: failed-unit=//p' | sort -u >"$_failed_file"
grep -v '^#' "$KNOWN_DEGRADED" | awk 'NF {print $1}' | sort -u >"$_known_file"

# Files, not a here-document into grep: the first version of this fed the known
# list on stdin to a grep that was already reading stdin, so both comparisons
# silently matched nothing and printed "ok" over a machine with a dozen failed
# units. A check that cannot fail is worse than no check.
_unexpected=$(grep -Fxv -f "$_known_file" "$_failed_file" || true)
if [ -z "$_unexpected" ]; then
	ok "no-unexpected-failed-units"
else
	bad "no-unexpected-failed-units" "$(printf '%s' "$_unexpected" | tr '\n' ' ')"
fi

_stale=$(grep -Fxv -f "$_failed_file" "$_known_file" || true)
if [ -z "$_stale" ]; then
	ok "known-degraded-list-is-current"
else
	bad "known-degraded-list-is-current" "these no longer fail and must leave the list: $(printf '%s' "$_stale" | tr '\n' ' ')"
fi

case "$(marker "$BOOT_LOG" "apt-update=")" in
*apt-update=ok) ok "apt-update" ;;
*) bad "apt-update" "apt could not read the overlay repository from inside the guest" ;;
esac
case "$(marker "$BOOT_LOG" "apt-install=")" in
*apt-install=ok) ok "apt-install" ;;
*) bad "apt-install" "apt could not install a package from the overlay inside the guest" ;;
esac

# The boot must have been marked good, or every later boot spends another try
# and the machine eventually falls back for no reason.
if clean_log "$BOOT_LOG" | grep -aq "DISTRO-SMOKE: boot-state=.*good=1"; then
	ok "boot-marked-good"
else
	bad "boot-marked-good" "no entry in the boot state is marked good after a successful boot"
fi

# The mounts systemd makes from units: the ESP from fstab, Debian's tmpfs units.
for _m in boot:vfat tmp:tmpfs runlock:tmpfs; do
	_what=${_m%%:*}
	_fs=${_m#*:}
	case "$(marker "$BOOT_LOG" "$_what-mounted=")" in
	*"$_what-mounted=$_fs") ok "$_what-mounted" ;;
	*) bad "$_what-mounted" "expected a $_fs mount, the guest said '$(marker "$BOOT_LOG" "$_what-mounted=" | sed 's/.*=//')'" ;;
	esac
done

# The package's modules reach the machine: IPv6 and NTFS are loadable
# modules, loaded from /lib/modules/<release> after the root mounts. (The CD
# filesystem used to be one too; the imported isofs is built in.)
_mods=$(marker "$BOOT_LOG" "modules=")
case "$_mods" in
*ipv6*ntfs* | *ntfs*ipv6*) ok "kernel-modules-loaded" ;;
*) bad "kernel-modules-loaded" "ipv6 and ntfs are not in /proc/modules: '${_mods#*modules=}'" ;;
esac

case "$(marker "$BOOT_LOG" "shadow-owner=")" in
*shadow-owner=root:shadow) ok "file-ownership" ;;
*) bad "file-ownership" "/etc/shadow is '$(marker "$BOOT_LOG" "shadow-owner=" | sed 's/.*=//')', not root:shadow" ;;
esac

case "$(marker "$BOOT_LOG" "sandbox=")" in
*sandbox=ok) ok "unit-sandbox" ;;
*) bad "unit-sandbox" "ProtectSystem=strict did not keep a unit off /usr: $(marker "$BOOT_LOG" "sandbox=" | sed 's/.*sandbox=//')" ;;
esac
case "$(marker "$BOOT_LOG" "strace-pid1=")" in
*strace-pid1=ok) ok "strace-pid1" ;;
*) bad "strace-pid1" "tracing PID 1 left it stopped, or strace never ran" ;;
esac

# ── stage 2b: the same image under UEFI ─────────────────────────────────────
# OVMF and Limine's EFI loader: the firmware path an installed machine with no
# CSM takes. The kernel is relocatable for exactly this, so the load offset it
# reports is printed beside the result.
UEFI_LOG="$OUT_DIR/distro-smoke-uefi.log"
info "booting the installed system under UEFI"
FIRMWARE=uefi qemu_boot "$IMG" "$UEFI_LOG"
if clean_log "$UEFI_LOG" | grep -aq "^Limine .*UEFI)"; then
	ok "uefi-loader"
else
	bad "uefi-loader" "no Limine EFI banner -- see $UEFI_LOG"
fi
_off=$(clean_log "$UEFI_LOG" | sed -n 's/.*kernel: loaded at \(+0x[0-9a-f]*\).*/\1/p' | head -1)
[ -z "$_off" ] || info "kernel load offset under UEFI: $_off"
if clean_log "$UEFI_LOG" | grep -aq "DISTRO-SMOKE: done"; then
	ok "uefi-reaches-multi-user"
else
	bad "uefi-reaches-multi-user" "the in-guest checks never ran to the end under UEFI"
fi
if clean_log "$UEFI_LOG" | grep -aq "DISTRO-SMOKE: boot-state=.*good=1"; then
	ok "uefi-boot-marked-good"
else
	bad "uefi-boot-marked-good" "the UEFI boot did not mark its entry good"
fi

# ── stage 3: the fallback ───────────────────────────────────────────────────
# A kernel that boots and cannot bring userspace up, three times, must hand the
# default back to the kernel that works.
if [ "$SKIP_BUILD" != "1" ]; then
	info "building the image with a deliberately failing kernel"
	PROFILE=broken CMDLINE_EXTRA="console=ttyS0 b1nix.smoke" \
		sh "$ROOT_DIR/tools/image/mk-b1nix-image.sh" >>"$LOG" 2>&1 || stage_die build-broken-image
fi
[ -f "$BROKEN_IMG" ] || stage_die no-broken-image

# The four boots share one disk: each has to see the try the one before it
# spent. A qcow2 overlay over the broken image is that disk, and throwaway --
# the image itself is never written.
_fb_disk="$OUT_DIR/distro-smoke-fallback.qcow2"
rm -f "$_fb_disk"
qemu-img create -q -f qcow2 -b "$BROKEN_IMG" -F raw "$_fb_disk" || stage_die fallback-overlay
_fell_back=0
for _try in 1 2 3 4; do
	_bl="$OUT_DIR/distro-smoke-fallback-$_try.log"
	IMG_FORMAT=qcow2 SNAPSHOT=off DISK_CACHE=unsafe qemu_boot "$_fb_disk" "$_bl"
	if clean_log "$_bl" | grep -aq "DISTRO-SMOKE: done"; then
		# The in-guest checks ran, so this boot reached userspace: the only
		# kernel that can do that here is the good one.
		_fell_back="$_try"
		break
	fi
done

if [ "$_fell_back" -gt 1 ]; then
	ok "fallback-after-failed-boots"
	info "the good kernel took over on boot $_fell_back"
elif [ "$_fell_back" = 1 ]; then
	bad "fallback-after-failed-boots" "the first boot already reached userspace: the broken kernel was not the default"
else
	bad "fallback-after-failed-boots" "four boots and the machine never came back on the good kernel"
fi

printf '\n%s: %d passed, %d failed (log: %s)\n' "$LANE" "$pass" "$fail" "$LOG"
[ "$fail" -eq 0 ] || exit 1
