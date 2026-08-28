#!/bin/sh
# arch-smoke.sh — boot b1nix with a stock Arch Linux root filesystem whose PID 1
# is Arch's own systemd, headless, console on the serial line.
#
#   sh tools/images/mk-arch-image.sh   # once, or after any image change
#   sh tests/arch-smoke.sh [x86_64]
#
# This is the Debian systemd test asked of a rolling distribution. Debian
# bookworm ships systemd 252 on glibc 2.36; Arch ships systemd 261 on glibc
# 2.44, and the newer manager reaches for kernel interfaces the older one never
# touched -- the new mount API for its sandboxing, pidfds for its children,
# openat2 for path resolution. Everything it asks for that this kernel does not
# answer shows up here and nowhere else.
#
# Skips cleanly (exit 0) when the image has not been built, so it can be wired
# into CI on a host with no network.
set -e

ARCH="${1:-x86_64}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build/$ARCH}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$PROJECT_DIR/$BUILD_DIR" ;; esac

IMG="$BUILD_DIR/arch-systemd.ext4"
IMG_LABEL="${IMG_LABEL:-b1nix-arch}"
RUN_TAG="${RUN_TAG:-$(date +%Y%m%d-%H%M%S)-$$}"
LOG="${ARCH_LOG:-$PROJECT_DIR/smoke_run/b1nix-arch-boot-$RUN_TAG.log}"
BUILD_LOG="${ARCH_BUILD_LOG:-$PROJECT_DIR/smoke_run/b1nix-arch-build-$RUN_TAG.log}"
ISO="$BUILD_DIR/${B1NIX_ISO_NAME:-b1nix-arch.iso}"
TIMEOUT="${TIMEOUT:-600}"

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

echo "=== B1NIX Arch Linux systemd Boot Test ($ARCH) ==="

if [ ! -f "$IMG" ]; then
	printf "  ${YELLOW}skipped${NC}: %s not built — run sh tools/images/mk-arch-image.sh first\n" "$IMG"
	exit 0
fi

if [ -n "${SMOKE_JOBS:-}" ]; then
	NPROC="$SMOKE_JOBS"
elif [ "$(uname -s)" = "Darwin" ]; then
	NPROC=$(sysctl -n hw.ncpu)
else
	NPROC=$(nproc)
fi

# Deliberately NO console=ttyS0: systemd's getty generator would turn that into
# serial-getty@ttyS0.service, which is BoundTo= a .device unit. b1nix's console
# is the serial line regardless, and the image enables console-getty.service.
# ARCH_INIT exists for one purpose: to put a wrapper in front of systemd when
# systemd itself has gone silent. /b1nix-probe-init.sh prints markers over
# stdout, /dev/console and /dev/kmsg and then execs the real manager, which
# separates "PID 1 cannot be heard" from "PID 1 has nothing to say".
CMDLINE="root=LABEL=$IMG_LABEL init=${ARCH_INIT:-/sbin/init} \
systemd.unit=${ARCH_TARGET:-multi-user.target} ${ARCH_EXTRA_CMDLINE:-}"

if [ "${SKIP_BUILD:-0}" = "1" ]; then
	[ -f "$ISO" ] || { printf "  ${RED}no prebuilt %s${NC}\n" "$ISO"; exit 1; }
	echo "  (SKIP_BUILD=1 — reusing $ISO)"
else
	echo "[BUILD] Building kernel ISO for the Arch boot..."
	if ! (cd "$PROJECT_DIR" && make -j"$NPROC" ARCH="$ARCH" ${SMOKE_MAKE_ARGS:-} \
		KERNEL_CMDLINE="$CMDLINE" iso) >"$BUILD_LOG" 2>&1; then
		printf "  ${RED}BUILD FAILED${NC} (log: %s)\n" "$BUILD_LOG"
		tail -60 "$BUILD_LOG"
		exit 1
	fi
	cp "$BUILD_DIR/b1nix.iso" "$ISO"
	sync
	pass "kernel builds without errors"
fi

# A per-run scratch copy, so a run that writes to its root never changes the
# image the next run starts from.
RUN_IMG="$PROJECT_DIR/smoke_run/arch-root-$$.img"
cp "$IMG" "$RUN_IMG"
QEMU_PID=""
cleanup() {
	[ -n "$QEMU_PID" ] && kill -9 "$QEMU_PID" 2>/dev/null || true
	rm -f "$RUN_IMG"
}
trap cleanup EXIT INT TERM

ACCEL_ARGS=""
if [ -w /dev/kvm ] && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw kvm; then
	ACCEL_ARGS="-accel kvm -cpu host,+invtsc"
elif [ "$(uname)" = "Darwin" ] && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw hvf; then
	ACCEL_ARGS="-accel hvf -cpu host"
fi

echo "[RUN] Booting QEMU with $RUN_IMG as root (label $IMG_LABEL)..."
: >"$LOG"
qemu-system-x86_64 $ACCEL_ARGS -m "${ARCH_MEM_MB:-2560}" -smp "${ARCH_SMP:-2}" \
	-cdrom "$ISO" \
	-serial stdio -serial null -display none -monitor none -no-reboot \
	-drive file="$RUN_IMG",if=none,id=sdroot,format=raw \
	-device virtio-blk-pci,drive=sdroot \
	-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	${EXTRA_QEMU_ARGS:-} >"$LOG" 2>&1 &
QEMU_PID=$!

DONE_PATTERN="${ARCH_DONE_PATTERN:-ARCH-SMOKE: done|KERNEL PANIC|\[PANIC\]}"
start_ts=$(date +%s)
done_ts=0
reported=0
while :; do
	lines=$(wc -l <"$LOG" | tr -d ' ')
	if [ "$lines" -gt "$reported" ]; then
		sed -n "$((reported + 1)),${lines}p" "$LOG" | grep -a "ARCH-SMOKE:" || true
		reported=$lines
	fi
	if grep -qa -E "$DONE_PATTERN" "$LOG" 2>/dev/null; then
		# The harness is finished, but the login prompt is not its output: the
		# getty is Type=idle, so systemd starts it once the boot transaction
		# settles, which is after the harness has run.
		if [ "$done_ts" = "0" ]; then
			done_ts=$(date +%s)
		fi
		if grep -qa -E "b1nix-arch login:|localhost login:" "$LOG" 2>/dev/null; then
			break
		fi
		if grep -qa -E "KERNEL PANIC|\[PANIC\]" "$LOG" 2>/dev/null; then
			break
		fi
		now_ts=$(date +%s)
		if [ $((now_ts - done_ts)) -ge "${LOGIN_GRACE:-25}" ]; then
			break
		fi
	fi
	if ! kill -0 "$QEMU_PID" 2>/dev/null; then
		sleep 1
		echo "[arch-smoke] QEMU exited before the done marker" >>"$LOG"
		break
	fi
	now_ts=$(date +%s)
	if [ $((now_ts - start_ts)) -ge "$TIMEOUT" ]; then
		echo "[arch-smoke] timeout after ${TIMEOUT}s" >>"$LOG"
		break
	fi
	sleep 1
done
# Kill BY PID — never pkill -f, which would match this script's own command line.
kill -9 "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

echo ""
echo "[CHECK] $LOG"
check_output() {
	if grep -qa -- "$1" "$LOG" 2>/dev/null; then
		pass "$2"
	else
		fail "$2" "missing expected output: $1"
	fi
}

# Every one of these is a marker the guest prints only after the named thing
# really happened. None of them is "the kernel did not panic".
check_output "ARCH-SMOKE: ok arch-release" "the booted tree is Arch (/etc/arch-release)"
check_output "ARCH-SMOKE: ok pid1-systemd" "Arch's systemd is PID 1"
check_output "ARCH-SMOKE: ok cgroup2" "cgroup v2 hierarchy, unit's own cgroup"
check_output "ARCH-SMOKE: ok systemctl-state" "systemctl reports the manager's state"
check_output "ARCH-SMOKE: ok unit-active systemd-journald.service" "journald active"
check_output "ARCH-SMOKE: ok unit-active sysinit.target" "sysinit.target reached"
check_output "ARCH-SMOKE: ok unit-active basic.target" "basic.target reached"
check_output "ARCH-SMOKE: ok unit-active multi-user.target" "multi-user.target reached"
check_output "ARCH-SMOKE: ok unit-active graphical.target" "graphical.target reached - the boot gets past multi-user"
check_output "ARCH-SMOKE: ok journalctl" "journalctl reads the journal back"
check_output "ARCH-SMOKE: ok systemd-run" "systemd-run starts a transient unit"
check_output "ARCH-SMOKE: ok unit-active systemd-udevd.service" "systemd-udevd active"
check_output "ARCH-SMOKE: ok udevadm-ping" "systemd-udevd answers udevadm --ping"
check_output "ARCH-SMOKE: ok device-unit" "a .device unit is active"
check_output "ARCH-SMOKE: ok unit-active systemd-logind.service" "systemd-logind active"
check_output "ARCH-SMOKE: ok logind-answers" "systemd-logind answers loginctl"
check_output "ARCH-SMOKE: ok console-getty" "a getty runs on the console"
check_output "ARCH-SMOKE: ok socket-listening" "systemd holds a listening AF_UNIX socket for a .socket unit"
check_output "ARCH-SMOKE: ok socket-activation" "connecting to that socket starts the service behind it"
check_output "ARCH-SMOKE: ok socket-listening-tcp" "the same for a socket unit on loopback TCP"
check_output "ARCH-SMOKE: ok socket-activation-tcp" "connecting to 127.0.0.1 starts the service behind it"
check_output "ARCH-SMOKE: ok notify-ready" "a Type=notify service reports READY=1 over NOTIFY_SOCKET"
check_output "ARCH-SMOKE: ok private-tmp" "PrivateTmp really is private - on systemd 256+ this is the new mount API"
check_output "ARCH-SMOKE: ok protect-system" "ProtectSystem=strict refuses the write rather than letting it through"
check_output "ARCH-SMOKE: ok timer-fires" "a .timer unit fires and runs the service it names"
check_output "ARCH-SMOKE: ok restart-on-failure" "Restart=on-failure notices the exit status and starts the unit again"
check_output "ARCH-SMOKE: ok journal-filter-unit" "journalctl -u returns one unit's entries, so the journal is indexed"
check_output "ARCH-SMOKE: ok unit-enable-disable" "systemctl enable/disable moves the unit between enabled and disabled"
check_output "ARCH-SMOKE: ok unit-mask-refuses" "a masked unit refuses to start even when asked directly"
# agetty's own prompt on the serial line — printed by login(1), not by us.
if grep -qa -E "b1nix-arch login:|localhost login:" "$LOG" 2>/dev/null; then
	pass "login prompt on the serial console"
else
	fail "login prompt on the serial console" "no 'login:' in the serial log"
fi
check_output "ARCH-SMOKE: done" "harness reached the end"

if grep -qa -E "KERNEL PANIC|\[PANIC\]" "$LOG" 2>/dev/null; then
	fail "no kernel panic" "the log contains a panic"
else
	pass "no kernel panic"
fi

if grep -qa "ARCH-SMOKE: FAIL" "$LOG" 2>/dev/null; then
	echo ""
	echo "  in-guest failures reported by the harness:"
	grep -a "ARCH-SMOKE: FAIL" "$LOG" | sed 's/^/    /'
fi

# Which syscalls this distribution asked for that the kernel has no route for.
# Not a check -- a report. It is the whole reason a newer userspace is worth
# booting, and it is invisible unless something prints it.
if grep -qa "unmapped syscall" "$LOG" 2>/dev/null; then
	echo ""
	echo "  syscalls Arch asked for that this kernel does not implement:"
	grep -ao "unmapped syscall[^\"]*nr=[0-9]*" "$LOG" | sed 's/.*nr=/    nr=/' |
		sort -n -k1.4 | uniq -c | sort -rn | head -20
fi

if [ "${ARCH_KEEP_LOGS:-1}" != "0" ] && [ -s "$LOG" ]; then
	ln -sf "$LOG" "$PROJECT_DIR/smoke_run/b1nix-arch-boot-last.log" 2>/dev/null
fi

echo ""
printf "Results: ${GREEN}%d passed${NC}, ${RED}%d failed${NC}\n" "$PASSED" "$FAILED"
echo "Log: $LOG"
[ "$FAILED" -eq 0 ] || exit 1
exit 0
