#!/bin/sh
# Debian (glibc) boot test: boot b1nix with a real debian:bookworm root
# filesystem attached as a virtio-blk disk and let the distro's own binaries
# exercise the Linux-ABI layer.
#
#   sh tools/images/mk-debian-image.sh     # once, builds build/$ARCH/debian.ext4
#   sh tests/debian-smoke.sh [x86_64]
#
# Skips cleanly (exit 0) when the image has not been built, so it can be wired
# into CI on a host with no network.
set -e

ARCH="${1:-x86_64}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build/$ARCH}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$PROJECT_DIR/$BUILD_DIR" ;; esac

IMG="$BUILD_DIR/debian.ext4"
IMG_LABEL="${IMG_LABEL:-b1nix-debian}"
LOG="$PROJECT_DIR/smoke_run/b1nix-debian-boot.log"
BUILD_LOG="$PROJECT_DIR/smoke_run/b1nix-debian-build.log"
ISO="$BUILD_DIR/${B1NIX_ISO_NAME:-b1nix-debian.iso}"
TIMEOUT="${TIMEOUT:-120}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASSED=0
FAILED=0

pass() {
	printf "  ${GREEN}PASS${NC} %s\n" "$1"
	PASSED=$((PASSED + 1))
}
fail() {
	printf "  ${RED}FAIL${NC} %s - %s\n" "$1" "$2"
	FAILED=$((FAILED + 1))
}

mkdir -p "$PROJECT_DIR/smoke_run"

echo "=== B1NIX Debian (glibc) Boot Test ($ARCH) ==="

if [ ! -f "$IMG" ]; then
	printf "  ${YELLOW}skipped${NC}: %s not built — run tools/images/mk-debian-image.sh first\n" "$IMG"
	exit 0
fi

if [ "$(uname -s)" = "Darwin" ]; then
	NPROC=$(sysctl -n hw.ncpu)
else
	NPROC=$(nproc)
fi

# ── Kernel command line ────────────────────────────────────────────────────
# The Debian filesystem is a DISK, not the Multiboot rootfs module: the kernel
# picks it as root from its ext4 label, then runs our harness as PID 1.
# Debian's own sysvinit is PID 1; it runs our harness from /etc/inittab.
# DEBIAN_INIT=/b1nix-stage.sh runs the harness directly as PID 1 instead.
CMDLINE="root=LABEL=$IMG_LABEL init=${DEBIAN_INIT:-/sbin/init} ${DEBIAN_EXTRA_CMDLINE:-}"

# ── Build ──────────────────────────────────────────────────────────────────
if [ "${SKIP_BUILD:-0}" = "1" ]; then
	[ -f "$ISO" ] || { printf "  ${RED}no prebuilt %s${NC}\n" "$ISO"; exit 1; }
	echo "  (SKIP_BUILD=1 — reusing $ISO)"
else
	echo "[BUILD] Building kernel ISO for the Debian boot..."
	if ! (cd "$PROJECT_DIR" && make -j"$NPROC" ARCH="$ARCH" ${SMOKE_MAKE_ARGS:-} \
		KERNEL_CMDLINE="$CMDLINE" iso) >"$BUILD_LOG" 2>&1; then
		printf "  ${RED}BUILD FAILED${NC} (log: %s)\n" "$BUILD_LOG"
		tail -60 "$BUILD_LOG"
		exit 1
	fi
	# `iso` always writes b1nix.iso; keep a stable copy for SKIP_BUILD reruns.
	cp "$BUILD_DIR/b1nix.iso" "$ISO"
	# The image is 550 MB and QEMU mmaps it: hand the copy to the page cache
	# before booting from it, or the first run can read a half-written ISO.
	sync
	pass "kernel builds without errors"
fi

# ── Per-run scratch copy of the image ──────────────────────────────────────
# The pristine image is never written by a test run.
RUN_IMG="$PROJECT_DIR/smoke_run/debian-root-$$.img"
cp "$IMG" "$RUN_IMG"
QEMU_PID=""
cleanup() {
	[ -n "$QEMU_PID" ] && kill -9 "$QEMU_PID" 2>/dev/null || true
	rm -f "$RUN_IMG"
}
trap cleanup EXIT INT TERM

# ── Run ────────────────────────────────────────────────────────────────────
ACCEL_ARGS=""
if [ -w /dev/kvm ] && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw kvm; then
	ACCEL_ARGS="-accel kvm -cpu host,+invtsc"
elif [ "$(uname)" = "Darwin" ] && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw hvf; then
	ACCEL_ARGS="-accel hvf -cpu host"
fi

echo "[RUN] Booting QEMU with $RUN_IMG as root (label $IMG_LABEL)..."
: >"$LOG"
qemu-system-x86_64 $ACCEL_ARGS -m "${DEBIAN_MEM_MB:-1024}" -smp "${DEBIAN_SMP:-2}" \
	-cdrom "$ISO" \
	-serial stdio -serial null -display none -monitor none -no-reboot \
	-drive file="$RUN_IMG",if=none,id=debroot,format=raw \
	-device virtio-blk-pci,drive=debroot \
	-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	${EXTRA_QEMU_ARGS:-} >"$LOG" 2>&1 &
QEMU_PID=$!

DONE_PATTERN="${DEBIAN_DONE_PATTERN:-DEBIAN-SMOKE: done|KERNEL PANIC|\[PANIC\]}"
start_ts=$(date +%s)
reported=0
while :; do
	lines=$(wc -l <"$LOG" | tr -d ' ')
	if [ "$lines" -gt "$reported" ]; then
		sed -n "$((reported + 1)),${lines}p" "$LOG" | grep -a "DEBIAN-SMOKE:" || true
		reported=$lines
	fi
	if grep -qa -E "$DONE_PATTERN" "$LOG" 2>/dev/null; then
		break
	fi
	if ! kill -0 "$QEMU_PID" 2>/dev/null; then
		sleep 1
		echo "[debian-smoke] QEMU exited before the done marker" >>"$LOG"
		break
	fi
	now_ts=$(date +%s)
	if [ $((now_ts - start_ts)) -ge "$TIMEOUT" ]; then
		echo "[debian-smoke] timeout after ${TIMEOUT}s" >>"$LOG"
		break
	fi
	sleep 1
done
# Kill BY PID — never pkill -f, which would match this script's own command line.
kill -9 "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

# ── Check ──────────────────────────────────────────────────────────────────
echo ""
echo "[CHECK] $LOG"
check_output() {
	if grep -qa "$1" "$LOG" 2>/dev/null; then
		pass "$2"
	else
		fail "$2" "missing expected output: $1"
	fi
}

check_output "DEBIAN-SMOKE: ok stage1-dash" "stage1: Debian /bin/dash (glibc dynamic ELF) ran"
check_output "DEBIAN-SMOKE: ok stage2-coreutils" "stage2: ls/cat/mount/ps from the distro"
check_output "DEBIAN-SMOKE: ok stage3-init" "stage3: running under a real init"
check_output "DEBIAN-SMOKE: done" "harness reached the end"

if grep -qa -E "KERNEL PANIC|\[PANIC\]" "$LOG" 2>/dev/null; then
	fail "no kernel panic" "the log contains a panic"
else
	pass "no kernel panic"
fi

if grep -qa "DEBIAN-SMOKE: FAIL" "$LOG" 2>/dev/null; then
	echo ""
	echo "  in-guest failures reported by the harness:"
	grep -a "DEBIAN-SMOKE: FAIL" "$LOG" | sed 's/^/    /'
fi

echo ""
printf "Results: ${GREEN}%d passed${NC}, ${RED}%d failed${NC}\n" "$PASSED" "$FAILED"
echo "Log: $LOG"
[ "$FAILED" -eq 0 ] || exit 1
exit 0
