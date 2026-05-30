#!/bin/sh
# B1NIX Smoke Test Suite (M24)
# Runs kernel in QEMU and checks for expected output patterns.
# Usage: ./tests/smoke.sh [x86]

set -e

ARCH="${1:-x86}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TIMEOUT=120  # seconds to let each test run (kernel takes longer under macOS load)
mkdir -p "$PROJECT_DIR/smoke_run"
SATA_IMG="$PROJECT_DIR/smoke_run/sata-smoke-$$.img"
NVME_IMG="$PROJECT_DIR/smoke_run/nvme-smoke-$$.img"
SWAP_IMG="$PROJECT_DIR/smoke_run/swap-smoke-$$.img"

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
			${EXTRA_QEMU_ARGS:-} \
			-cdrom "$PROJECT_DIR/build/x86/b1nix.iso" \
			-serial stdio -display none -monitor none -no-reboot \
			-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
				-netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
				-object filter-dump,id=f0,netdev=net0,file="$PROJECT_DIR/smoke_run/net.pcap" \
				-device ich9-ahci,id=ahci \
				-drive file="$SATA_IMG",if=none,id=satadrive,format=raw \
				-device ide-hd,drive=satadrive,bus=ahci.0 \
				-drive file="$SWAP_IMG",if=none,id=swapdrive,format=raw \
				-device ide-hd,drive=swapdrive,bus=ahci.1 \
				-drive file="$NVME_IMG",if=none,id=nvmedrive,format=raw \
				-device nvme,serial=deadbeef,drive=nvmedrive \
				>"$log" 2>&1 &
		pid=$!

		# Wait for the final success marker or a panic, then kill QEMU.
		(
			timeout "$TIMEOUT" bash -c "tail -n +1 -f \"$log\" 2>/dev/null | grep -m 1 -E 'B1NIX-TEST: done|KERNEL PANIC|\\[PANIC\\]'" >/dev/null 2>&1
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
make ARCH="$ARCH" KERNEL_CMDLINE="b1nix.test=1 b1nix.kvtest=abc123" iso >/dev/null 2>&1 || {
	echo "  ${RED}BUILD FAILED${NC}"
	exit 1
}
pass "kernel builds without errors"

# Create dummy images for SATA, NVMe and Swap tests
dd if=/dev/zero of="$SATA_IMG" bs=1M count=4 2>/dev/null
dd if=/dev/zero of="$NVME_IMG" bs=1M count=4 2>/dev/null
dd if=/dev/zero of="$SWAP_IMG" bs=1M count=2 2>/dev/null

MKE2FS="/opt/homebrew/opt/e2fsprogs/sbin/mke2fs"
if [ ! -x "$MKE2FS" ]; then
    MKE2FS=$(command -v mke2fs 2>/dev/null || echo "/sbin/mke2fs")
fi
if [ -z "$MKE2FS" ] || ! command -v "$MKE2FS" >/dev/null 2>&1; then
    echo "Error: mke2fs utility not found. Please install e2fsprogs."
    exit 1
fi

# Format with minimal ext4 features (metadata_csum, 64bit, flex_bg not supported by kernel driver)
"$MKE2FS" -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q "$SATA_IMG" 2>/dev/null || {
    "$MKE2FS" -t ext4 -q "$SATA_IMG" 2>/dev/null || {
        echo "Error: Failed to format sata.img as ext4."
        exit 1
    }
}
"$MKE2FS" -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q "$NVME_IMG" 2>/dev/null || {
    "$MKE2FS" -t ext4 -q "$NVME_IMG" 2>/dev/null || {
        echo "Error: Failed to format nvme.img as ext4."
        exit 1
    }
}


# ── Test 1: Kernel boots ──
echo ""
echo "[TEST] Boot and basic output..."
mkdir -p "$PROJECT_DIR/smoke_run"
LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-boot.log"
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
# M28 #9: ctx-switch + light-syscall rdtsc benchmark (single-CPU only)
check_output "$LOG" "M28-BENCH: ok" "M28 ctx-switch benchmark completes"

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

# ── M13 Userspace ABI / libc / POSIX runtime hardening ──
echo ""
echo "[TEST] M13 Userspace ABI / libc / POSIX runtime..."
check_output "$LOG" "M13-SMOKE: start" "M13 smoke starts"
check_output "$LOG" "M13-SMOKE: ok argc-argv0" "argc/argv[0] baseline is stable"
check_output "$LOG" "M13-SMOKE: ok stack-align" "initial userspace stack alignment is sane"
check_output "$LOG" "M13-SMOKE: ok libc-rw-open-close-lseek" "libc wrappers for read/write/open/close/lseek work"
check_output "$LOG" "M13-SMOKE: ok getpid-uid-gid" "getpid/getuid/getgid path works"
check_output "$LOG" "M13-SMOKE: ok brk-mmap-munmap" "brk/mmap/munmap baseline works"
check_output "$LOG" "M13-SMOKE: ok puts" "puts works"
check_output "$LOG" "M13-SMOKE: ok printf" "printf works"
check_output "$LOG" "M13-SMOKE: ok snprintf" "snprintf works"
check_output "$LOG" "M13-SMOKE: ok stdio-file" "fopen/fread/fwrite/fclose path works"
if grep -q "M13-SMOKE: ok execve-argv-env" "$LOG" 2>/dev/null; then
	pass "execve preserves argv/envp semantics"
elif grep -q "M13-SMOKE: unsupported execve-argv-env-native-elf" "$LOG" 2>/dev/null; then
	fail "execve preserves argv/envp semantics" "native ELF argv/envp is explicitly unsupported"
else
	fail "execve argv/envp marker emitted" "missing execve argv/envp support/unsupported marker"
fi
check_output "$LOG" "M13-SMOKE: ok execve-fail-deterministic" "failed execve returns deterministic child status"
if grep -q "M13-SMOKE: ok builtin-exec" "$LOG" 2>/dev/null; then
	pass "builtin exec path works through execve"
elif grep -q "M13-SMOKE: unsupported builtin-exec" "$LOG" 2>/dev/null; then
	fail "builtin exec path works through execve" "builtin exec is explicitly unsupported"
else
	fail "builtin exec marker emitted" "missing builtin exec support/unsupported marker"
fi
if grep -q "M13-SMOKE: ok sh-c-argv" "$LOG" 2>/dev/null; then
	pass "/bin/sh -c preserves command argv semantics"
elif grep -q "M13-SMOKE: unsupported sh-c-argv" "$LOG" 2>/dev/null; then
	fail "/bin/sh -c preserves command argv semantics" "sh -c argv is explicitly unsupported"
else
	fail "sh -c argv marker emitted" "missing sh -c argv support/unsupported marker"
fi
if grep -q "M13-SMOKE: ok sh-c-status" "$LOG" 2>/dev/null; then
	pass "/bin/sh -c execution returns stable status"
elif grep -q "M13-SMOKE: unsupported sh-c-status" "$LOG" 2>/dev/null; then
	fail "/bin/sh -c execution returns stable status" "sh -c status is explicitly unsupported"
else
	fail "sh -c status marker emitted" "missing sh -c status support/unsupported marker"
fi
if grep -q "M13-SMOKE: ok fd-inherit-exec" "$LOG" 2>/dev/null; then
	pass "fd inheritance survives exec boundary"
elif grep -q "M13-SMOKE: unsupported fd-inherit-exec" "$LOG" 2>/dev/null; then
	fail "fd inheritance survives exec boundary" "fd inheritance exec-boundary is explicitly unsupported"
else
	fail "fd inheritance exec marker emitted" "missing fd inheritance support/unsupported marker"
fi
check_output "$LOG" "M13-SMOKE: ok dup2" "dup2 behavior remains correct"
if grep -q "M13-SMOKE: ok cloexec-exec" "$LOG" 2>/dev/null; then
	pass "close-on-exec is enforced across exec boundary"
elif grep -q "M13-SMOKE: unsupported cloexec-exec" "$LOG" 2>/dev/null; then
	fail "close-on-exec is enforced across exec boundary" "close-on-exec exec-boundary is explicitly unsupported"
else
	fail "close-on-exec exec marker emitted" "missing close-on-exec support/unsupported marker"
fi
check_output "$LOG" "M13-SMOKE: ok parent-intact" "failed child exec path does not corrupt parent runtime"
check_output "$LOG" "M13-SMOKE: ok errno-negative" "negative syscall result path is exposed to userspace"
check_output "$LOG" "M13-SMOKE: done" "M13 smoke completes successfully"
check_output "$LOG" "M13-JC-SMOKE: start" "M13 job-control smoke starts"
check_output "$LOG" "M13-JC-SMOKE: ok tcsetpgrp-child" "foreground pgrp switches to child group"
check_output "$LOG" "M13-JC-SMOKE: ok wuntraced" "waitpid reports stopped child with WUNTRACED"
check_output "$LOG" "M13-JC-SMOKE: ok tcsetpgrp-self" "foreground pgrp switches back to parent group"
check_output "$LOG" "M13-JC-SMOKE: ok wcontinued" "waitpid reports continued child with WCONTINUED"
check_output "$LOG" "M13-JC-SMOKE: ok sigttin" "background terminal read stops with SIGTTIN"
check_output "$LOG" "M13-JC-SMOKE: ok sigttou" "background terminal write stops with SIGTTOU when TOSTOP is set"
check_output "$LOG" "M13-JC-SMOKE: done" "M13 job-control smoke completes"

# ── M17 errno matrix smoke ──
echo ""
echo "[TEST] M17 errno matrix..."
check_output "$LOG" "M17-SMOKE: start" "M17 smoke starts"
check_output "$LOG" "M17-SMOKE: ok eloop" "M17 ELOOP symlink-depth behavior is correct"
check_output "$LOG" "M17-SMOKE: ok enametoolong" "M17 ENAMETOOLONG path-component limit is correct"
check_output "$LOG" "M17-SMOKE: ok enotdir" "M17 ENOTDIR file-as-directory behavior is correct"
check_output "$LOG" "M17-SMOKE: ok eisdir" "M17 EISDIR write-to-directory behavior is correct"
if grep -q "M17-SMOKE: ok erofs" "$LOG" 2>/dev/null; then
	pass "M17 EROFS readonly mount behavior is correct"
elif grep -q "M17-SMOKE: ok erofs-skip" "$LOG" 2>/dev/null; then
	pass "M17 EROFS test skipped (no readonly mount candidate)"
else
	fail "M17 EROFS marker emitted" "missing erofs/erofs-skip marker"
fi
check_output "$LOG" "M17-SMOKE: ok errno-isolation" "M17 errno isolation across successful syscall is correct"
check_output "$LOG" "M17-SMOKE: done" "M17 smoke completes successfully"

# ── M8 AIO / completion queues ──
echo ""
echo "[TEST] M8 AIO completion queues..."
check_output "$LOG" "M8-AIO-SMOKE: start" "M8 AIO smoke starts"
check_output "$LOG" "M8-AIO-SMOKE: ok write" "M8 AIO async write completes"
check_output "$LOG" "M8-AIO-SMOKE: ok read" "M8 AIO async read completes"
check_output "$LOG" "M8-AIO-SMOKE: done" "M8 AIO smoke completes"

# ── M14 Advanced Storage, Swap & File Systems ──
echo ""
echo "[TEST] M14 Storage, Swap & File Systems..."
check_output "$LOG" "M14-SMOKE: ok swap-smoke" "swap page swap-out and swap-in verified"
check_output "$LOG" "M14-SMOKE: ok mount-ext4-sata" "mount sata0 as ext4 successful"
check_output "$LOG" "M14-SMOKE: ok mount-ext4-nvme" "mount nvme0 as ext4 successful"
check_output "$LOG" "M14-SMOKE: ok ext4-persistence" "ext4 read, write, and remount persistence verified"
check_output "$LOG" "M14-SMOKE: ok block-cache" "cached read and dirty write verified"
check_output "$LOG" "M14-SMOKE: ok persistence" "persistence through sync, umount, and remount verified"
check_output "$LOG" "M14-SMOKE: ok invalid-device" "mounting invalid device fails gracefully"
check_output "$LOG" "M14-SMOKE: ok invalid-fs" "mounting invalid filesystem type fails gracefully"
check_output "$LOG" "M14-SMOKE: ok stress-loop" "repeated create/write/read/delete loop completes successfully"
check_output "$LOG" "M14-SMOKE: ok large-file" "large file bounds allocation and verification successful"
check_output "$LOG" "M14-SMOKE: ok VFS-normalization" "VFS path normalization works on mounts"
check_output "$LOG" "M14-SMOKE: done" "M14 smoke completes successfully"

# ── M15 IPC, Security & Standard OS Features ──
echo ""
echo "[TEST] M15 IPC, security, and OS baseline..."
check_output "$LOG" "M15-SMOKE: start" "M15 smoke starts"
check_output "$LOG" "M15-SMOKE: ok signal-baseline" "signal baseline syscall path works"
check_output "$LOG" "M15-SMOKE: ok signal-ignore" "ignored signal does not terminate process"
check_output "$LOG" "M15-SMOKE: ok signal-handler" "userspace signal handler is delivered"
check_output "$LOG" "M15-SMOKE: ok signal-mask" "blocked signal is delivered after sigprocmask unblock"
check_output "$LOG" "M15-SMOKE: ok ipc-mq" "message queue roundtrip works"
check_output "$LOG" "M15-SMOKE: ok shm" "shared memory create/map/read/write lifecycle works"
check_output "$LOG" "M15-SMOKE: ok semaphore" "cooperative semaphore baseline works"
check_output "$LOG" "M15-SMOKE: ok clock-timer" "clock_gettime and nanosleep work"
check_output "$LOG" "M15-SMOKE: ok permissions-chmod" "chmod changes mode bits"
check_output "$LOG" "M15-SMOKE: ok permissions-enforcement" "permissions are enforced for non-root uid"
check_output "$LOG" "M15-SMOKE: ok audit-logging" "audit marker appears after privileged syscall"
check_output "$LOG" "M15-SMOKE: done" "M15 smoke completes"

# ── M25 Native Toolchain ──
echo ""
echo "[TEST] M25 Native C Toolchain..."
check_output "$LOG" "M25-SMOKE: start" "M25 smoke starts"
check_output "$LOG" "M25-SMOKE: ok tcc-launch" "tcc launches"
check_output "$LOG" "M25-SMOKE: ok compile-hello" "tcc compiles hello.c"
check_output "$LOG" "M25-SMOKE: ok run-hello" "compiled hello program runs"
check_output "$LOG" "M25-HELLO: hello from native tcc" "hello program outputs correct greeting"
check_output "$LOG" "M25-SMOKE: ok compile-utility" "tcc compiles and runs mini-echo utility"
check_output "$LOG" "M25-SMOKE: ok argv-check" "compiled program receives argc/argv"
check_output "$LOG" "M25-SMOKE: ok stderr-check" "compiled program stderr path works"
check_output "$LOG" "M25-SMOKE: ok exit-check" "compiled program non-zero exit status propagates"
check_output "$LOG" "M25-SMOKE: ok float-check" "compiled program float/double parsing works"
check_output "$LOG" "M25-FLOAT: all float tests passed" "float program outputs correct results"
check_output "$LOG" "M25-SMOKE: done" "M25 smoke completes"

# ── M26 Native Toolchain & Self-Host ──
echo ""
echo "[TEST] M26 Native C Toolchain & Self-Host..."
check_output "$LOG" "M26-SMOKE: start" "M26 smoke starts"
check_output "$LOG" "M26-SMOKE: ok selfhost-status" "selfhost status syscall works"
check_output "$LOG" "M26-SMOKE: ok toolchain-ready" "native toolchain (gcc/binutils/make) is ported"
check_output "$LOG" "M26-SMOKE: ok readdir" "libc opendir/readdir over SYS_GETDENTS works"
# NOTE: in-guest full kernel self-build is not yet verified, so the marker is
# "pending can-build-kernel" (not "ok"). Flip the kernel flag + this check to
# "ok can-build-kernel" only once an in-guest kernel.elf actually builds.
check_output "$LOG" "M26-SMOKE: done" "M26 smoke completes"


# ── M16 User Space Applications & TUI ──
echo ""
echo "[TEST] M16 user space applications and TUI..."
check_output "$LOG" "M16-SMOKE: start" "M16 smoke starts"
check_output "$LOG" "M16-SMOKE: ok tui-key-decode" "shared TUI key decoding works"
check_output "$LOG" "M16-SMOKE: ok file-explorer-hotkeys" "file explorer hotkeys work"
check_output "$LOG" "M16-SMOKE: ok editor-hotkeys" "text editor hotkeys work"
check_output "$LOG" "M16-SMOKE: ok editor-persist" "editor persistence save+reload works"
check_output "$LOG" "M16-SMOKE: ok file-clipboard" "clipboard VFS copy+delete works"
check_output "$LOG" "M16-SMOKE: ok terminal-restore" "terminal raw mode is restored"
check_output "$LOG" "M16-SMOKE: ok app-lifecycle" "app lifecycle completes"
check_output "$LOG" "M16-SMOKE: done" "M16 smoke completes"

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
check_output "$LOG" "M22-POLISH: start" "M22 Polish starts"
check_output "$LOG" "M22-POLISH: ok utility-flags" "M22 Polish utility flags verify"
check_output "$LOG" "M22-POLISH: ok text-pipeline" "M22 Polish text pipeline verifies"
check_output "$LOG" "M22-POLISH: ok file-workflow" "M22 Polish file workflow verifies"
check_output "$LOG" "M22-POLISH: ok failure-status" "M22 Polish failure status propagates"
check_output "$LOG" "M24-SMOKE: ok ENOENT" "M24 errno ENOENT is mapped"
check_output "$LOG" "M24-SMOKE: ok EEXIST" "M24 errno EEXIST is mapped"
check_output "$LOG" "M24-SMOKE: ok EINVAL" "M24 errno EINVAL is mapped"
check_output "$LOG" "M24-SMOKE: ok EBADF" "M24 errno EBADF is mapped"
check_output "$LOG" "M24-SMOKE: ok ENOTDIR" "M24 errno ENOTDIR is mapped"
check_output "$LOG" "M24-SMOKE: ok EISDIR" "M24 errno EISDIR is mapped"
check_output "$LOG" "M24-SMOKE: ok EACCES" "M24 errno EACCES is mapped"
check_output "$LOG" "M24-SMOKE: ok errno-mapping" "M24 userspace errno mapping verifies"
check_output "$LOG" "M24-SMOKE: ok diagnostics" "M24 userspace diagnostics verify"
check_output "$LOG" "M22-POLISH: done" "M22 Polish completes successfully"

# ── M11 Shell & Utilities ──
echo ""
echo "[TEST] M11 Shell baseline..."
check_output "$LOG" "M11-SMOKE: start" "M11 shell smoke starts"
check_output "$LOG" "M11-SMOKE: ok pipe-eof" "pipe EOF when all writers close"
check_output "$LOG" "M11-SMOKE: ok pipe-nonblock-read" "pipe nonblocking read returns EAGAIN"
check_output "$LOG" "M11-SMOKE: ok pipe-nonblock-write" "pipe nonblocking write returns EAGAIN"
check_output "$LOG" "M11-SMOKE: done" "M11 shell smoke completes"

# ── M33 Shell compliance: globbing + arithmetic expansion ──
check_output "$LOG" "M33-SHELL: start" "M33 shell smoke starts"
check_output "$LOG" "M33-SHELL: ok glob-star" "pathname glob '*.txt' expands and sorts"
check_output "$LOG" "M33-SHELL: ok glob-class" "glob '?' and '[ab]' bracket expressions match"
check_output "$LOG" "M33-SHELL: ok glob-nomatch" "non-matching glob stays literal"
check_output "$LOG" "M33-SHELL: ok arith" "arithmetic expansion \$((...)) evaluates"
check_output "$LOG" "M33-SHELL: ok heredoc" "here-document collection + body expansion"
check_output "$LOG" "M33-SHELL: ok cmdsubst" "command substitution \$(...) and backticks"
check_output "$LOG" "M33-SHELL: ok subshell" "subshell ( ... ) isolates env side effects"
check_output "$LOG" "M33-SHELL: ok function" "shell functions define + invoke with positionals"
check_output "$LOG" "M33-SHELL: ok case" "case ... esac selects matching glob branch"
check_output "$LOG" "M33-SHELL: ok array" "arrays: assignment, index, [@], length"
check_output "$LOG" "M33-SHELL: ok jobs" "job control: SIGTSTP stop + bg/SIGCONT resume"
check_output "$LOG" "M33-SHELL: ok grep-flags" "coreutils flag broadening: grep -i / -c"
check_output "$LOG" "M33-SHELL: ok param-expand" "parameter expansion \${x:-w}/:+/:=/#"
check_output "$LOG" "M33-SHELL: ok for-loop" "for VAR in LIST; do ...; done"
check_output "$LOG" "M33-SHELL: ok while-loop" "while COND; do ...; done + scalar assign"
check_output "$LOG" "M33-SHELL: ok trap" "trap 'cmd' EXIT registers + fires"
check_output "$LOG" "M33-SHELL: done" "M33 shell smoke completes"

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
# ── M27 Terminal OS Polish: kernel command line parsing ──
check_output "$LOG" "M27-CMDLINE: ok kv-parse" "kernel command line key=value parser works"
check_output "$LOG" "M27-INIT: ok rc-script" "boot rc script runs via /bin/sh"
check_output "$LOG" "M27-INIT: first-boot /persist initialised" "first-boot persistent-root setup runs"
check_output "$LOG" "M27-USER: ok getpwnam-root" "getpwnam parses root from /etc/passwd"
check_output "$LOG" "M27-USER: ok getpwnam-user" "getpwnam parses a non-root user from /etc/passwd"
check_output "$LOG" "M27-USER: ok getpwuid" "getpwuid resolves uid to name"
check_output "$LOG" "M27-USER: ok unknown-user" "getpwnam returns NULL for unknown user"
check_output "$LOG" "M27-USER: ok setuid-drop" "setgid/setuid privilege drop works (login machinery)"
check_output "$LOG" "M27-USER: done" "M27 user/passwd smoke completes"
# ── M29 POSIX threads / futex / TLS ──
check_output "$LOG" "M29-PTHREAD: start" "M29 pthread smoke starts"
check_output "$LOG" "M29-PTHREAD: ok create-join" "pthread_create + pthread_join works"
check_output "$LOG" "M29-PTHREAD: ok mutex" "pthread mutex serialises two threads"
check_output "$LOG" "M29-PTHREAD: ok condvar" "pthread condvar signal/wait works"
check_output "$LOG" "M29-PTHREAD: ok tls" "SYS_SET_TLS + %fs:0 round-trip works"
check_output "$LOG" "M29-PTHREAD: ok gettid" "SYS_GETTID returns distinct ids per thread"
check_output "$LOG" "M29-PTHREAD: done" "M29 pthread smoke completes"
check_output "$LOG" "B1NIX-TEST: done" "test-mode shutdown marker appears"
check_output "$LOG" "reboot: restarting" "SYS_REBOOT performs a real machine restart"
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

# ── M24b SMP work-stealing (multi-core) ──
echo ""
echo "[TEST] M24b SMP work-stealing (-smp 4)..."
# Re-create the disk images so the SMP boot starts from a clean state.
"$MKE2FS" -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q "$SATA_IMG" 2>/dev/null || true
"$MKE2FS" -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q "$NVME_IMG" 2>/dev/null || true
dd if=/dev/zero of="$SWAP_IMG" bs=1M count=2 2>/dev/null
SMP_LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-smp.log"
EXTRA_QEMU_ARGS="-smp 4"
run_qemu "$SMP_LOG"
EXTRA_QEMU_ARGS=""
check_output "$SMP_LOG" "smp: AP 1 ready" "Application Processor boots (INIT-SIPI)"
check_output "$SMP_LOG" "M24B-SMP: ok work-stealing" "cross-CPU work-stealing runs stolen tasks on APs"
check_output "$SMP_LOG" "B1NIX-TEST: done" "full suite completes under SMP"

# ── Summary ──
echo ""
echo "=== Results ==="
echo "  Passed:  $PASSED"
echo "  Failed:  $FAILED"
echo "  Skipped: $SKIPPED"

rm -f "$SATA_IMG" "$NVME_IMG" "$SWAP_IMG"
echo ""

# Clean up SATA, NVMe and Swap dummy images
rm -f "$PROJECT_DIR/smoke_run/sata.img" "$PROJECT_DIR/smoke_run/nvme.img" "$PROJECT_DIR/smoke_run/swap.img"

if [ "$FAILED" -gt 0 ]; then
	exit 1
fi
exit 0
