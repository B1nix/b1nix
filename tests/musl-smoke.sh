#!/bin/sh
# M92: musl libc smoke test.
#
# Builds musl, compiles the dynamic smoke test against it, embeds it in the
# initramfs, and runs it in QEMU. Reports pass/fail based on M92-MUSL-DYN markers.
#
# Usage:
#   tests/musl-smoke.sh              # build + run
#   tests/musl-smoke.sh --build-only # build only, no QEMU
#   tests/musl-smoke.sh --run-only   # run only (requires prior build)
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

BUILD_ONLY=0
RUN_ONLY=0
case "${1:-}" in
  --build-only) BUILD_ONLY=1 ;;
  --run-only)   RUN_ONLY=1 ;;
esac

MUSL_BUILD="build/x86_64/ports/musl"
MUSL_INSTALL="$MUSL_BUILD/install/usr"
SMOKE_DYN_BIN="build/m92-musl-dyn-smoke"
SMOKE_DYN_INC="build/x86_64/initramfs_m92_musl_dyn_smoke.inc"
LOG_DIR="smoke_run"
LOG="$LOG_DIR/b1nix-musl-smoke.log"
QEMU_TIMEOUT=60

mkdir -p "$LOG_DIR"

# --- Step 1: Build musl ------------------------------------------------------
if [ "$RUN_ONLY" = "0" ]; then
  echo "[M92-MUSL] Building musl..."
  if [ ! -f "$MUSL_INSTALL/lib/libc.a" ]; then
    tools/ports/build-musl.sh 2>&1
  else
    echo "  musl already built at $MUSL_INSTALL"
  fi

  if [ ! -f "$MUSL_INSTALL/lib/libc.a" ]; then
    echo "[M92-MUSL] FAILED — musl build did not produce lib/libc.a" >&2
    exit 1
  fi
  echo "  lib: $MUSL_INSTALL/lib/libc.a"

  # --- Step 2: Compile dynamic smoke test against musl -------------------------
  echo "[M92-MUSL] Compiling dynamic smoke test against musl..."
  tools/b1nix-musl-cc -dynamic userspace/bin/m92_musl_dyn_test.c -o "$SMOKE_DYN_BIN" 2>&1

  if [ ! -f "$SMOKE_DYN_BIN" ]; then
    echo "[M92-MUSL] FAILED — dynamic smoke test compilation failed" >&2
    exit 1
  fi
  echo "  dynamic binary: $SMOKE_DYN_BIN ($(stat -c%s "$SMOKE_DYN_BIN" 2>/dev/null || stat -f%z "$SMOKE_DYN_BIN") bytes)"

  # --- Step 2b: Compile real-ld.so smoke test (ldso-migration plan) -----------
  echo "[M92-MUSL] Compiling userspace-ld.so smoke test against musl..."
  SMOKE_LDSO_BIN="build/m92-musl-ldso-smoke"
  SMOKE_LDSO_INC="build/x86_64/initramfs_m92_musl_ldso_smoke.inc"
  tools/b1nix-musl-cc -ldso userspace/bin/m92_musl_ldso_test.c -o "$SMOKE_LDSO_BIN" 2>&1

  if [ ! -f "$SMOKE_LDSO_BIN" ]; then
    echo "[M92-MUSL] FAILED — ld.so smoke test compilation failed" >&2
    exit 1
  fi
  echo "  ldso binary: $SMOKE_LDSO_BIN ($(stat -c%s "$SMOKE_LDSO_BIN" 2>/dev/null || stat -f%z "$SMOKE_LDSO_BIN") bytes)"

  # --- Step 3: Embed in initramfs --------------------------------------------
  echo "[M92-MUSL] Embedding in initramfs..."
  xxd -i -n vfs_m92_musl_dyn_smoke_elf "$SMOKE_DYN_BIN" > "$SMOKE_DYN_INC"
  xxd -i -n vfs_m92_musl_ldso_smoke_elf "$SMOKE_LDSO_BIN" > "$SMOKE_LDSO_INC"

  # Regenerate the embedded musl ld.so (libc.so doubles as its own dynamic
  # linker) so the initramfs copy never drifts from a freshly rebuilt musl.
  MUSL_SRC_BUILD="build/musl-src/x86_64-b1nix/musl-1.2.5"
  if [ -f "$MUSL_SRC_BUILD/lib/libc.so" ]; then
    xxd -i -n vfs_ld_musl_x86_64_so_1 "$MUSL_SRC_BUILD/lib/libc.so" \
      > build/x86_64/initramfs_ld_musl_x86_64_so_1.inc
    echo "  regenerated initramfs_ld_musl_x86_64_so_1.inc from libc.so"
    mkdir -p build/x86_64/rootfs/lib
    cp "$MUSL_SRC_BUILD/lib/libc.so" build/x86_64/rootfs/lib/ld-musl-x86_64.so.1
    echo "  installed ld-musl-x86_64.so.1 to rootfs"
  fi

  echo "  initramfs: $SMOKE_DYN_INC $SMOKE_LDSO_INC"
fi

if [ "$BUILD_ONLY" = "1" ]; then
  echo "[M92-MUSL] Build complete (--build-only)."
  exit 0
fi

# --- Step 4: Rebuild kernel with embedded smoke test --------------------------
echo "[M92-MUSL] Rebuilding kernel with embedded musl smoke test..."
make ARCH=x86_64 KERNEL_CMDLINE="init=/bin/init b1nix.test=1 b1nix.muslrun" iso 2>&1 | tail -5

# --- Step 5: Run in QEMU -----------------------------------------------------
echo "[M92-MUSL] Running smoke test in QEMU (timeout ${QEMU_TIMEOUT}s)..."
timeout "$QEMU_TIMEOUT" qemu-system-x86_64 \
  -cdrom build/x86_64/b1nix.iso \
  -display none \
  -serial mon:stdio \
  -m 512 \
  -no-reboot \
  > "$LOG" 2>&1 || true

# --- Step 6: Check results ----------------------------------------------------
echo ""
echo "=== Results ==="
PASS_COUNT=$(grep -cE "M92-MUSL-DYN: ok|M92-LDSO: ok" "$LOG" 2>/dev/null || echo 0)
FAIL_COUNT=$(grep -cE "M92-MUSL-DYN: fail|M92-LDSO: fail" "$LOG" 2>/dev/null || echo 0)
DONE=$(grep -a "M92-MUSL-DYN: done" "$LOG" 2>/dev/null | head -1)
LDSO_DONE=$(grep -a "M92-LDSO: done" "$LOG" 2>/dev/null | head -1)

echo "  Passed: $PASS_COUNT"
echo "  Failed: $FAIL_COUNT"
echo "  ldso:   $LDSO_DONE"
echo "  Log: $LOG"

if [ "$DONE" != "" ] && [ "$LDSO_DONE" != "" ]; then
  echo "  Status: OK"
  exit 0
else
  echo "  Status: FAIL"
  # Show relevant output
  grep -aE "M92-MUSL|SIGSEGV|SIGABRT|panic|B1NIX-TEST" "$LOG" | tail -30
  exit 1
fi
