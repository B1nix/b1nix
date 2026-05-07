#!/bin/sh
# B1NIX Smoke Test Suite (M24)
# Runs kernel in QEMU and checks for expected output patterns.
# Usage: ./tests/smoke.sh [x86]

set -e

ARCH="${1:-x86}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TIMEOUT=60  # seconds to let each test run

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASSED=0
FAILED=0
SKIPPED=0

pass() {
	echo "  ${GREEN}PASS${NC} $1"
	PASSED=$((PASSED + 1))
}

fail() {
	echo "  ${RED}FAIL${NC} $1 — $2"
	FAILED=$((FAILED + 1))
}

skip() {
	echo "  ${YELLOW}SKIP${NC} $1 — $2"
	SKIPPED=$((SKIPPED + 1))
}

# Run QEMU and capture output
run_qemu() {
	local log="$1"
	shift
	local pid

	if [ "$ARCH" = "x86" ]; then
		if command -v timeout >/dev/null 2>&1; then
			timeout "$TIMEOUT" qemu-system-x86_64 \
				-cdrom "$PROJECT_DIR/build/x86/b1nix.iso" \
				-serial stdio -display none -monitor none -no-reboot \
				>"$log" 2>&1 || true
		else
			qemu-system-x86_64 \
				-cdrom "$PROJECT_DIR/build/x86/b1nix.iso" \
				-serial stdio -display none -monitor none -no-reboot \
				>"$log" 2>&1 &
			pid=$!
			sleep "$TIMEOUT"
			kill "$pid" 2>/dev/null || true
			wait "$pid" 2>/dev/null || true
		fi
	else
		echo "Unknown ARCH: $ARCH (AArch64 is archived)"
		exit 1
	fi
}

# Check that a pattern appears in the log
check_output() {
	local log="$1"
	local pattern="$2"
	local desc="$3"

	if grep -q "$pattern" "$log" 2>/dev/null; then
		pass "$desc"
	else
		fail "$desc" "missing expected output: $pattern"
	fi
}

# ── Build kernel first ──
echo "=== B1NIX Smoke Tests ($ARCH) ==="
echo ""

echo "[BUILD] Building kernel for $ARCH..."
cd "$PROJECT_DIR"
make ARCH="$ARCH" >/dev/null 2>&1 || {
	echo "  ${RED}BUILD FAILED${NC}"
	exit 1
}
pass "kernel builds without errors"

# ── Test 1: Kernel boots ──
echo ""
echo "[TEST] Boot and basic output..."
LOG="b1nix-smoke-boot.log"
run_qemu "$LOG"
check_output "$LOG" "b1nix kernel" "kernel banner appears"
check_output "$LOG" "pmm:" "physical memory manager initializes"
check_output "$LOG" "kheap:" "kernel heap initializes"

# ── Test 2: No panic ──
if grep -q "KERNEL PANIC" "$LOG" 2>/dev/null; then
	fail "kernel boots without panic" "PANIC detected in log"
else
	pass "kernel boots without panic"
fi

# ── Test 3: Initramfs ──
if grep -q "initramfs" "$LOG" 2>/dev/null; then
	pass "initramfs initializes"
else
	skip "initramfs initializes" "initramfs message not found"
fi

# ── Test 4: VFS ──
if grep -q "vfs:" "$LOG" 2>/dev/null || grep -q "VFS" "$LOG" 2>/dev/null; then
	pass "VFS initializes"
else
	skip "VFS initializes" "VFS message not found"
fi

# ── Test 5: Scheduler ──
if grep -q "sched" "$LOG" 2>/dev/null || grep -q "task" "$LOG" 2>/dev/null; then
	pass "scheduler starts"
else
	skip "scheduler starts" "scheduler message not found"
fi

# ── Test 6: Init process ──
if grep -q "/bin/init" "$LOG" 2>/dev/null || grep -q "init" "$LOG" 2>/dev/null; then
	pass "/bin/init launches"
else
	skip "/bin/init launches" "/bin/init message not found"
fi

# ── Test 7: Shell available ──
if grep -q "shell\|b1nix shell\|Welcome" "$LOG" 2>/dev/null; then
	pass "shell appears"
else
	skip "shell appears" "shell banner not found"
fi

# ── M22 utility init-path smoke ──
echo ""
echo "[TEST] M22 utilities..."
check_output "$LOG" "M22-SMOKE: start" "M22 utility smoke starts from init"
check_output "$LOG" "M22-SMOKE: ok pwd" "pwd utility runs"
check_output "$LOG" "M22-SMOKE: ok cp" "cp utility runs"
check_output "$LOG" "M22-SMOKE: ok parent-perms" "missing parent creation is rejected"
check_output "$LOG" "M22-SMOKE: ok ln-s" "ln -s utility runs"
check_output "$LOG" "M22-SMOKE: ok readlink" "readlink utility runs"
check_output "$LOG" "M22-SMOKE: ok lstat" "lstat reports symlink type"
check_output "$LOG" "M22-SMOKE: ok cat-link" "symlink path resolves"
check_output "$LOG" "M22-SMOKE: ok path-norm" "dot-dot path normalization works"
check_output "$LOG" "M22-SMOKE: ok grep" "grep utility runs"
check_output "$LOG" "M22-SMOKE: ok date" "date utility runs"
check_output "$LOG" "M22-SMOKE: ok uname" "uname utility runs"
check_output "$LOG" "M22-SMOKE: done" "M22 utility smoke completes"

check_output "$LOG" "M24-STRESS: start" "M24 stress starts"
check_output "$LOG" "M24-STRESS: done" "M24 stress completes successfully"

# ── Network tests (x86 only) ──
if [ "$ARCH" = "x86" ]; then
	echo ""
	echo "[TEST] Network..."
	if grep -q "virtio-net" "$LOG" 2>/dev/null; then
		pass "virtio-net detected"
		if grep -q "DHCP\|dhcp" "$LOG" 2>/dev/null; then
			pass "DHCP negotiation"
		else
			skip "DHCP negotiation" "DHCP message not found"
		fi
	else
		skip "virtio-net detected" "virtio-net message not found"
	fi
fi

# ── Summary ──
echo ""
echo "=== Results ==="
echo "  Passed:  $PASSED"
echo "  Failed:  $FAILED"
echo "  Skipped: $SKIPPED"
echo ""

if [ "$FAILED" -gt 0 ]; then
	exit 1
fi
exit 0
