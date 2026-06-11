#!/bin/sh
# M39 — end-to-end serial getty/login session over COM1.
#
# Boots b1nix in NORMAL mode (no in-kernel test suite). The inittab supervisor
# spawns `/bin/getty -L 115200 ttyS0 vt100` on /dev/ttyS0, which owns COM1
# input while open. This host script drives QEMU's serial stdio like a real
# serial terminal: it answers the getty login prompt as root, verifies an
# interactive bash session is running on the serial tty, logs out, and checks
# that init respawns getty (a fresh login prompt appears).
#
# It is intentionally separate from tests/smoke.sh (which runs the in-kernel
# suite in test mode, where the supervisor never starts).
#
# Usage: sh tests/serial-getty.sh [x86|x86_64]
#
# Requires on the host: qemu-system-x86_64, python3.

set -u

ARCH="${1:-x86}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_DIR"

# x86_64 emulates notably slower than i386 under TCG.
if [ "$ARCH" = "x86_64" ]; then
	TIMEOUT="${TIMEOUT:-300}"
else
	TIMEOUT="${TIMEOUT:-180}"
fi

RUN_DIR="$PROJECT_DIR/smoke_run"
mkdir -p "$RUN_DIR"
LOG="$RUN_DIR/b1nix-serial-getty.log"

RED=$(printf '\033[31m'); GREEN=$(printf '\033[32m'); NC=$(printf '\033[0m')
pass() { echo "  ${GREEN}PASS${NC} $1"; }
fail() { echo "  ${RED}FAIL${NC} $1${2:+ - $2}"; }

echo "=== B1NIX Serial getty/login Smoke ($ARCH) ==="

for t in qemu-system-x86_64 python3; do
	command -v "$t" >/dev/null 2>&1 || { fail "host tool present" "$t missing"; exit 1; }
done

# ── Build a NORMAL-mode ISO (no b1nix.test: the inittab supervisor runs). ──
echo "[BUILD] Building $ARCH ISO (normal mode)..."
if ! make ARCH="$ARCH" KERNEL_CMDLINE="" iso >"$RUN_DIR/serial-getty-build.log" 2>&1; then
	fail "kernel/ISO builds" "see $RUN_DIR/serial-getty-build.log"
	exit 1
fi
pass "kernel/ISO builds"

# ── Drive the serial console. python3 owns QEMU's stdin/stdout. ──
B1NIX_ISO="$PROJECT_DIR/build/$ARCH/b1nix.iso" \
B1NIX_LOG="$LOG" B1NIX_TIMEOUT="$TIMEOUT" \
python3 - <<'PYEOF'
import os, re, subprocess, sys, time

ISO = os.environ["B1NIX_ISO"]
LOG = os.environ["B1NIX_LOG"]
TIMEOUT = int(os.environ["B1NIX_TIMEOUT"])

qemu = [
    "qemu-system-x86_64",
    "-cdrom", ISO, "-m", "256",
    "-serial", "stdio", "-display", "none", "-monitor", "none", "-no-reboot",
]

f = open(LOG, "wb", buffering=0)
p = subprocess.Popen(qemu, stdin=subprocess.PIPE, stdout=f, stderr=subprocess.STDOUT)

def send(s):
    p.stdin.write(s.encode())
    p.stdin.flush()

PROBE = "echo SERIAL-E2E-$((6*7))\n"
results = {"getty-prompt": False, "login-shell": False, "getty-respawn": False}
state = "wait_login"
pw_sent = False
pw_time = 0.0
logins_before_exit = 0
last_probe = 0.0
last_exit = 0.0
start = time.time()

def note(msg):
    print(f"  [{int(time.time() - start)}s] {msg}", flush=True)

try:
    while time.time() - start < TIMEOUT:
        time.sleep(0.5)
        if p.poll() is not None:
            break
        text = open(LOG, "rb").read().decode("latin1", "replace")
        logins = text.count("login:")

        if state == "wait_login" and logins > 0:
            results["getty-prompt"] = True
            note("getty prompt: sending user")
            send("root\n")
            state = "auth"
        elif state == "auth":
            if not pw_sent and "Password:" in text:
                note("password prompt: sending password")
                send("root\n")
                pw_sent = True
                pw_time = time.time()
            # Probe for an interactive shell (resend every 5s; give login a
            # moment first so the probe is not eaten as a password retry).
            if pw_sent and time.time() - pw_time > 3 and \
               time.time() - last_probe > 5:
                send(PROBE)
                last_probe = time.time()
            if "SERIAL-E2E-42" in text:
                results["login-shell"] = True
                note("login shell verified: sending exit")
                logins_before_exit = logins
                send("exit\n")
                last_exit = time.time()
                state = "wait_respawn"
        elif state == "wait_respawn":
            if logins > logins_before_exit:
                results["getty-respawn"] = True
                note("getty respawned with a fresh login prompt")
                break
            if time.time() - last_exit > 10:
                send("exit\n")
                last_exit = time.time()
finally:
    if p.poll() is None:
        p.terminate()
        time.sleep(1)
        if p.poll() is None:
            p.kill()

ok = True
for name in ("getty-prompt", "login-shell", "getty-respawn"):
    mark = "PASS" if results[name] else "FAIL"
    if not results[name]:
        ok = False
    print(f"  {mark} {name}")
sys.exit(0 if ok else 1)
PYEOF
RC=$?

if [ "$RC" -eq 0 ]; then
	pass "serial getty/login session end-to-end"
	echo "=== Serial getty smoke PASSED ($ARCH) ==="
else
	fail "serial getty/login session" "see $LOG"
	echo "=== Serial getty smoke FAILED ($ARCH) ==="
fi
exit "$RC"
