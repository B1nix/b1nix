#!/bin/sh
set -e

ARCH="${1:-x86_64}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$PROJECT_DIR/smoke_run"
LOG="$PROJECT_DIR/smoke_run/b1nix-graphics-smoke-$ARCH.log"
TIMEOUT="${TIMEOUT:-180}"

cd "$PROJECT_DIR"
make ARCH="$ARCH" KERNEL_CMDLINE="b1nix.test=1" iso >/dev/null 2>&1

if [ "$ARCH" = "x86" ]; then
  QEMU=qemu-system-i386
else
  QEMU=qemu-system-x86_64
fi

"$QEMU" \
  -cdrom "$PROJECT_DIR/build/$ARCH/b1nix.iso" \
  -serial stdio -display none -monitor none -no-reboot \
  -device virtio-gpu-pci \
  -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
  >"$LOG" 2>&1 &
PID=$!
elapsed=0
while kill -0 "$PID" 2>/dev/null && [ "$elapsed" -lt "$TIMEOUT" ]; do
  if grep -q "M47-DSP: ok server-restart" "$LOG" 2>/dev/null; then
    break
  fi
  sleep 1
  elapsed=$((elapsed + 1))
done
kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true

grep -q "M47-GFX: ok fb-mmap" "$LOG"
grep -q "M47-GFX: ok input-event" "$LOG"
grep -q "M47-DSP: ok console-reclaim" "$LOG"
grep -q "M47-DSP: ok server-restart" "$LOG"
grep -q "M48-FDPASS: ok scm-rights" "$LOG"
grep -q "M48-FDPASS: ok scm-refcount-close" "$LOG"
grep -q "M48-FDPASS: ok memfd" "$LOG"
grep -q "M48-FDPASS: ok shared-fork-cow" "$LOG"
grep -q "M49-WL: ok registry" "$LOG"
grep -q "M49-WL: ok xdg-shell" "$LOG"
grep -q "M49-WL: ok shm-frame" "$LOG"
grep -q "M49-WL: ok libb1gui" "$LOG"
grep -q "M49-LIBWL: ok upstream-client" "$LOG"
grep -q "M49-LIBWL: ok keymap" "$LOG"
grep -q "M49-LIBWLS: ok server-core" "$LOG"
! grep -q "KERNEL PANIC" "$LOG"
echo "graphics smoke ($ARCH): ok"
