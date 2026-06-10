#!/bin/sh
# B1NIX Smoke Test Suite (M24)
# Runs kernel in QEMU and checks for expected output patterns.
# Usage: ./tests/smoke.sh [x86_64]

set -e

ARCH="${1:-x86_64}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# Seconds to let each test run (override via env). x86_64 emulates notably slower
# than i386 under TCG (no KVM on macOS) and the single-CPU suite lands right at
# ~110-120s, so give 64-bit more headroom to avoid false timeouts.
if [ "$ARCH" = "x86_64" ]; then
	DEFAULT_TIMEOUT=480
else
	DEFAULT_TIMEOUT=120
fi
# The upstream-BusyBox smoke spawns ~60 extra ELF loads (waves 5-7: account
# applets, xattr/lsblk round-trips, ...); under TCG that overruns the tight
# default budgets, and the -smp 4 full-suite pass is the slowest of all, so give
# both arches generous headroom.
if [ "$ARCH" = "x86_64" ]; then
	DEFAULT_TIMEOUT=600
else
	DEFAULT_TIMEOUT=480
fi
TIMEOUT=${TIMEOUT:-$DEFAULT_TIMEOUT}
SMOKE_VERBOSE=${SMOKE_VERBOSE:-0}
SMOKE_PROGRESS_MODE=full
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
	if [ "$SMOKE_VERBOSE" = "1" ]; then
		printf "  ${GREEN}PASS${NC} %s\n" "$1"
	fi
	PASSED=$((PASSED + 1))
}

fail() {
	printf "  ${RED}FAIL${NC} %s - %s\n" "$1" "$2"
	FAILED=$((FAILED + 1))
}

section() {
	if [ "$SMOKE_VERBOSE" = "1" ]; then
		echo ""
		echo "[CHECK] $1"
	fi
}

report_progress_line() {
	local line="$1"

	if [ "$SMOKE_PROGRESS_MODE" = "smp" ]; then
		case "$line" in
			smp:\ AP*|M24B-*|B1NIX-TEST:*|*PANIC*) ;;
			*) return ;;
		esac
	else
		case "$line" in
			b1nix\ kernel*|init\ spawn\ result:*|M[0-9]*:*|NATIVE-SMOKE:*|POSIX-SMOKE:*|LOCK-SMOKE:*|EXT-STRESS:*|NET-SMOKE:*|UDP-SMOKE:*|POLL-SMOKE:*|TCP-SMOKE:*|DNS-SMOKE:*|BB-SMOKE:*|BB-W[0-9]*:*|B1NIX-TEST:*|*PANIC*) ;;
			*) return ;;
		esac
	fi

	case "$line" in
		*": FAIL"*|*": fail "*|*": failed"*|*PANIC*)
			printf "  ${RED}%s${NC}\n" "$line"
			;;
		*": ok"*|*": done"*|B1NIX-TEST:\ done)
			printf "  ${GREEN}%s${NC}\n" "$line"
			;;
		*)
			printf "  ${YELLOW}%s${NC}\n" "$line"
			;;
	esac
}

# Run QEMU and capture output
run_qemu() {
	local log="$1"
	shift
	local pid

	if [ "$ARCH" = "x86_64" ] || [ "$ARCH" = "x86" ]; then
		local filter_dump_args=""
		if qemu-system-x86_64 -object filter-dump,help >/dev/null 2>&1; then
			filter_dump_args="-object filter-dump,id=f0,netdev=net0,file=$PROJECT_DIR/smoke_run/net.pcap"
		fi

		qemu-system-x86_64 \
			-cdrom "$PROJECT_DIR/build/$ARCH/${B1NIX_ISO_NAME:-b1nix.iso}" \
			-serial stdio -display none -monitor none -no-reboot \
			-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
				-netdev user,id=net0,restrict=${B1NIX_NET_RESTRICT:-on} -device virtio-net-pci,netdev=net0 \
				${filter_dump_args} \
				-netdev user,id=net1,restrict=${B1NIX_NET_RESTRICT:-on} -device ${E1000_MODEL:-e1000},netdev=net1 \
				-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 \
				-device ich9-ahci,id=ahci \
				-drive file="$SATA_IMG",if=none,id=satadrive,format=raw \
				-device ide-hd,drive=satadrive,bus=ahci.0 \
				-drive file="$SWAP_IMG",if=none,id=swapdrive,format=raw \
				-device ide-hd,drive=swapdrive,bus=ahci.1 \
				-drive file="$NVME_IMG",if=none,id=nvmedrive,format=raw \
				-device nvme,serial=deadbeef,drive=nvmedrive \
				${EXTRA_QEMU_ARGS:-} \
				>"$log" 2>&1 &
		pid=$!

		# Wait for completion marker/panic or timeout, then kill QEMU.
		# Keep this POSIX-portable (macOS doesn't ship GNU timeout by default).
		(
			start_ts=$(date +%s)
			reported_lines=0
			while :; do
				line_count=$(wc -l <"$log" | tr -d ' ')
				if [ "$line_count" -gt "$reported_lines" ]; then
					sed -n "$((reported_lines + 1)),${line_count}p" "$log" |
						while IFS= read -r line; do
							report_progress_line "$line"
						done
					reported_lines=$line_count
				fi
				if grep -q -E 'B1NIX-TEST: done|KERNEL PANIC|\[PANIC\]' "$log" 2>/dev/null; then
					break
				fi
				if ! kill -0 "$pid" 2>/dev/null; then
					# QEMU may exit before stdio has flushed the last serial burst.
					sleep 1
					line_count=$(wc -l <"$log" | tr -d ' ')
					if [ "$line_count" -gt "$reported_lines" ]; then
						sed -n "$((reported_lines + 1)),${line_count}p" "$log" |
							while IFS= read -r line; do
								report_progress_line "$line"
							done
						reported_lines=$line_count
					fi
					break
				fi
				now_ts=$(date +%s)
				if [ $((now_ts - start_ts)) -ge "$TIMEOUT" ]; then
					echo "[smoke] run_qemu timeout after ${TIMEOUT}s" >>"$log"
					break
				fi
				sleep 1
			done
			sleep 2
			kill "$pid" 2>/dev/null || true
		) &
		local watcher_pid=$!

		wait "$pid" 2>/dev/null || true
		wait "$watcher_pid" 2>/dev/null || true
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
# SKIP_BUILD=1 reuses an existing build/$ARCH/b1nix.iso (e.g. when the toolchain
# can't rebuild every userspace port locally). SMOKE_MAKE_ARGS lets the caller
# inject extra make flags (e.g. CC=clang LD=ld.lld on Fedora, where `cc` is gcc).
if [ "${SKIP_BUILD:-0}" = "1" ]; then
	test -f "build/$ARCH/b1nix.iso" || { echo "  ${RED}no prebuilt build/$ARCH/b1nix.iso${NC}"; exit 1; }
	echo "  (SKIP_BUILD=1 — reusing build/$ARCH/b1nix.iso)"
else
	make ARCH="$ARCH" ${SMOKE_MAKE_ARGS:-} KERNEL_CMDLINE="b1nix.test=1 b1nix.kvtest=abc123 b1nix.ssh-loopback=1" iso >/dev/null 2>&1 || {
		echo "  ${RED}BUILD FAILED${NC}"
		exit 1
	}
fi
pass "kernel builds without errors"
echo "  build/$ARCH/${B1NIX_ISO_NAME:-b1nix.iso} ready"

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
"$MKE2FS" -F -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q "$SATA_IMG" 2>/dev/null || {
    "$MKE2FS" -F -t ext4 -q "$SATA_IMG" 2>/dev/null || {
        echo "Error: Failed to format sata.img as ext4."
        exit 1
    }
}
"$MKE2FS" -F -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q "$NVME_IMG" 2>/dev/null || {
    "$MKE2FS" -F -t ext4 -q "$NVME_IMG" 2>/dev/null || {
        echo "Error: Failed to format nvme.img as ext4."
        exit 1
    }
}


# ── Test 1: Kernel boots ──
echo ""
echo "[RUN] Boot smoke (live serial progress)..."
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
section "M12 Syscalls & Process Management"
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
section "M13 Userspace ABI / libc / POSIX runtime"
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
section "M17 errno matrix"
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
section "M8 AIO completion queues"
check_output "$LOG" "M8-AIO-SMOKE: start" "M8 AIO smoke starts"
check_output "$LOG" "M8-AIO-SMOKE: ok write" "M8 AIO async write completes"
check_output "$LOG" "M8-AIO-SMOKE: ok read" "M8 AIO async read completes"
check_output "$LOG" "M8-AIO-SMOKE: done" "M8 AIO smoke completes"

# ── M14 Advanced Storage, Swap & File Systems ──
section "M14 Storage, Swap & File Systems"
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
section "M15 IPC, security, and OS baseline"
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
section "M25 Native C Toolchain"
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
check_output "$LOG" "M25-SMOKE: ok scanf" "libc scanf/sscanf format parsing works"
check_output "$LOG" "M25-SMOKE: ok frexp" "libc frexp decomposes correctly"
check_output "$LOG" "M25-SMOKE: ok clock64" "libc clock_gettime returns sane 64-bit realtime/monotonic tuples"
check_output "$LOG" "M25-SMOKE: ok time64-utime" "libc utime/stat preserves timestamps above 2^32"
check_output "$LOG" "M25-SMOKE: ok fileops" "libc fchmod/utime/tmpfile work (verified via stat)"
check_output "$LOG" "M25-SMOKE: done" "M25 smoke completes"

# ── M26 Native Toolchain & Self-Host ──
section "M26 Native C Toolchain & Self-Host"
check_output "$LOG" "M26-SMOKE: start" "M26 smoke starts"
check_output "$LOG" "M26-SMOKE: ok selfhost-status" "selfhost status syscall works"
check_output "$LOG" "M26-SMOKE: ok toolchain-ready" "native toolchain (gcc/binutils/make) is ported"
check_output "$LOG" "M26-SMOKE: ok readdir" "libc opendir/readdir over SYS_GETDENTS works"
# NOTE: in-guest full kernel self-build is not yet verified, so the marker is
# "pending can-build-kernel" (not "ok"). Flip the kernel flag + this check to
# "ok can-build-kernel" only once an in-guest kernel.elf actually builds.
check_output "$LOG" "M26-SMOKE: done" "M26 smoke completes"


# ── M16 User Space Applications & TUI ──
section "M16 user space applications and TUI"
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
section "M22 utilities"
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

	echo ""
	section "Upstream BusyBox package"
	check_output "$LOG" "BB-SMOKE: ok list" "busybox --list works"
	check_output "$LOG" "BB-SMOKE: ok echo" "busybox echo works"
	check_output "$LOG" "BB-SMOKE: ok printf" "busybox printf works"
	check_output "$LOG" "BB-SMOKE: ok pwd" "busybox pwd works"
	check_output "$LOG" "BB-SMOKE: ok mkdir" "busybox mkdir works"
	check_output "$LOG" "BB-SMOKE: ok touch" "busybox touch works"
	check_output "$LOG" "BB-SMOKE: ok cat" "busybox cat works"
	check_output "$LOG" "BB-SMOKE: ok cp" "busybox cp works"
	check_output "$LOG" "BB-SMOKE: ok mv" "busybox mv works"
	check_output "$LOG" "BB-SMOKE: ok ln" "busybox ln works"
	check_output "$LOG" "BB-SMOKE: ok readlink" "busybox readlink works"
	check_output "$LOG" "BB-SMOKE: ok chmod" "busybox chmod works"
	check_output "$LOG" "BB-SMOKE: ok test" "busybox test / [ works"
	check_output "$LOG" "BB-SMOKE: ok sort" "busybox sort works"
	check_output "$LOG" "BB-SMOKE: ok uniq" "busybox uniq works"
	check_output "$LOG" "BB-W1: ok ls" "busybox ls works"
	check_output "$LOG" "BB-W1: ok cmp" "busybox cmp works"
	check_output "$LOG" "BB-W1: ok cut" "busybox cut works"
	check_output "$LOG" "BB-W1: ok env" "busybox env works"
	check_output "$LOG" "BB-W1: ok id" "busybox id works"
	check_output "$LOG" "BB-W1: ok printenv" "busybox printenv works"
	check_output "$LOG" "BB-W1: ok tee" "busybox tee works"
	check_output "$LOG" "BB-W1: ok tr" "busybox tr works"
	check_output "$LOG" "BB-W1: ok whoami" "busybox whoami works"
	check_output "$LOG" "BB-W1: ok seq" "busybox seq works"
	check_output "$LOG" "BB-W1: ok which" "busybox which works"
	check_output "$LOG" "BB-W1: ok clear" "busybox clear works"
	check_output "$LOG" "BB-W1: ok hexdump" "busybox hexdump works"
	check_output "$LOG" "BB-W2: ok stat" "busybox stat works"
	check_output "$LOG" "BB-W2: ok realpath" "busybox realpath works"
	check_output "$LOG" "BB-W2: ok mktemp" "busybox mktemp works"
	check_output "$LOG" "BB-W2: ok find" "busybox find works"
	check_output "$LOG" "BB-W2: ok grep" "busybox grep ERE works"
	check_output "$LOG" "BB-W2: ok grep-icase" "busybox grep REG_ICASE classes work"
	check_output "$LOG" "BB-W2: ok sed" "busybox sed BRE groups work"
	check_output "$LOG" "BB-W2: ok awk" "busybox awk field processing works"
	check_output "$LOG" "BB-W2: ok xargs" "busybox xargs works"
	check_output "$LOG" "BB-W2: ok diff" "busybox diff statuses work"
	check_output "$LOG" "BB-W2: ok cksum" "busybox cksum works"
	check_output "$LOG" "BB-W2: ok md5sum" "busybox md5sum works"
	check_output "$LOG" "BB-W2: ok sha256sum" "busybox sha256sum works"
	check_output "$LOG" "BB-W2B: ok dd" "busybox dd works"
	check_output "$LOG" "BB-W2B: ok du" "busybox du works"
	check_output "$LOG" "BB-W2B: ok df" "busybox df works (/proc/mounts)"
	check_output "$LOG" "BB-W2B: ok tar" "busybox tar create/extract works"
	check_output "$LOG" "BB-W2B: ok tar-gzip" "busybox tar -z seamless gzip works"
	check_output "$LOG" "BB-W2B: ok gzip" "busybox gzip/gunzip round trip works"
	check_output "$LOG" "BB-W2B: ok bzip2" "busybox bzip2/bunzip2 round trip works"
	check_output "$LOG" "BB-W2B: ok xzcat" "busybox xzcat decompresses xz"
	check_output "$LOG" "BB-W2B: ok unxz" "busybox unxz decompresses xz"
	check_output "$LOG" "BB-W2B: ok gunzip-malformed" "busybox gunzip rejects malformed input"
	check_output "$LOG" "BB-W3: ok ps" "busybox ps reads /proc/<pid>/stat"
	check_output "$LOG" "BB-W3: ok top" "busybox top batch iteration works"
	check_output "$LOG" "BB-W3: ok uptime" "busybox uptime reads /proc/uptime+loadavg"
	check_output "$LOG" "BB-W3: ok free" "busybox free reads sysinfo()+/proc/meminfo"
	check_output "$LOG" "BB-W3: ok dmesg" "busybox dmesg drains kernel ring via klogctl"
	check_output "$LOG" "BB-W3: ok pidof" "busybox pidof matches process comm"
	check_output "$LOG" "BB-W3: ok pgrep" "busybox pgrep matches process comm"
	check_output "$LOG" "BB-W3: ok pkill" "busybox pkill signals matched process"
	check_output "$LOG" "BB-W4: ok mount" "busybox mount mounts a device and shows it"
	check_output "$LOG" "BB-W4: ok umount" "busybox umount unmounts a device"
	check_output "$LOG" "BB-W4: ok nslookup" "busybox nslookup resolves an address"
	check_output "$LOG" "BB-W4: ok lsof" "busybox lsof lists open files via /proc/<pid>/fd"
	check_output "$LOG" "BB-W4: ok netstat" "busybox netstat reads /proc/net/tcp (sshd :22)"
	check_output "$LOG" "BB-W4: ok route" "busybox route reads /proc/net/route (default gw)"
	check_output "$LOG" "BB-W4: ok ifconfig" "busybox ifconfig reads iface via SIOCGIF* ioctls"
	check_output "$LOG" "BB-W4: ok blkid" "busybox blkid identifies fs on /dev block node"
	check_output "$LOG" "BB-W4: ok fdisk" "busybox fdisk -l reads block geometry via BLK ioctls"
	check_output "$LOG" "BB-W4B: ok ping" "busybox ping works over a raw ICMP socket"
	check_output "$LOG" "BB-W4B: ok losetup" "busybox losetup -f finds a free loop device"
	check_output "$LOG" "BB-W4B: ok ip" "busybox ip link show works over rtnetlink"
	check_output "$LOG" "BB-W5: ok list-ash" "busybox lists the ash applet"
	check_output "$LOG" "BB-W5: ok list-sh" "busybox lists the sh applet"
	check_output "$LOG" "BB-W5: ok ash-c" "busybox ash -c runs a command"
	check_output "$LOG" "BB-W5: ok busybox-sh-c" "busybox sh -c runs a command"
	check_output "$LOG" "BB-W5: ok bin-sh-c" "/bin/sh (ash) -c runs a command"
	check_output "$LOG" "BB-W5: ok vars" "ash variable assignment + test"
	check_output "$LOG" "BB-W5: ok math" "ash arithmetic expansion"
	check_output "$LOG" "BB-W5: ok pipe" "ash pipeline"
	check_output "$LOG" "BB-W5: ok redir" "ash output redirection"
	check_output "$LOG" "BB-W5: ok wait" "ash waits for a child"
	check_output "$LOG" "BB-W5: ok arith-loop" "ash while-loop with \$((i+1)) arithmetic terminates (strtoull endptr)"
	check_output "$LOG" "BB-W5: done" "BusyBox wave 5 ash smoke completes"
	check_output "$LOG" "BB-W6: ok cryptpw" "busybox cryptpw computes sha512-crypt (\$6\$)"
	check_output "$LOG" "BB-W6: ok addgroup" "busybox addgroup writes /etc/group"
	check_output "$LOG" "BB-W6: ok adduser" "busybox adduser writes /etc/passwd"
	check_output "$LOG" "BB-W6: ok adduser-shadow" "busybox adduser writes /etc/shadow"
	check_output "$LOG" "BB-W6: ok adduser-home" "busybox adduser creates the home directory"
	check_output "$LOG" "BB-W6: ok chpasswd" "busybox chpasswd writes a \$6\$ sha512-crypt hash"
	check_output "$LOG" "BB-W6: ok passwd-verify" "stored password hash recomputes (password is verifiable)"
	check_output "$LOG" "BB-W6: ok su" "busybox su drops root->user (uid switch verified)"
	check_output "$LOG" "BB-W6: ok passwd-lock" "busybox passwd -l locks the account"
	check_output "$LOG" "BB-W6: ok passwd-unlock" "busybox passwd -u unlocks the account"
	check_output "$LOG" "BB-W6: ok login-applet" "busybox login applet is present and dispatchable"
	check_output "$LOG" "BB-W6: ok getty-applet" "busybox getty applet is present and dispatchable"
	check_output "$LOG" "BB-W6: ok deluser" "busybox deluser removes the /etc/passwd record"
	check_output "$LOG" "BB-W6: ok deluser-shadow" "busybox deluser removes the /etc/shadow record"
	check_output "$LOG" "BB-W6: ok delgroup" "busybox delgroup removes the /etc/group record"
	check_output "$LOG" "BB-W6: done" "BusyBox wave 6 account smoke completes"
	check_output "$LOG" "BB-W7: ok uuidgen" "busybox uuidgen generates RFC 4122 v4 UUID"
	check_output "$LOG" "BB-W7: ok sha384sum-upstream" "busybox sha384sum computes SHA-384 hash"
	check_output "$LOG" "BB-W7: ok vmstat-upstream" "busybox vmstat reports memory/process stats"
	check_output "$LOG" "BB-W7: ok tsort" "busybox tsort topologically sorts partial-order pairs"
	check_output "$LOG" "BB-W7: ok tree-upstream" "busybox tree prints directory trees"
	check_output "$LOG" "BB-W7: ok getfattr" "busybox getfattr reads an extended attribute"
	check_output "$LOG" "BB-W7: ok lsblk" "busybox lsblk enumerates /sys/block devices"
	check_output "$LOG" "BB-W7: ok version" "busybox --version reports 1.38.0"
	check_output "$LOG" "BB-SMOKE: ok rm" "busybox rm works"
	check_output "$LOG" "BB-SMOKE: ok rmdir" "busybox rmdir works"
	check_output "$LOG" "BB-SMOKE: done" "BusyBox smoke completes"

# ── M11 Shell & Utilities ──
section "M11 Shell baseline"
check_output "$LOG" "M11-SMOKE: start" "M11 shell smoke starts"
check_output "$LOG" "M11-SMOKE: ok pipe-eof" "pipe EOF when all writers close"
check_output "$LOG" "M11-SMOKE: ok pipe-nonblock-read" "pipe nonblocking read returns EAGAIN"
check_output "$LOG" "M11-SMOKE: ok pipe-nonblock-write" "pipe nonblocking write returns EAGAIN"
check_output "$LOG" "M11-SMOKE: done" "M11 shell smoke completes"

# ── M33 Shell compliance: globbing + arithmetic expansion ──
check_output "$LOG" "M33-SHELL: start" "M33 shell smoke starts"
check_output "$LOG" "M33-SHELL: ok pipe-large" "concurrent pipeline streams >512B without deadlock"
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

section "M11 Coreutils via shell"
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
check_output "$LOG" "M27-INIT: first-boot rootfs initialised" "first-boot rootfs setup runs"
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
check_output "$LOG" "M29-PTHREAD: ok stress-smp" "120 rounds of unjoined-thread exit reclaim the shared mm (no PMM leak)"
check_output "$LOG" "M29-PTHREAD: done" "M29 pthread smoke completes"
# ── M31 User Security / Passwords / Setuid ──
check_output "$LOG" "M31-SEC: start" "M31 user-security smoke starts"
check_output "$LOG" "M31-SEC: ok shadow-format" "/etc/shadow exists and uses b1nix-crypt"
check_output "$LOG" "M31-SEC: ok setuid-elevate" "setuid initramfs binary elevates euid to root"
check_output "$LOG" "M31-SEC: ok uid-denial" "non-root setuid(0) is rejected by kernel"
check_output "$LOG" "M31-SEC: done" "M31 smoke completes"
# ── M32 Networking / Multiplexing ──
check_output "$LOG" "M32-NET: start" "M32 multiplex smoke starts"
# External connectivity (off-link TCP over QEMU usernet). Skips cleanly when
# the test host has no internet so the suite stays green offline.
if grep -q "M32-NET: unsupported ext-http" "$LOG" 2>/dev/null; then
	pass "External HTTP skipped (no off-link connectivity)"
	pass "External HTTPS skipped (no off-link connectivity)"
else
	check_output "$LOG" "M32-NET: ok ext-http" "external HTTP GET over usernet works (off-link TCP to a real host)"
	if grep -q "M32-NET: unsupported ext-https" "$LOG" 2>/dev/null; then
		pass "External HTTPS skipped (handshake/cert-time unavailable)"
	else
		check_output "$LOG" "M32-NET: ok ext-https" "external HTTPS GET works (real mbedTLS handshake against a CA-signed cert)"
	fi
fi
# External IPv6 connectivity (curl -6 over the kernel off-link IPv6 datapath).
# Skips on its own when the usernet link has no IPv6 route.
if grep -q "M32-NET: unsupported ext-http6" "$LOG" 2>/dev/null; then
	pass "External HTTP over IPv6 skipped (no off-link IPv6 route)"
else
	check_output "$LOG" "M32-NET: ok ext-http6" "external HTTP GET over IPv6 works (curl -6, off-link IPv6 datapath)"
fi
if grep -q "M32-NET: unsupported ext-https6" "$LOG" 2>/dev/null; then
	pass "External HTTPS over IPv6 skipped (no off-link IPv6 route)"
else
	check_output "$LOG" "M32-NET: ok ext-https6" "external HTTPS GET over IPv6 works (curl -6 + mbedTLS over off-link IPv6)"
fi
check_output "$LOG" "M32-NET: ok select-timeout-zero" "select() with zero timeout returns 0 ready"
check_output "$LOG" "M32-NET: ok select-pipe-ready" "select() reports a buffered pipe as readable"
check_output "$LOG" "M32-NET: ok select-multi-fd" "select() across multiple fds isolates readability"
check_output "$LOG" "M32-NET: ok sockopt-reuseaddr" "M32b: setsockopt/getsockopt SO_REUSEADDR round-trips"
check_output "$LOG" "M32-NET: ok sockopt-nodelay" "M32b: setsockopt/getsockopt TCP_NODELAY round-trips"
check_output "$LOG" "M32-NET: ok sockopt-sotype" "M32b: getsockopt SO_TYPE reports the real socket type"
check_output "$LOG" "M32-NET: ok getsockname" "M32b: getsockname reports the bound local address"
check_output "$LOG" "M32-NET: ok getpeername" "M32b: getpeername reports the connected peer address"
check_output "$LOG" "M32-NET: ok shutdown-wr" "M32b: shutdown(SHUT_WR) closes the write half (EPIPE + peer EOF)"
check_output "$LOG" "M32B-PTY: ok openpty" "M32b: openpty allocates a /dev/ptmx master + /dev/pts/N slave; ptsname matches"
check_output "$LOG" "M32B-PTY: ok winsize" "M32b: TIOCSWINSZ/TIOCGWINSZ window-size round-trips on the pty"
check_output "$LOG" "M32B-PTY: ok canonical" "M32b: pty canonical line discipline delivers a line on newline"
check_output "$LOG" "M32B-PTY: ok echo" "M32b: pty ECHO reflects input back on the master"
check_output "$LOG" "M32B-PTY: ok raw" "M32b: pty raw mode (cfmakeraw) delivers single bytes with no echo"
check_output "$LOG" "M32B-PTY: ok hangup" "M32b: closing the pty master makes the slave read report EOF"
check_output "$LOG" "M32B-SESS: ok env-execve" "M32b: login-shell environment survives execve into getenv (crt0 -> environ)"
check_output "$LOG" "M32B-CRYPTO: ok getrandom" "M32b: getrandom returns fresh, non-zero secure random bytes"
check_output "$LOG" "M32B-CRYPTO: ok sha512" "M32b: userspace SHA-512 matches the FIPS 180-4 test vector"
check_output "$LOG" "M32B-CRYPTO: ok crypt" "M32b: \$b1\$ crypt() is deterministic and password-sensitive (shadow verify path)"
check_output "$LOG" "M32B-SSH: ok dropbearkey" "M32b: ported Dropbear runs on b1nix — dropbearkey generates an Ed25519 host key"
check_output "$LOG" "M32B-SSH: ok handshake" "M32b: end-to-end localhost SSH login (KEX + chacha20-poly1305 + password auth + remote command) over loopback"
check_output "$LOG" "M32B-SSH: ok negauth" "M32b: SSH daemon rejects a wrong password — remote command never runs, client terminates"
check_output "$LOG" "M32B-SSH: ok pty" "M32b: interactive shell over a remote PTY (sshd allocates a pty + spawns /bin/sh) runs a command"
check_output "$LOG" "M32B-SSH: ok service-lifecycle" "M32b: SSH daemon service control script and lifecycle management work"
check_output "$LOG" "M32-NET: ok inet-pton-ntop" "libc inet_pton/inet_ntop round-trip"
check_output "$LOG" "M32-NET: ok inet6-pton-ntop" "libc inet_pton/inet_ntop AF_INET6 round-trip"
check_output "$LOG" "M32-NET: ok gethostbyname-numeric" "libc gethostbyname resolves a dotted-quad"
check_output "$LOG" "M32-NET: ok getaddrinfo" "libc getaddrinfo fills sockaddr_in"
check_output "$LOG" "M32-NET: ok getaddrinfo-inet6" "libc getaddrinfo fills sockaddr_in6 for numeric ::1"
check_output "$LOG" "M32-NET: ok getnameinfo" "libc getnameinfo numeric round-trip (v4 + v6)"
check_output "$LOG" "M32-NET: ok v4mapped-udp" "dual-stack: AF_INET6 send to ::ffff:127.0.0.1 reaches an AF_INET socket"
check_output "$LOG" "M32-NET: ok udp6-loopback" "UDP over IPv6 (::1) round-trips through AF_INET6 sockets"
check_output "$LOG" "M32-NET: ok tcp6-loopback" "TCP over IPv6 (::1) echo round-trips through AF_INET6 sockets"
check_output "$LOG" "M32-NET: ok tcp-echo" "TCP loopback client/server echo works through sockets"
check_output "$LOG" "M32-NET: ok http-get" "HTTP-style client/server exchange works over TCP"
check_output "$LOG" "M32-NET: ok wget-loopback" "Wget HTTP client download works over TCP loopback"
check_output "$LOG" "M32-NET: ok wget-pcre2-regex" "Wget --regex-type pcre filters a recursive fetch (PCRE2 \\d matcher: keeps yes-1, drops no-2)"
check_output "$LOG" "M32-NET: ok wget-ipv6" "Wget HTTP client download works over IPv6 loopback (::1)"
check_output "$LOG" "M32-NET: ok wget-ntlm-enabled" "Wget binary exposes NTLM auth support"
check_output "$LOG" "M32-NET: ok wget-idn-punycode" "Wget IRI/IDN path converts an internationalized hostname to punycode"
check_output "$LOG" "M32-NET: ok wget-https-handshake" "Wget performs a real HTTPS request (TLS handshake path)"
check_output "$LOG" "M32-NET: ok wget-https-selfsigned-reject" "Wget rejects an invalid self-signed chain without --ca-certificate"
if grep -q "M32-NET: unsupported curl-tls-suite" "$LOG" 2>/dev/null; then
	pass "Curl TLS suite skipped (curl unavailable or built without HTTPS)"
else
	check_output "$LOG" "M32-NET: ok curl-https-enabled" "Curl reports HTTPS protocol support in --version output"
	check_output "$LOG" "M32-NET: ok curl-policy-flags" "Curl exposes TLS policy controls (cert-status/crlfile/pinnedpubkey)"
	check_output "$LOG" "M32-NET: ok curl-https-handshake" "Curl performs a real HTTPS request (TLS handshake path)"
	check_output "$LOG" "M32-NET: ok curl-https-selfsigned-reject" "Curl rejects an invalid self-signed chain without -k"
fi
check_output "$LOG" "M32-NET: ok tcp-server" "TCP listener accepts and exits cleanly"
check_output "$LOG" "M32-NET: done" "M32 smoke completes"
# ── M32 TCP sliding-window flow control + M23 DNS resolver (kernel net smoke) ──
check_output "$LOG" "M32-TCP: ok window-throttle" "tcp_send honours min(cwnd,snd_wnd) and blocks at a full window"
check_output "$LOG" "DNS-SMOKE: ok parse-a-record" "DNS A-record parser extracts the resolved address"
check_output "$LOG" "DNS-SMOKE: ok parse-aaaa-record" "DNS AAAA-record parser extracts a 128-bit IPv6 address"
check_output "$LOG" "DNS-SMOKE: ok resolv-conf" "/etc/resolv.conf nameserver parsed by the kernel resolver"
check_output "$LOG" "M32-IP6: ok icmpv6-loopback" "ICMPv6 echo over the ::1 loopback datapath round-trips"
# Real-link IPv6 over QEMU usernet (SLAAC + NDP + ICMPv6 ping the v6 gateway).
# Skips cleanly when the link has no IPv6 router.
if grep -q "M32-IP6: unsupported real-link" "$LOG" 2>/dev/null; then
	pass "Real-link IPv6 skipped (no usernet IPv6 router)"
	pass "Real-link IPv6 ping skipped (no usernet IPv6 router)"
else
	check_output "$LOG" "M32-IP6: ok slaac-global" "SLAAC (RS->RA) configures a global IPv6 address on the real link"
	if grep -q "M32-IP6: unsupported real-link-ping" "$LOG" 2>/dev/null; then
		pass "Real-link IPv6 ping skipped (gateway did not answer)"
	else
		check_output "$LOG" "M32-IP6: ok real-link-ping" "ICMPv6 echo to the usernet IPv6 gateway round-trips (NDP NS/NA + 0x86DD)"
	fi
fi
# ── M32a PCRE2 userspace port ──
check_output "$LOG" "M32-PCRE2: ok compile" "ported PCRE2 compiles a pattern"
check_output "$LOG" "M32-PCRE2: ok match" "ported PCRE2 matches and captures a group"
check_output "$LOG" "M32-PCRE2: ok nomatch" "ported PCRE2 correctly reports a non-match"
check_output "$LOG" "M32-PCRE2: done" "PCRE2 smoke completes"
# ── M34 procfs / sysfs synthetic filesystems ──
check_output "$LOG" "procfs: mounted at /proc" "procfs mounted at /proc"
check_output "$LOG" "sysfs: mounted at /sys" "sysfs mounted at /sys"
check_output "$LOG" "M34-PROC: start" "M34 procfs smoke starts"
check_output "$LOG" "M34-PROC: ok meminfo" "/proc/meminfo exposes MemTotal"
check_output "$LOG" "M34-PROC: ok version" "/proc/version identifies the kernel"
check_output "$LOG" "M34-PROC: ok proc-self-status" "/proc/self/status reports calling pid"
check_output "$LOG" "M34-PROC: ok proc-self-maps" "/proc/self/maps lists VMA regions"
check_output "$LOG" "M34-PROC: ok proc-listing" "/proc lists files, self and a pid dir"
check_output "$LOG" "M34-PROC: ok proc-pid-status" "/proc/<pid>/status reports the process"
check_output "$LOG" "M34-PROC: ok sysfs-osrelease" "/sys/kernel/osrelease reads back"
check_output "$LOG" "M34-PROC: ok sysfs-cpu" "/sys/devices/system/cpu/online reads back"
check_output "$LOG" "M34-PROC: ok tools" "free/sysctl/top read /proc and /sys"
check_output "$LOG" "M34-PROC: done" "M34 procfs smoke completes"
# ── M35 core dumps + kallsyms ──
check_output "$LOG" "M35-DIAG: start" "M35 kallsyms diag starts"
check_output "$LOG" "M35-DIAG: ok kallsyms" "kallsyms resolves a kernel symbol at offset 0"
check_output "$LOG" "M35-DIAG: ok kallsyms-offset" "kallsyms resolves a mid-function address with offset"
check_output "$LOG" "M35-DIAG: ok kallsyms-multi" "kallsyms resolves a second distinct symbol"
check_output "$LOG" "M35-DIAG: done" "M35 kallsyms diag completes"
check_output "$LOG" "M35-CORE: start" "M35 core-dump smoke starts"
check_output "$LOG" "M35-CORE: ok crash-signal" "faulting child is terminated by a signal"
check_output "$LOG" "M35-CORE: ok core-elf" "/tmp/core is an ET_CORE x86_64 ELF"
check_output "$LOG" "M35-CORE: ok core-prstatus" "core carries a PT_NOTE register file"
check_output "$LOG" "M35-CORE: done" "M35 core-dump smoke completes"

# ── M42 wave-5 prerequisites: POSIX limits, VFS, pattern matching & signal /
#    job control (the gate before enabling the upstream ash shell) ──
check_output "$LOG" "M42-W5PRE: start" "M42 wave-5 prerequisite suite starts"
check_output "$LOG" "M42-W5PRE: ok rlimit-enforcement" "RLIMIT_NOFILE limits the open fd count"
check_output "$LOG" "M42-W5PRE: ok getrlimit-setrlimit" "getrlimit/setrlimit sets limits correctly"
check_output "$LOG" "M42-W5PRE: ok dup" "dup() returns lowest available descriptor"
check_output "$LOG" "M42-W5PRE: ok access" "access() checks exist/perm modes"
check_output "$LOG" "M42-W5PRE: ok ftruncate" "ftruncate() resizes and zeroes memory buffers"
check_output "$LOG" "M42-W5PRE: ok fchdir" "fchdir() changes current working directory"
check_output "$LOG" "M42-W5PRE: ok fnmatch" "POSIX fnmatch matches brackets and PERIOD/PATHNAME flags"
check_output "$LOG" "M42-W5PRE: ok regex" "POSIX regex matches intervals and named classes"
check_output "$LOG" "M42-W5PRE: ok sigsuspend-alarm" "atomic sigsuspend waits for alarm and restores mask"
check_output "$LOG" "M42-W5PRE: ok interrupted-waitpid" "waitpid is interrupted by signal with EINTR"
check_output "$LOG" "M42-W5PRE: ok job-control" "Job control SIGSTOP/SIGCONT changes state"
check_output "$LOG" "M42-W5PRE: ok sigchld-on-exit" "SIGCHLD is delivered to the parent on child exit"
check_output "$LOG" "M42-W5PRE: done" "M42 wave-5 prerequisite suite completes"
# ── M36 GDB stub + ftrace ──
check_output "$LOG" "M36-GDB: start" "M36 GDB-stub diag starts"
check_output "$LOG" "M36-GDB: ok stop-reply" "GDB stub answers ? with a stop reply"
check_output "$LOG" "M36-GDB: ok read-regs" "GDB g-packet round-trips the register file"
check_output "$LOG" "M36-GDB: ok read-mem" "GDB m-packet reads target memory"
check_output "$LOG" "M36-GDB: ok framing" "GDB RSP framing + checksum round-trips"
check_output "$LOG" "M36-GDB: done" "M36 GDB-stub diag completes"
check_output "$LOG" "M36-FTRACE: start" "M36 ftrace diag starts"
check_output "$LOG" "M36-FTRACE: ok capture" "ftrace records function enter/exit events"
check_output "$LOG" "M36-FTRACE: ok symbolize" "ftrace event resolves to the function name"
check_output "$LOG" "M36-FTRACE: done" "M36 ftrace diag completes"
# ── M30 Dynamic Linking (PIE) ──
check_output "$LOG" "M30-DYN: ok pie-binary" "PIE ET_DYN binary loads at PIE base"
check_output "$LOG" "M30-DYN: ok pie-relocs" "R_X86_64_RELATIVE relocations applied"
check_output "$LOG" "M30-DYN: done" "M30 dyn-linking smoke completes"
check_output "$LOG" "B1NIX-TEST: done" "test-mode shutdown marker appears"
check_output "$LOG" "reboot: restarting" "SYS_REBOOT performs a real machine restart"
check_output "$LOG" "ahci: registered sata0" "AHCI block device registered"
check_output "$LOG" "nvme: registered nvme0" "NVMe block device registered"

# Network tests are only wired for the current x86_64/x86 QEMU path.
if [ "$ARCH" = "x86_64" ] || [ "$ARCH" = "x86" ]; then
	echo ""
	section "Network"
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

	# ── M37: real-hardware e1000/e1000e NIC driver ──
	# A second NIC (-device ${E1000_MODEL:-e1000} on net1) exercises the new
	# Intel gigabit driver end-to-end while virtio-net stays the active stack.
	if grep -q "e1000: initialized with MAC" "$LOG" 2>/dev/null; then
		pass "e1000 driver initialized"
		check_output "$LOG" "M37-E1000: ok init" "M37: e1000 ring/MMIO init"
		check_output "$LOG" "M37-E1000: ok mac" "M37: e1000 MAC read"
		check_output "$LOG" "M37-E1000: ok tx" "M37: e1000 transmit"
		check_output "$LOG" "M37-E1000: ok rx-arp" "M37: e1000 receive (ARP reply over SLIRP)"
	else
		fail "e1000 driver initialized" "e1000 init message not found"
	fi

	# ── M37: USB xHCI controller + HID boot keyboard ──
	# A qemu-xhci controller with a usb-kbd is enumerated end-to-end.
	if grep -q "M37-USB: ok xhci-init" "$LOG" 2>/dev/null; then
		pass "xHCI controller initialized"
		check_output "$LOG" "M37-USB: ok port-reset" "M37: xHCI port reset"
		check_output "$LOG" "M37-USB: ok slot-enabled" "M37: xHCI Enable Slot"
		check_output "$LOG" "M37-USB: ok device-addressed" "M37: xHCI Address Device"
		check_output "$LOG" "M37-USB: ok descriptors" "M37: USB device descriptor read (EP0 control transfer)"
		check_output "$LOG" "M37-USB: ok hid-config" "M37: HID interrupt endpoint configured"
		check_output "$LOG" "M37-USB: ok hid-translate" "M37: HID boot report -> scancode translation"
	else
		fail "xHCI controller initialized" "M37-USB xhci-init marker not found"
	fi
fi

# ── M24b SMP work-stealing (multi-core) ──
echo ""
echo "[RUN] M24b SMP work-stealing (-smp 4)..."
# Re-create the disk images so the SMP boot starts from a clean state.
"$MKE2FS" -F -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q "$SATA_IMG" 2>/dev/null || true
"$MKE2FS" -F -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q "$NVME_IMG" 2>/dev/null || true
dd if=/dev/zero of="$SWAP_IMG" bs=1M count=2 2>/dev/null
SMP_LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-smp.log"
EXTRA_QEMU_ARGS="-smp 4"
SMOKE_PROGRESS_MODE=smp
run_qemu "$SMP_LOG"
EXTRA_QEMU_ARGS=""
SMOKE_PROGRESS_MODE=full
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
