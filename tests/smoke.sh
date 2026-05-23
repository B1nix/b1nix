#!/bin/sh
# B1NIX Smoke Test Suite (M24)
# Runs kernel in QEMU and checks for expected output patterns.
# Usage: ./tests/smoke.sh [x86]

set -e

ARCH="${1:-x86}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TIMEOUT=300  # seconds to let each test run

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

# Run QEMU and capture output
run_qemu() {
	local log="$1"
	shift
	local pid

	if [ "$ARCH" = "x86" ]; then
		qemu-system-x86_64 \
			-cdrom "$PROJECT_DIR/build/x86/b1nix.iso" \
			-serial stdio -display none -monitor none -no-reboot \
			-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
			-netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
			-object filter-dump,id=f0,netdev=net0,file="$PROJECT_DIR/logs/net.pcap" \
			-device ich9-ahci,id=ahci \
			-drive file="$PROJECT_DIR/sata.img",if=none,id=satadrive,format=raw \
			-device ide-hd,drive=satadrive,bus=ahci.0 \
			-drive file="$PROJECT_DIR/nvme.img",if=none,id=nvmedrive,format=raw \
			-device nvme,serial=deadbeef,drive=nvmedrive \
			>"$log" 2>&1 &
		pid=$!
		
		# Instant monitoring using tail -f
		# We wait for the final success marker or a panic
		(
			timeout "$TIMEOUT" bash -c "tail -n +1 -f \"$log\" 2>/dev/null | grep -m 1 -E 'B1NIX-TEST: done|KERNEL PANIC'" >/dev/null 2>&1
			sleep 2
			kill "$pid" 2>/dev/null || true
		) &
		local watcher_pid=$!

		wait "$pid" 2>/dev/null || true
		kill "$watcher_pid" 2>/dev/null || true
		kill -9 "$pid" 2>/dev/null || true
	else
		echo "Unknown ARCH: $ARCH"
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
make ARCH="$ARCH" KERNEL_CMDLINE="b1nix.test=1" iso >/dev/null 2>&1 || {
	echo "  ${RED}BUILD FAILED${NC}"
	exit 1
}
pass "kernel builds without errors"

# Create dummy images for SATA and NVMe tests
dd if=/dev/zero of="$PROJECT_DIR/sata.img" bs=1M count=4 2>/dev/null
dd if=/dev/zero of="$PROJECT_DIR/nvme.img" bs=1M count=4 2>/dev/null

# ── Test 1: Kernel boots ──
echo ""
echo "[TEST] Boot and basic output..."
mkdir -p "$PROJECT_DIR/logs"
LOG="$PROJECT_DIR/logs/b1nix-smoke-boot.log"
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

# ── Test 3-7: Core boot path markers ──
check_output "$LOG" "initramfs: files" "initramfs initializes"
check_output "$LOG" "M22-SMOKE: start" "VFS initializes"
check_output "$LOG" "M24-STRESS: start" "scheduler starts"
check_output "$LOG" "init spawn result:" "/bin/init launches"
check_output "$LOG" "M11-SMOKE: start" "shell appears"

# ── M12 Syscalls & Process Management ──
echo ""
echo "[TEST] M12 Syscalls & Process Management..."
check_output "$LOG" "M12-SMOKE: start" "M12 smoke starts"
check_output "$LOG" "M12-SMOKE: ok spawn" "spawn basic command works"
check_output "$LOG" "M12-SMOKE: ok execve" "execve works"
check_output "$LOG" "M12-SMOKE: ok status-prop" "child exit status propagation works"
check_output "$LOG" "M12-SMOKE: ok waitpid" "waitpid success path works"
check_output "$LOG" "M12-SMOKE: ok stress" "repeated spawn/wait stress works"
check_output "$LOG" "M12-SMOKE: ok zombie" "zombie reaping behavior works"
check_output "$LOG" "M12-SMOKE: ok fd-inheritance" "inherited stdout/stderr across exec works"
check_output "$LOG" "M12-SMOKE: ok dup2" "dup2 behavior works"
check_output "$LOG" "M12-SMOKE: ok close-on-exec" "close-on-exec works"
check_output "$LOG" "M12-SMOKE: ok brk" "brk basic growth/shrink sanity works"
check_output "$LOG" "M12-SMOKE: ok mmap" "mmap/munmap mapping lifecycle works"
check_output "$LOG" "M12-SMOKE: ok invalid-args" "invalid pointer/argument handling works"
check_output "$LOG" "M12-SMOKE: ok kill" "basic kill signal works"
check_output "$LOG" "M12-SMOKE: ok sigaction" "sigaction signal behavior works"
check_output "$LOG" "M12-SMOKE: ok setsid-pgrp" "process group / session sanity works"
check_output "$LOG" "M12-SMOKE: ok uid-gid" "uid/gid getter/setter sanity works"
check_output "$LOG" "M12-SMOKE: done" "M12 smoke completes successfully"

# ── M22 utility init-path smoke ──
echo ""
echo "[TEST] M22 utilities..."
check_output "$LOG" "NATIVE-SMOKE: ok" "native ELF enters ring3 and performs syscall"
check_output "$LOG" "NATIVE-SMOKE: done" "native ELF exits cleanly"
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

check_output "$LOG" "M24-STRESS: done" "M24 stress completes successfully"
check_output "$LOG" "LOCK-SMOKE: done" "LOCK-SMOKE completes successfully"
check_output "$LOG" "EXT-STRESS: done" "EXT-STRESS completes successfully"
check_output "$LOG" "ok eloop" "circular symlink returns ELOOP"
check_output "$LOG" "POSIX-SMOKE: done" "POSIX shell-driven smoke tests complete"

# ── M11 Shell & Utilities ──
echo ""
echo "[TEST] M11 Shell baseline..."
check_output "$LOG" "M11-SMOKE: start" "M11 shell smoke starts"
check_output "$LOG" "M11-SMOKE: ok pipe-eof" "pipe EOF when all writers close"
check_output "$LOG" "M11-SMOKE: ok pipe-nonblock-read" "pipe nonblocking read returns EAGAIN"
check_output "$LOG" "M11-SMOKE: ok pipe-nonblock-write" "pipe nonblocking write returns EAGAIN"
check_output "$LOG" "M11-SMOKE: done" "M11 shell smoke completes"
check_output "$LOG" "M11-SHELL: ok simple-success" "simple command success"
check_output "$LOG" "M11-SHELL: ok simple-fail" "simple command failure propagates status"
check_output "$LOG" "M11-SHELL: ok exec-127" "failed exec returns 127"
check_output "$LOG" "M11-SHELL: ok var-expand" "variable expansion works"
check_output "$LOG" "M11-SHELL: ok path-lookup" "PATH lookup resolves command"
check_output "$LOG" "M11-SHELL: ok quoted-string" "double-quoted string with spaces"
check_output "$LOG" "M11-SHELL: ok single-quote" "single-quoted string preserves special chars"
check_output "$LOG" "M11-SHELL: ok and-op" "&& operator runs second on success"
check_output "$LOG" "M11-SHELL: ok or-op" "|| operator runs second on failure"
check_output "$LOG" "M11-SHELL: ok semicolon" "semicolon separates commands"
check_output "$LOG" "M11-SHELL: ok redir-out" "stdout redirection > creates file"
check_output "$LOG" "M11-SHELL: ok redir-in" "stdin redirection < reads file"
check_output "$LOG" "M11-SHELL: ok redir-append" "append redirection >> works"
check_output "$LOG" "M11-SHELL: ok redir-stderr" "stderr redirection 2> captures errors"
check_output "$LOG" "M11-SHELL: ok redir-2>&1" "2>&1 merges stderr into stdout"
check_output "$LOG" "M11-SHELL: ok redir-failure" "redirection failure returns nonzero"
check_output "$LOG" "M11-SHELL: ok pipeline-output" "pipeline passes data between commands"
check_output "$LOG" "M11-SHELL: ok pipeline-status" "pipeline exit status = last command"
check_output "$LOG" "M11-SHELL: ok pipeline-chain" "3-stage pipeline: echo | grep | wc"
check_output "$LOG" "M11-SHELL: ok combo-redir-pipe" "combined redirection and pipeline path works"
check_output "$LOG" "M11-SHELL: ok combo-quote-redir" "quoted variable through pipeline+redirection works"
check_output "$LOG" "M11-SHELL: ok script-exec" "script execution via /bin/sh"
if grep -q "M11-SHELL: ok shebang" "$LOG" 2>/dev/null || grep -q "M11-SHELL: ok shebang-unsupported" "$LOG" 2>/dev/null; then
	pass "shebang behavior marker emitted"
else
	fail "shebang behavior marker emitted" "missing shebang support/unsupported marker"
fi

echo ""
echo "[TEST] M11 Coreutils via shell..."
check_output "$LOG" "M11-UTIL: ok cat" "cat reads file via pipeline"
check_output "$LOG" "M11-UTIL: ok grep" "grep finds pattern (exit 0)"
check_output "$LOG" "M11-UTIL: ok grep-nomatch" "grep exits nonzero when no match"
check_output "$LOG" "M11-UTIL: ok wc" "wc -l counts lines correctly"
check_output "$LOG" "M11-UTIL: ok head" "head -n 2 returns 2 lines"
check_output "$LOG" "M11-UTIL: ok tail" "tail -n 2 returns 2 lines"
check_output "$LOG" "M11-UTIL: ok sort" "sort produces ordered output"
check_output "$LOG" "M11-UTIL: ok uniq" "uniq removes adjacent duplicates"
check_output "$LOG" "M11-UTIL: ok cp" "cp copies file"
check_output "$LOG" "M11-UTIL: ok mv" "mv renames file"
check_output "$LOG" "M11-UTIL: ok mkdir" "mkdir creates directory"
check_output "$LOG" "M11-UTIL: ok rmdir" "rmdir removes directory"
check_output "$LOG" "M11-UTIL: ok rm" "rm removes file"
check_output "$LOG" "M11-UTIL: ok ln-readlink" "ln -s and readlink work together"
check_output "$LOG" "M11-UTIL: ok ps" "ps runs without error"
check_output "$LOG" "M11-UTIL: ok date" "date runs without error"
check_output "$LOG" "M11-UTIL: ok uname" "uname -a runs without error"
check_output "$LOG" "M11-UTIL: ok id" "id prints identity"
check_output "$LOG" "M11-UTIL: ok whoami" "whoami prints user name"
check_output "$LOG" "M11-UTIL: ok sleep" "sleep 0 returns successfully"
check_output "$LOG" "M11-UTIL: ok bad-flag-ls" "ls rejects unsupported flags"
check_output "$LOG" "M11-UTIL: ok bad-flag-grep" "grep rejects unsupported flags"
check_output "$LOG" "NET-SMOKE: ok ping-gateway" "ping -c 2 10.0.2.2 succeeds"
check_output "$LOG" "UDP-SMOKE: probe-sent" "UDP probe command runs"
check_output "$LOG" "UDP-SMOKE: icmp-port-unreachable" "UDP unbound port triggers ICMP unreachable"
check_output "$LOG" "UDP-SMOKE: queue-2pkt-ok" "UDP socket queue preserves two packets"
check_output "$LOG" "POLL-SMOKE: ready-udp" "socket poll readiness path exercised"
check_output "$LOG" "ARP-SMOKE: request-sent" "ARP request path exercised"
    if grep -q "ARP-SMOKE: resolution-ready" "$LOG" 2>/dev/null; then
        pass "ARP resolution became available"
    else
        fail "ARP resolution became available" "no ARP resolution marker observed in this run"
    fi
    if grep -q "ARP-SMOKE: reply-received" "$LOG" 2>/dev/null; then
        pass "ARP reply path exercised"
    else
        fail "ARP reply path exercised" "no ARP reply observed in this run"
    fi
if grep -q "TCP-SMOKE: path-exercised" "$LOG" 2>/dev/null; then
	pass "TCP connect/listen/accept/send/recv path exercised"
elif grep -q "TCP-SMOKE: unsupported" "$LOG" 2>/dev/null; then
	pass "TCP baseline limitation explicitly reported"
else
	fail "TCP path marker emitted" "missing TCP smoke marker"
fi
check_output "$LOG" "B1NIX-TEST: done" "test-mode shutdown marker appears"
check_output "$LOG" "ahci: registered sata0" "AHCI block device registered"
check_output "$LOG" "nvme: registered nvme0" "NVMe block device registered"

# ── Network tests (x86 only) ──
if [ "$ARCH" = "x86" ]; then
	echo ""
	echo "[TEST] Network..."
	if grep -q "virtio-net: initialized with MAC" "$LOG" 2>/dev/null && ! grep -q "virtio-net: no device found" "$LOG" 2>/dev/null; then
		pass "virtio-net initialized"
		if grep -q "DHCP-SMOKE: lease-acquired\|DHCP-SMOKE: fallback-static" "$LOG" 2>/dev/null; then
			pass "DHCP lease or deterministic fallback"
		else
			fail "DHCP lease or deterministic fallback" "missing DHCP-SMOKE marker"
		fi
	else
		fail "virtio-net initialized" "virtio-net message not found"
	fi
fi

# ── Summary ──
echo ""
echo "=== Results ==="
echo "  Passed:  $PASSED"
echo "  Failed:  $FAILED"
echo "  Skipped: $SKIPPED"
echo ""

# Clean up SATA and NVMe dummy images
rm -f "$PROJECT_DIR/sata.img" "$PROJECT_DIR/nvme.img"

if [ "$FAILED" -gt 0 ]; then
	exit 1
fi
exit 0
