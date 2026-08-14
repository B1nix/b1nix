#!/bin/sh
# M26 native-Clang KERNEL self-host proof: b1nix compiles+links its OWN kernel,
# in-guest, with its own clang + ld.lld.
#
# Ships the self-host module (tools/inguest/build-selfhost-module.sh) as the ram0
# ext4 Multiboot2 module and boots with b1nix.selfhostbuild, which makes the kernel
# spawn /mnt/build/bin/selfhost-build (userspace ELF) that reads srcs.txt,
# compiles every kernel TU with clang, and links them with ld.lld, then verifies
# the produced kernel.elf. Mirrors the rustc/clang proofs. Slow: ~92 clang
# invocations, each loading the 94MB clang via the M69 loader — give it a long
# timeout.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
KELF="$ROOT_DIR/build/x86_64/kernel.elf"
# OUT/IMG are overridable so two runs (e.g. ram0 vs disk) can execute in parallel
# without clobbering each other's ISO/iso dir. Point SELFHOST_IMG at an existing
# module (+ SKIP_PACK=1) to share one toolchain image between both.
IMG="${SELFHOST_IMG:-$ROOT_DIR/build/selfhost-out/selfhost.img}"
OUT="${SELFHOST_OUT:-$ROOT_DIR/build/selfhost-out}"
mkdir -p "$OUT"
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

# b1nix.selfhostonly keeps the initramfs as / (so the boot device isn't grabbed
# as the real root) AND makes init idle — NO smoke suite (Mesa/V8/NetSurf) and NO
# production getty/login/sshd competing with the build for RAM. This isolates the
# self-host's true memory need, unlike the old b1nix.test=1 config which ran the
# whole heavy smoke sequence alongside the build. Set SELFHOST_TESTMODE=1 to fall
# back to the old b1nix.test=1 behaviour for comparison.
# SELFHOST_DISK=1 sources the toolchain from a real SATA disk (b1nix.selfhostdisk
# -> mount sda) instead of the ram0 Multiboot2 module. The module is a ramdisk whose
# ~217 MB stay pinned in RAM the whole build; a disk leaves that free and streams
# the toolchain off AHCI through the (read-ahead) block cache, so the self-host
# fits in less RAM.
MODE_FLAG="b1nix.selfhostonly"
[ "${SELFHOST_TESTMODE:-0}" = "1" ] && MODE_FLAG="b1nix.test=1"
DISK_IMG=""
if [ "${SELFHOST_DISK:-0}" = "1" ]; then
	CMDLINE="$MODE_FLAG b1nix.selfhostbuild b1nix.selfhostdisk"
	# Build writes .o/TMPDIR onto the toolchain fs — use a throwaway copy so the
	# source image is never mutated.
	DISK_IMG="$OUT/selfhost-disk.img"
	cp "$IMG" "$DISK_IMG"
else
	CMDLINE="$MODE_FLAG b1nix.selfhostbuild"
fi
echo "=== [1] pack self-contained ISO (cmdline: $CMDLINE) ==="
ISODIR="$OUT/iso"
rm -rf "$ISODIR"
set -- --stage "$ISODIR" --out "$ISO" --arch x86_64 --kernel "$KELF" \
       --timeout 0 --cmdline "$CMDLINE"
[ "${SELFHOST_DISK:-0}" = "1" ] || set -- "$@" --module "$IMG:selfhostimg"
"$ROOT_DIR/tools/mkiso.sh" "$@" >/dev/null

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
if [ -n "$DISK_IMG" ]; then
	# Toolchain on a SATA/AHCI disk (sda) — the kernel mounts it instead of ram0.
	set -- "$@" -device ich9-ahci,id=ahci \
		-drive file="$DISK_IMG",if=none,id=shdisk,format=raw \
		-device ide-hd,drive=shdisk,bus=ahci.0
	# SELFHOST_SWAP_MB>0 attaches a blank swap disk on the next AHCI port (sdb).
	# swap_init() auto-detects "sdb" and activates swap, so when clang's per-TU
	# compile heap exceeds the (small) RAM the kernel spills cold pages to swap
	# instead of OOM-killing it — this is what lets the self-host link its kernel
	# at 512 MiB and below (slower, but it completes). Default 2048 MiB of swap.
	SWAP_MB="${SELFHOST_SWAP_MB:-2048}"
	if [ "$SWAP_MB" -gt 0 ]; then
		SWAP_IMG="$OUT/selfhost-swap.img"
		truncate -s "${SWAP_MB}M" "$SWAP_IMG" 2>/dev/null || \
			dd if=/dev/zero of="$SWAP_IMG" bs=1M count="$SWAP_MB" status=none
		set -- "$@" \
			-drive file="$SWAP_IMG",if=none,id=swapdisk,format=raw \
			-device ide-hd,drive=swapdisk,bus=ahci.1
	fi
fi
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
