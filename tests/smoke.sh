#!/bin/sh
# B1NIX Smoke Test Suite (M24)
# Runs kernel in QEMU and checks for expected output patterns.
# Usage: ./tests/smoke.sh [x86_64]

set -e

ARCH="${1:-x86_64}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

OS="$(uname -s)"
if [ "$OS" = "Darwin" ]; then
	NPROC=$(sysctl -n hw.ncpu)
else
	NPROC=$(nproc)
fi

if [ "$ARCH" = "x86" ]; then
	ARCH_LABEL="x86   "
else
	ARCH_LABEL="$ARCH"
fi

echo() {
	command echo "[$ARCH_LABEL] $*"
}

printf() {
	local fmt="$1"
	shift
	command printf "[$ARCH_LABEL]$fmt" "$@"
}
# Seconds to let each test run (override via env). Detect hardware acceleration
# (KVM on Linux, HVF on macOS) — TCG pure-emulation is ~10-20x slower and needs
# a much larger budget to avoid false timeouts.
_HAVE_ACCEL=0
if [ "$ARCH" = "x86_64" ] || [ "$ARCH" = "x86" ]; then
	if [ -w /dev/kvm ] 2>/dev/null && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw kvm; then
		_HAVE_ACCEL=1
	elif [ "$(uname)" = "Darwin" ] && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw hvf; then
		_HAVE_ACCEL=1
	fi
fi
if [ "$ARCH" = "x86_64" ]; then
	if [ "$_HAVE_ACCEL" = "1" ]; then
		DEFAULT_TIMEOUT=600
	else
		DEFAULT_TIMEOUT=1800
	fi
else
	DEFAULT_TIMEOUT=480
fi
TIMEOUT=${TIMEOUT:-$DEFAULT_TIMEOUT}
SMOKE_VERBOSE=${SMOKE_VERBOSE:-0}
SMOKE_QUICK=${SMOKE_QUICK:-0}
SMOKE_LEGACY=${SMOKE_LEGACY:-0}
SMOKE_PARALLEL=1
if [ "$SMOKE_QUICK" = "1" ] || [ "$SMOKE_LEGACY" = "1" ]; then
	SMOKE_PARALLEL=0
fi
SMOKE_PROGRESS_MODE=full

# Optional V8/d8 instance. d8 (13 MB x86_64 ELF) and its ext4 disk are pre-built
# artifacts that `make` cannot reproduce from source (they need a multi-GB V8
# checkout + manual build), so the v8 instance auto-enables ONLY when those
# artifacts are present and ARCH=x86_64 — and skips honestly otherwise. It boots
# a dedicated b1nix-v8.iso (b1nix.v8run, no test rc) with the d8 disk as sata0 and
# checks the result-gated M58-V8 markers. Force-off with SMOKE_V8=0.
V8_DISK_SRC="$PROJECT_DIR/build/v8-out/v8-ext4.img"
SMOKE_V8=${SMOKE_V8:-auto}
if [ "$SMOKE_V8" = "auto" ]; then
	if [ "$ARCH" = "x86_64" ] && [ "$SMOKE_PARALLEL" = "1" ] && [ -f "$V8_DISK_SRC" ]; then
		SMOKE_V8=1
	else
		SMOKE_V8=0
	fi
fi

mkdir -p "$PROJECT_DIR/smoke_run"

# Pause the KDE file indexer (baloo) for the duration of the run: under parallel
# QEMU it competes for host CPU and is a documented source of smoke flakiness
# (timeouts/spurious fails). Best-effort and guarded — a no-op where baloo isn't
# installed. Resumes on exit (incl. interrupt). Set SMOKE_NO_BALOO=1 to skip.
BALOOCTL="$(command -v balooctl6 2>/dev/null || command -v balooctl 2>/dev/null || true)"
if [ "${SMOKE_NO_BALOO:-0}" != "1" ] && [ -n "$BALOOCTL" ]; then
	"$BALOOCTL" suspend >/dev/null 2>&1 || true
	trap '"$BALOOCTL" resume >/dev/null 2>&1 || true' EXIT INT TERM
fi

# Disk image path helper: disk_img <sata|nvme|swap> <instance-name>
disk_img() { command echo "$PROJECT_DIR/smoke_run/$1-smoke-$2-$$.img"; }
V8_LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-v8-$ARCH.log"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASSED=0
FAILED=0
SKIPPED=0
BLOCKED=0

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

# A marker that is missing because the instance that would have printed it died
# early (panic, watchdog kill, host stall) is NOT a failure of the thing being
# checked — it never ran. Reporting hundreds of those as FAIL buries the one
# real defect that wedged the instance. Count them separately as BLOCKED. This
# is bookkeeping only: BLOCKED still makes the suite exit non-zero.
blocked() {
	printf "  ${YELLOW}BLOCKED${NC} %s - %s\n" "$1" "$2"
	BLOCKED=$((BLOCKED + 1))
}

# An instance is "wedged" when its log never reached "B1NIX-TEST: done" (the
# marker PID 1 prints last). Memoised: check_output runs ~900 times and these
# logs are megabytes.
WEDGE_SCANNED=""
WEDGE_LOGS=""
log_wedged() {
	_wl="$1"
	case " $WEDGE_SCANNED " in
	*" $_wl "*) ;;
	*)
		WEDGE_SCANNED="$WEDGE_SCANNED $_wl"
		if [ ! -f "$_wl" ] || ! grep -qa "B1NIX-TEST: done" "$_wl" 2>/dev/null; then
			WEDGE_LOGS="$WEDGE_LOGS $_wl"
		fi
		;;
	esac
	case " $WEDGE_LOGS " in
	*" $_wl "*) return 0 ;;
	esac
	return 1
}

# The concatenated $LOG mixes several instances, so a marker missing from it is
# unattributable: treat it as blocked if ANY contributing instance wedged.
any_instance_wedged() {
	for _l in "$SYS_LOG" "$BLK_LOG" "$POSIX_LOG" "$GFX_LOG"; do
		[ -f "$_l" ] || continue
		log_wedged "$_l" && return 0
	done
	return 1
}

# Prints where each wedged instance stopped — the actual thing to debug.
report_wedged_instances() {
	_any=0
	for _l in "$SYS_LOG" "$BLK_LOG" "$POSIX_LOG" "$GFX_LOG" "$SMP_LOG" "$V8_LOG"; do
		[ -f "$_l" ] || continue
		grep -qa "B1NIX-TEST: done" "$_l" 2>/dev/null && continue
		# SMP/V8 instances stop at their own done-pattern by design.
		case "$_l" in
		"$SMP_LOG") grep -qa "M24B-SMP: \(ok\|fail\|skip\)" "$_l" 2>/dev/null && continue ;;
		"$V8_LOG") grep -qa "M58-V8: done" "$_l" 2>/dev/null && continue ;;
		esac
		_any=1
		printf "  ${YELLOW}WEDGED${NC} %s\n" "$_l"
		printf "         last output: %s\n" "$(grep -a . "$_l" 2>/dev/null | tail -1 | cut -c1-140)"
	done
	[ "$_any" = "1" ] && printf "  (checks against a wedged instance are reported BLOCKED, not FAIL)\n"
	return 0
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
			b1nix\ kernel*|init\ spawn\ result:*|M[0-9]*:*|NATIVE-SMOKE:*|B1CC-*:*|POSIX-SMOKE:*|LOCK-SMOKE:*|EXT-STRESS:*|NET-SMOKE:*|UDP-SMOKE:*|POLL-SMOKE:*|TCP-SMOKE:*|DNS-SMOKE:*|BB-SMOKE:*|BB-W[0-9]*:*|BPKG-SMOKE:*|B1NIX-TEST:*|B1NIX-QUICK:*|*PANIC*) ;;
			*) return ;;
		esac
	fi

	case "$line" in
		*": FAIL"*|*": fail "*|*": failed"*|*PANIC*)
			printf "  ${PROGRESS_PREFIX:-}${RED}%s${NC}\n" "$line"
			;;
		*": ok"*|*": done"*|B1NIX-TEST:\ done|B1NIX-QUICK:\ done)
			printf "  ${PROGRESS_PREFIX:-}${GREEN}%s${NC}\n" "$line"
			;;
		*)
			printf "  ${PROGRESS_PREFIX:-}${YELLOW}%s${NC}\n" "$line"
			;;
	esac
}

# Run QEMU and capture output
run_qemu() {
	local log="$1"
	shift
	local pid
	local done_pattern="${SMOKE_DONE_PATTERN:-B1NIX-TEST: done|KERNEL PANIC|\[PANIC\]}"
  
	if [ "$ARCH" = "x86_64" ] || [ "$ARCH" = "x86" ]; then
		local filter_dump_args=""
		if [ "${SMOKE_PCAP:-0}" = "1" ] &&
		   qemu-system-x86_64 -object filter-dump,help >/dev/null 2>&1; then
			filter_dump_args="-object filter-dump,id=f0,netdev=net0,file=${NET_PCAP:-$PROJECT_DIR/smoke_run/net-$ARCH.pcap}"
		fi

		# Use KVM hardware acceleration when available (Linux /dev/kvm) or HVF on macOS.
		# The default QEMU accelerator is TCG (pure emulation), which is many times
		# slower — heavy software workloads like the Mesa softpipe demo never
		# finish a context within the timeout under TCG. Auto-detected so this is
		# a no-op on hosts without KVM/HVF, which fall back to TCG.
		local accel_args=""
		if [ -w /dev/kvm ] && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw kvm; then
			# -cpu host exposes the full host instruction set to the guest and
			# cuts KVM exits (vs the conservative default model) — a free speedup
			# with hardware virt, no extra VMs. Only with KVM/HVF, never TCG.
			accel_args="-accel kvm -cpu host"
		elif [ "$(uname)" = "Darwin" ] && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw hvf; then
			accel_args="-accel hvf -cpu host"
		fi

		# RAM: the historical default (no -m → 128 MiB) was enough until the real
		# Mesa softpipe demo (m52-osmesa) actually ran — it exhausts 127 MiB and
		# OOMs, starving later graphics tests (setcrtc, console-reclaim) and Mesa
		# context creation. Give the VM headroom. x86_64 only: the 32-bit port
		# caps usable RAM at 1 GiB, so keep it modest there.
		local mem_args="-m ${SMOKE_MEM_MB:-1024}"
		if [ "$ARCH" = "x86" ]; then
			mem_args="-m ${SMOKE_MEM_MB:-768}"
		fi
		# Ordinary smoke instances need a second vCPU so PID 1's per-child
		# watchdog can run even when a test child spins without yielding.  The
		# dedicated SMP instance supplies its own -smp 4 argument below.
		local cpu_args=""
		if [ "${SMOKE_FAST_SMP:-0}" != "1" ] && [ "${SMOKE_V8_MODE:-0}" != "1" ]; then
			cpu_args="-smp ${SMOKE_SMP:-2}"
		fi

		set -- qemu-system-x86_64 ${accel_args} ${mem_args} ${cpu_args} \
			-cdrom "$PROJECT_DIR/build/$ARCH/${B1NIX_ISO_NAME:-b1nix.iso}" \
			-serial stdio -display ${GPU_DISPLAY:-none} -monitor none -no-reboot \
			-device isa-debug-exit,iobase=0xf4,iosize=0x04

		if [ "${SMOKE_V8_MODE:-0}" = "1" ]; then
			# V8/d8 instance: the d8 binary is now embedded in the ISO as
			# Multiboot2 module (ram0), no separate sata0 disk needed.
			set -- "$@" -nic none -vga none \
				${EXTRA_QEMU_ARGS:-}
		elif [ "${SMOKE_FAST_SMP:-0}" != "1" ]; then
			# restrict=off by default: the NET-SMOKE ping-gateway and BusyBox
			# nslookup/ping checks exercise real ICMP/DNS to the SLIRP gateway
			# (10.0.2.2/10.0.2.3), which QEMU's user net categorically blocks under
			# restrict=on (guest fully isolated) — so those checks can only pass
			# with restrict=off. The b1nix net stack itself is fine (they pass here).
			# Set B1NIX_NET_RESTRICT=on for a hermetic, network-isolated run.
			set -- "$@" \
				-device ${GPU_DEVICE:-virtio-gpu-pci} \
				-netdev user,id=net0,restrict=${B1NIX_NET_RESTRICT:-off} -device virtio-net-pci,netdev=net0 \
				${filter_dump_args} \
				-netdev user,id=net1,restrict=${B1NIX_NET_RESTRICT:-off} -device ${E1000_MODEL:-e1000},netdev=net1 \
			-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 \
			-device virtio-tablet-pci,id=vtablet \
			-device virtio-tablet-pci,id=vtouch \
			-device intel-hda,id=hda -device hda-duplex,bus=hda.0 \
			-device ich9-ahci,id=ahci \
				-drive file="$SATA_IMG",if=none,id=satadrive,format=raw \
				-device ide-hd,drive=satadrive,bus=ahci.0 \
				-drive file="$SWAP_IMG",if=none,id=swapdrive,format=raw \
				-device ide-hd,drive=swapdrive,bus=ahci.1 \
				-drive file="$NVME_IMG",if=none,id=nvmedrive,format=raw \
				-device nvme,serial=deadbeef,drive=nvmedrive \
				${EXTRA_QEMU_ARGS:-}
		else
			set -- "$@" -nic none -vga none ${EXTRA_QEMU_ARGS:-}
		fi

		"$@" >"$log" 2>&1 &
		pid=$!

		# Wait for completion marker/panic or timeout, then kill QEMU.
		# Keep this POSIX-portable (macOS doesn't ship GNU timeout by default).
		(
			set +e
			start_ts=$(date +%s)
			reported_lines=0
			# Stall detector: a healthy b1nix boot streams markers continuously, so a
			# long gap with NO new serial output means the instance wedged (a hung
			# test, a host suspend, or KVM starvation under load) — far more common
			# here than a clean run that legitimately runs to the full TIMEOUT. Kill
			# it after STALL_TIMEOUT of silence instead of blocking the whole TIMEOUT
			# (which with a large TIMEOUT meant 8-25 min hangs). Generous default so a
			# slow-but-alive module (V8 GC, a big mmap) is not killed mid-work.
			last_progress_ts=$start_ts
			stall_after=${STALL_TIMEOUT:-120}
			while :; do
				line_count=$(wc -l <"$log" | tr -d ' ')
				if [ "$line_count" -gt "$reported_lines" ]; then
					sed -n "$((reported_lines + 1)),${line_count}p" "$log" |
						while IFS= read -r line; do
							report_progress_line "$line"
						done
					reported_lines=$line_count
					last_progress_ts=$(date +%s)
				fi
				if grep -qa -E "$done_pattern" "$log" 2>/dev/null; then
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
					# QEMU exiting on its own before the done marker is NOT the
					# same failure as a stall: with -no-reboot a guest triple
					# fault / reset makes QEMU quit silently, leaving a log that
					# simply stops. Say so, so the truncation is not mistaken for
					# a hung test.
					if ! grep -qa -E "$done_pattern" "$log" 2>/dev/null; then
						command echo "[smoke] QEMU exited before the done marker (guest reset/triple fault or external kill)" >>"$log"
						command echo "SMOKE-WATCHDOG: qemu-exited child=qemu log=$log" >>"$log"
					fi
					break
				fi
				now_ts=$(date +%s)
				if [ $((now_ts - start_ts)) -ge "$TIMEOUT" ]; then
					command echo "[smoke] run_qemu timeout after ${TIMEOUT}s" >>"$log"
					command echo "SMOKE-WATCHDOG: timeout child=qemu log=$log" >>"$log"
					break
				fi
				if [ $((now_ts - last_progress_ts)) -ge "$stall_after" ]; then
					command echo "[smoke] run_qemu STALLED — no serial output for ${stall_after}s (hung test / host suspend / KVM starvation); killing instance" >>"$log"
					command echo "SMOKE-WATCHDOG: stalled child=qemu log=$log" >>"$log"
					break
				fi
				sleep 1
			done
			kill -9 "$pid" 2>/dev/null || true
		) &
		local watcher_pid=$!

		wait "$watcher_pid" 2>/dev/null || true
		kill -9 "$pid" 2>/dev/null || true
		wait "$pid" 2>/dev/null || true
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
	elif { [ "$log" = "$LOG" ] && any_instance_wedged; } || log_wedged "$log"; then
		blocked "$desc" "instance died before this ran: $pattern"
	else
		fail "$desc" "missing expected output: $pattern"
	fi
}

# ── Build kernel first ──
echo "=== B1NIX Smoke Tests ($ARCH) ==="
echo ""

echo "[BUILD] Building kernel for $ARCH..."
cd "$PROJECT_DIR"
BUILD_LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-build-$ARCH.log"
print_build_failure() {
	echo "  ${RED}BUILD FAILED${NC}"
	echo "  build log: $BUILD_LOG"
	if [ -f "$BUILD_LOG" ]; then
		echo "  --- likely build errors ---"
		grep -aEn "fatal error:| error:|undefined reference|No such file|not found|cannot (find|open|create|compute)|FAILED:|make(\\[[0-9]+\\])?: \\*\\*\\*" "$BUILD_LOG" | tail -120 || true
		echo "  --- end likely build errors ---"
		echo "  --- build log tail ---"
		tail -80 "$BUILD_LOG"
		echo "  --- end build log tail ---"
	fi
}
# SKIP_BUILD=1 reuses an existing build/$ARCH/b1nix.iso (e.g. when the toolchain
# can't rebuild every userspace port locally). SMOKE_MAKE_ARGS lets the caller
# inject extra make flags (e.g. CC=clang LD=ld.lld on Fedora, where `cc` is gcc).
if [ "${SKIP_BUILD:-0}" = "1" ]; then
	if [ "$SMOKE_PARALLEL" = "1" ]; then
		test -f "build/$ARCH/b1nix-sys.iso" &&
		test -f "build/$ARCH/b1nix-blk.iso" &&
		test -f "build/$ARCH/b1nix-posix.iso" &&
		test -f "build/$ARCH/b1nix-gfx.iso" || {
			echo "  ${RED}no prebuilt split smoke ISOs${NC}"
			exit 1
		}
	else
		test -f "build/$ARCH/b1nix.iso" || { echo "  ${RED}no prebuilt build/$ARCH/b1nix.iso${NC}"; exit 1; }
	fi
	echo "  (SKIP_BUILD=1 — reusing prebuilt ISO)"
else
	QUICK_CMDLINE=""
	[ "$SMOKE_QUICK" = "1" ] && QUICK_CMDLINE="b1nix.smoke=quick"
	if [ "$SMOKE_PARALLEL" = "1" ]; then
		V8_ISO_TARGET=""
		[ "$SMOKE_V8" = "1" ] && V8_ISO_TARGET="iso-v8"
		make -j"$NPROC" ARCH="$ARCH" ${SMOKE_MAKE_ARGS:-} iso-sys iso-blk iso-posix iso-gfx iso-openrc $V8_ISO_TARGET >"$BUILD_LOG" 2>&1 || {
			print_build_failure
			exit 1
		}
	else
		make -j"$NPROC" ARCH="$ARCH" ${SMOKE_MAKE_ARGS:-} KERNEL_CMDLINE="init=/sbin/openrc-init b1nix.test=1 b1nix.kvtest=abc123 b1nix.ssh-loopback=1 $QUICK_CMDLINE" iso >"$BUILD_LOG" 2>&1 || {
			print_build_failure
			exit 1
		}
	fi
fi
pass "kernel builds without errors"
echo "  build/$ARCH/${B1NIX_ISO_NAME:-b1nix.iso} ready"
if [ ! -x "$MKE2FS" ]; then
    MKE2FS=$(command -v mke2fs 2>/dev/null || command echo "/sbin/mke2fs")
fi
if [ -z "$MKE2FS" ] || ! command -v "$MKE2FS" >/dev/null 2>&1; then
    echo "Error: mke2fs utility not found. Please install e2fsprogs."
    exit 1
fi
_mkimg() {  # mkimg <instance-suffix>
    _sata=$(disk_img sata "$1"); _nvme=$(disk_img nvme "$1"); _swap=$(disk_img swap "$1")
    dd if=/dev/zero of="$_sata" bs=1M count=4 2>/dev/null
    dd if=/dev/zero of="$_nvme" bs=1M count=4 2>/dev/null
    dd if=/dev/zero of="$_swap" bs=1M count=2 2>/dev/null
    "$MKE2FS" -F -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q "$_sata" 2>/dev/null || {
        "$MKE2FS" -F -t ext4 -q "$_sata" 2>/dev/null || {
            echo "Error: Failed to format sata $1 image as ext4."; exit 1
        }
    }
    "$MKE2FS" -F -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q "$_nvme" 2>/dev/null || {
        "$MKE2FS" -F -t ext4 -q "$_nvme" 2>/dev/null || {
            echo "Error: Failed to format nvme $1 image as ext4."; exit 1
        }
    }
}
_mkimg sys
[ "$SMOKE_PARALLEL" = "1" ] && {
    _mkimg blk; _mkimg posix; _mkimg gfx; _mkimg openrc
}

# Define logs
LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-$ARCH.log"
SMP_LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-smp-$ARCH.log"
SYS_LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-sys-$ARCH.log"
BLK_LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-blk-$ARCH.log"
POSIX_LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-posix-$ARCH.log"
GFX_LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-gfx-$ARCH.log"
OPENRC_LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-openrc-$ARCH.log"

echo ""
if [ "$SMOKE_PARALLEL" = "1" ]; then
	echo "[RUN] Booting sys, blk, posix, gfx, and SMP QEMU instances in parallel..."
else
	echo "[RUN] Booting both QEMU instances (Single-CPU and SMP) in parallel..."
fi

# Stagger (parallel mode): run the SMP (-smp 4) instance FIRST, alone. It is the
# heaviest (4 vCPUs) but the quickest to finish (SMOKE_FAST_SMP stops it at the
# M24B-SMP work-stealing marker), so giving it the whole 8-core host briefly and
# then reclaiming its 4 vCPUs keeps the long single-CPU instances that follow
# from starving. Launching all five at once oversubscribes the host (4 + 4 vCPUs
# + the GPU instance on 8 cores) and the core instance times out — its ~245
# markers then read as "missing" (spurious failures, not real regressions).
if [ "$SMOKE_PARALLEL" = "1" ] && { [ -z "${SMOKE_INSTANCES:-}" ] || echo " $SMOKE_INSTANCES " | grep -q " smp "; }; then
	(
		B1NIX_ISO_NAME=b1nix-sys.iso
		EXTRA_QEMU_ARGS="-smp 4"
		SMOKE_FAST_SMP=1
		# Stop on the USERSPACE AP proof, not the kernel work-stealing selftest:
		# /bin/m24b_smoke (which emits "M24B-BKL: instance ran-on-ap") runs from
		# init, long after the selftest marker, so cutting the instance at the
		# selftest made that check permanently unreachable (reported BLOCKED).
		SMOKE_DONE_PATTERN="M24B-SMP: ok work-stealing|KERNEL PANIC|\[PANIC\]"
		SMOKE_PROGRESS_MODE=smp
		PROGRESS_PREFIX="[smp]  "
		run_qemu "$SMP_LOG"
	) &
	pid_smp=$!
	wait $pid_smp
fi

# ── Post-SMP instances as launcher functions ──
# 4 categories: sys (kernel+ipc+elf+diag), blk (storage), posix (shell+coreutils),
# gfx (graphics). 3 slots run concurrently; when one finishes, the next starts
# immediately. gfx is last so it doesn't block faster instances.
launch_sys() {
	(
		SATA_IMG=$(disk_img sata sys)
		NVME_IMG=$(disk_img nvme sys)
		SWAP_IMG=$(disk_img swap sys)
		B1NIX_ISO_NAME=b1nix-sys.iso
		SMOKE_PROGRESS_MODE=full
		PROGRESS_PREFIX="[sys]   "
		run_qemu "$SYS_LOG"
	) &
	pid_sys=$!
}

launch_blk() {
	(
		SATA_IMG=$(disk_img sata blk)
		NVME_IMG=$(disk_img nvme blk)
		SWAP_IMG=$(disk_img swap blk)
		B1NIX_ISO_NAME=b1nix-blk.iso
		SMOKE_PROGRESS_MODE=full
		PROGRESS_PREFIX="[blk]   "
		run_qemu "$BLK_LOG"
	) &
	pid_blk=$!
}

launch_posix() {
	(
		SATA_IMG=$(disk_img sata posix)
		NVME_IMG=$(disk_img nvme posix)
		SWAP_IMG=$(disk_img swap posix)
		B1NIX_ISO_NAME=b1nix-posix.iso
		SMOKE_PROGRESS_MODE=full
		PROGRESS_PREFIX="[posix] "
		run_qemu "$POSIX_LOG"
	) &
	pid_posix=$!
}

launch_gfx() {
	(
		SATA_IMG=$(disk_img sata gfx)
		NVME_IMG=$(disk_img nvme gfx)
		SWAP_IMG=$(disk_img swap gfx)
		B1NIX_ISO_NAME=b1nix-gfx.iso
		# Skia (raster + Graphite/Dawn) plus a live compositor and the Mesa/
		# Cairo/HarfBuzz client tests do not fit in the default 1 GiB: the
		# instance OOM-kills displayd mid-run and then panics in kheap growth.
		SMOKE_MEM_MB=${SMOKE_MEM_MB:-1536}
		SMOKE_PROGRESS_MODE=full
		PROGRESS_PREFIX="[gfx]   "
		# Drive the VirGL 3D-accelerated GPU on the graphics instance when the
		# host QEMU + GPU support it (virtio-gpu-gl device + a DRM render node).
		# egl-headless routes virglrenderer to the host GL stack. On hosts without
		# it (e.g. macOS QEMU built without virglrenderer) we fall back to the
		# plain 2D device and the kernel's VirGL selftest is a no-op — the
		# software OpenGL path still runs and is verified.
		if qemu-system-x86_64 -device help 2>/dev/null | grep -q "name \"virtio-gpu-gl-pci\"" &&
		   qemu-system-x86_64 -display help 2>/dev/null | grep -qw egl-headless &&
		   { [ -e /dev/dri/renderD128 ] || [ -e /dev/dri/renderD129 ]; }; then
			GPU_DEVICE="virtio-gpu-gl-pci"
			GPU_DISPLAY="egl-headless"
			export GPU_DEVICE GPU_DISPLAY
		fi
		run_qemu "$GFX_LOG"
	) &
	pid_gfx=$!
}

# OpenRC as PID 1: no test orchestrator, the real init system drives the boot and
# then powers the machine off through its control FIFO (see iso-openrc). The
# instance ends by itself — "reboot: powering off" IS the pass condition.
launch_openrc() {
	(
		SATA_IMG=$(disk_img sata openrc)
		NVME_IMG=$(disk_img nvme openrc)
		SWAP_IMG=$(disk_img swap openrc)
		B1NIX_ISO_NAME=b1nix-openrc.iso
		SMOKE_DONE_PATTERN="reboot: powering off|KERNEL PANIC|\[PANIC\]"
		SMOKE_PROGRESS_MODE=full
		PROGRESS_PREFIX="[openrc]"
		run_qemu "$OPENRC_LOG"
	) &
	pid_openrc=$!
}

launch_v8() {
	(
		SATA_IMG=$(disk_img sata v8)
		SMOKE_V8_MODE=1
		B1NIX_ISO_NAME=b1nix-v8.iso
		SMOKE_MEM_MB=${SMOKE_MEM_MB:-2048}
		SMOKE_DONE_PATTERN="M58-V8: done|KERNEL PANIC|\[PANIC\]"
		SMOKE_PROGRESS_MODE=full
		PROGRESS_PREFIX="[v8]   "
		run_qemu "$V8_LOG"
	) &
	pid_v8=$!
}

launch_smp_solo() {
	(
		SATA_IMG=$(disk_img sata smp)
		NVME_IMG=$(disk_img nvme smp)
		SWAP_IMG=$(disk_img swap smp)
		NET_PCAP="$PROJECT_DIR/smoke_run/net-$ARCH-smp.pcap"
		EXTRA_QEMU_ARGS="-smp 4"
		SMOKE_FAST_SMP=1
		# Stop on the USERSPACE AP proof, not the kernel work-stealing selftest:
		# /bin/m24b_smoke (which emits "M24B-BKL: instance ran-on-ap") runs from
		# init, long after the selftest marker, so cutting the instance at the
		# selftest made that check permanently unreachable (reported BLOCKED).
		SMOKE_DONE_PATTERN="M24B-BKL: instance ran-on-ap|M24B-SMP: fail work-stealing|KERNEL PANIC|\[PANIC\]"
		SMOKE_PROGRESS_MODE=smp
		PROGRESS_PREFIX="[smp]  "
		run_qemu "$SMP_LOG"
	) &
	pid_smp=$!
}

# ── Dynamic slot pool: 3 concurrent, fill freed slots immediately ──
# Polls finished PIDs every second (POSIX-sh compatible) rather than waiting
# on full batches, so a fast instance exiting early makes room for the next one
# without blocking on the others.
run_slot_pool() {
	_max=$1; shift
	_pids="" _queue="" _idx=0
	for _n; do
		if [ "$_idx" -lt "$_max" ]; then
			"launch_$_n"
			_pids="$_pids $!"
			_idx=$((_idx + 1))
		else
			_queue="$_queue $_n"
		fi
	done
	for _n in $_queue; do
		_done=0
		while [ "$_done" = "0" ]; do
			for _p in $_pids; do
				if ! kill -0 "$_p" 2>/dev/null; then
					wait "$_p" 2>/dev/null || true
					_new=""
					for _pp in $_pids; do [ "$_pp" != "$_p" ] && _new="$_new $_pp"; done
					_pids=$_new
					_done=1
					break
				fi
			done
			[ "$_done" = "0" ] && sleep 1
		done
		"launch_$_n"
		_pids="$_pids $!"
	done
	for _p in $_pids; do wait "$_p" 2>/dev/null || true; done
}

if [ "$SMOKE_PARALLEL" = "1" ]; then
	SMOKE_MAX_CONCURRENT=${SMOKE_MAX_CONCURRENT:-3}
	echo "[RUN] post-SMP instances, $SMOKE_MAX_CONCURRENT at a time"
	_inst_list="sys blk posix gfx openrc"
	[ "$SMOKE_V8" = "1" ] && _inst_list="$_inst_list v8"
	[ -n "${SMOKE_INSTANCES:-}" ] && _inst_list="$SMOKE_INSTANCES"
	run_slot_pool $SMOKE_MAX_CONCURRENT $_inst_list
	cat "$SYS_LOG" "$BLK_LOG" "$POSIX_LOG" "$GFX_LOG" 2>/dev/null >"$LOG" || true
else
	launch_sys
	launch_smp_solo
	wait $pid_sys
	wait $pid_smp
fi

if [ "$SMOKE_QUICK" = "1" ]; then
	echo ""
	echo "=== Analyzing Quick Smoke Results ==="
	check_output "$LOG" "b1nix kernel" "kernel banner appears"
	check_output "$LOG" "B1NIX-QUICK: ok native" "native userspace smoke passes"
	check_output "$LOG" "B1NIX-QUICK: done" "quick smoke completes"
	check_output "$SMP_LOG" "M24B-BKL: instance ran-on-ap" "SMP work-stealing passes"
	if grep -q -E "KERNEL PANIC|\[PANIC\]" "$LOG" "$SMP_LOG" 2>/dev/null; then
		fail "quick smoke completes without panic" "PANIC detected in log"
	else
		pass "quick smoke completes without panic"
	fi
	echo ""
	echo "=== Results ==="
	echo "  Passed:  $PASSED"
	echo "  Failed:  $FAILED"
	for _i in sys blk posix gfx openrc smp v8; do
	    rm -f "$(disk_img sata "$_i")" "$(disk_img nvme "$_i")" "$(disk_img swap "$_i")"
	done
	[ "$FAILED" -eq 0 ]
	exit
fi

echo ""
echo "=== Analyzing Test Results ==="

# ── Test 1: Kernel boots ──
echo ""
echo "[RUN] Boot smoke checks..."
check_output "$LOG" "b1nix kernel" "kernel banner appears"
check_output "$LOG" "pmm:" "physical memory manager initializes"
check_output "$LOG" "kheap:" "kernel heap initializes"
# b1cc temporarily cut from the build (B1NIX_NO_B1CC) — restored as a separate
# change. These checks are disabled until b1cc is re-added.
# check_output "$LOG" "B1CC-R42-SMOKE: ok" "b1cc return_42 runs and exits with 42"
# check_output "$LOG" "B1CC-HELLO-SMOKE: ok" "b1cc hello runs and exits with 0"
# check_output "$LOG" "B1CC-ARGV-SMOKE: ok" "b1cc argv propagation works"
# check_output "$LOG" "B1CC-FILE-SMOKE: ok" "b1cc file write works"
# check_output "$LOG" "B1CC-STDERR-SMOKE: ok" "b1cc stderr exit status propagates"
# check_output "$LOG" "B1CC-BETTER-C-SMOKE: ok" "b1cc better C features work (M7)"
# check_output "$LOG" "B1CC-M34-SMOKE: ok" "b1cc M34 features run on x86_64-b1nix"
# check_output "$LOG" "B1CC-M34-TARGET: all ok" "b1cc M34 target corpus passes on-device"

# ── Test 2: No panic ──
if grep -q "KERNEL PANIC" "$LOG" 2>/dev/null; then
	fail "kernel boots without panic" "PANIC detected in log"
else
	pass "kernel boots without panic"
fi

# ── Test 3-7: Core boot path markers ──
check_output "$LOG" "initramfs: files" "initramfs initializes"
check_output "$LOG" "init: /sbin/openrc-init pid=" "openrc-init launches as PID 1"
check_output "$LOG" "M94-INIT:" "M94 init-path parsing self-test runs"
check_output "$LOG" "M94-INIT: ok \(default\|init=\|no-override-flags\)" "M94 init-path logic correct"
# Linking policy: the rootfs must not carry a statically linked executable that
# is not in tools/configs/static-allowlist.txt with a reason. Run the same gate
# the build uses, so a regression shows up as a failed check and not only as a
# build error someone might bypass.
if sh "$PROJECT_DIR/tools/check-dynamic.sh" "$PROJECT_DIR/build/$ARCH/rootfs" >/dev/null 2>&1; then
	pass "rootfs has no unexpected statically linked executables"
else
	fail "rootfs has no unexpected statically linked executables" "$(sh "$PROJECT_DIR/tools/check-dynamic.sh" "$PROJECT_DIR/build/$ARCH/rootfs" 2>&1 | head -3 | tr '\n' ' ')"
fi
check_output "$LOG" "M94-CTL: ok tmpfs-mount" "tmpfs mounts on a VFS directory (the /run an init system expects)"
check_output "$LOG" "M94-CTL: ok tmpfs-state" "state written through a dirfd inside the tmpfs is visible afterwards"
check_output "$LOG" "M94-CTL: ok fifo-on-tmpfs" "mkfifo works on a tmpfs mount"
check_output "$LOG" "M94-CTL: ok fifo-command" "a command written by another process is read back from the control FIFO"
# OpenRC instance: the real init system as PID 1, driving its own runlevels and
# shutting the machine down through /run/openrc/init.ctl.
# Note: "openrc-init runs as PID 1" is already checked in $LOG above.
check_output "$OPENRC_LOG" "Caching service dependencies" "OpenRC builds its dependency cache (popen/posix_spawn work)"
check_output "$OPENRC_LOG" "/etc/init.d/local start" "OpenRC reaches the default runlevel and starts services"
check_output "$OPENRC_LOG" "M94-OPENRC: ok init-fifo-present" "openrc-init creates its control FIFO once the boot finishes"
check_output "$OPENRC_LOG" "/etc/init.d/killprocs start" "the shutdown runlevel runs when PID 1 gets the command"
check_output "$OPENRC_LOG" "reboot: powering off" "openrc-shutdown powers the machine off through the control FIFO"
# M28 #9: ctx-switch + light-syscall rdtsc benchmark. It is single-CPU only by
# design (the rdtsc yield loop races under the SMP high-syscall-density path, see
# m28_ctxbench.c), so on the -smp 2/4 smoke runs it correctly reports "skip smp".
# Accept either the measurement or the deliberate skip.
check_output "$LOG" "M28-BENCH: \(ok\|skip smp\)" "M28 ctx-switch benchmark completes"

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
check_output "$LOG" "M14-SMOKE: ok ext4-fifo-persistence" "a FIFO created with mkfifo is a real ext4 inode and survives umount/mount"
check_output "$LOG" "M14-SMOKE: ok block-cache" "cached read and dirty write verified"
check_output "$LOG" "M14-SMOKE: ok persistence" "persistence through sync, umount, and remount verified"
check_output "$LOG" "M14-SMOKE: ok invalid-device" "mounting invalid device fails gracefully"
check_output "$LOG" "M14-SMOKE: ok invalid-fs" "mounting invalid filesystem type fails gracefully"
check_output "$LOG" "M14-SMOKE: ok stress-loop" "repeated create/write/read/delete loop completes successfully"
check_output "$LOG" "M14-SMOKE: ok large-file" "large file bounds allocation and verification successful"
check_output "$LOG" "M14-SMOKE: ok VFS-normalization" "VFS path normalization works on mounts"
check_output "$LOG" "M14-SMOKE: ok mmap-durable" "M72: a writable MAP_SHARED mmap store survives forced page-cache reclaim (drop_caches) and is read back from disk"
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
check_output "$LOG" "M15-SMOKE: ok shm-exit-cleanup" "shm attachment released on exit (IPC_RMID no longer blocked)"
check_output "$LOG" "M15-SMOKE: ok shm-kill-cleanup" "shm attachment released when a child is SIGKILL'd (OOM path) + fork nattch accounting"
check_output "$LOG" "M15-SMOKE: ok semaphore" "cooperative semaphore baseline works"
check_output "$LOG" "M15-SMOKE: ok clock-timer" "clock_gettime and nanosleep work"
check_output "$LOG" "M15-SMOKE: ok permissions-chmod" "chmod changes mode bits"
check_output "$LOG" "M15-SMOKE: ok permissions-enforcement" "permissions are enforced for non-root uid"
# ── M74: RT signals (POSIX real-time signals queue, do not coalesce) ──
check_output "$LOG" "M74-SMOKE: ok rt-queue" "M74: 3 blocked SIGRTMIN sends are all delivered after unblock (RT signals queue, not coalesce)"
check_output "$LOG" "M74-SMOKE: ok rt-sigqueue" "M74: sigqueue payloads reach an SA_SIGINFO handler as si_value in FIFO order"
check_output "$LOG" "M74-SMOKE: ok rt-timer" "M74: a POSIX interval timer (timer_create/settime) repeatedly raises an RT signal carrying its sigev_value"
check_output "$LOG" "M15-SMOKE: ok audit-logging" "audit marker appears after privileged syscall"
check_output "$LOG" "M15-SMOKE: done" "M15 smoke completes"

# ── M56 Event-loop & IPC primitives ──
section "M56 Event-loop & IPC Primitives"
check_output "$LOG" "M56-SMOKE: start" "M56 smoke starts"
check_output "$LOG" "M56-SMOKE: ok eventfd" "eventfd counter write/read + semaphore mode work"
check_output "$LOG" "M56-SMOKE: ok epoll" "epoll_wait wakes on a ready fd and times out when idle"
check_output "$LOG" "M56-SMOKE: ok timerfd" "timerfd fires and is pollable via epoll"
check_output "$LOG" "M56-SMOKE: ok signalfd" "signalfd delivers a raised signal as a readable record"
check_output "$LOG" "M56-SMOKE: ok seal" "F_SEAL_WRITE on a sealable memfd rejects writes"
check_output "$LOG" "M56-SMOKE: done" "M56 smoke completes"
# ── POSIX memory/signal primitives (madvise, MAP_NORESERVE, sigaltstack) ──
section "POSIX memory/signal primitives"
check_output "$LOG" "MM-SMOKE: start" "MM smoke starts"
check_output "$LOG" "MM-SMOKE: ok madvise" "madvise(MADV_DONTNEED) zeroes a refaulted anonymous page"
check_output "$LOG" "MM-SMOKE: ok noreserve" "MAP_NORESERVE large mapping commits lazily on touch"
check_output "$LOG" "MM-SMOKE: ok sigaltstack" "sigaltstack get/set/disable + SA_ONSTACK handler runs on alt stack"
check_output "$LOG" "MM-SMOKE: done" "MM smoke completes"

# ── M40 Linux ABI compatibility (x86_64 only: it runs a Linux x86_64 ELF) ──
if [ "$ARCH" = "x86_64" ]; then
	section "M40 Linux ABI Compatibility"
	check_output "$LOG" "M40-LINUX: start" "M40 Linux ABI smoke starts"
	check_output "$LOG" "ELF load: Linux personality detected: /bin/m40-linux-hello" "loader tags the static Linux binary with the Linux personality"
	check_output "$LOG" "M40-LINUX: hello from a static linux x86_64 binary" "translated Linux write(1,...) reached the console"
	check_output "$LOG" "M40-LINUX: ok fstat" "Linux fstat(2) result is translated to the Linux struct stat layout (st_mode at offset 24)"
	check_output "$LOG" "M40-LINUX: ok uname" "Linux uname(2) result is translated to the Linux struct utsname layout (machine at offset 260)"
	check_output "$LOG" "M40-LINUX: ok getdents64" "Linux getdents64(2) result is repacked into variable-length linux_dirent64 records"
	check_output "$LOG" "M40-LINUX: ok arch-prctl" "Linux arch_prctl(ARCH_SET_FS) sets the FS base (TLS) and a %fs:0 round-trip works"
	check_output "$LOG" "M40-LINUX: ok mmap" "Linux mmap(MAP_ANONYMOUS) gives a Linux binary writable memory (flags/prot already match)"
	check_output "$LOG" "M40-LINUX: ok clock" "Linux clock_gettime(CLOCK_MONOTONIC) works (timespec layout matches b1nix)"
	check_output "$LOG" "M40-LINUX: ok gettid" "Linux gettid(2) is mapped to the native SYS_GETTID (returns a valid thread id)"
	check_output "$LOG" "M40-LINUX: ok signal" "Linux rt_sigaction+kill deliver a SIGUSR1 handler with signo remap (b1nix 19 <-> Linux 10) and Linux sigreturn trampoline"
	check_output "$LOG" "M40-LINUX: ok sigmask" "Linux rt_sigprocmask remaps the sigset_t bit positions and the swapped SIG_UNBLOCK/SIG_SETMASK how-values (blocked SIGUSR1 defers delivery)"
	check_output "$LOG" "M40-LINUX: ok tgkill" "Linux tgkill(2) self-signal (glibc raise/pthread_kill path) delivers with signo remap"
	check_output "$LOG" "M40-LINUX: ok alarm" "Linux alarm/sync/fchdir/setpriority mapped to native handlers"
	check_output "$LOG" "M40-LINUX: ok siginfo" "Linux SA_SIGINFO 3-arg handler gets a kernel-built siginfo_t (si_signo) + ucontext_t"
	check_output "$LOG" "M40-LINUX: ok run-static" "static Linux ELF ran and exited 0 via translated exit_group(231)"
	check_output "$LOG" "M40-LINUX: done" "M40 Linux ABI smoke completes"

	# M40 closeout: the rest of the translated Linux surface, exercised by a
	# second static Linux ELF (tools/m40/linux_abi_test.c).
	check_output "$LOG" "M40-ABI: start" "M40 Linux ABI conformance blob starts"
	check_output "$LOG" "M40-ABI: ok pread64" "Linux pread64 reads at an explicit offset and leaves the fd offset untouched"
	check_output "$LOG" "M40-ABI: ok pwrite64" "Linux pwrite64 writes at an explicit offset"
	check_output "$LOG" "M40-ABI: ok preadv" "Linux preadv scatters a positional read across two iovecs"
	check_output "$LOG" "M40-ABI: ok truncate" "Linux truncate(2) by path resizes the file (verified through fstat)"
	check_output "$LOG" "M40-ABI: ok utime" "Linux utime(2) installs the requested mtime (verified through fstat)"
	check_output "$LOG" "M40-ABI: ok creat-renameat" "Linux creat(2) + renameat(2) create and rename a file"
	check_output "$LOG" "M40-ABI: ok statfs" "Linux statfs(2) fills the Linux struct statfs (matching b1nix layout)"
	check_output "$LOG" "M40-ABI: ok fstatfs" "Linux fstatfs(2) reports the same filesystem as statfs"
	check_output "$LOG" "M40-ABI: ok syncfs" "Linux syncfs(2) flushes the filesystem behind a descriptor"
	check_output "$LOG" "M40-ABI: ok readahead" "Linux readahead(2) is accepted as the advisory hint it is"
	check_output "$LOG" "M40-ABI: ok gettimeofday" "Linux gettimeofday(2) returns a sane wall clock and microseconds"
	check_output "$LOG" "M40-ABI: ok time" "Linux time(2) agrees with gettimeofday"
	check_output "$LOG" "M40-ABI: ok sched-getscheduler" "Linux sched_getscheduler reports SCHED_OTHER"
	check_output "$LOG" "M40-ABI: ok sched-getparam" "Linux sched_getparam reports priority 0 under SCHED_OTHER"
	check_output "$LOG" "M40-ABI: ok sched-priority-max" "Linux sched_get_priority_max(SCHED_OTHER) is 0"
	check_output "$LOG" "M40-ABI: ok sched-rr-interval" "Linux sched_rr_get_interval reports the 100 Hz tick"
	check_output "$LOG" "M40-ABI: ok sched-setaffinity" "Linux sched_setaffinity accepts an all-CPU mask and rejects an empty one"
	check_output "$LOG" "M40-ABI: ok getresuid" "Linux getresuid/getresgid return the task's real/effective/saved ids"
	check_output "$LOG" "M40-ABI: ok capget" "Linux capget(v3) reports the full capability set for root"
	check_output "$LOG" "M40-ABI: ok personality" "Linux personality(0xffffffff) queries the (fixed) PER_LINUX personality"
	check_output "$LOG" "M40-ABI: ok unshare" "Linux unshare(0) is a valid no-op"
	check_output "$LOG" "M40-ABI: ok mlock-resident" "Linux mlock(2) populates an untouched lazy mapping — mincore reports it resident before anything faults it in"
	check_output "$LOG" "M40-ABI: ok mlock-munlock" "Linux mlock/munlock pin a range against swap eviction"
	check_output "$LOG" "M40-ABI: ok mincore" "Linux mincore reports the mapped pages as resident"
	check_output "$LOG" "M40-ABI: ok rt-sigpending" "Linux rt_sigpending returns the (empty) pending set in Linux numbering"
	check_output "$LOG" "M40-ABI: ok sethostname" "Linux sethostname(2) changes the name uname(2) reports"
	check_output "$LOG" "M40-ABI: ok proc-sys-hostname" "/proc/sys/kernel/hostname reflects sethostname(2)"
	check_output "$LOG" "M40-ABI: ok proc-statm" "/proc/self/statm reports a real resident page count"
	check_output "$LOG" "M40-ABI: ok proc-limits" "/proc/self/limits renders the task's rlimits in the Linux layout"
	check_output "$LOG" "M40-ABI: ok proc-swaps" "/proc/swaps renders the Linux swap-area table"
	check_output "$LOG" "M40-ABI: ok proc-sys-pid-max" "/proc/sys/kernel/pid_max reports the task ceiling"
	check_output "$LOG" "M40-ABI: ok sys-class-net" "/sys/class/net/lo exposes the Linux interface attributes"
	check_output "$LOG" "M40-ABI: ok proc-cwd-link" "/proc/self/cwd resolves to the task's working directory"
	check_output "$LOG" "M40-ABI: ok epoll-create" "Linux epoll_create(size) maps onto epoll_create1"
	check_output "$LOG" "M40-ABI: ok ptrace-nonparent" "a tracer that is not the tracee's parent can attach and see its stops"
	check_output "$LOG" "M40-ABI: ok sched-affinity-pin" "sched_setaffinity really pins the task (mask reads back, the task still runs, an empty mask is refused)"
	check_output "$LOG" "M40-ABI: ok capset-drop" "capset(2) drops CAP_SYS_TIME for real — settimeofday then reports EPERM to a uid-0 task"
	check_output "$LOG" "M40-ABI: ok setfsuid" "setfsuid(2) moves the id the VFS checks against: a root-only file stops being readable, and reading works again once fsuid is 0"
	check_output "$LOG" "M40-ABI: ok chroot-keeps-cwd" "chroot(2) keeps a working directory inside the new root, rewritten root-relative"
	check_output "$LOG" "M40-ABI: ok enosys-unmapped" "an unassigned syscall number reports ENOSYS instead of being silently accepted"
	check_output "$LOG" "M40-ABI: ok getdents-legacy" "Linux getdents(2) emits the pre-64-bit record layout (d_type in the last byte)"
	check_output "$LOG" "M40-ABI: ok getdents-ino" "getdents64's d_ino is the filesystem's real inode number (distinct per entry, matching stat)"
	check_output "$LOG" "M40-ABI: ok rseq-abort" "an rseq critical section interrupted by the scheduler resumes at its abort handler, and the descriptor is consumed"
	check_output "$LOG" "M40-ABI: ok vmsplice" "Linux vmsplice(2) moves user memory into a pipe"
	check_output "$LOG" "M40-ABI: ok tee" "Linux tee(2) duplicates pipe data without consuming the source"
	check_output "$LOG" "M40-ABI: ok ioprio" "Linux ioprio_set/ioprio_get round-trip the task's I/O class and level"
	check_output "$LOG" "M40-ABI: ok sysv-sem" "SysV semaphores: SETVAL/GETVAL, an atomic down/up pair, and IPC_NOWAIT reporting EAGAIN"
	check_output "$LOG" "M40-ABI: ok sysv-msg" "SysV message queues: type-selective msgrcv picks the requested type, IPC_NOWAIT reports ENOMSG"
	check_output "$LOG" "M40-ABI: ok file-handle" "name_to_handle_at + open_by_handle_at reopen a file through an opaque handle"
	check_output "$LOG" "M40-ABI: ok file-handle-stale" "a handle whose file was replaced reports ESTALE instead of opening the impostor"
	check_output "$LOG" "M40-IOPRIO: ok elevator-order" "the block layer admits the best-priority waiter first, and ages a starving idle-class request past fresh best-effort ones"
	check_output "$LOG" "M40-ABI: ok rseq" "rseq(2) registration publishes a live cpu_id into the task's own memory"
	check_output "$LOG" "M40-ABI: ok swapon-swapoff" "swapoff(2) pages everything back in and detaches; swapon(2) re-attaches the device"
	check_output "$LOG" "M40-ABI: ok chroot" "chroot(2) confines a child: the jailed path resolves, an outside path and a \"..\" escape do not"
	check_output "$LOG" "M40-ABI: ok ptrace-stop" "ptrace(2): a traced child stops on a signal and its parent sees WIFSTOPPED"
	check_output "$LOG" "M40-ABI: ok ptrace-getregs" "PTRACE_GETREGS returns the stopped tracee's user registers"
	check_output "$LOG" "M40-ABI: ok ptrace-peek" "PTRACE_PEEKDATA reads the tracee's memory through its own page tables"
	check_output "$LOG" "M40-ABI: ok ptrace-cont" "PTRACE_CONT resumes the tracee, which then runs to exit"
	check_output "$LOG" "M40-ABI: done" "M40 Linux ABI conformance completes with no failures"
fi

# ── M67 Rust cross-toolchain (x86_64 only: it runs a Rust std program) ──
if [ "$ARCH" = "x86_64" ]; then
	section "M67 Rust Cross-Toolchain"
	check_output "$LOG" "M67-RUST: start" "M67 Rust std smoke starts"
	check_output "$LOG" "rust on b1nix squares=\[0, 1, 4, 9, 16\] sum=30" "Rust Vec/String/HashMap + iterator sum ran (println! reached the console)"
	check_output "$LOG" "thread returned 42" "Rust std::thread::spawn + join works (futex bridge to native SYS_FUTEX)"
	check_output "$LOG" "M67-RUST: ok run-std" "static Rust ELF ran and exited 0"
	check_output "$LOG" "M67-RUST: done" "M67 Rust smoke completes"
fi

# ── M25 Native C compiler (b1cc) ──
section "M94 Linux ABI conformance (through musl)"
# Each of these reached the kernel as "unmapped syscall -> -ENOSYS" before the
# translation table gained them, so a regression shows up here rather than as a
# port that mysteriously degrades.
check_output "$LOG" "MUSL-POSIX: ok clock-getres" "clock_getres reports the real 10 ms tick resolution (Linux nr 229)"
check_output "$LOG" "MUSL-POSIX: ok times" "times() returns process CPU accounting (Linux nr 100)"
check_output "$LOG" "MUSL-POSIX: ok sysinfo" "sysinfo() reports total RAM (Linux nr 99)"
check_output "$LOG" "MUSL-POSIX: ok sched-getaffinity" "sched_getaffinity returns a non-empty CPU mask (Linux nr 204)"
check_output "$LOG" "MUSL-POSIX: ok statx-dirfd" "statx resolves a relative path against a real dirfd (Linux nr 332)"
check_output "$LOG" "MUSL-POSIX: ok faccessat2" "faccessat2 resolves against a real dirfd (Linux nr 439)"
check_output "$LOG" "MUSL-POSIX: ok sigtimedwait" "sigtimedwait consumes a pending signal without running its handler (Linux nr 128)"
check_output "$LOG" "MUSL-POSIX: ok sigtimedwait-timeout" "sigtimedwait times out with EAGAIN instead of blocking forever"
check_output "$LOG" "MUSL-POSIX: done" "the musl POSIX smoke completes"

# Address-space layout: the loader is not tied to one load base. Three distinct
# layouts run in the same boot — a stock Linux ET_EXEC at its own vaddr
# (0x200000), b1nix ET_EXEC images at 0x2000000, and musl PIEs the loader places
# itself (randomized under b1nix.aslr, see the M71 check) — none of which can
# collide with the kernel, which lives in the higher half.
check_output "$LOG" "PIE base=" "musl PIE images are placed by the loader, not by a fixed link address"

section "M25 Native C compiler (b1cc)"
check_output "$LOG" "M25-SMOKE: start" "M25 smoke starts"
check_output "$LOG" "M25-SMOKE: ok cc-launch" "the native C compiler /bin/b1cc is present"
check_output "$LOG" "M25-SMOKE: ok compile-hello" "b1cc compiles hello.c on the target"
check_output "$LOG" "M25-SMOKE: ok run-hello" "compiled hello program runs"
check_output "$LOG" "M25-HELLO: hello from native b1cc" "hello program outputs correct greeting"
check_output "$LOG" "M25-SMOKE: ok compile-utility" "b1cc compiles and runs a mini-echo utility"
check_output "$LOG" "M25-SMOKE: ok argv-check" "compiled program receives argc/argv"
check_output "$LOG" "M25-SMOKE: ok stderr-check" "compiled program stderr path works"
check_output "$LOG" "M25-SMOKE: ok exit-check" "compiled program non-zero exit status propagates"
check_output "$LOG" "M25-SMOKE: ok float-check" "compiled program does real double arithmetic (SSE/x87 encodings)"
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
check_output "$LOG" "M26-SMOKE: ok toolchain-ready" "native toolchain (clang/binutils/make) is ported"
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
check_output "$LOG" "LOCK-SMOKE: ok nonblock-conflict" "fcntl nonblock lock conflict returns EAGAIN"
check_output "$LOG" "LOCK-SMOKE: ok wake-on-close" "fcntl blocking lock wakes on parent close"
check_output "$LOG" "EXT-STRESS: start" "EXT-STRESS starts"
check_output "$LOG" "M24-STRESS: start" "M24 scheduler stress starts"
check_output "$LOG" "ok eloop" "circular symlink returns ELOOP"
check_output "$LOG" "POSIX-SMOKE: done" "POSIX shell-driven smoke tests complete"

# bpkg minimal package manager: real pipeline (curl file:// + sha256sum -c + tar).
check_output "$LOG" "BPKG-SMOKE: start" "bpkg smoke starts"
check_output "$LOG" "BPKG-SMOKE: ok update" "bpkg update fetches the index"
check_output "$LOG" "BPKG-SMOKE: ok install" "bpkg install verifies sha256 and extracts"
check_output "$LOG" "BPKG-SMOKE: ok list" "bpkg list reports the installed package"
check_output "$LOG" "BPKG-SMOKE: ok checksum-reject" "bpkg install rejects a wrong sha256"
check_output "$LOG" "BPKG-SMOKE: ok remove" "bpkg remove deletes files and metadata"
check_output "$LOG" "BPKG-SMOKE: ok dep-resolution" "bpkg install resolves dependencies transitively"
check_output "$LOG" "BPKG-SMOKE: done" "bpkg smoke completes"
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
	check_output "$LOG" "M43: ok create-runtime-mountpoint" "file/dir creation works at a mountpoint created at runtime (mkdir then mount)"
	check_output "$LOG" "BB-W4: ok umount" "busybox umount unmounts a device"
	# Needs a real off-link DNS path; skips cleanly when the usernet has none.
	if grep -q "BB-W4: unsupported nslookup" "$LOG" 2>/dev/null; then
		pass "busybox nslookup skipped (no external DNS path)"
	else
		check_output "$LOG" "BB-W4: ok nslookup" "busybox nslookup resolves an address"
	fi
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
	check_output "$LOG" "BB-W8: ok id" "/bin/id (promoted to upstream) reports uid 0"
	check_output "$LOG" "BB-W8: ok whoami" "/bin/whoami (promoted to upstream) reports root"
	check_output "$LOG" "BB-W8: ok uuidgen" "/bin/uuidgen (promoted) generates a UUID"
	check_output "$LOG" "BB-W8: ok sha384sum" "/bin/sha384sum (promoted) computes a SHA-384 hash"
	check_output "$LOG" "BB-W8: ok vmstat" "/bin/vmstat (promoted) reports stats"
	check_output "$LOG" "BB-W8: ok tree" "/bin/tree (promoted) prints a directory tree"
	check_output "$LOG" "BB-W8: done" "BusyBox wave 8 applet promotion completes"
	check_output "$LOG" "BB-W9: ok chmod" "/bin/chmod (promoted to upstream) sets mode 600"
	check_output "$LOG" "BB-W9: ok chown" "/bin/chown (promoted to upstream) sets owner 0"
	check_output "$LOG" "BB-W9: ok tsort" "/bin/tsort (promoted) topologically sorts"
	check_output "$LOG" "BB-W9: done" "BusyBox wave 9 applet promotion completes"
	check_output "$LOG" "BB-SMOKE: ok rm" "busybox rm works"
	check_output "$LOG" "BB-SMOKE: ok rmdir" "busybox rmdir works"
	check_output "$LOG" "BB-SMOKE: done" "BusyBox smoke completes"

# ── M11 Shell & Utilities ──
section "M11 Shell baseline"
check_output "$LOG" "M11-SMOKE: start" "M11 shell smoke starts"
check_output "$LOG" "M11-SMOKE: ok pipe-eof" "pipe EOF when all writers close"
check_output "$LOG" "M11-SMOKE: ok pipe-nonblock-read" "pipe nonblocking read returns EAGAIN"
check_output "$LOG" "M11-SMOKE: ok pipe-nonblock-write" "pipe nonblocking write returns EAGAIN"
check_output "$LOG" "M11-SMOKE: ok fifo-mkfifo" "mkfifo creates a FIFO node in /run that stat() reports as S_IFIFO"
check_output "$LOG" "M11-SMOKE: ok fifo-nonblock-enxio" "FIFO O_WRONLY|O_NONBLOCK with no reader fails with ENXIO"
check_output "$LOG" "M11-SMOKE: ok fifo-rendezvous" "FIFO blocking open waits for the peer, then transfers data across a fork"
check_output "$LOG" "M11-SMOKE: ok fifo-eof" "FIFO reader sees EOF once the writer closes"
check_output "$LOG" "M11-SMOKE: done" "M11 shell smoke completes"

# ── M33 Shell compliance: POSIX sh features under BusyBox ash ──
# Re-implemented as ash-script tests after the in-kernel builtin shell was
# retired. Arrays are bash-only and job control is meaningless here; signal
# traps are exercised asynchronously while ash is blocked in wait.
check_output "$LOG" "M33-SHELL: start" "M33 shell smoke starts"
check_output "$LOG" "M33-SHELL: ok pipe-large" "concurrent pipeline streams >512B without deadlock"
check_output "$LOG" "M33-SHELL: ok cmdsubst" "command substitution \$(...)"
check_output "$LOG" "M33-SHELL: ok subshell" "subshell ( ... ) isolates env side effects"
check_output "$LOG" "M33-SHELL: ok function" "shell functions define + invoke with positionals"
check_output "$LOG" "M33-SHELL: ok case" "case ... esac selects matching glob branch"
check_output "$LOG" "M33-SHELL: ok for-loop" "for VAR in LIST; do ...; done"
check_output "$LOG" "M33-SHELL: ok while-loop" "while COND; do ...; done + scalar assign"
check_output "$LOG" "M33-SHELL: ok arith" "arithmetic expansion \$((...)) evaluates"
check_output "$LOG" "M33-SHELL: ok param-expand" "parameter expansion \${x:-w}"
check_output "$LOG" "M33-SHELL: ok heredoc" "here-document body"
check_output "$LOG" "M33-SHELL: ok glob-star" "pathname glob '*.txt' expands"
check_output "$LOG" "M33-SHELL: ok async-trap" "concurrent SIGUSR1 runs an ash trap"
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
check_output "$LOG" "M29-PTHREAD: ok attr" "pthread_attr stack size + detach state are honored"
check_output "$LOG" "M29-PTHREAD: ok mutex" "pthread mutex serialises two threads"
check_output "$LOG" "M29-PTHREAD: ok condvar" "pthread condvar signal/wait works"
check_output "$LOG" "M29-PTHREAD: ok thread-local" "real ELF __thread storage is per-thread in spawned pthreads"
check_output "$LOG" "M29-PTHREAD: ok tls" "SYS_SET_TLS + %fs:0 round-trip works"
check_output "$LOG" "M29-PTHREAD: ok gettid" "SYS_GETTID returns distinct ids per thread"
check_output "$LOG" "M29-PTHREAD: ok stress-smp" "120 rounds of unjoined-thread exit reclaim the shared mm (no PMM leak)"
check_output "$LOG" "M29-PTHREAD: ok tsd" "pthread TSD key creation and dtor calls work"
check_output "$LOG" "M29-PTHREAD: ok syslog" "syslog open/write/close works"
check_output "$LOG" "/dev/log: .*M54-LOG sink-delivers-ok" "kernel /dev/log sink forwards a syslog datagram to the kernel log (no userspace syslogd)"
check_output "$LOG" "M29-PTHREAD: ok utmp" "utmpname/setutent/pututline/getutline login accounting works"
check_output "$LOG" "M29-PTHREAD: ok pam" "pam_start/pam_get_item/pam_authenticate nonexistent user checks work"
check_output "$LOG" "M29-PTHREAD: ok locale" "setlocale + localeconv + nl_langinfo work"
check_output "$LOG" "M29-PTHREAD: ok iconv" "iconv UTF-8/Latin-1/ASCII conversion + error semantics work"
check_output "$LOG" "M29-PTHREAD: ok time-hammer" "tight gettimeofday/clock_gettime loop is crash-free and monotonic"
check_output "$LOG" "M29-PTHREAD: ok cancel" "pthread_cancel deferred cancellation + setcancelstate work"
check_output "$LOG" "M29-PTHREAD: done" "M29 pthread smoke completes"
# ── M31 User Security / Passwords / Setuid ──
check_output "$LOG" "M31-SEC: start" "M31 user-security smoke starts"
check_output "$LOG" "M31-SEC: ok shadow-format" "/etc/shadow exists and uses \$6\$ SHA-512 crypt"
check_output "$LOG" "M31-SEC: ok getspnam-root" "getspnam(3) returns root's \$6\$ hash (the path SSH password auth uses)"
check_output "$LOG" "M31-SEC: ok setuid-elevate" "setuid initramfs binary elevates euid to root"
check_output "$LOG" "M31-SEC: ok uid-denial" "non-root setuid(0) is rejected by kernel"
check_output "$LOG" "M31-SEC: done" "M31 smoke completes"
# ── M59 EGL over Mesa OSMesa (off-screen, software OpenGL) ──
check_output "$LOG" "M59-SMOKE: ok egl-init" "M59: eglInitialize/eglChooseConfig over the b1nix display (EGL 1.x)"
check_output "$LOG" "M59-SMOKE: ok egl-context" "M59: eglCreateContext + pbuffer surface + eglMakeCurrent (real Mesa softpipe GL bound)"
check_output "$LOG" "M59-SMOKE: ok egl-render" "M59: GL clear + triangle drawn off-screen via EGL; read-back pixels verified (clear color, triangle ink, center == triangle)"
check_output "$LOG" "M59-SMOKE: done" "M59 EGL smoke completes"
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
check_output "$LOG" "M32-NET: ok curl-loopback" "the HTTP client downloads over TCP loopback (connect + request + response parsing + file write)"
check_output "$LOG" "M32-NET: ok curl-ipv6" "the HTTP client downloads over IPv6 loopback (::1)"
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
check_output "$LOG" "M32-IP6: ok icmpv6-errors" "ICMPv6 reports an unreachable closed UDP port"
check_output "$LOG" "M32-IP6: ok mld" "MLD joins solicited-node groups and answers membership queries"
check_output "$LOG" "M32-NET: ok ipv6-v6only" "IPV6_V6ONLY rejects IPv4-mapped peers"
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
# ── M53 zlib userspace port (NetSurf image-codec dependency) ──
check_output "$LOG" "M53-ZLIB: ok compress" "ported zlib compresses (one-shot compress2)"
check_output "$LOG" "M53-ZLIB: ok uncompress" "ported zlib decompresses (one-shot uncompress)"
check_output "$LOG" "M53-ZLIB: ok roundtrip" "zlib one-shot roundtrip byte-for-byte identical"
check_output "$LOG" "M53-ZLIB: ok crc32" "zlib crc32 (incremental == single-shot)"
check_output "$LOG" "M53-ZLIB: ok stream" "zlib streaming deflate/inflate roundtrip (libpng path)"
check_output "$LOG" "M53-ZLIB: done" "zlib smoke completes"
# ── M53 libpng userspace port (NetSurf image-codec dependency) ──
check_output "$LOG" "M53-PNG: ok encode" "ported libpng encodes a valid PNG stream"
check_output "$LOG" "M53-PNG: ok decode-header" "libpng decodes IHDR (dimensions/depth/color)"
check_output "$LOG" "M53-PNG: ok decode" "libpng decodes image; pixels byte-for-byte identical"
check_output "$LOG" "M53-PNG: done" "libpng smoke completes"
# ── M53 libjpeg userspace port (NetSurf image-codec dependency) ──
check_output "$LOG" "M53-JPEG: ok encode" "ported libjpeg encodes a valid JPEG stream"
check_output "$LOG" "M53-JPEG: ok decode-header" "libjpeg decodes header (dimensions/components)"
check_output "$LOG" "M53-JPEG: ok decode" "libjpeg decodes image; pixels within tolerance"
check_output "$LOG" "M53-JPEG: done" "libjpeg smoke completes"
# ── M53 libwebp userspace port (image + VP8 video-keyframe codec) ──
check_output "$LOG" "M53-WEBP: ok encode" "ported libwebp encodes a valid RIFF/WEBP stream"
check_output "$LOG" "M53-WEBP: ok info" "libwebp reads back image dimensions"
check_output "$LOG" "M53-WEBP: ok decode" "libwebp lossless decode; pixels byte-for-byte identical"
check_output "$LOG" "M53-WEBP: done" "libwebp smoke completes"
# ── M53 libvpx userspace port (VP8 full-motion video decode) ──
check_output "$LOG" "M53-VPX: ok webp-vp8-frame" "extracts a VP8 keyframe from a lossy WebP"
check_output "$LOG" "M53-VPX: ok decode-init" "libvpx VP8 decoder initializes"
check_output "$LOG" "M53-VPX: ok decode" "libvpx decodes the VP8 frame to an I420 image"
check_output "$LOG" "M53-VPX: ok luma" "decoded luma plane matches the original within tolerance"
check_output "$LOG" "M53-VPX: done" "libvpx smoke completes"
# ── M53 libwapcaplet userspace port (NetSurf browser-library chain, step 1) ──
check_output "$LOG" "M53-WAPCAPLET: ok intern" "interning the same bytes twice returns the identical pointer"
check_output "$LOG" "M53-WAPCAPLET: ok distinct" "a different string interns to a different pointer"
check_output "$LOG" "M53-WAPCAPLET: ok data" "lwc_string_data/length round-trip the original bytes"
check_output "$LOG" "M53-WAPCAPLET: ok caseless" "case-insensitive comparison matches mixed-case strings"
check_output "$LOG" "M53-WAPCAPLET: ok tolower" "lwc_string_tolower produces the lowercased form"
check_output "$LOG" "M53-WAPCAPLET: done" "libwapcaplet smoke completes"
# ── M53 libparserutils userspace port (NetSurf browser-library chain, step 2) ──
check_output "$LOG" "M53-PARSERUTILS: ok utf8-roundtrip" "UTF-8 encode/decode round-trips 1/2/3/4-byte codepoints"
check_output "$LOG" "M53-PARSERUTILS: ok utf8-length" "utf8_length counts characters (not bytes) in a mixed-width string"
check_output "$LOG" "M53-PARSERUTILS: ok mibenum" "charset alias table resolves UTF-8 case-insensitively + classifies Unicode"
check_output "$LOG" "M53-PARSERUTILS: ok codec-decode" "ISO-8859-1 codec decodes a high byte to the right UCS-4 codepoint"
check_output "$LOG" "M53-PARSERUTILS: ok codec-encode" "UTF-8 codec encodes UCS-4 back to the correct bytes"
check_output "$LOG" "M53-PARSERUTILS: done" "libparserutils smoke completes"
# ── M53 libhubbub userspace port (NetSurf browser-library chain, step 3) ──
check_output "$LOG" "M53-HUBBUB: ok create" "hubbub HTML5 parser instantiates"
check_output "$LOG" "M53-HUBBUB: ok parse" "hubbub tokenises a real HTML document chunk to completion"
check_output "$LOG" "M53-HUBBUB: ok doctype" "tokeniser emits a DOCTYPE token named html"
check_output "$LOG" "M53-HUBBUB: ok tags" "tokeniser emits matching start/end tag tokens"
check_output "$LOG" "M53-HUBBUB: ok attribute" "tokeniser parses the <p class=\"x\"> attribute"
check_output "$LOG" "M53-HUBBUB: ok text" "tokeniser emits the body character data \"Hi\""
check_output "$LOG" "M53-HUBBUB: ok eof" "tokeniser emits EOF after completion"
check_output "$LOG" "M53-HUBBUB: done" "libhubbub smoke completes"
# ── M53 libcss userspace port (NetSurf browser-library chain, step 4) ──
check_output "$LOG" "M53-LIBCSS: ok create" "libcss stylesheet object instantiates"
check_output "$LOG" "M53-LIBCSS: ok parse" "libcss lexes/parses a real stylesheet to completion"
check_output "$LOG" "M53-LIBCSS: ok ctx" "libcss selection context accepts the parsed sheet"
check_output "$LOG" "M53-LIBCSS: ok select" "libcss runs the cascade/selection over a node"
check_output "$LOG" "M53-LIBCSS: ok color" "computed color is opaque red (#rrggbb parse + cascade verified)"
check_output "$LOG" "M53-LIBCSS: ok display" "computed display is block"
check_output "$LOG" "M53-LIBCSS: done" "libcss smoke completes"
# ── M53 libdom userspace port (NetSurf browser-library chain, step 5) ──
check_output "$LOG" "M53-LIBDOM: ok create" "libdom hubbub-binding parser instantiates a document"
check_output "$LOG" "M53-LIBDOM: ok parse" "libdom builds a DOM tree from an HTML document"
check_output "$LOG" "M53-LIBDOM: ok root" "document element is named HTML"
check_output "$LOG" "M53-LIBDOM: ok query" "getElementsByTagName(p) returns the single <p> node"
check_output "$LOG" "M53-LIBDOM: ok attribute" "the <p> node's id attribute is x"
check_output "$LOG" "M53-LIBDOM: ok text" "the <p> node's text content is Hi"
check_output "$LOG" "M53-LIBDOM: done" "libdom smoke completes"
# ── M53 NetSurf helper/decoder libs (libnsutils/libnsgif/libnsbmp/libnslog) ──
check_output "$LOG" "M53-NSUTILS: ok base64" "libnsutils base64 encode/decode round-trips"
check_output "$LOG" "M53-NSUTILS: ok monotonic" "libnsutils monotonic clock advances"
check_output "$LOG" "M53-NSGIF: ok info" "libnsgif reads a 1x1 GIF's dimensions"
check_output "$LOG" "M53-NSGIF: ok decode" "libnsgif decodes the GIF to an opaque-red RGBA pixel"
check_output "$LOG" "M53-NSBMP: ok info" "libnsbmp reads a 2x2 BMP's dimensions"
check_output "$LOG" "M53-NSBMP: ok decode" "libnsbmp decodes the BMP to opaque-red RGBA pixels"
check_output "$LOG" "M53-NSLOG: ok deliver" "libnslog delivers a logged line to the render callback"
check_output "$LOG" "M53-NSLIBS: done" "NetSurf helper/decoder smoke completes"
# ── M53 NetSurf framebuffer browser: render a real HTML page ──
check_output "$LOG" "M53-NS: ok load" "NetSurf loads the local file:// HTML page to completion"
check_output "$LOG" "M53-NS: ok redraw" "NetSurf redraws the laid-out page into the framebuffer surface"
check_output "$LOG" "M53-NS: ok render" "rendered framebuffer is non-blank and structured (real page paint)"
check_output "$LOG" "M53-NS: ok svg" "inline SVG painted its solid block (libsvgtiny decoded it — NETSURF_USE_NSSVG)"
check_output "$LOG" "M53-NS: ok js" "page JavaScript painted its solid block (Duktape executed it — enable_javascript)"
check_output "$LOG" "M53-NS: ok jxl" "JPEG-XL image painted its solid block (libjxl decoded it — NETSURF_USE_JPEGXL)"
check_output "$LOG" "M53-NS: done" "NetSurf framebuffer render self-test completes"
# ── M53 NetSurf on-screen frontend: render straight to /dev/fb0 ──
check_output "$LOG" "M53-FB: ok redraw" "NetSurf draws the page directly into the hardware framebuffer (/dev/fb0)"
check_output "$LOG" "M53-FB: ok render" "the on-screen /dev/fb0 framebuffer is non-blank and structured"
check_output "$LOG" "M53-FB: done" "NetSurf on-screen render completes"
# ── M53 NetSurf interactive input: synthesized keyboard/mouse drive the frontend ──
check_output "$LOG" "M53-INPUT: ok ready" "NetSurf reaches its interactive event loop on the fb frontend"
check_output "$LOG" "M53-INPUT: ok mouse-move" "synthesized pointer motion reaches NetSurf (via /dev/input -> libnsfb -> fbtk)"
check_output "$LOG" "M53-INPUT: ok mouse-click" "synthesized mouse click reaches NetSurf"
check_output "$LOG" "M53-INPUT: ok key" "synthesized keyboard key reaches NetSurf"
check_output "$LOG" "M53-INPUT: done" "NetSurf interactive-input self-test completes"
# ── M53 NetSurf WEB access: fetch + render a page over HTTP (loopback) ──
check_output "$LOG" "M53-HTTPD: ready" "in-VM HTTP server is listening on loopback"
check_output "$LOG" "M53-WEB: has-content=1" "NetSurf fetched the page over HTTP (content attached)"
check_output "$LOG" "M53-WEB: ok redraw" "NetSurf redraws the network-fetched page"
check_output "$LOG" "M53-WEB: ok render" "network-fetched page paints a non-blank, structured framebuffer"
check_output "$LOG" "M53-WEB: done" "NetSurf HTTP render self-test completes"
# ── M53 NetSurf HTTPS: fetch + render a page over TLS (loopback, cert verified) ──
check_output "$LOG" "M53-HTTPSD: ready" "in-VM HTTPS (TLS 1.2) server is listening on loopback"
check_output "$LOG" "M53-HTTPS: has-content=1" "NetSurf fetched the page over HTTPS (TLS handshake + cert verified)"
check_output "$LOG" "M53-HTTPS: ok render" "TLS-fetched page paints a non-blank, structured framebuffer"
check_output "$LOG" "M53-HTTPS: done" "NetSurf HTTPS render self-test completes"
# ── M53 NetSurf public-internet HTTPS (off-link TLS to a real site) ──
# Skips cleanly when the test host/usernet has no off-link route, so the suite
# stays green offline (same policy as the M32 external probes).
if grep -q "M53-EXT-HTTPS: unsupported" "$LOG" 2>/dev/null; then
	pass "NetSurf public-internet HTTPS skipped (no off-link connectivity)"
else
	check_output "$LOG" "M53-EXT-HTTPS: has-content=1" "NetSurf fetched a real public site over HTTPS (Mozilla CA verified)"
	check_output "$LOG" "M53-EXT-HTTPS: ok render" "public HTTPS page paints a non-blank, structured framebuffer"
	check_output "$LOG" "M53-EXT-HTTPS: done" "NetSurf public-internet HTTPS render completes"
fi
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
# ── M46: VFS integrity + POSIX process conformance ──
check_output "$LOG" "M46-SMOKE: start" "M46 conformance suite starts"
check_output "$LOG" "M46-SMOKE: ok exit-status-139" "exit(139) reports as a normal exit, not a signal death"
check_output "$LOG" "M46-SMOKE: ok signal-death" "SIGKILL death reports WIFSIGNALED with the right signal"
check_output "$LOG" "M46-SMOKE: ok kill-zero-pgrp" "kill(0, sig) signals the caller's process group"
check_output "$LOG" "M46-SMOKE: ok kill-all-probe" "kill(-1, 0) broadcast permission probe succeeds"
check_output "$LOG" "M46-SMOKE: ok waitpid-pgid" "waitpid(-pgid) reaps a child from that process group"
check_output "$LOG" "M46-SMOKE: ok setpgid-esrch" "setpgid on a nonexistent pid returns ESRCH"
check_output "$LOG" "M46-SMOKE: ok setpgid-eperm-pgrp" "setpgid into a nonexistent group returns EPERM"
check_output "$LOG" "M46-SMOKE: ok getpgid" "getpgid(0) matches getpgrp()"
check_output "$LOG" "M46-SMOKE: ok getpgid-esrch" "getpgid on a nonexistent pid returns ESRCH"
check_output "$LOG" "M46-SMOKE: ok nice-roundtrip" "nice() and getpriority() round-trip"
check_output "$LOG" "M46-SMOKE: ok fork-sigmask" "fork child inherits the blocked-signal mask"
check_output "$LOG" "M46-SMOKE: ok append-atomic" "concurrent O_APPEND writers never overwrite each other"
check_output "$LOG" "M46-SMOKE: ok truncate-zeros" "shrink-then-grow truncate reads back zeros"
check_output "$LOG" "M46-SHEBANG-OK" "the #! script's interpreter actually ran"
check_output "$LOG" "M46-SMOKE: ok shebang-exec" "direct execve() of a #! script works"
check_output "$LOG" "M46-SMOKE: ok exit-group" "exit_group semantics terminate all thread group members"
check_output "$LOG" "M46-SMOKE: ok setresuid-setresgid" "setresuid/setresgid set credentials and EPERM is enforced"
check_output "$LOG" "M46-SMOKE: ok waitid" "waitid waiting for child state transitions works"
check_output "$LOG" "M46-SMOKE: ok times-getrusage" "times() and getrusage() accounting works"
check_output "$LOG" "M46-SMOKE: ok orphaned-pgrp" "orphaned process groups with stopped tasks receive SIGHUP+SIGCONT"
check_output "$LOG" "M46-SMOKE: ok nice-biasing" "nice value biases cooperative stride scheduling"
check_output "$LOG" "M46-SMOKE: done" "M46 conformance suite completes"
# ── M57: multiprocess broker primitives (fork/exec/FD inheritance + brokering) ──
check_output "$LOG" "M57-SMOKE: ok fork-fdshare" "fork shares the open-file-description file offset with the child"
check_output "$LOG" "M57-SMOKE: ok cloexec" "FD_CLOEXEC via F_SETFD/O_CLOEXEC round-trips through F_GETFD"
check_output "$LOG" "M57-SMOKE: ok exec-inherit" "non-CLOEXEC fd and dup2-stdio survive execve while CLOEXEC fd is closed"
check_output "$LOG" "M57-SMOKE: ok fd-broker" "socketpair + SCM_RIGHTS hands a live fd to a forked child"
check_output "$LOG" "M57-SMOKE: ok fd-broker-death" "in-flight passed fd survives sender close and peer hangup is reported"
check_output "$LOG" "M57-SMOKE: ok dupfd-cloexec" "F_DUPFD_CLOEXEC sets FD_CLOEXEC while F_DUPFD leaves it clear"
check_output "$LOG" "M57-SMOKE: done" "M57 broker-primitive suite completes"
# ── M73: modern I/O & introspection syscalls ──
check_output "$LOG" "M73-SMOKE: ok statx" "statx returns size/mode/nlink/ino matching fstat (path + AT_EMPTY_PATH)"
check_output "$LOG" "M73-SMOKE: ok sendfile" "sendfile copies a range, advances the explicit offset, leaves the src fd offset"
check_output "$LOG" "M73-SMOKE: ok copy-file-range" "copy_file_range copies a byte range using independent explicit offsets"
check_output "$LOG" "M73-SMOKE: ok fallocate" "fallocate mode 0 grows + zero-fills; KEEP_SIZE does not grow"
check_output "$LOG" "M73-SMOKE: ok splice" "splice moves data file->pipe->file intact"
# ── M73: inotify (real file-change notification, was an ENOSYS stub) ──
check_output "$LOG" "M73-SMOKE: ok inotify-modify" "inotify reports IN_MODIFY on a write to a watched file"
check_output "$LOG" "M73-SMOKE: ok inotify-dir" "inotify reports IN_CREATE/IN_DELETE (with entry name) for a watched directory"
check_output "$LOG" "M73-SMOKE: ok inotify-rmwatch" "inotify_rm_watch removes a watch"
check_output "$LOG" "M72-SMOKE: ok msync" "msync validates flags (EINVAL), unmapped range (ENOMEM), and syncs a mapped MAP_SHARED range"
# ── M88: PROT_NONE is enforced (wild access faults, not zero-fills) ──
check_output "$LOG" "M88-SMOKE: ok prot-none" "a PROT_NONE reservation SIGSEGVs on access; mprotect to RW then succeeds"
# ── M85: libc Tier-A correctness (Chromium-debt overlap) ──
check_output "$LOG" "M73-SMOKE: ok strtoull" "strtoull parses the full uint64 range + ERANGE; strtoll honors signed range/base16"
check_output "$LOG" "M73-SMOKE: ok sysconf-ncpu" "sysconf(_SC_NPROCESSORS_ONLN) reports the real online-CPU count (not a hardcoded 1)"
check_output "$LOG" "M73-SMOKE: ok abort-sigabrt" "abort() raises SIGABRT (not exit(127))"
check_output "$LOG" "M73-SMOKE: ok realpath" "realpath resolves ./.. and a symlink to the canonical path (was a strcpy stub)"
# ── M71: ASLR (PIE load-base randomization, opt-in via b1nix.aslr) ──
check_output "$LOG" "M71-ASLR: ok randomized" "two execs of the same PIE binary land at different load bases under b1nix.aslr"
# ── M63: seccomp-bpf ──
check_output "$LOG" "M63-SMOKE: ok seccomp-errno" "a seccomp filter denies a targeted syscall with ERRNO while others run"
check_output "$LOG" "M63-SMOKE: ok seccomp-errno-zero" "SECCOMP_RET_ERRNO with errno 0 blocks the syscall and returns 0 (not run)"
check_output "$LOG" "M63-SMOKE: ok seccomp-kill" "a seccomp KILL_PROCESS verdict terminates the task with SIGSYS"
check_output "$LOG" "M63-SMOKE: ok seccomp-strict" "SECCOMP_MODE_STRICT kills on a syscall outside read/write/exit/sigreturn"
check_output "$LOG" "M63-SMOKE: ok seccomp-inherit" "a forked child inherits the parent's seccomp filter"
check_output "$LOG" "M63-SMOKE: ok seccomp-nnp" "PR_SET/GET_NO_NEW_PRIVS round-trips"
check_output "$LOG" "M63-SMOKE: done" "M63 seccomp-bpf suite completes"
check_output "$LOG" "M73-SMOKE: done" "M73 modern-I/O suite completes"
# ── M98: GNU-free in-guest build tools (bmake + samurai replaced GNU Make) ──
check_output "$LOG" "M98-SMOKE: ok make-is-bmake" "/bin/make answers bmake's -V (GNU Make rejects it), so it is the BSD make"
check_output "$LOG" "M98-SMOKE: ok make-not-gnu" "/bin/make identifies as something other than GNU Make"
check_output "$LOG" "M98-SMOKE: ok make-build" "bmake parses a Makefile, expands a variable and runs the recipe that creates the target"
check_output "$LOG" "M98-SMOKE: ok make-uptodate" "a second bmake run does not re-run the recipe (target newer than its prerequisites)"
check_output "$LOG" "M98-SMOKE: ok samu-version" "/bin/samu reports the Ninja file-format version it implements"
check_output "$LOG" "M98-SMOKE: ok ninja-alias" "/bin/ninja is the samurai binary and runs"
check_output "$LOG" "M98-SMOKE: ok samu-build" "samurai executes a build.ninja edge and produces the declared output"
check_output "$LOG" "M98-SMOKE: ok samu-uptodate" "re-running a satisfied build graph is a no-op"
check_output "$LOG" "M98-SMOKE: done" "M98 GNU-free build-tool suite completes"
# ── zsh: the interactive/login shell (replaced GNU bash in M98) ──
check_output "$LOG" "ZSH-SMOKE: ok version" "zsh reports ZSH_VERSION"
check_output "$LOG" "ZSH-SMOKE: ok arrays" "zsh indexed arrays work (1-based, no KSH_ARRAYS)"
check_output "$LOG" "ZSH-SMOKE: ok dbracket-glob" "zsh [[ ]] glob matching works"
check_output "$LOG" "ZSH-SMOKE: ok regex-match" "zsh [[ =~ ]] regex matching works (zsh/regex linked in)"
check_output "$LOG" "ZSH-SMOKE: ok arithmetic" "zsh \$(( )) arithmetic works"
check_output "$LOG" "ZSH-SMOKE: ok brace-range" "zsh {a..b} brace ranges work"
check_output "$LOG" "ZSH-SMOKE: ok cstyle-for" "zsh C-style for loops work"
check_output "$LOG" "ZSH-SMOKE: ok pattern-subst" "zsh \${var//x/y} substitution works"
check_output "$LOG" "ZSH-SMOKE: ok local-vars" "zsh function local variables work"
check_output "$LOG" "ZSH-SMOKE: ok utf8-length" "zsh counts UTF-8 characters, not bytes"
check_output "$LOG" "ZSH-SMOKE: ok utf8-substr" "zsh string subscripting is UTF-8 character-aware"
check_output "$LOG" "ZSH-SMOKE: done" "zsh feature smoke completes"
# ── M39: configurable init system ──
check_output "$LOG" "M39-INIT: start" "M39 configurable-init self-test starts"
check_output "$LOG" "M39-INIT: ok parse-inittab" "init parses /etc/inittab entries"
check_output "$LOG" "M39-INIT: ok initdefault" "init reads the initdefault runlevel"
check_output "$LOG" "M39-INIT: ok runlevel-match" "inittab runlevel matching works"
check_output "$LOG" "M39-INIT: ok telinit" "telinit writes a runlevel request init consumes"
check_output "$LOG" "M39-INIT: ok getty-applet" "getty applet is present for tty/serial sessions"
check_output "$LOG" "M39-INIT: ok ttys0-open" "/dev/ttyS0 exists and opens as a serial tty"
check_output "$LOG" "M39-INIT: ok tty-termios-independent" "ttyS0 termios is independent of the boot console"
check_output "$LOG" "M39-INIT: ok tty-canon-read" "ttyS0 canonical line discipline assembles and reads a line"
check_output "$LOG" "M39-INIT: ok tty-eof" "ttyS0 VEOF on an empty line reads as EOF"
check_output "$LOG" "M39-INIT: ok tty-raw-read" "ttyS0 raw (non-canonical) read passes bytes through"
check_output "$LOG" "M39-INIT: ok tty-isig" "ttyS0 ISIG intercepts VINTR instead of queueing it"
check_output "$LOG" "M39-INIT: ok tty-pgrp-independent" "ttyS0 foreground pgrp is independent of the console"
check_output "$LOG" "M39-INIT: ok tty-sctty" "TIOCSCTTY claims ttyS0 as a controlling terminal"
check_output "$LOG" "M39-INIT: ok ttys0-write" "ttyS0 write path accepts a full buffer"
check_output "$LOG" "M39-TTYS0-TX-OK" "ttyS0 TX actually reaches COM1 (marker travelled the UART path)"
check_output "$LOG" "M39-INIT: ok ttys0-release" "closing the last ttyS0 handle releases the COM1 claim"
check_output "$LOG" "M39-INIT: done" "M39 configurable-init self-test completes"
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
if [ "$ARCH" = "x86_64" ]; then
  check_output "$LOG" "M30-DYN: ok shared-libc" "DT_NEEDED libc.so.1 resolves GOT/PLT symbols"
  check_output "$LOG" "M69-DL: dlsym ok" "M69 P1: dlopen/dlsym resolves+calls a libc.so.1 symbol"
  check_output "$LOG" "M69-PLUGIN: ctor" "M69 P2: dlopen runs the new object's DT_INIT_ARRAY ctor"
  check_output "$LOG" "M69-DL2: dlopen-run ok" "M69 P2: runtime-loaded .so relocated, dlsym'd, and called"
  check_output "$LOG" "M69-DL5: tls-gd ok" "M69 P4: general-dynamic TLS (DTPMOD64/DTPOFF64 + __tls_get_addr) in a dlopen'd object"
  check_output "$LOG" "M69-DL3: refcount-scope ok" "M69 P3: refcount + RTLD_DEFAULT scope work"
  check_output "$LOG" "M69-PLUGIN: dtor" "M69 P3: final dlclose runs DT_FINI_ARRAY dtor + unmaps"
fi
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

	# ── M38: Intel HDA sound controller + /dev/dsp ──
	if grep -q "M38-SOUND: ok probe" "$LOG" 2>/dev/null; then
		pass "HDA controller probed"
		check_output "$LOG" "M38-SOUND: ok dma-buf" "HDA DMA buffer accessible"
		check_output "$LOG" "M38-SOUND: ok dev-dsp" "HDA /dev/dsp device node"
		check_output "$LOG" "M38-SOUND: ok sound-api" "HDA sound device API"
		check_output "$LOG" "M38-SMOKE: ok open-dsp" "userspace /dev/dsp open"
		check_output "$LOG" "M38-SMOKE: ok write-pcm" "userspace PCM write"
		check_output "$LOG" "M38-SMOKE: ok wav-parse" "userspace WAV parse"
		check_output "$LOG" "M38-SMOKE: ok wav-play" "userspace WAV play"
	else
		# HDA may not be present in all QEMU configs — treat as skip
		pass "HDA controller (skipped — no device)"
	fi

	# ── M47: display substrate — /dev/fb0 + /dev/input/event* ──
	check_output "$LOG" "M47-GFX: start" "M47 smoke started"
	if grep -q "fb0: ready" "$LOG" 2>/dev/null; then
		check_output "$LOG" "M47-GFX: ok fb-info" "M47: /dev/fb0 mode query"
		check_output "$LOG" "M47-GFX: ok fb-mmap" "M47: fb mmap aliases device memory"
		check_output "$LOG" "M47-GFX: ok fb-flush" "M47: FBIOFLUSH dirty-rect push"
		check_output "$LOG" "M47-GFX: ok fb-persist" "M47: fb survives munmap+remap"
	else
		# No 32bpp boot framebuffer in this config — device legitimately absent
		check_output "$LOG" "M47-GFX: skip no-fb" "M47: fb skipped (no framebuffer)"
	fi
	check_output "$LOG" "M47-GFX: ok input-open" "M47: input devices open + EAGAIN"
	check_output "$LOG" "M47-GFX: ok input-event" "M47: injected mouse events received"
	check_output "$LOG" "M47-GFX: done" "M47 smoke completed"

	check_output "$LOG" "M48-FDPASS: ok scm-rights" "M48: SCM_RIGHTS fd transfer"
	check_output "$LOG" "M48-FDPASS: ok scm-refcount-close" "M48: in-flight fd survives sender close"
	check_output "$LOG" "M48-FDPASS: ok memfd" "M48: anonymous mmap-able memfd"
	check_output "$LOG" "M48-FDPASS: ok shared-fork-cow" "M48: MAP_SHARED pages shared across fork"
	check_output "$LOG" "M48-FDPASS: ok unix-blocking-send" "M48: blocking AF_UNIX send blocks on a full buffer instead of EAGAIN"

	check_output "$LOG" "M50-DRM: ok card0" "M50: /dev/dri/card0"
	check_output "$LOG" "M50-DRM: ok mode" "M50: KMS mode discovery"
	check_output "$LOG" "M50-DRM: ok multi-buffer" "M50: multiple dumb buffers"
	check_output "$LOG" "M50-DRM: ok setcrtc" "M50: SETCRTC presentation"
	check_output "$LOG" "M50-DRM: ok flip-event" "M50: poll/read page-flip event"
	check_output "$LOG" "M50-DRM: ok rmfb" "M50: framebuffer removal"
	check_output "$LOG" "M50-DRM: ok cleanup" "M50: close/munmap cleanup"

	check_output "$LOG" "CXX-SMOKE: ok ctors" "C++: crt0 runs .init_array (static constructors)"
	check_output "$LOG" "CXX-SMOKE: ok stl" "C++: libstdc++ STL (map/vector/string) runs on b1nix"
	check_output "$LOG" "CXX-SMOKE: ok exceptions" "C++: exception throw/catch unwinds on b1nix"
	check_output "$LOG" "CXX-SMOKE: ok rtti" "M55 C++: RTTI dynamic_cast/typeid + bad_cast throw"
	check_output "$LOG" "CXX-SMOKE: ok static-init" "M55 C++: thread-safe function-local static (__cxa_guard)"
	check_output "$LOG" "CXX-SMOKE: ok threads" "M55 C++: std::thread/mutex/atomic over pthreads"
	# M64 clang++ frontend is x86_64-only (size_t mangling clash with the
	# GCC-built libstdc++ on i686-b1nix); GCC stays the C++ compiler on x86.
	[ "$ARCH" = "x86_64" ] && check_output "$LOG" "M64-CLANG: ok" "M64: clang++ frontend with GNU C++ runtime"
	check_output "$LOG" "M55-LITEHTML: ok parse" "M55 C++: litehtml parses HTML/CSS (gumbo + STL)"
	check_output "$LOG" "M55-LITEHTML: ok layout" "M55 C++: litehtml lays out a box tree (render)"
	check_output "$LOG" "M55-LITEHTML: ok draw" "M55 C++: litehtml cascade+draw (h1>p font, ordered)"
	check_output "$LOG" "M55-IOSTREAM: ok cout" "M55 C++: std::cout/cerr formatted output (iostream locale facets)"
	check_output "$LOG" "M55-IOSTREAM: ok sstream" "M55 C++: std::ostringstream/istringstream round-trip"
	check_output "$LOG" "M55-IOSTREAM: ok cin" "M55 C++: std::cin extraction from a real fd 0 (piped stdin)"
	check_output "$LOG" "M55-IOSTREAM: ok filesystem" "M55 C++: std::filesystem create/iterate/stat/remove over VFS"
	check_output "$LOG" "M58-SMOKE: ok eval" "M58 JS: /bin/js (Duktape) evaluates arithmetic + String method"
	check_output "$LOG" "M58-SMOKE: ok json" "M58 JS: JSON.parse/stringify round-trip"
	check_output "$LOG" "M58-SMOKE: ok print" "M58 JS: print()/console.log native binding"
	check_output "$LOG" "M58-SMOKE: done" "M58 JS: all interpreter checks passed"
	check_output "$LOG" "M51-GFX: ok libm" "M51: ported libm (openlibm) runtime math"
	check_output "$LOG" "M51-GFX: ok pixman" "M51: ported pixman compositing"
	check_output "$LOG" "M51-GFX: ok freetype" "M51: ported FreeType glyph rasterization"
	check_output "$LOG" "M51-GFX: ok cairo" "M51: ported Cairo text rendering (full stack)"
	check_output "$LOG" "M51-GFX: ok xkbcommon" "M51: ported xkbcommon keycode->keysym"
	check_output "$LOG" "M51-GFX: ok harfbuzz" "M51: ported HarfBuzz OpenType shaping"
	check_output "$LOG" "M51-GFX: ok fontconfig" "M51: ported Fontconfig font matching"

	# ── M49: displayd Wayland protocol ──
	if grep -q "fb0: ready" "$LOG" 2>/dev/null; then
		check_output "$LOG" "displayd: ready" "M47: displayd started on /dev/fb0"
		check_output "$LOG" "M49-WL: ok registry" "M49: Wayland registry globals"
		check_output "$LOG" "M49-WL: ok xdg-shell" "M49: xdg-shell configure handshake"
		check_output "$LOG" "M49-WL: ok xkb-keymap" "M49: wl_keyboard sends a real XKB_V1 keymap"
		check_output "$LOG" "M49-WL: ok maximize" "M49: xdg_toplevel.set_maximized → work-area configure"
		check_output "$LOG" "M49-WL: ok decoration" "M49: xdg-decoration negotiates server_side mode"
		check_output "$LOG" "M49-WL: ok viewporter" "M49 B: wp_viewporter get_viewport/set_source/set_destination"
		check_output "$LOG" "M49-WL: ok subcompositor" "M49 B: wl_subcompositor get_subsurface/set_position"
		check_output "$LOG" "M49-WL: ok presentation" "M49 B: wp_presentation clock_id + presented feedback"
		check_output "$LOG" "M49-WL: ok dmabuf-reject" "M49 B: zwp_linux_dmabuf_v1 advertises formats, rejects create() honestly"
		check_output "$LOG" "M49-WL: ok touch" "M49 B: wl_seat advertises touch + wl_touch get_touch"
		check_output "$LOG" "M49-WL: ok dnd-start" "M49 B: wl_data_device.start_drag DnD grab accepted"
		check_output "$LOG" "M49-WL: ok shm-frame" "M49: Wayland SHM frame"
		check_output "$LOG" "M49-WL: ok libb1gui" "M49: native GUI library uses Wayland"
		check_output "$LOG" "M49-LIBWL: ok upstream-client" "M49: upstream libwayland-client"
		check_output "$LOG" "M49-LIBWL: ok keymap" "M49: wl_keyboard keymap fd"
		check_output "$LOG" "M49-LIBWLS: ok server-core" "M49: upstream libwayland-server core"
		check_output "$LOG" "M51-GFX: ok wl-output" "M51: wl_output advertises mode geometry"
		check_output "$LOG" "M51-GFX: ok cairo-wayland" "M51: Cairo Wayland app renders text to displayd"
		check_output "$LOG" "M52-GFX: ok egl" "M52: EGL initialize on b1nix display"
		check_output "$LOG" "M52-GFX: ok tinygl" "M52: TinyGL software GL context current"
		check_output "$LOG" "M52-GFX: ok gl-triangle" "M52: EGL/OpenGL app renders 3D triangle to displayd"
		check_output "$LOG" "M52-GFX: ok mesa-context" "M52: real Mesa OSMesa context creates (software OpenGL)"
		check_output "$LOG" "M52-GFX: ok mesa-render" "M52: Mesa softpipe renders a verified 3D triangle off-screen"
		check_output "$LOG" "M52-GFX: ok mesa" "M52: Mesa OSMesa app presents to displayd"
		check_output "$LOG" "M52-GFX: ok shader-compile" "M52: Mesa GLSL vertex+fragment shaders compile"
		check_output "$LOG" "M52-GFX: ok shader-link" "M52: Mesa GLSL shader program links"
		check_output "$LOG" "M52-GFX: ok shader-render" "M52: softpipe runs the shader program (Gouraud triangle pixel-verified)"
		check_output "$LOG" "M52-GFX: ok glsl" "M52: programmable GL 2.x pipeline app presents to displayd"
		# ── M53: NetSurf as a windowed display-server client (Wayland frontend) ──
		check_output "$LOG" "M53-WL: has-content=1" "M53: NetSurf runs as a displayd window and loads the page"
		check_output "$LOG" "M53-WL: ok render" "M53: NetSurf paints the page into a displayd/libgui window and presents it"
		check_output "$LOG" "M53-WL: done" "M53: NetSurf windowed (Wayland) frontend completes"
		# ── M52: VirGL 3D acceleration (host virglrenderer) ──
		# Only assert the accelerated path when the host actually offered VirGL
		# (virtio-gpu-gl device). On a plain 2D host this is a real host
		# limitation, not a b1nix bug, so it is recorded as a skip — the software
		# OpenGL/Mesa path above is the verified path there.
		if grep -q "M52-GFX: ok virgl-negotiate" "$LOG" 2>/dev/null; then
			check_output "$LOG" "M52-GFX: ok virgl-capset" "M52: host virglrenderer returns a VirGL capset"
			check_output "$LOG" "M52-GFX: ok virgl-3d-clear" "M52: guest submits a virgl 3D command stream (CTX_CREATE + RESOURCE_CREATE_3D + SUBMIT_3D clear)"
			check_output "$LOG" "M52-GFX: ok path-accelerated" "M52: host GPU renders the virgl clear; pixels verified via TRANSFER_FROM_HOST_3D"
			# M53: the same accelerated path driven from USERSPACE via
			# /dev/virtio-gpu (the kernel/userspace split a Mesa virgl winsys uses).
			check_output "$LOG" "M53-VIRGL: ok caps" "M53: userspace queries the VirGL capset via /dev/virtio-gpu"
			check_output "$LOG" "M53-VIRGL: ok caps-data" "M53: userspace fetches the full VirGL capset blob (Mesa winsys prerequisite)"
			check_output "$LOG" "M53-VIRGL: ok device-api" "M53: VirGL context-init/getparam/res-info ioctls (Mesa winsys prerequisites)"
			check_output "$LOG" "M53-VIRGL: ok transfer-roundtrip" "M53: VirGL guest->host upload + host->guest readback round-trip"
			check_output "$LOG" "M53-VIRGL: ok resource" "M53: userspace creates a 3D render-target resource + mmap window"
			check_output "$LOG" "M53-VIRGL: ok submit" "M53: userspace submits a virgl command stream"
			check_output "$LOG" "M53-VIRGL: ok path-accelerated" "M53: host GPU renders userspace-submitted virgl clear; pixels verified via mmap"
			check_output "$LOG" "M53-GFX: ok gl-accelerated" "M53 variant B: Mesa's gallium virgl driver renders on the host GPU (gallium pipe clear, pixels verified) — full hardware OpenGL"
			check_output "$LOG" "M53-GFX: ok gl-triangle" "M53 variant B: full draw pipeline — Mesa vertex/fragment shaders + vertex buffer rasterise a triangle on the host GPU (centre red, corner black)"
		else
			pass "M52: VirGL 3D acceleration (skipped — host QEMU lacks a virglrenderer GL device)"
		fi
		check_output "$LOG" "M51-GFX: ok clipboard" "M51: wl_data_device clipboard selection round-trip"
		check_output "$LOG" "M47-DSP: ok console-reclaim" "M47: framebuffer returns to kernel console"
		check_output "$LOG" "M47-DSP: ok server-restart" "M47: displayd restarts after reclaim"
	fi
fi

# ── M24b SMP work-stealing (multi-core) ──
echo ""
echo "[RUN] M24b SMP work-stealing (-smp 4) checks..."
check_output "$SMP_LOG" "smp: AP 1 ready" "Application Processor boots (INIT-SIPI)"
check_output "$LOG" "M24B-BKL: instance ran-on-ap" "cross-CPU work-stealing runs stolen tasks on APs"
if grep -q -E "KERNEL PANIC|\[PANIC\]" "$SMP_LOG" 2>/dev/null; then
	fail "SMP self-test completes without panic" "PANIC detected in log"
else
	pass "SMP self-test completes without panic"
fi

# ── M58 V8 / d8: real V8 engine runs JavaScript on b1nix (x86_64 only) ──
# Runs only when the prebuilt d8 artifacts are present (see SMOKE_V8 gating at the
# top). d8 boots off the ext4 disk, deserializes its embedded snapshot, inits the
# isolate, and runs m58.js; each marker is gated on a correct computed result.
if [ "$SMOKE_V8" = "1" ]; then
	echo ""
	echo "[RUN] M58 V8/d8 JavaScript-engine checks..."
	check_output "$V8_LOG" "ELF load: /mnt/v8/d8" "kernel loads the d8 ELF off the ext4 disk"
	check_output "$V8_LOG" "M58-V8: ok hello" "d8 inits the V8 isolate and runs print()"
	check_output "$V8_LOG" "M58-V8: ok loop-sum" "d8 evaluates a 100k-iteration arithmetic loop correctly"
	check_output "$V8_LOG" "M58-V8: ok array-reduce" "d8 builds an array and reduces it correctly"
	check_output "$V8_LOG" "M58-V8: ok object-sort" "d8 allocates/sorts 500 objects correctly"
	check_output "$V8_LOG" "M58-V8: ok json" "d8 round-trips nested JSON correctly"
	check_output "$V8_LOG" "M58-V8: ok gc-churn" "d8 survives 30k short-lived allocations (GC)"
	check_output "$V8_LOG" "M58-V8: ok recursion" "d8 computes fib(25) via recursion correctly"
	check_output "$V8_LOG" "M58-V8: ok closure" "d8 captures a closure variable correctly"
	check_output "$V8_LOG" "M58-V8: ok try-catch" "d8 throws and catches an exception"
	check_output "$V8_LOG" "M58-V8: ok map-set" "d8 builds Map/Set with correct membership"
	check_output "$V8_LOG" "M58-V8: ok typed-array" "d8 reads/writes an Int32Array correctly"
	check_output "$V8_LOG" "M58-V8: ok string-regex" "d8 runs string split/join + regex match"
	check_output "$V8_LOG" "M58-V8: done" "d8 runs m58.js to completion"
	if grep -qa -E "KERNEL PANIC|\[PANIC\]|task 'd8'.*SIGSEGV" "$V8_LOG" 2>/dev/null; then
		fail "d8 runs without crashing" "PANIC/SIGSEGV detected in v8 log"
	else
		pass "d8 runs without crashing"
	fi
elif [ "$ARCH" = "x86_64" ] && [ "$SMOKE_PARALLEL" = "1" ]; then
	echo ""
	echo "[RUN] M58 V8/d8 — skipped (no prebuilt build/v8-out/v8-ext4.img; build d8 to enable)"
fi

# ── Summary ──
echo ""
echo "=== Results ==="
echo "  Passed:  $PASSED"
echo "  Failed:  $FAILED"
echo "  Blocked: $BLOCKED"
echo "  Skipped: $SKIPPED"
if [ "$BLOCKED" -gt 0 ]; then
	echo ""
	echo "  $BLOCKED checks never ran — an instance stopped early:"
	report_wedged_instances
fi

for _i in sys blk posix gfx openrc smp v8; do
    rm -f "$(disk_img sata "$_i")" "$(disk_img nvme "$_i")" "$(disk_img swap "$_i")"
done
echo ""

# Clean up SATA, NVMe and Swap dummy images
rm -f "$PROJECT_DIR/smoke_run/sata.img" "$PROJECT_DIR/smoke_run/nvme.img" "$PROJECT_DIR/smoke_run/swap.img"

if [ "$FAILED" -gt 0 ] || [ "$BLOCKED" -gt 0 ]; then
	exit 1
fi
exit 0
