#!/bin/sh
# Boot the x86_64 b1nix ISO with the V8 ext4 disk attached and run d8.
#
# The ISO must be built with b1nix.v8run on the cmdline:
#   make ARCH=x86_64 KERNEL_CMDLINE="b1nix.test=1 b1nix.v8run" iso
# That flag makes kernel/main.c mount sata0 -> /mnt/v8 and launch
#   d8 --jitless /mnt/v8/m58.js
# m58.js prints "M58-V8: ok hello", a series of result-gated "ok" markers
# (loop-sum/array-reduce/object-sort/json/gc-churn/recursion), then
# "M58-V8: done".
#
# d8 (x86_64, 13 MB) is too big for the xxd-embedded initramfs, so it ships on
# build/v8-out/v8-ext4.img (ext4: /d8 + /hello.js) attached as the AHCI sata0.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT_DIR/build/x86_64/b1nix.iso"
DISK="$ROOT_DIR/build/v8-out/v8-ext4.img"
LOG="$ROOT_DIR/smoke_run/v8-run.log"
TIMEOUT="${TIMEOUT:-120}"

[ -f "$ISO" ]  || { echo "missing $ISO — build the x86_64 ISO first"; exit 1; }
[ -f "$DISK" ] || { echo "missing $DISK"; exit 1; }
mkdir -p "$ROOT_DIR/smoke_run"

accel=""
if [ -w /dev/kvm ] && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw kvm; then
	accel="-accel kvm"
fi

# d8 needs real RAM headroom (V8 heap + snapshot deserialize).
set -- qemu-system-x86_64 $accel -m "${SMOKE_MEM_MB:-2048}" \
	-cdrom "$ISO" -serial stdio -display none -monitor none -no-reboot \
	-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	-device ich9-ahci,id=ahci \
	-drive file="$DISK",if=none,id=satadrive,format=raw \
	-device ide-hd,drive=satadrive,bus=ahci.0

echo "[v8-run] $*"
"$@" >"$LOG" 2>&1 &
pid=$!

# Wait for a terminal marker or timeout, then kill QEMU.
start=$(date +%s)
while :; do
	if grep -qaE "M58-V8: done|KERNEL PANIC|\[PANIC\]" "$LOG" 2>/dev/null; then
		break
	fi
	kill -0 "$pid" 2>/dev/null || break
	[ $(( $(date +%s) - start )) -ge "$TIMEOUT" ] && { echo "[v8-run] timeout ${TIMEOUT}s"; break; }
	sleep 1
done
kill "$pid" 2>/dev/null || true
sleep 1
kill -9 "$pid" 2>/dev/null || true

echo "==================== v8 run result ===================="
echo "markers seen:"
grep -aoE "M58-V8: (ok [a-z-]+|done|hello)" "$LOG" | sort -u | sed 's/^/  /'
# Expect: hello + 6 result-gated oks + done = 8 distinct markers.
n=$(grep -aoE "M58-V8: (ok [a-z-]+|done|hello)" "$LOG" | sort -u | wc -l | tr -d ' ')
if grep -qa "M58-V8: done" "$LOG" && [ "$n" -ge 8 ]; then
	echo "PASS: d8 ran m58.js to completion ($n/8 markers)"
else
	echo "FAIL ($n/8 markers) — last v8/d8-related lines:"
	grep -anE "v8:|d8|M58-V8|snapshot|ELF load|PANIC|Fatal|page fault|#GP|TLS" "$LOG" | tail -40
fi
echo "full log: $LOG"
