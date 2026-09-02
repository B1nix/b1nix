#!/bin/sh
# tools/t.sh — fast single-test iteration for b1nix
#
# Compiles ONE userspace binary directly (bypassing the full Makefile),
# drops it into the 9P hostshare, and launches a minimal QEMU instance
# with init= pointing at that binary.  The whole cycle takes ~10-15 s on
# Apple Silicon instead of 2-4 minutes for a full smoke run.
#
# Usage:
#   sh tools/t.sh <test>            # aarch64, default
#   sh tools/t.sh <test> x86_64
#   sh tools/t.sh <test> aarch64 blk   # storage lane (adds SATA/NVMe disks)
#
# <test> is the binary name with or without path / extension, e.g.:
#   m32_smoke  m14_smoke  m32  net_smoke  m110_9p_smoke
#
# Extra QEMU args: T_EXTRA_QEMU=""   (e.g. "-smp 2")
# Skip compile:   T_SKIP_BUILD=1
# Keep VM alive:  T_KEEP=1          (adds b1nix.keep-running to cmdline)
# Stall timeout:  T_STALL=30        (seconds of silence before kill, default 40)
# Memory:         T_MEM=512         (MB, default 512)

set -e

#───────────────────────────── args ─────────────────────────────────────────
TEST_ARG="${1:-}"
ARCH="${2:-aarch64}"
LANE="${3:-sys}"   # sys | blk | posix | gfx

if [ -z "$TEST_ARG" ]; then
    echo "Usage: sh tools/t.sh <test> [arch] [lane]" >&2
    echo "  e.g. sh tools/t.sh m32_smoke" >&2
    echo "       sh tools/t.sh m14_smoke aarch64 blk" >&2
    exit 1
fi

# Strip path prefix and .c / binary extension if supplied
TEST_NAME="$(basename "$TEST_ARG" .c)"
TEST_NAME="${TEST_NAME%.o}"

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$PROJECT_DIR/build/$ARCH"

#───────────────────────────── locate source ────────────────────────────────
# Try smoke/, tools/, helpers/, then top-level bin/
SRC=""
for dir in bin/smoke bin/tools bin/helpers bin; do
    candidate="$PROJECT_DIR/userspace/$dir/$TEST_NAME.c"
    if [ -f "$candidate" ]; then
        SRC="$candidate"
        break
    fi
done

if [ -z "$SRC" ]; then
    echo "ERROR: cannot find source for '$TEST_NAME'" >&2
    echo "  searched: userspace/bin/smoke/ userspace/bin/tools/ userspace/bin/helpers/ userspace/bin/" >&2
    exit 1
fi
echo "[t] source: $SRC"

#───────────────────────────── toolchain vars ────────────────────────────────
CLANG="${CLANG:-$(command -v /opt/homebrew/opt/llvm/bin/clang 2>/dev/null || command -v clang)}"
LD="${LD:-$(command -v ld.lld 2>/dev/null || echo /opt/homebrew/bin/ld.lld)}"
CCACHE="$(command -v ccache 2>/dev/null || true)"
[ -n "$CCACHE" ] && CLANG="$CCACHE $CLANG"

if [ "$ARCH" = "aarch64" ]; then
    TARGET="aarch64-unknown-elf"
    LDFLAGS_M="-m aarch64elf"
    DYN_LINKER="/lib/ld-musl-aarch64.so.1"
else
    TARGET="x86_64-unknown-elf"
    LDFLAGS_M="-m elf_x86_64"
    DYN_LINKER="/lib/ld-musl-x86_64.so.1"
fi

MUSL="$BUILD/ports/musl/install"
USBUILD="$PROJECT_DIR/userspace/build/$ARCH"
BUILTINS="$(ls "$BUILD/compiler-rt-builtins/libclang_rt.builtins-$ARCH.a" \
              "$MUSL/lib/libclang_rt.builtins-$ARCH.a" \
              "$MUSL/lib/libcompiler_rt.a" 2>/dev/null | head -1)"

CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector \
  -nostdinc -isystem $MUSL/include -isystem $PROJECT_DIR/userspace/include \
  -I $PROJECT_DIR/userspace/include \
  -fPIC -D__b1nix__ -D__linux__ -D_GNU_SOURCE -Db1nix \
  -Wall -Wextra -O2"

LINK_EXTRA="-pie -z norelro --hash-style=sysv --allow-shlib-undefined \
  --dynamic-linker $DYN_LINKER -L$MUSL/lib"

LINK_LIBS="$MUSL/lib/crti.o $USBUILD/compat/utmp.o $USBUILD/compat/crypt.o \
  -lc $MUSL/lib/crtn.o $BUILTINS"

#───────────────────────────── hostshare dir ────────────────────────────────
HOSTSHARE="${T_HOSTSHARE:-/tmp/b1nix_9p_host}"
mkdir -p "$HOSTSHARE/bin"

OUT_BIN="$HOSTSHARE/bin/$TEST_NAME"
OBJ_TMP="/tmp/b1nix_t_$TEST_NAME.o"

#───────────────────────────── compile ──────────────────────────────────────
if [ "${T_SKIP_BUILD:-0}" != "1" ]; then
    echo "[t] compiling $TEST_NAME for $ARCH ..."
    t0=$(date +%s)

    # shellcheck disable=SC2086
    $CLANG $CFLAGS -c "$SRC" -o "$OBJ_TMP"

    # Link
    $LD $LDFLAGS_M $LINK_EXTRA -o "$OUT_BIN" \
        "$MUSL/lib/Scrt1.o" \
        "$OBJ_TMP" \
        $LINK_LIBS

    # Stamp EI_OSABI = 3 (ELFOSABI_LINUX)
    python3 -c "import sys; fh=open(sys.argv[1],'r+b'); fh.seek(7); fh.write(bytes([3]))" \
        "$OUT_BIN"

    rm -f "$OBJ_TMP"
    t1=$(date +%s)
    echo "[t] compiled in $((t1 - t0))s → $OUT_BIN"
else
    echo "[t] T_SKIP_BUILD=1: skipping compile, using $OUT_BIN"
    if [ ! -x "$OUT_BIN" ]; then
        echo "ERROR: $OUT_BIN not found, build first" >&2
        exit 1
    fi
fi

#───────────────────────────── QEMU args ────────────────────────────────────
MEM="${T_MEM:-512}"
KERNEL_CMDLINE="b1nix.test=1 init=/bin/$TEST_NAME"
[ "${T_KEEP:-0}" = "1" ] && KERNEL_CMDLINE="$KERNEL_CMDLINE b1nix.keep-running"

STALL="${T_STALL:-40}"

# 9P hostshare (always)
FSDEV_ARGS="-fsdev local,path=$HOSTSHARE,security_model=none,id=fsdev9p \
  -device virtio-9p-pci,fsdev=fsdev9p,mount_tag=hostshare"

if [ "$ARCH" = "aarch64" ]; then
    QEMU="qemu-system-aarch64"
    # aarch64 has no SATA controller by default; use virtio-mmio for rootfs
    QEMU_MACHINE="-machine virt -accel hvf -cpu host"
    ROOT_DEV="-drive if=none,file=$BUILD/root.ext4,format=raw,snapshot=on,id=vblk0 \
              -device virtio-blk-device,drive=vblk0"
    FSDEV_ARGS="-fsdev local,path=$HOSTSHARE,security_model=none,id=fsdev9p \
      -device virtio-9p-device,fsdev=fsdev9p,mount_tag=hostshare"
    NET_ARGS="-netdev user,id=net0 -device virtio-net-device,netdev=net0"
    KERNEL_ARGS="-kernel $BUILD/Image"
else
    QEMU="qemu-system-x86_64"
    QEMU_MACHINE="-machine q35 -accel hvf -cpu host"
    ROOT_DEV="-cdrom $BUILD/b1nix.iso -boot d"
    NET_ARGS="-netdev user,id=net0 -device e1000,netdev=net0"
    KERNEL_ARGS=""
    FSDEV_ARGS="-fsdev local,path=$HOSTSHARE,security_model=none,id=fsdev9p \
      -device virtio-9p-pci,fsdev=fsdev9p,mount_tag=hostshare"
fi

# Lane-specific extra devices
EXTRA_LANE_ARGS=""
if [ "$LANE" = "blk" ]; then
    SATA_IMG="/tmp/b1nix_t_sata_$$.img"
    NVME_IMG="/tmp/b1nix_t_nvme_$$.img"
    MKE2FS="$(command -v mke2fs 2>/dev/null || command -v /opt/homebrew/sbin/mke2fs 2>/dev/null || true)"
    if [ -z "$MKE2FS" ]; then
        echo "WARNING: mke2fs not found, blk lane disks may be empty" >&2
        dd if=/dev/zero of="$SATA_IMG" bs=1M count=64 2>/dev/null
        dd if=/dev/zero of="$NVME_IMG" bs=1M count=64 2>/dev/null
    else
        dd if=/dev/zero of="$SATA_IMG" bs=1M count=64 2>/dev/null
        dd if=/dev/zero of="$NVME_IMG" bs=1M count=64 2>/dev/null
        "$MKE2FS" -F -t ext4 -O '^metadata_csum,^64bit,^flex_bg,^huge_file' \
            -q "$SATA_IMG" 2>/dev/null || true
        "$MKE2FS" -F -t ext4 -L m109label \
            -O '^metadata_csum,^64bit,^flex_bg,^huge_file' \
            -q "$NVME_IMG" 2>/dev/null || true
    fi
    if [ "$ARCH" = "aarch64" ]; then
        EXTRA_LANE_ARGS="-drive if=none,file=$SATA_IMG,format=raw,id=sata0 \
          -device virtio-blk-device,drive=sata0 \
          -drive if=none,file=$NVME_IMG,format=raw,id=nvme0 \
          -device virtio-blk-device,drive=nvme0"
    else
        EXTRA_LANE_ARGS="-device ich9-ahci,id=ahci \
          -drive if=none,file=$SATA_IMG,format=raw,id=sata0 \
          -device ide-hd,drive=sata0,bus=ahci.0 \
          -drive if=none,file=$NVME_IMG,format=raw,id=nvme0 \
          -device nvme,drive=nvme0,serial=b1nixnvme"
    fi
    # cleanup on exit
    # shellcheck disable=SC2064
    trap "rm -f '$SATA_IMG' '$NVME_IMG'" EXIT INT TERM
fi

LOG="/tmp/b1nix_t_$TEST_NAME.log"
: > "$LOG"

#───────────────────────────── launch QEMU ──────────────────────────────────
echo "[t] launching QEMU ($ARCH, lane=$LANE, mem=${MEM}M, stall=${STALL}s) ..."
echo "[t] log: $LOG"
echo "[t] ---"

# shellcheck disable=SC2086
$QEMU \
    $QEMU_MACHINE \
    -m "$MEM" \
    $KERNEL_ARGS \
    -append "$KERNEL_CMDLINE" \
    $ROOT_DEV \
    $NET_ARGS \
    $FSDEV_ARGS \
    $EXTRA_LANE_ARGS \
    ${T_EXTRA_QEMU:-} \
    -serial stdio \
    -display none \
    -monitor none \
    -no-reboot \
    2>&1 | tee "$LOG" &

QEMU_PID=$!

# ── stall watchdog ───────────────────────────────────────────────────────────
# Poll every 0.5s; kill immediately when test finishes — init exits but QEMU
# keeps running, so we must kill it rather than wait for poweroff.
last_size=0
stall_ticks=0
boot_seen=0
STALL_TICKS=$((STALL * 2))   # each tick = 0.5s
while kill -0 "$QEMU_PID" 2>/dev/null; do
    sleep 0.5
    cur_size=$(wc -c < "$LOG" 2>/dev/null || echo 0)
    if [ "$cur_size" != "$last_size" ]; then
        last_size=$cur_size
        stall_ticks=0
        # Confirm kernel actually started before matching terminal markers
        [ "$boot_seen" = "0" ] && grep -q 'b1nix kernel\|Step 1:' "$LOG" 2>/dev/null && boot_seen=1
    else
        stall_ticks=$((stall_ticks + 1))
    fi
    # Kill immediately on any terminal marker — only after kernel booted
    if [ "$boot_seen" = "1" ] && grep -q 'INIT-EXIT\|B1NIX-TEST: done\|KERNEL PANIC\|\[PANIC\]' "$LOG" 2>/dev/null; then
        sleep 0.5   # let tee flush the last bytes
        break
    fi
    if [ "$stall_ticks" -ge "$STALL_TICKS" ]; then
        echo ""
        echo "[t] STALL: no output for ${STALL}s — killing QEMU"
        kill "$QEMU_PID" 2>/dev/null || true
        break
    fi
done

wait "$QEMU_PID" 2>/dev/null || true

echo ""
echo "[t] ---"

#───────────────────────────── result summary ────────────────────────────────
# Print only lines matching test markers or errors (skip kernel noise)
MARKER_PREFIX="$(echo "$TEST_NAME" | tr '[:lower:]_' '[:upper:]-' | sed 's/-SMOKE$//')"

echo "[t] === markers for $TEST_NAME ==="
grep -a "^${MARKER_PREFIX}\|PANIC\|panic\|INIT-EXIT\|B1NIX-TEST: done\|Killed\|SMOKE-WATCHDOG" \
    "$LOG" 2>/dev/null || true

# Exit code: 0 if no FAIL marker, 1 if any FAIL seen
if grep -q "PANIC\|\[PANIC\]" "$LOG" 2>/dev/null; then
    echo "[t] RESULT: PANIC"
    exit 1
fi

# Count ok vs missing markers
OK=$(grep -ca "^${MARKER_PREFIX}.*: ok" "$LOG" 2>/dev/null || true)
DONE=$(grep -ca "^${MARKER_PREFIX}.*: done" "$LOG" 2>/dev/null || true)
echo "[t] ok=$OK done=$DONE"
[ "$DONE" -ge 1 ] && echo "[t] RESULT: PASS" || echo "[t] RESULT: INCOMPLETE (no 'done' marker)"
