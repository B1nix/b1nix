#!/bin/sh
# tests/smoke-b1cc.sh — fast, focused smoke for b1cc / native-compiler iteration.
#
# Why this exists: the full suite (tests/smoke.sh) builds the ~500 MB full
# initramfs and runs 3-4 QEMU instances across every module — far too heavy to
# run on every compiler tweak. This builds ONLY the minimal initramfs (the b1cc
# + native test binaries, ~20 s, 3 MB kernel), boots a SINGLE QEMU, and checks
# ONLY the b1cc/native markers. A clean PASS/FAIL in well under a minute.
#
# The kernel already defaults to MINIMAL_INITRAMFS (kernel/fs/initramfs.c:1); we
# also pass MINIMAL_INITRAMFS=1 to make so the Makefile builds only the minimal
# .inc set instead of the full 500 MB of port binaries.
#
# Usage: sh tests/smoke-b1cc.sh [x86_64]
set -eu

ARCH="${1:-x86_64}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_DIR"

BUILD_LOG="smoke_run/b1cc-iso-build.log"
RUN_LOG="smoke_run/b1cc-run.log"
TIMEOUT="${SMOKE_B1CC_TIMEOUT:-90}"
mkdir -p smoke_run

# Markers the b1cc/native test sequence must emit (kernel/user/programs.c).
EXPECTED="NATIVE-SMOKE: ok
B1CC-R42-SMOKE: ok
B1CC-HELLO-SMOKE: ok
B1CC-ARGV-SMOKE: ok
B1CC-FILE-SMOKE: ok
B1CC-STDERR-SMOKE: ok
B1CC-BETTER-C-SMOKE: ok
B1CC-SELF-COMPILE-SMOKE: ok
B1CC-PIE-SMOKE: ok
B1CC-SO-SMOKE: ok"

echo "[b1cc] === b1cc / native-compiler smoke ($ARCH) ==="

echo "[b1cc] [BUILD] minimal ISO (MINIMAL_INITRAMFS=1)..."
if ! make ARCH="$ARCH" MINIMAL_INITRAMFS=1 KERNEL_CMDLINE="b1nix.test=1" iso >"$BUILD_LOG" 2>&1; then
	echo "[b1cc]   BUILD FAILED — tail of $BUILD_LOG:"
	tail -20 "$BUILD_LOG"
	exit 1
fi

# KVM acceleration when available (Linux /dev/kvm), else fall back to TCG.
accel_args=""
if [ -w /dev/kvm ] && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw kvm; then
	accel_args="-accel kvm -cpu host"
fi

echo "[b1cc] [RUN] single QEMU (timeout ${TIMEOUT}s)..."
# shellcheck disable=SC2086
timeout "$TIMEOUT" qemu-system-x86_64 $accel_args -m "${SMOKE_MEM_MB:-1024}" \
	-cdrom "build/$ARCH/b1nix.iso" \
	-serial stdio -display none -monitor none -no-reboot \
	-device isa-debug-exit,iobase=0xf4,iosize=0x04 -nic none -vga none \
	>"$RUN_LOG" 2>&1 || true

if grep -aqE "KERNEL PANIC|\[PANIC\]" "$RUN_LOG"; then
	echo "[b1cc]   KERNEL PANIC — tail of $RUN_LOG:"
	tail -20 "$RUN_LOG"
	exit 1
fi

pass=0
fail=0
echo "$EXPECTED" | while IFS= read -r marker; do
	[ -n "$marker" ] || continue
	if grep -aqF "$marker" "$RUN_LOG"; then
		echo "[b1cc]   PASS  $marker"
	else
		echo "[b1cc]   FAIL  $marker  (missing)"
	fi
done

# Recompute counts outside the subshell (the while above runs in a pipe subshell).
pass=$(echo "$EXPECTED" | while IFS= read -r m; do [ -n "$m" ] && grep -aqF "$m" "$RUN_LOG" && echo x; done | wc -l | tr -d ' ')
total=$(echo "$EXPECTED" | grep -c .)
fail=$((total - pass))

echo "[b1cc] === $pass/$total passed ==="
if [ "$fail" -ne 0 ]; then
	echo "[b1cc] FAILED ($fail missing) — see $RUN_LOG"
	exit 1
fi
echo "[b1cc] OK"
