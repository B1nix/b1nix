#!/bin/sh
# Quick M32 smoke test — boots ONE QEMU instance and checks only M32/M32b markers.
#
# Usage:
#   sh tests/smoke-m32.sh            # build + run
#   SKIP_BUILD=1 sh tests/smoke-m32.sh   # reuse existing ISO, run only
#   REUSE_LOG=1  sh tests/smoke-m32.sh   # skip QEMU entirely, grep last log
#
# Env overrides:
#   TIMEOUT=300   SMOKE_MEM_MB=1024   SMOKE_VERBOSE=1
#   B1NIX_NET_RESTRICT=off  (enable real internet for ext-http checks)

set -e

ARCH="${1:-x86_64}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_DIR"

OS="$(uname -s)"
if [ "$OS" = "Darwin" ]; then
	NPROC=$(sysctl -n hw.ncpu)
else
	NPROC=$(nproc)
fi

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASSED=0
FAILED=0

pass() { printf "  ${GREEN}PASS${NC} %s\n" "$1"; PASSED=$((PASSED + 1)); }
fail() { printf "  ${RED}FAIL${NC} %s  [missing: %s]\n" "$1" "$2"; FAILED=$((FAILED + 1)); }
info() { printf "  ${YELLOW}INFO${NC} %s\n" "$1"; }

check() {
	local log="$1" pattern="$2" desc="$3"
	if grep -q "$pattern" "$log" 2>/dev/null; then
		pass "$desc"
	else
		fail "$desc" "$pattern"
	fi
}

# ── 1. Build ────────────────────────────────────────────────────────────────
LOG="$PROJECT_DIR/smoke_run/m32-quick.log"
mkdir -p "$PROJECT_DIR/smoke_run"

if [ "${REUSE_LOG:-0}" = "1" ]; then
	if [ ! -f "$LOG" ]; then
		echo "REUSE_LOG=1 but $LOG not found"
		exit 1
	fi
	info "Reusing existing log: $LOG"
else
	if [ "${SKIP_BUILD:-0}" != "1" ]; then
		echo "[BUILD] Building ISO for $ARCH..."
		BUILD_LOG="$PROJECT_DIR/smoke_run/m32-quick-build.log"
		make -j"$NPROC" ARCH="$ARCH" \
			KERNEL_CMDLINE="b1nix.test=1 b1nix.ssh-loopback=1" \
			iso >"$BUILD_LOG" 2>&1 || {
			echo "BUILD FAILED — see $BUILD_LOG"
			tail -40 "$BUILD_LOG"
			exit 1
		}
		echo "  build/$ARCH/b1nix.iso ready"
	else
		echo "  (SKIP_BUILD=1 — reusing existing ISO)"
	fi

	# ── 2. Disk images ──────────────────────────────────────────────────────
	SATA_IMG="$PROJECT_DIR/smoke_run/m32-sata-$$.img"
	NVME_IMG="$PROJECT_DIR/smoke_run/m32-nvme-$$.img"
	SWAP_IMG="$PROJECT_DIR/smoke_run/m32-swap-$$.img"
	trap 'rm -f "$SATA_IMG" "$NVME_IMG" "$SWAP_IMG"; pgrep -f "b1nix.iso" | xargs kill 2>/dev/null || true' EXIT INT TERM

	dd if=/dev/zero of="$SATA_IMG" bs=1M count=4  2>/dev/null
	dd if=/dev/zero of="$NVME_IMG" bs=1M count=4  2>/dev/null
	dd if=/dev/zero of="$SWAP_IMG" bs=1M count=2  2>/dev/null

	MKE2FS="/opt/homebrew/opt/e2fsprogs/sbin/mke2fs"
	[ -x "$MKE2FS" ] || MKE2FS="$(command -v mke2fs 2>/dev/null || echo /sbin/mke2fs)"
	"$MKE2FS" -F -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q "$SATA_IMG" 2>/dev/null
	"$MKE2FS" -F -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q "$NVME_IMG" 2>/dev/null

	# ── 3. QEMU ─────────────────────────────────────────────────────────────
	TIMEOUT="${TIMEOUT:-300}"
	MEM="${SMOKE_MEM_MB:-1024}"
	RESTRICT="${B1NIX_NET_RESTRICT:-on}"

	accel_args=""
	if [ -w /dev/kvm ] && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw kvm; then
		accel_args="-accel kvm -cpu host"
	elif [ "$OS" = "Darwin" ] && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw hvf; then
		accel_args="-accel hvf -cpu host"
	fi

	echo "[RUN]   Booting QEMU (timeout ${TIMEOUT}s)..."
	rm -f "$LOG"

	qemu-system-x86_64 \
		$accel_args -m "$MEM" \
		-cdrom "$PROJECT_DIR/build/$ARCH/b1nix.iso" \
		-serial stdio -display none -monitor none -no-reboot \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-device virtio-gpu-pci \
		-netdev "user,id=net0,restrict=${RESTRICT}" -device virtio-net-pci,netdev=net0 \
		-netdev "user,id=net1,restrict=${RESTRICT}" -device e1000,netdev=net1 \
		-device ich9-ahci,id=ahci \
			-drive file="$SATA_IMG",if=none,id=satadrive,format=raw \
			-device ide-hd,drive=satadrive,bus=ahci.0 \
			-drive file="$SWAP_IMG",if=none,id=swapdrive,format=raw \
			-device ide-hd,drive=swapdrive,bus=ahci.1 \
			-drive file="$NVME_IMG",if=none,id=nvmedrive,format=raw \
			-device nvme,serial=deadbeef,drive=nvmedrive \
		>"$LOG" 2>&1 &
	QEMU_PID=$!

	# Watcher: stream progress + kill on done/panic/timeout
	(
		set +e
		start_ts=$(date +%s)
		reported=0
		stall_ts=$start_ts
		stall_after=120
		done_pattern="B1NIX-TEST: done|KERNEL PANIC|\[PANIC\]"
		while :; do
			lc=$(wc -l <"$LOG" 2>/dev/null | tr -d ' ')
			lc=${lc:-0}
			if [ "$lc" -gt "$reported" ]; then
				sed -n "$((reported+1)),${lc}p" "$LOG" | while IFS= read -r line; do
					case "$line" in
						M32*:*|M32B*:*|DNS-SMOKE*:*|B1NIX-TEST*|*PANIC*)
							case "$line" in
								*": ok"*|*": done"*|"B1NIX-TEST: done")
									printf "  ${GREEN}%s${NC}\n" "$line" ;;
								*": fail"*|*": FAIL"*|*PANIC*)
									printf "  ${RED}%s${NC}\n" "$line" ;;
								*)
									printf "  ${YELLOW}%s${NC}\n" "$line" ;;
							esac ;;
					esac
				done
				reported=$lc
				stall_ts=$(date +%s)
			fi
			grep -qaE "$done_pattern" "$LOG" 2>/dev/null && break
			! kill -0 "$QEMU_PID" 2>/dev/null && sleep 1 && break
			now=$(date +%s)
			[ $((now - start_ts)) -ge "$TIMEOUT" ] && {
				echo "[m32] TIMEOUT after ${TIMEOUT}s" >>"$LOG"; break; }
			[ $((now - stall_ts)) -ge "$stall_after" ] && {
				echo "[m32] STALL — no output for ${stall_after}s" >>"$LOG"; break; }
			sleep 1
		done
		kill "$QEMU_PID" 2>/dev/null || true
	) &
	WATCH_PID=$!
	wait $WATCH_PID 2>/dev/null || true
	kill -9 "$QEMU_PID" 2>/dev/null || true
	wait "$QEMU_PID" 2>/dev/null || true
fi

# ── 4. Check results ────────────────────────────────────────────────────────
echo ""
echo "=== M32 Smoke Results ==="

# Core NET smoke
check "$LOG" "M32-NET: start"              "M32 smoke starts"
check "$LOG" "M32-NET: ok select-timeout-zero"  "select() zero-timeout"
check "$LOG" "M32-NET: ok select-pipe-ready"    "select() pipe readable"
check "$LOG" "M32-NET: ok select-multi-fd"      "select() multi-fd"
check "$LOG" "M32-NET: ok sockopt-reuseaddr"    "SO_REUSEADDR round-trip"
check "$LOG" "M32-NET: ok sockopt-nodelay"      "TCP_NODELAY round-trip"
check "$LOG" "M32-NET: ok sockopt-sotype"       "SO_TYPE getsockopt"
check "$LOG" "M32-NET: ok getsockname"          "getsockname"
check "$LOG" "M32-NET: ok getpeername"          "getpeername"
check "$LOG" "M32-NET: ok shutdown-wr"          "shutdown(SHUT_WR)"
check "$LOG" "M32-NET: ok tcp-echo"             "TCP loopback echo"
check "$LOG" "M32-NET: ok http-get"             "HTTP client/server over TCP"
check "$LOG" "M32-NET: ok wget-loopback"        "wget HTTP loopback"
check "$LOG" "M32-NET: ok tcp-server"           "TCP listener lifecycle"
check "$LOG" "M32-NET: ok inet-pton-ntop"       "inet_pton/inet_ntop (v4)"
check "$LOG" "M32-NET: ok inet6-pton-ntop"      "inet_pton/inet_ntop (v6)"
check "$LOG" "M32-NET: ok getaddrinfo"          "getaddrinfo (v4)"
check "$LOG" "M32-NET: ok getaddrinfo-inet6"    "getaddrinfo (v6)"
check "$LOG" "M32-NET: ok getnameinfo"          "getnameinfo round-trip"
check "$LOG" "M32-NET: ok tcp6-loopback"        "TCP over IPv6 loopback"
check "$LOG" "M32-NET: ok udp6-loopback"        "UDP over IPv6 loopback"

# PTY
check "$LOG" "M32B-PTY: ok openpty"   "openpty /dev/ptmx + /dev/pts/N"
check "$LOG" "M32B-PTY: ok winsize"   "TIOCSWINSZ/TIOCGWINSZ"
check "$LOG" "M32B-PTY: ok canonical" "pty canonical mode"
check "$LOG" "M32B-PTY: ok echo"      "pty ECHO"
check "$LOG" "M32B-PTY: ok raw"       "pty raw mode"
check "$LOG" "M32B-PTY: ok hangup"    "pty master close → slave EOF"

# Crypto / session
check "$LOG" "M32B-SESS: ok env-execve"   "env survives execve"
check "$LOG" "M32B-CRYPTO: ok getrandom"  "getrandom()"
check "$LOG" "M32B-CRYPTO: ok sha512"     "SHA-512 FIPS vector"
check "$LOG" "M32B-CRYPTO: ok crypt"      "crypt() shadow path"

# ── SSH / Dropbear (the key part) ──
echo ""
echo "  -- SSH / Dropbear --"
check "$LOG" "M32B-SSH: ok dropbearkey"       "dropbearkey Ed25519 host key"
check "$LOG" "M32B-SSH: ok handshake"         "SSH localhost login (password auth)"
check "$LOG" "M32B-SSH: ok negauth"           "SSH rejects wrong password"
check "$LOG" "M32B-SSH: ok pty"               "SSH interactive pty shell"
check "$LOG" "M32B-SSH: ok service-lifecycle" "SSH service wrapper lifecycle"

# TCP sliding window
check "$LOG" "M32-TCP: ok window-throttle" "TCP sliding-window throttle"

check "$LOG" "M32-NET: done" "M32 smoke completes"

# Panic guard
if grep -qE "KERNEL PANIC|\[PANIC\]" "$LOG" 2>/dev/null; then
	fail "no kernel panic" "PANIC in log"
else
	pass "no kernel panic"
fi

# ── 5. Summary ──────────────────────────────────────────────────────────────
echo ""
echo "=== Summary ==="
printf "  ${GREEN}PASS${NC}: %d\n" "$PASSED"
printf "  ${RED}FAIL${NC}: %d\n" "$FAILED"
echo ""
echo "  Full log: $LOG"

[ "$FAILED" -eq 0 ]
