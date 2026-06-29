#!/bin/sh
# M64 native-Clang self-host proof for b1nix.
#
# The b1nix-native clang-22 (~94MB; statically links LLVM + libstdc++, so its
# only DT_NEEDED is libc.so.1) is far too big for the xxd-embedded initramfs, so
# — exactly like rustc (tools/rust/rust-proof.sh) and d8 — it ships inside a
# single self-contained ISO as a GRUB Multiboot2 module (ext4), which b1nix
# exposes as the ram0 block device. With b1nix.clangrun (handled in
# kernel/main.c) the kernel mounts ram0 -> /mnt/clang and:
#   (1) runs `clang --version` — the real version banner on serial proves the
#       loaded compiler executes on b1nix (load proof);
#   (2) runs `clang -c hello.c -o hello.o` and checks the emitted object's ELF
#       magic — proves the frontend+backend+integrated assembler produce a valid
#       b1nix object natively (compile proof / genuine self-host).
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
USR="$ROOT_DIR/build/native-clang/b1nix/usr"
KELF="$ROOT_DIR/build/x86_64/kernel.elf"
OUT="$ROOT_DIR/build/clang-out"
IMG="$OUT/clang.img"
ISO="$OUT/b1nix-clang.iso"
LOG="${LOG:-$ROOT_DIR/smoke_run/clang-run.log}"
TIMEOUT="${TIMEOUT:-240}"

CLANG="$USR/bin/clang-22"
RESDIR="$(ls -d "$USR"/lib/clang/* 2>/dev/null | head -1)"   # .../lib/clang/22
LIBC="$ROOT_DIR/userspace/build/x86_64/libc.so.1"

# M75/self-host: CLANG_DYNAMIC=1 proves the DYNAMIC clang (44MB, NEEDED
# libLLVM-22.so) + the 72MB shared libLLVM-22.so load + run via the M69 loader —
# i.e. the kernel demand-pages a 72MB .so. This is the runtime validation of the
# dynamic toolchain (lower self-host floor) and the libLLVM.so path for llvmpipe.
LIBLLVM=""
if [ "${CLANG_DYNAMIC:-0}" = "1" ]; then
	DI="$ROOT_DIR/build/native-clang/b1nix-dyn-install"
	CLANG="$(find "$DI" -name clang-22 -type f 2>/dev/null | head -1)"
	# RESDIR stays the static install's lib/clang/22 — same clang 22 resource
	# headers (and the function-only compile proof needs none anyway).
	LIBLLVM="$USR/lib/libLLVM.so"   # the real 72MB file (soname libLLVM-22.so)
	[ -f "$LIBLLVM" ] || { echo "missing $LIBLLVM — run tools/build-native-clang-dynamic.sh"; exit 1; }
fi

for f in "$CLANG" "$LIBC" "$KELF"; do
	[ -f "$f" ] || { echo "missing: $f (build it first: tools/build-native-clang.sh --b1nix-elf; make ARCH=x86_64 iso)"; exit 1; }
done
[ -n "$RESDIR" ] && [ -d "$RESDIR/include" ] || { echo "missing clang resource headers under $USR/lib/clang"; exit 1; }
STRIP="${STRIP:-strip}"

if [ "${SKIP_PACK:-0}" = "1" ] && [ -f "$IMG" ]; then
	echo "=== [1-2/4] SKIP_PACK: reusing existing $IMG ==="
else
echo "=== [1/4] stage clang + libc.so.1 + hello.c ==="
STAGE="$OUT/stage"
rm -rf "$STAGE"; mkdir -p "$STAGE/bin" "$STAGE/lib"
cp "$CLANG" "$STAGE/bin/clang"
# strip --strip-unneeded keeps .dynsym/.dynstr/.rela.* (what the kernel loader
# needs for DT_NEEDED loading) and drops only .symtab/.debug_*.
"$STRIP" --strip-unneeded "$STAGE/bin/clang" 2>/dev/null || true
cp "$LIBC" "$STAGE/lib/libc.so.1"
# M75: ship the 72MB shared LLVM next to the dynamic clang (resolved by soname).
[ -n "$LIBLLVM" ] && cp "$LIBLLVM" "$STAGE/lib/libLLVM-22.so"
# Header-free TU: the compile proof needs only the backend + integrated
# assembler, no sysroot and no clang resource headers (a function-only source
# pulls in no builtin headers). Add CLANG_STAGE_RESOURCE=1 to ship them anyway.
printf 'int main(void) { return 42; }\n' > "$STAGE/hello.c"
if [ "${CLANG_STAGE_RESOURCE:-0}" = "1" ]; then
	RESV="$(basename "$RESDIR")"
	mkdir -p "$STAGE/lib/clang/$RESV"
	cp -R "$RESDIR/include" "$STAGE/lib/clang/$RESV/"
fi
SZ=$(du -sm "$STAGE" | cut -f1)
echo "  staged ${SZ}MB"

echo "=== [2/4] build ext4 RAM module clang.img ==="
mkdir -p "$OUT"
IMG_MB=$(( SZ + 64 ))
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$IMG_MB" status=none
mke2fs -F -q -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -b 4096 "$IMG"
debugfs -w "$IMG" >/dev/null 2>&1 <<EOF
mkdir /bin
mkdir /lib
cd /bin
write $STAGE/bin/clang clang
cd /lib
write $STAGE/lib/libc.so.1 libc.so.1
$([ -n "$LIBLLVM" ] && echo "write $STAGE/lib/libLLVM-22.so libLLVM-22.so")
cd /
write $STAGE/hello.c hello.c
EOF
if [ "${CLANG_STAGE_RESOURCE:-0}" = "1" ]; then
	( cd "$STAGE" && find lib/clang -type d | while IFS= read -r d; do
		debugfs -w "$IMG" -R "mkdir /$d" >/dev/null 2>&1 || true
	done )
	( cd "$STAGE" && find lib/clang -type f | while IFS= read -r f; do
		debugfs -w "$IMG" -R "write $STAGE/$f /$f" >/dev/null 2>&1 || true
	done )
fi
echo "  clang.img = ${IMG_MB}MB"
fi

echo "=== [3/4] pack self-contained ISO (cmdline: b1nix.test=1 b1nix.clangrun) ==="
MKRESCUE="$(command -v grub-mkrescue 2>/dev/null || command -v grub2-mkrescue 2>/dev/null || command -v i686-elf-grub-mkrescue 2>/dev/null)"
[ -n "$MKRESCUE" ] || { echo "missing grub-mkrescue"; exit 1; }
ISODIR="$OUT/iso"
rm -rf "$ISODIR"; mkdir -p "$ISODIR/boot/grub"
cp "$KELF" "$ISODIR/boot/kernel.elf"
cp "$IMG" "$ISODIR/boot/clang.img"
# The clangrun block fires after the b1nix.test=1 smoke modules; CLANG_FOCUS=1
# drops b1nix.test=1 so the (slow) compiler proof is reached promptly instead of
# competing with the whole suite for the timeout budget.
CMDLINE="${CLANG_CMDLINE:-b1nix.test=1 b1nix.clangrun}"
[ "${CLANG_FOCUS:-0}" = "1" ] && CMDLINE="b1nix.clangrun"
sed -e 's|@TIMEOUT@|0|g' -e 's|@ARCH@|x86_64|g' \
    -e "s|@CMDLINE@|$CMDLINE|g" \
    -e 's|@MODULE_CMD@|module2 /boot/clang.img clangimg|g' \
    "$ROOT_DIR/boot/grub/grub.cfg" > "$ISODIR/boot/grub/grub.cfg"
"$MKRESCUE" -o "$ISO" "$ISODIR" 2>/dev/null

echo "=== [4/4] run in QEMU ==="
mkdir -p "$ROOT_DIR/smoke_run"
accel=""
if [ -w /dev/kvm ] && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw kvm; then
	accel="-accel kvm -cpu host"
fi
set -- qemu-system-x86_64 $accel -m "${CLANG_MEM_MB:-2048}" \
	-cdrom "$ISO" -serial stdio -display none -monitor none -no-reboot \
	-device isa-debug-exit,iobase=0xf4,iosize=0x04
echo "[clang-run] $*"
"$@" >"$LOG" 2>&1 &
pid=$!

start=$(date +%s)
while :; do
	if grep -qaE "M64-NATIVE-CLANG: ok compile|M64-NATIVE-CLANG: fail|KERNEL PANIC|\[PANIC\]" "$LOG" 2>/dev/null; then break; fi
	kill -0 "$pid" 2>/dev/null || break
	[ $(( $(date +%s) - start )) -ge "$TIMEOUT" ] && { echo "[clang-run] timeout ${TIMEOUT}s"; break; }
	sleep 2
done
kill "$pid" 2>/dev/null || true; sleep 1; kill -9 "$pid" 2>/dev/null || true

echo "==================== clang run result ===================="
grep -aiE "clang:|clang version|M64-NATIVE-CLANG|mount ram0|ELF|reloc|PANIC|page fault|#GP|#PF|out of memory" "$LOG" | tail -50
echo "----"
if grep -qa "M64-NATIVE-CLANG: ok clang-load" "$LOG" && grep -qa "M64-NATIVE-CLANG: ok compile" "$LOG"; then
	echo "PASS: native clang loaded and compiled a b1nix object on b1nix"
else
	echo "FAIL — missing M64-NATIVE-CLANG markers; see $LOG"
fi
echo "full log: $LOG"
