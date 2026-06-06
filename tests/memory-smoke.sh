#!/bin/sh
# On-demand M41 check: prove that the x86_64 kernel uses a 16 GiB firmware map.

set -eu

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LOG="$PROJECT_DIR/smoke_run/b1nix-memory-16g.log"
TIMEOUT="${TIMEOUT:-240}"

cd "$PROJECT_DIR"
make ARCH=x86_64 KERNEL_CMDLINE="b1nix.single b1nix.nographics" iso >/dev/null
mkdir -p "$PROJECT_DIR/smoke_run"
rm -f "$LOG"

qemu-system-x86_64 \
	-m 16G -smp 2 \
	-cdrom "$PROJECT_DIR/build/x86_64/b1nix.iso" \
	-serial "file:$LOG" -display none -monitor none -no-reboot -net none \
	>/dev/null 2>&1 &
qemu_pid=$!

start="$(date +%s)"
while :; do
	if grep -q "pmm: firmware RAM" "$LOG" 2>/dev/null; then
		break
	fi
	if grep -q -E "KERNEL PANIC|\\[PANIC\\]" "$LOG" 2>/dev/null; then
		kill "$qemu_pid" 2>/dev/null || true
		wait "$qemu_pid" 2>/dev/null || true
		echo "M41 memory smoke: kernel panic"
		exit 1
	fi
	now="$(date +%s)"
	if [ $((now - start)) -ge "$TIMEOUT" ]; then
		kill "$qemu_pid" 2>/dev/null || true
		wait "$qemu_pid" 2>/dev/null || true
		echo "M41 memory smoke: timeout"
		exit 1
	fi
	sleep 1
done

kill "$qemu_pid" 2>/dev/null || true
wait "$qemu_pid" 2>/dev/null || true

summary="$(grep "pmm: firmware RAM" "$LOG" | tail -n 1)"
usable="$(printf "%s\n" "$summary" | sed -n 's/.*usable \([0-9][0-9]*\) MiB.*/\1/p')"
if [ -z "$usable" ] || [ "$usable" -lt 15000 ]; then
	echo "M41 memory smoke: expected at least 15000 MiB usable"
	echo "$summary"
	exit 1
fi

echo "M41 memory smoke: PASS"
echo "$summary"
