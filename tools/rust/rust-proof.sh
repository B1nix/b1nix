#!/bin/sh
# M68 native-Rust proof for b1nix.
#
# The native rustc ELF plus its shared-object chain (librustc_driver.so ~89MB
# stripped, libLLVM.so ~160MB stripped, libgcc_s.so) are far too big for the
# xxd-embedded initramfs, so — exactly like d8 in tools/v8/ — they ship inside a
# single self-contained ISO as a GRUB Multiboot2 module (ext4), which b1nix
# exposes as the ram0 block device. With b1nix.rustrun (handled in
# kernel/main.c) the kernel mounts ram0 -> /mnt/rust and launches /bin/rustc.
#
# rustc has librustc_driver.so + libLLVM.so as DT_NEEDED and NO PT_INTERP, so
# merely loading it drives the M69 recursive exec-time dynamic linker across the
# whole ~250MB .so graph (rustc -> librustc_driver.so -> libLLVM.so ->
# libgcc_s.so). `rustc --version` then proves the loaded compiler executes on
# b1nix by printing its real version string to serial — that string is the
# pass marker (no fake: only the genuinely-loaded compiler can emit it).
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ST="$ROOT_DIR/build/rust-native/rust-src-full/build/x86_64-unknown-b1nix/stage2"
KELF="$ROOT_DIR/build/x86_64/kernel.elf"
OUT="$ROOT_DIR/build/rust-out"
IMG="$OUT/rust.img"
ISO="$OUT/b1nix-rust.iso"
LOG="${LOG:-$ROOT_DIR/smoke_run/rust-run.log}"
TIMEOUT="${TIMEOUT:-300}"

RUSTC="$ST/bin/rustc"
DRV="$ST/lib/librustc_driver-b089cdab8985b1db.so"
LLVM="$ST/lib/libLLVM.so.22.1-rust-1.98.0-nightly"
LIBGCC="$ROOT_DIR/build/toolchain_build/x86_64-b1nix/cross/x86_64-b1nix/lib/libgcc_s.so.1"

for f in "$RUSTC" "$DRV" "$LLVM" "$LIBGCC" "$KELF"; do
	[ -f "$f" ] || { echo "missing: $f"; exit 1; }
done
STRIP="${STRIP:-strip}"

if [ "${SKIP_PACK:-0}" = "1" ] && [ -f "$IMG" ]; then
	echo "=== [1-2/4] SKIP_PACK: reusing existing $IMG ==="
else
echo "=== [1/4] stage + strip the .so chain ==="
STAGE="$OUT/stage"
rm -rf "$STAGE"; mkdir -p "$STAGE/bin" "$STAGE/lib"
cp "$RUSTC" "$STAGE/bin/rustc"
# strip --strip-unneeded keeps .dynsym/.dynstr/.rela.* (what the kernel loader
# needs) and drops only .symtab/.debug_* — safe for DT_NEEDED loading.
cp "$DRV"  "$STAGE/lib/librustc_driver-b089cdab8985b1db.so"; "$STRIP" --strip-unneeded "$STAGE/lib/librustc_driver-b089cdab8985b1db.so"
cp "$LLVM" "$STAGE/lib/libLLVM.so.22.1-rust-1.98.0-nightly";  "$STRIP" --strip-unneeded "$STAGE/lib/libLLVM.so.22.1-rust-1.98.0-nightly"
cp "$LIBGCC" "$STAGE/lib/libgcc_s.so"   # tiny; leave symbols intact
SZ=$(du -sm "$STAGE" | cut -f1)
echo "  staged ${SZ}MB"

echo "=== [2/4] build ext4 RAM module rust.img ==="
mkdir -p "$OUT"
IMG_MB=$(( SZ + 80 ))
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$IMG_MB" status=none
# Flags the b1nix ext4 driver supports (see CLAUDE.md).
mke2fs -F -q -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -b 4096 "$IMG"
debugfs -w "$IMG" >/dev/null 2>&1 <<EOF
mkdir /bin
mkdir /lib
cd /bin
write $STAGE/bin/rustc rustc
cd /lib
write $STAGE/lib/librustc_driver-b089cdab8985b1db.so librustc_driver-b089cdab8985b1db.so
write $STAGE/lib/libLLVM.so.22.1-rust-1.98.0-nightly libLLVM.so.22.1-rust-1.98.0-nightly
write $STAGE/lib/libgcc_s.so libgcc_s.so
EOF
echo "  rust.img = ${IMG_MB}MB"
fi

echo "=== [3/4] pack self-contained ISO (cmdline: b1nix.test=1 b1nix.rustrun) ==="
MKRESCUE="$(command -v grub-mkrescue 2>/dev/null || command -v grub2-mkrescue 2>/dev/null || command -v i686-elf-grub-mkrescue 2>/dev/null)"
[ -n "$MKRESCUE" ] || { echo "missing grub-mkrescue"; exit 1; }
ISODIR="$OUT/iso"
rm -rf "$ISODIR"; mkdir -p "$ISODIR/boot/grub"
cp "$KELF" "$ISODIR/boot/kernel.elf"
cp "$IMG" "$ISODIR/boot/rust.img"
sed -e 's|@TIMEOUT@|0|g' -e 's|@ARCH@|x86_64|g' \
    -e 's|@CMDLINE@|b1nix.test=1 b1nix.rustrun|g' \
    -e 's|@MODULE_CMD@|module2 /boot/rust.img rustimg|g' \
    "$ROOT_DIR/boot/grub/grub.cfg" > "$ISODIR/boot/grub/grub.cfg"
"$MKRESCUE" -o "$ISO" "$ISODIR" 2>/dev/null

echo "=== [4/4] run in QEMU ==="
mkdir -p "$ROOT_DIR/smoke_run"
accel=""
if [ -w /dev/kvm ] && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw kvm; then
	accel="-accel kvm"
fi
set -- qemu-system-x86_64 $accel -m "${RUST_MEM_MB:-4096}" \
	-cdrom "$ISO" -serial stdio -display none -monitor none -no-reboot \
	-device isa-debug-exit,iobase=0xf4,iosize=0x04
echo "[rust-run] $*"
"$@" >"$LOG" 2>&1 &
pid=$!

start=$(date +%s)
while :; do
	if grep -qaE "rustc 1\.|M68-RUST: fail|KERNEL PANIC|\[PANIC\]" "$LOG" 2>/dev/null; then break; fi
	kill -0 "$pid" 2>/dev/null || break
	[ $(( $(date +%s) - start )) -ge "$TIMEOUT" ] && { echo "[rust-run] timeout ${TIMEOUT}s"; break; }
	sleep 2
done
kill "$pid" 2>/dev/null || true; sleep 1; kill -9 "$pid" 2>/dev/null || true

echo "==================== rust run result ===================="
grep -aiE "rust:|rustc |M68-RUST|mount ram0|ELF|dynamic|reloc|PANIC|page fault|#GP|#PF|out of memory" "$LOG" | tail -50
echo "----"
if grep -qa "M68-RUST: ok rustc-load" "$LOG" && grep -qaE "rustc 1\." "$LOG"; then
	echo "PASS: native rustc loaded its ~250MB .so chain via the M69 linker and ran on b1nix"
else
	echo "FAIL — missing M68-RUST load marker or rustc version string; see $LOG"
fi
echo "full log: $LOG"
