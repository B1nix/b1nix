#!/bin/sh
# M32c — host-to-guest SSH over QEMU user-mode networking (hostfwd).
#
# This is the "external SSH access" smoke: it boots b1nix in NORMAL mode (no
# in-kernel test suite) with DHCP enabled and the SSH daemon bound to all
# interfaces (b1nix.ssh-external=1), forwards the host's 127.0.0.1:2222 to the
# guest's :22, then logs in from THIS host with the OpenSSH client (password
# auth driven by expect) and runs a command, verifying a guest-produced marker.
#
# It is intentionally separate from tests/smoke.sh so the default CI run stays
# deterministic and never exposes a forwarded port by accident.
#
# Usage: sh tests/ssh-hostfwd.sh [x86|x86_64]
#
# Requires on the host: qemu-system-x86_64, ssh (OpenSSH client), expect.

set -u

ARCH="${1:-x86_64}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_DIR"

HOSTPORT="${HOSTPORT:-2222}"
# x86_64 emulates notably slower than i386 under TCG, so give 64-bit more room.
if [ "$ARCH" = "x86_64" ]; then
	BOOT_TIMEOUT="${BOOT_TIMEOUT:-150}"
	SSH_TIMEOUT="${SSH_TIMEOUT:-120}"
else
	BOOT_TIMEOUT="${BOOT_TIMEOUT:-90}"
	SSH_TIMEOUT="${SSH_TIMEOUT:-60}"
fi
# This test is specifically about EXTERNAL reachability, so the guest is not
# isolated by default (restrict=off) — the host needs the hostfwd path to the
# listener. Set B1NIX_NET_RESTRICT=on to also isolate the guest from the host's
# real network (QEMU still serves DHCP and honours hostfwd in that mode).
NET_RESTRICT="${B1NIX_NET_RESTRICT:-off}"

RUN_DIR="$PROJECT_DIR/smoke_run"
mkdir -p "$RUN_DIR"
LOG="$RUN_DIR/b1nix-ssh-hostfwd.log"
SSH_LOG="$RUN_DIR/b1nix-ssh-client.log"
SATA_IMG="$RUN_DIR/ssh_sata.img"
SWAP_IMG="$RUN_DIR/ssh_swap.img"
NVME_IMG="$RUN_DIR/ssh_nvme.img"

RED=$(printf '\033[31m'); GREEN=$(printf '\033[32m'); NC=$(printf '\033[0m')
pass() { echo "  ${GREEN}PASS${NC} $1"; }
fail() { echo "  ${RED}FAIL${NC} $1${2:+ — $2}"; }

QEMU_PID=""
cleanup() {
	[ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null
	[ -n "$QEMU_PID" ] && sleep 1 && kill -9 "$QEMU_PID" 2>/dev/null
	return 0
}
trap cleanup EXIT INT TERM

echo "=== B1NIX External SSH Smoke ($ARCH, hostfwd ${HOSTPORT}->22) ==="

# ── Preflight ──
for t in qemu-system-x86_64 ssh expect; do
	command -v "$t" >/dev/null 2>&1 || { fail "host tool present" "$t missing"; exit 1; }
done

# ── Build a NORMAL-mode ISO: networking is on by default, SSH bound externally,
#    no in-kernel test suite. (b1nix.net=off would disable DHCP.) ──
echo "[BUILD] Building $ARCH ISO (b1nix.ssh-external=1, default networking)..."
if ! make ARCH="$ARCH" KERNEL_CMDLINE="b1nix.ssh-external=1" iso >"$RUN_DIR/ssh-hostfwd-build.log" 2>&1; then
	fail "kernel/ISO builds" "see $RUN_DIR/ssh-hostfwd-build.log"
	exit 1
fi
pass "kernel/ISO builds"

dd if=/dev/zero of="$SATA_IMG" bs=1M count=4 2>/dev/null
dd if=/dev/zero of="$NVME_IMG" bs=1M count=4 2>/dev/null
dd if=/dev/zero of="$SWAP_IMG" bs=1M count=2 2>/dev/null

# ── Boot QEMU in the background, serial to a file we poll ──
rm -f "$LOG"
echo "[BOOT] Launching QEMU (restrict=$NET_RESTRICT)..."
qemu-system-x86_64 \
	-cdrom "$PROJECT_DIR/build/$ARCH/b1nix.iso" \
	-serial "file:$LOG" -display none -monitor none -no-reboot \
	-netdev "user,id=net0,restrict=${NET_RESTRICT},hostfwd=tcp:127.0.0.1:${HOSTPORT}-:22" \
	-device virtio-net-pci,netdev=net0 \
	-object filter-dump,id=f0,netdev=net0,file="$RUN_DIR/ssh-net.pcap" \
	-device ich9-ahci,id=ahci \
	-drive file="$SATA_IMG",if=none,id=satadrive,format=raw \
	-device ide-hd,drive=satadrive,bus=ahci.0 \
	-drive file="$SWAP_IMG",if=none,id=swapdrive,format=raw \
	-device ide-hd,drive=swapdrive,bus=ahci.1 \
	-drive file="$NVME_IMG",if=none,id=nvmedrive,format=raw \
	-device nvme,serial=deadbeef,drive=nvmedrive \
	>/dev/null 2>&1 &
QEMU_PID=$!

# ── Wait for: networking up (DHCP bound or fallback) AND sshd started ──
echo "[WAIT] Waiting for DHCP + sshd (up to ${BOOT_TIMEOUT}s)..."
start_ts=$(date +%s)
net_ok=0
ssh_ok=0
while :; do
	if [ "$net_ok" = 0 ] && grep -q -E "net: dhcp bound to|DHCP-SMOKE: (lease-acquired|fallback-static)" "$LOG" 2>/dev/null; then
		net_ok=1
		echo "  net: $(grep -aE 'net: dhcp bound to|fallback-static' "$LOG" | tail -1)"
	fi
	if [ "$ssh_ok" = 0 ] && grep -q "sshd: started" "$LOG" 2>/dev/null; then
		ssh_ok=1
		echo "  $(grep -a 'sshd: starting daemon' "$LOG" | tail -1)"
	fi
	if grep -q -E 'KERNEL PANIC|\[PANIC\]' "$LOG" 2>/dev/null; then
		fail "guest boots cleanly" "panic in $LOG"
		exit 1
	fi
	if [ "$net_ok" = 1 ] && [ "$ssh_ok" = 1 ]; then
		break
	fi
	now_ts=$(date +%s)
	if [ $((now_ts - start_ts)) -ge "$BOOT_TIMEOUT" ]; then
		[ "$net_ok" = 1 ] && pass "networking up (DHCP)" || fail "networking up (DHCP)" "no DHCP marker"
		[ "$ssh_ok" = 1 ] && pass "sshd started" || fail "sshd started" "no 'sshd: started' marker"
		fail "guest ready in ${BOOT_TIMEOUT}s" "see $LOG"
		exit 1
	fi
	sleep 1
done
pass "networking up (DHCP)"
pass "sshd started"

# Give the guest a moment to finish binding the listener after the rc message.
sleep 2

# ── First proof: inbound TCP works (we get the SSH banner over the NIC) ──
echo "[TCP] Probing inbound TCP — expecting an SSH banner..."
BANNER=""
i=0
while [ "$i" -lt 4 ]; do
	# Send a client identification line and hold the connection open briefly so
	# the server completes its half of the SSH ident exchange (and a bare
	# </dev/null half-close doesn't abort the peer before its banner arrives).
	BANNER=$( { printf 'SSH-2.0-b1nix-probe\r\n'; sleep 3; } | nc -w 8 127.0.0.1 "$HOSTPORT" 2>/dev/null | grep -a -m1 'SSH-' | tr -d '\r\n' )
	printf '%s' "$BANNER" | grep -q "SSH-2.0" && break
	i=$((i + 1)); sleep 2
done
if printf '%s' "$BANNER" | grep -q "SSH-2.0"; then
	pass "inbound TCP + SSH banner ($BANNER)"
else
	fail "inbound TCP + SSH banner" "got: '${BANNER:-<nothing>}' (informational; the SSH login below is authoritative)"
fi

# ── Authoritative check: real SSH login from the host, run a command ──
# The marker is *computed on the guest* (arithmetic "$((6*7))" -> "42"), so the
# expanded "EXTSSH-42-OK" can never match the literal command string echoed by
# expect's spawn line (which still shows "EXTSSH-$((6*7))-OK"). That proves the
# remote shell actually ran, not just that the command text was transmitted.
# A quoted heredoc keeps the host shell out of the Tcl script; parameters cross
# via the environment, and the remote command is brace-quoted so Tcl leaves the
# expansion for the GUEST shell.
echo "[SSH] Logging in from host (password auth via expect)..."
rm -f "$SSH_LOG"
EXSSH_PORT="$HOSTPORT" EXSSH_TIMEOUT="$SSH_TIMEOUT" expect >"$SSH_LOG" 2>&1 <<'EXPECT'
set timeout $env(EXSSH_TIMEOUT)
set port $env(EXSSH_PORT)
log_user 1
spawn ssh -p $port \
	-o StrictHostKeyChecking=no \
	-o UserKnownHostsFile=/dev/null \
	-o GlobalKnownHostsFile=/dev/null \
	-o PreferredAuthentications=password \
	-o PubkeyAuthentication=no \
	-o NumberOfPasswordPrompts=1 \
	-o ConnectTimeout=20 \
	-o HostKeyAlgorithms=+ssh-ed25519 \
	-o KexAlgorithms=+curve25519-sha256 \
	root@127.0.0.1 {id; uname -a; echo EXTSSH-$((6*7))-OK}
expect {
	-re {[Pp]assword:} { send "root\r"; exp_continue }
	"EXTSSH-42-OK"     { }
	timeout            { puts "EXPECT-TIMEOUT"; exit 2 }
	eof                { }
}
expect eof
EXPECT
SSH_RC=$?

echo "---- ssh client transcript (tail) ----"
tail -20 "$SSH_LOG" | sed 's/^/  | /'
echo "--------------------------------------"

if grep -q "EXTSSH-42-OK" "$SSH_LOG" 2>/dev/null ||
   { grep -q "uid=0" "$SSH_LOG" 2>/dev/null && grep -q "B1NIX b1nix" "$SSH_LOG" 2>/dev/null; }; then
	pass "host-to-guest SSH login + remote command (root@guest via ${HOSTPORT})"
	echo ""
	echo "${GREEN}=== M32C-EXTSSH: host-to-guest SSH OK ===${NC}"
	RESULT=0
else
	fail "host-to-guest SSH login + remote command" "marker EXTSSH-0-OK not seen (ssh rc=$SSH_RC)"
	echo ""
	echo "${RED}=== M32C-EXTSSH: host-to-guest SSH FAILED ===${NC}"
	echo "  guest serial tail:"
	tail -25 "$LOG" | sed 's/^/  > /'
	RESULT=1
fi

exit $RESULT
