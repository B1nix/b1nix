#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# UEFI boot test: the same ISO the BIOS lanes use, started by OVMF instead of
# SeaBIOS.
#
# The kernel is relocatable (see the multiboot2 header tag in
# kernel/arch/x86_64/boot.S) precisely because of this lane: under UEFI the
# address it is linked at is not necessarily free, and Limine answered
#   multiboot2: Could not find viable load address for executable
# and stopped. The check is therefore two things at once -- that the firmware
# path works, and that the kernel runs from wherever it was put.
#
#   sh tests/uefi-smoke.sh [x86_64]
#
# Skips cleanly when the host has no OVMF firmware.
set -e

ARCH="${1:-x86_64}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build/$ARCH}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$PROJECT_DIR/$BUILD_DIR" ;; esac
ISO="${ISO:-$BUILD_DIR/b1nix.iso}"
OUT="$PROJECT_DIR/smoke_run/uefi-boot.log"
TIMEOUT="${TIMEOUT:-180}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass() { printf "  ${GREEN}PASS${NC} %s\n" "$1"; }
fail() { printf "  ${RED}FAIL${NC} %s - %s\n" "$1" "$2"; FAILED=$((FAILED + 1)); }
FAILED=0

CODE=""
VARS=""
for d in /usr/share/edk2/x64 /usr/share/OVMF /usr/share/ovmf/x64 /usr/share/qemu; do
	for c in OVMF_CODE.4m.fd OVMF_CODE.fd OVMF_CODE_4M.fd; do
		[ -f "$d/$c" ] && { CODE="$d/$c"; break; }
	done
	for v in OVMF_VARS.4m.fd OVMF_VARS.fd OVMF_VARS_4M.fd; do
		[ -f "$d/$v" ] && { VARS="$d/$v"; break; }
	done
	[ -n "$CODE" ] && [ -n "$VARS" ] && break
done
if [ -z "$CODE" ] || [ -z "$VARS" ]; then
	printf "  ${YELLOW}SKIP${NC} uefi-boot - this host has no OVMF firmware\n"
	exit 0
fi
[ -f "$ISO" ] || { printf "  ${YELLOW}SKIP${NC} uefi-boot - no ISO at %s\n" "$ISO"; exit 0; }

mkdir -p "$PROJECT_DIR/smoke_run"
# The variable store is written by the firmware, so it has to be our own copy.
cp -f "$VARS" "$PROJECT_DIR/smoke_run/uefi-vars.fd"

printf "${YELLOW}booting %s under OVMF${NC}\n" "$(basename "$ISO")"
timeout "$TIMEOUT" qemu-system-x86_64 \
	${ACCEL:--accel kvm} -m "${MEM_MB:-2048}" -smp "${SMP:-2}" \
	-display none -no-reboot -serial stdio -monitor none \
	-drive if=pflash,format=raw,readonly=on,file="$CODE" \
	-drive if=pflash,format=raw,file="$PROJECT_DIR/smoke_run/uefi-vars.fd" \
	-cdrom "$ISO" \
	-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	${EXTRA_QEMU_ARGS:-} >"$OUT" 2>&1 || true

# What each check reads, in the order the boot produces it.
if grep -qa "Could not find viable load address" "$OUT"; then
	fail "loader-accepts-kernel" "Limine could not place the kernel"
else
	pass "loader-accepts-kernel"
fi
if grep -qa "b1nix" "$OUT" && grep -qa "cmdline:" "$OUT"; then
	pass "kernel-runs-under-uefi"
else
	fail "kernel-runs-under-uefi" "no kernel output on the serial console"
fi
if grep -qa "KERNEL PANIC\|\[PANIC\]" "$OUT"; then
	fail "no-panic" "the boot panicked"
else
	pass "no-panic"
fi
# The load offset the kernel reports: zero when it was loaded where it was
# linked, and the whole point of the lane when it is not.
sed -n 's/.*\(kernel: loaded at [^ ]*\).*/  \1/p' "$OUT" | tail -1

if [ "$FAILED" -gt 0 ]; then
	printf "${RED}%d check(s) failed${NC} (log: %s)\n" "$FAILED" "$OUT"
	exit 1
fi
printf "${GREEN}UEFI boot ok${NC} (log: %s)\n" "$OUT"
