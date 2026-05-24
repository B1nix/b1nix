#!/usr/bin/env bash
set -euo pipefail

# Crash/recovery persistence stress for ext3 image.
# Requires host tools: qemu-system-x86_64, fsck.ext3/e2fsck.
#
# Usage:
#   ./tests/ext3-persistence.sh [iterations]
#
# Environment:
#   DISK_IMAGE=path/to/disk.img
#   BOOT_ISO=path/to/b1nix.iso

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ITERATIONS="${1:-10}"
DISK_IMAGE="${DISK_IMAGE:-$ROOT_DIR/smoke_run/disk.img}"
BOOT_ISO="${BOOT_ISO:-$ROOT_DIR/build/x86/os.iso}"
LOG_DIR="$ROOT_DIR/smoke_run/ext3-persist"
mkdir -p "$LOG_DIR"

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
  echo "missing qemu-system-x86_64"
  exit 1
fi

FSCK_BIN=""
if command -v fsck.ext3 >/dev/null 2>&1; then
  FSCK_BIN="fsck.ext3"
elif command -v e2fsck >/dev/null 2>&1; then
  FSCK_BIN="e2fsck"
else
  echo "missing fsck.ext3/e2fsck"
  exit 1
fi

if [ ! -f "$BOOT_ISO" ]; then
  echo "missing boot iso: $BOOT_ISO"
  exit 1
fi
if [ ! -f "$DISK_IMAGE" ]; then
  echo "missing disk image: $DISK_IMAGE"
  exit 1
fi

run_once() {
  local n="$1"
  local log="$LOG_DIR/iter-${n}.log"

  # Run with no reboot so power-cut can be emulated by terminating QEMU.
  qemu-system-x86_64 \
    -cdrom "$BOOT_ISO" \
    -drive file="$DISK_IMAGE",format=raw,if=virtio \
    -m 512M \
    -serial file:"$log" \
    -display none \
    -monitor none \
    -no-reboot >/dev/null 2>&1 &
  local qpid=$!

  # Let system boot and touch FS workload from init/userspace path.
  sleep 3
  kill -9 "$qpid" >/dev/null 2>&1 || true
  wait "$qpid" >/dev/null 2>&1 || true

  # Force fsck check after abrupt termination.
  "$FSCK_BIN" -fy "$DISK_IMAGE" >/dev/null
}

for i in $(seq 1 "$ITERATIONS"); do
  echo "iteration $i/$ITERATIONS"
  run_once "$i"
done

echo "ext3 persistence stress: ok ($ITERATIONS iterations)"
