#!/bin/sh
# M26 native-Clang KERNEL self-host proof: b1nix compiles+links its OWN kernel,
# in-guest, with its own clang + ld.lld.
#
# Ships the self-host module (tools/inguest/build-selfhost-module.sh) as the ram0
# ext4 GRUB module and boots with b1nix.selfhostbuild, which makes the kernel
# (kernel/main.c run_selfhost_build) compile every kernel TU with clang and link
# them with ld.lld, then verify the produced kernel.elf. Mirrors the rustc/clang
# proofs. Slow: ~92 clang invocations, each loading the 94MB clang via the M69
# loader — give it a long timeout.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
KELF="$ROOT_DIR/build/x86_64/kernel.elf"
IMG="$ROOT_DIR/build/selfhost-out/selfhost.img"
OUT="$ROOT_DIR/build/selfhost-out"
ISO="$OUT/b1nix-selfhost.iso"
LOG="${LOG:-$ROOT_DIR/smoke_run/selfhost-run.log}"
TIMEOUT="${TIMEOUT:-5400}"

if [ "${SKIP_PACK:-0}" != "1" ] || [ ! -f "$IMG" ]; then
	echo "=== [0] build self-host module ==="
	sh "$ROOT_DIR/tools/inguest/build-selfhost-module.sh"
fi
for f in "$KELF" "$IMG"; do
	[ -f "$f" ] || { echo "missing: $f"; exit 1; }
done

echo "=== [1] pack self-contained ISO (cmdline: b1nix.test=1 b1nix.selfhostbuild) ==="
MKRESCUE="$(command -v grub-mkrescue 2>/dev/null || command -v grub2-mkrescue 2>/dev/null || command -v i686-elf-grub-mkrescue 2>/dev/null)"
[ -n "$MKRESCUE" ] || { echo "missing grub-mkrescue"; exit 1; }
ISODIR="$OUT/iso"
rm -rf "$ISODIR"; mkdir -p "$ISODIR/boot/grub"
cp "$KELF" "$ISODIR/boot/kernel.elf"
cp "$IMG" "$ISODIR/boot/selfhost.img"
sed -e 's|@TIMEOUT@|0|g' -e 's|@ARCH@|x86_64|g' \
    -e 's|@CMDLINE@|b1nix.test=1 b1nix.selfhostbuild|g' \
    -e 's|@MODULE_CMD@|module2 /boot/selfhost.img selfhostimg|g' \
    "$ROOT_DIR/boot/grub/grub.cfg" > "$ISODIR/boot/grub/grub.cfg"
"$MKRESCUE" -o "$ISO" "$ISODIR" 2>/dev/null

echo "=== [2] run in QEMU ==="
mkdir -p "$ROOT_DIR/smoke_run"
accel=""
if [ -w /dev/kvm ] && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw kvm; then
	accel="-accel kvm -cpu host"
fi
# 107 in-guest clang invocations each load the ~94MB clang via the M69 loader;
# the kernel reaps each on exit, but the peak working set across the build needs
# real headroom — 4-8GB triple-faults around TU 73 (main.c). 16GB completes.
set -- qemu-system-x86_64 $accel -m "${SELFHOST_MEM_MB:-16384}" \
	-cdrom "$ISO" -serial stdio -display none -monitor none -no-reboot \
	-device isa-debug-exit,iobase=0xf4,iosize=0x04
echo "[selfhost-run] $*"
"$@" >"$LOG" 2>&1 &
pid=$!

start=$(date +%s)
while :; do
	if grep -qaE "M26-SELFHOST: ok kernel-elf|M26-SELFHOST: fail|KERNEL PANIC|\[PANIC\]" "$LOG" 2>/dev/null; then break; fi
	kill -0 "$pid" 2>/dev/null || break
	[ $(( $(date +%s) - start )) -ge "$TIMEOUT" ] && { echo "[selfhost-run] timeout ${TIMEOUT}s"; break; }
	sleep 5
done
kill "$pid" 2>/dev/null || true; sleep 1; kill -9 "$pid" 2>/dev/null || true

echo "==================== self-host run result ===================="
grep -aiE "selfhost:|M26-SELFHOST|cc-fail|mount ram0|PANIC|page fault|#GP|#PF|out of memory" "$LOG" | tail -60
echo "----"
if grep -qa "M26-SELFHOST: ok kernel-elf" "$LOG"; then
	echo "PASS: b1nix compiled and linked its own kernel with native clang + ld.lld"
else
	echo "FAIL — see $LOG"
fi
echo "full log: $LOG"
