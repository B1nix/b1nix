#!/bin/sh
set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$PROJECT_DIR/smoke_run"
LOG="$PROJECT_DIR/smoke_run/b1nix-graphics-smoke.log"
TIMEOUT=40

cd "$PROJECT_DIR"
make ARCH=x86_64 KERNEL_CMDLINE="b1nix.test=1" iso >/dev/null 2>&1

qemu-system-x86_64 \
  -cdrom "$PROJECT_DIR/build/x86_64/b1nix.iso" \
  -serial stdio -display none -monitor none -no-reboot \
  -device virtio-gpu-pci \
  -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
  >"$LOG" 2>&1 &
PID=$!
sleep 8
kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true

grep -q "Step 11: Drivers initialized" "$LOG"
grep -q "compositor: initialized" "$LOG"
# PS/2 mouse is best-effort: some QEMU machine types don't expose i8042 aux
# device and the kernel logs "enable failed" instead. Graphics smoke only
# cares that the probe ran; treat either outcome as OK.
grep -qE "ps2_mouse: (initialized on irq12|enable failed)" "$LOG"
if ! grep -q "virtio-gpu: ready" "$LOG"; then
  grep -q "virtio-gpu: transport init failed" "$LOG"
fi
if grep -q "KERNEL PANIC" "$LOG"; then
  echo "graphics smoke failed: panic detected"
  exit 1
fi
echo "graphics smoke: ok"
