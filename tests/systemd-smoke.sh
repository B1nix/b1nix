#!/bin/sh
# systemd boot test: boot b1nix with a Debian root filesystem whose PID 1 is
# Debian's own systemd, headless, console on the serial line.
#
#   sh tools/images/mk-debian-image.sh   # once
#   PROFILE=systemd sh tools/images/mk-debian-image.sh
#   sh tests/systemd-smoke.sh [x86_64]
#
# Skips cleanly (exit 0) when the image has not been built, so it can be wired
# into CI on a host with no network.
set -e

ARCH="${1:-x86_64}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build/$ARCH}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$PROJECT_DIR/$BUILD_DIR" ;; esac

IMG="$BUILD_DIR/debian-systemd.ext4"
IMG_LABEL="${IMG_LABEL:-b1nix-systemd}"
LOG="$PROJECT_DIR/smoke_run/b1nix-systemd-boot.log"
BUILD_LOG="$PROJECT_DIR/smoke_run/b1nix-systemd-build.log"
ISO="$BUILD_DIR/${B1NIX_ISO_NAME:-b1nix-systemd.iso}"
# systemd's boot is not a shell script: it starts units in parallel and waits
# on timers. 240 s is a deliberately longer bound than the 120 s of the normal
# smoke, and it is still a bound.
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

echo "=== B1NIX systemd Boot Test ($ARCH) ==="

if [ ! -f "$IMG" ]; then
	printf "  ${YELLOW}skipped${NC}: %s not built — run PROFILE=systemd tools/images/mk-debian-image.sh first\n" "$IMG"
	exit 0
fi

if [ -n "${SMOKE_JOBS:-}" ]; then
	NPROC="$SMOKE_JOBS"
elif [ "$(uname -s)" = "Darwin" ]; then
	NPROC=$(sysctl -n hw.ncpu)
else
	NPROC=$(nproc)
fi

# ── Kernel command line ────────────────────────────────────────────────────
# systemd.unit picks the target explicitly rather than relying on a
# /etc/systemd/system/default.target symlink the image does not ship.
#
# Deliberately NO console=ttyS0: systemd's getty generator would turn that into
# serial-getty@ttyS0.service, which is bound to a .device unit that only udev
# can activate — and this image has no udev. b1nix's console is the serial line
# regardless, and the image enables console-getty.service, which runs agetty on
# /dev/console and needs nothing but that device.
CMDLINE="root=LABEL=$IMG_LABEL init=/sbin/init \
systemd.unit=${SYSTEMD_TARGET:-multi-user.target} ${SYSTEMD_EXTRA_CMDLINE:-}"

# ── Build ──────────────────────────────────────────────────────────────────
if [ "${SKIP_BUILD:-0}" = "1" ]; then
	[ -f "$ISO" ] || { printf "  ${RED}no prebuilt %s${NC}\n" "$ISO"; exit 1; }
	echo "  (SKIP_BUILD=1 — reusing $ISO)"
else
	echo "[BUILD] Building kernel ISO for the systemd boot..."
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

# ── Per-run scratch copy of the image ──────────────────────────────────────
RUN_IMG="$PROJECT_DIR/smoke_run/systemd-root-$$.img"
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
qemu-system-x86_64 $ACCEL_ARGS -m "${SYSTEMD_MEM_MB:-1536}" -smp "${SYSTEMD_SMP:-2}" \
	-cdrom "$ISO" \
	-serial stdio -serial null -display none -monitor none -no-reboot \
	-drive file="$RUN_IMG",if=none,id=sdroot,format=raw \
	-device virtio-blk-pci,drive=sdroot \
	-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	${EXTRA_QEMU_ARGS:-} >"$LOG" 2>&1 &
QEMU_PID=$!

DONE_PATTERN="${SYSTEMD_DONE_PATTERN:-SYSTEMD-SMOKE: done|KERNEL PANIC|\[PANIC\]}"
start_ts=$(date +%s)
done_ts=0
reported=0
while :; do
	lines=$(wc -l <"$LOG" | tr -d ' ')
	if [ "$lines" -gt "$reported" ]; then
		sed -n "$((reported + 1)),${lines}p" "$LOG" | grep -a "SYSTEMD-SMOKE:" || true
		reported=$lines
	fi
	if grep -qa -E "$DONE_PATTERN" "$LOG" 2>/dev/null; then
		# The harness is finished, but the login prompt is not its output: the
		# getty is Type=idle, so systemd starts it once the boot transaction
		# settles, which is after the harness has run. Keep the machine alive
		# for a bounded grace period so agetty's prompt lands in the log too.
		if [ "$done_ts" = "0" ]; then
			done_ts=$(date +%s)
		fi
		if grep -qa -E "b1nix login:|localhost login:" "$LOG" 2>/dev/null; then
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
		echo "[systemd-smoke] QEMU exited before the done marker" >>"$LOG"
		break
	fi
	now_ts=$(date +%s)
	if [ $((now_ts - start_ts)) -ge "$TIMEOUT" ]; then
		echo "[systemd-smoke] timeout after ${TIMEOUT}s" >>"$LOG"
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
	if grep -qa -- "$1" "$LOG" 2>/dev/null; then
		pass "$2"
	else
		fail "$2" "missing expected output: $1"
	fi
}

# Every one of these is a marker the guest prints only after the named thing
# really happened. None of them is "the kernel did not panic".
check_output "SYSTEMD-SMOKE: ok pid1-systemd" "systemd is PID 1"
check_output "SYSTEMD-SMOKE: ok cgroup2" "cgroup v2 hierarchy, unit's own cgroup"
check_output "SYSTEMD-SMOKE: ok systemctl-state" "systemctl reports the manager's state"
check_output "SYSTEMD-SMOKE: ok unit-active systemd-journald.service" "journald active"
check_output "SYSTEMD-SMOKE: ok unit-active sysinit.target" "sysinit.target reached"
check_output "SYSTEMD-SMOKE: ok unit-active basic.target" "basic.target reached"
check_output "SYSTEMD-SMOKE: ok unit-active multi-user.target" "multi-user.target reached"
check_output "SYSTEMD-SMOKE: ok journalctl" "journalctl reads the journal back"
check_output "SYSTEMD-SMOKE: ok systemd-run" "systemd-run starts a transient unit"
check_output "SYSTEMD-SMOKE: ok unit-active systemd-udevd.service" "systemd-udevd active"
check_output "SYSTEMD-SMOKE: ok udevadm-ping" "systemd-udevd answers udevadm --ping"
check_output "SYSTEMD-SMOKE: ok device-unit" "a .device unit is active"
check_output "SYSTEMD-SMOKE: ok unit-active systemd-logind.service" "systemd-logind active"
check_output "SYSTEMD-SMOKE: ok logind-answers" "systemd-logind answers loginctl"
check_output "SYSTEMD-SMOKE: ok console-getty" "a getty runs on the console"

# The unit types nothing exercised before: 16 checks stood against an image
# carrying 225 unit files and 45 helper daemons. Each of these drives a
# different kernel surface -- AF_UNIX listeners and fd inheritance across exec,
# datagram readiness, mount namespaces, timers, SIGCHLD to PID 1, the journal's
# index.
check_output "SYSTEMD-SMOKE: ok socket-listening" "systemd holds a listening AF_UNIX socket for a .socket unit"
check_output "SYSTEMD-SMOKE: ok socket-activation" "connecting to that socket starts the service behind it - systemd's central idea"
check_output "SYSTEMD-SMOKE: ok socket-listening-tcp" "the same for a socket unit on loopback TCP: systemd holds the listening AF_INET socket"
check_output "SYSTEMD-SMOKE: ok socket-activation-tcp" "connecting to 127.0.0.1 starts the service behind it, so the loopback datapath carries a real connection"
check_output "SYSTEMD-SMOKE: ok notify-ready" "a Type=notify service reports READY=1 over NOTIFY_SOCKET and the manager believes it"
check_output "SYSTEMD-SMOKE: ok private-tmp" "PrivateTmp really is private: the unit's file is NOT visible outside its namespace"
check_output "SYSTEMD-SMOKE: ok protect-system" "ProtectSystem=strict refuses the write rather than letting it through"
check_output "SYSTEMD-SMOKE: ok timer-fires" "a .timer unit fires and runs the service it names"
check_output "SYSTEMD-SMOKE: ok restart-on-failure" "Restart=on-failure notices the exit status and starts the unit again"
check_output "SYSTEMD-SMOKE: ok journal-filter-unit" "journalctl -u returns one unit's entries, so the journal is indexed rather than only appended"
check_output "SYSTEMD-SMOKE: ok unit-enable-disable" "systemctl enable/disable moves the unit between enabled and disabled"
check_output "SYSTEMD-SMOKE: ok unit-mask-refuses" "a masked unit refuses to start even when asked directly"
# agetty's own prompt on the serial line — printed by login(1), not by us.
if grep -qa -E "b1nix login:|localhost login:" "$LOG" 2>/dev/null; then
	pass "login prompt on the serial console"
else
	fail "login prompt on the serial console" "no 'login:' in the serial log"
fi
check_output "SYSTEMD-SMOKE: done" "harness reached the end"

if grep -qa -E "KERNEL PANIC|\[PANIC\]" "$LOG" 2>/dev/null; then
	fail "no kernel panic" "the log contains a panic"
else
	pass "no kernel panic"
fi

if grep -qa "SYSTEMD-SMOKE: FAIL" "$LOG" 2>/dev/null; then
	echo ""
	echo "  in-guest failures reported by the harness:"
	grep -a "SYSTEMD-SMOKE: FAIL" "$LOG" | sed 's/^/    /'
fi

# Keep this run's log. Every run wrote to the same path and truncated it at
# start, so a run destroyed the evidence for the one before it -- which matters
# most exactly when comparing a fix against the boot that motivated it. The
# snapshot is timestamped and the newest is also linked as -last.log; set
# SYSTEMD_KEEP_LOGS=0 to opt out.
if [ "${SYSTEMD_KEEP_LOGS:-1}" != "0" ] && [ -s "$LOG" ]; then
	SNAP="$PROJECT_DIR/smoke_run/b1nix-systemd-boot-$(date +%Y%m%d-%H%M%S).log"
	cp "$LOG" "$SNAP" 2>/dev/null &&
		cp "$LOG" "$PROJECT_DIR/smoke_run/b1nix-systemd-boot-last.log" 2>/dev/null
	echo "Kept: $SNAP"
fi

echo ""
printf "Results: ${GREEN}%d passed${NC}, ${RED}%d failed${NC}\n" "$PASSED" "$FAILED"
echo "Log: $LOG"
[ "$FAILED" -eq 0 ] || exit 1
exit 0
