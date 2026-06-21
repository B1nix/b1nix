#!/bin/sh
# One-shot V8 pipeline for b1nix: gen -> build -> link -> stamp disk -> ISO -> run.
#
# Replaces the manual gn-gen / ninja / v8-link-d8 / debugfs / make iso / qemu
# dance with a single command. Each stage reuses the existing per-stage scripts;
# this just chains them and threads one PROFILE name through.
#
#   sh tools/v8-build-run.sh                 # default: maglev + code-cage, TurboFan tier
#   PROFILE=b1nix-jit-pcdata TIER=sparkplug sh tools/v8-build-run.sh   # rebuild the proven config
#   SKIP_BUILD=1 sh tools/v8-build-run.sh    # binary already linked -> just stamp+ISO+run
#
# Env knobs:
#   PROFILE   gn out dir under v8/out/  (default b1nix-jit-maglev)
#   GN_ARGS   full gn --args string     (default: maglev+code-cage on, see below)
#   TIER      jitless | sparkplug | turbofan  (default turbofan -> exercises Maglev tier-up)
#   SKIP_BUILD=1   skip gen+ninja+link, reuse the existing out/$PROFILE/d8.b1nix
#   TIMEOUT   qemu marker wait (default 120)
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SK="$ROOT_DIR/build/toolchain_build/v8-skeleton"
GN="$SK/gn-src/out/gn"
V8="$SK/v8"
PROFILE="${PROFILE:-b1nix-jit-maglev}"
TIER="${TIER:-turbofan}"
OUT="$V8/out/$PROFILE"
D8="$OUT/d8.b1nix"
DISK="$ROOT_DIR/build/v8-out/v8-$PROFILE-ext4.img"
SEED="$ROOT_DIR/build/v8-out/v8-jit-ext4.img"   # carries m58.js + a d8 to overwrite

# Default config: the proven JIT base + Maglev (mid-tier) + external code space
# (code cage) + the sandbox (TrustedSpace/sandboxed pointers; needs
# use_safe_libstdcxx for the hardening assert — see tools/v8-gen-jit.sh).
GN_ARGS="${GN_ARGS:-target_os=\"b1nix\" target_cpu=\"x64\" is_clang=false treat_warnings_as_errors=false v8_enable_i18n_support=false is_debug=false v8_jitless=false v8_use_external_startup_data=false symbol_level=0 use_custom_libcxx=false use_safe_libstdcxx=true v8_enable_temporal_support=false v8_enable_sparkplug=true v8_enable_maglev=true v8_enable_turbofan=true v8_enable_webassembly=false v8_enable_sandbox=true v8_enable_pointer_compression=true v8_enable_external_code_space=true}"

case "$TIER" in
	jitless)   V8FLAGS="b1nix.v8run" ;;
	sparkplug) V8FLAGS="b1nix.v8run b1nix.v8jit" ;;
	turbofan)  V8FLAGS="b1nix.v8run b1nix.v8jit b1nix.v8opt" ;;
	*) echo "TIER must be jitless|sparkplug|turbofan"; exit 1 ;;
esac

if [ "${SKIP_BUILD:-0}" != "1" ]; then
	echo "=== [1/6] gn gen -> out/$PROFILE ==="
	[ -x "$GN" ] || { echo "gn missing — run sh tools/build-gn.sh"; exit 1; }
	sh "$ROOT_DIR/tools/patches/v8/apply.sh" "$V8" || true
	# Route the compiler through ccache when available. Flipping a global gn flag
	# (sandbox/wasm/i18n) changes a define that ~every TU depends on, so ninja
	# legitimately recompiles almost everything — but ccache turns a re-run of any
	# config (or a flip back) into near-instant cache hits. No-op if ccache absent.
	if command -v ccache >/dev/null 2>&1; then
		GN_ARGS="$GN_ARGS cc_wrapper=\"ccache\""
		echo "  (ccache detected -> cc_wrapper=ccache)"
	fi
	( cd "$V8" && "$GN" gen "out/$PROFILE" --args="$GN_ARGS" )

	echo "=== [2/6] ninja d8 (the final crt0 link is EXPECTED to fail; we relink) ==="
	ninja -C "$OUT" d8 || true

	echo "=== [3/6] relink d8 as a b1nix ELF ==="
	sh "$ROOT_DIR/tools/v8-link-d8.sh" "$PROFILE"
fi

[ -f "$D8" ] || { echo "missing $D8 — build failed"; exit 1; }

echo "=== [4/6] stamp d8 onto $DISK ==="
[ -f "$SEED" ] || { echo "missing seed disk $SEED"; exit 1; }
cp "$SEED" "$DISK"
debugfs -w -R "rm /d8" "$DISK" >/dev/null 2>&1 || true
debugfs -w -R "write $D8 d8" "$DISK"
# Refresh m58.js from the repo (the seed disk often carries a stale/short variant).
M58JS="$ROOT_DIR/build/v8-out/m58.js"
if [ -f "$M58JS" ]; then
	debugfs -w -R "rm /m58.js" "$DISK" >/dev/null 2>&1 || true
	debugfs -w -R "write $M58JS m58.js" "$DISK"
fi
# Verify the on-disk copy byte-for-byte (stale-disk bugs have bitten before).
debugfs -R "dump /d8 /tmp/v8-d8-check.$$" "$DISK" >/dev/null 2>&1
cmp "$D8" "/tmp/v8-d8-check.$$" && echo "  disk d8 matches host" || { echo "  DISK STAMP MISMATCH"; rm -f "/tmp/v8-d8-check.$$"; exit 1; }
rm -f "/tmp/v8-d8-check.$$"

echo "=== [5/6] build ISO (cmdline: b1nix.test=1 $V8FLAGS) ==="
# Lightweight: reuse the existing kernel.elf (its xxd-embedded initramfs carries
# the base rootfs; d8 + m58.js ride the sata disk). Just re-sed grub.cfg with our
# cmdline and re-pack — NO full `make iso`, which would needlessly rebuild Mesa/
# curl and the whole userspace ports tree for a cmdline-only change.
KELF="$ROOT_DIR/build/x86_64/kernel.elf"
[ -f "$KELF" ] || { echo "missing $KELF — run 'make ARCH=x86_64' once to build the kernel"; exit 1; }
MKRESCUE="$(command -v grub-mkrescue 2>/dev/null || command -v grub2-mkrescue 2>/dev/null || command -v i686-elf-grub-mkrescue 2>/dev/null)"
[ -n "$MKRESCUE" ] || { echo "missing grub-mkrescue"; exit 1; }
ISODIR="$ROOT_DIR/build/x86_64/iso-v8-$PROFILE"
ISO="$ROOT_DIR/build/x86_64/b1nix-v8-$PROFILE.iso"
mkdir -p "$ISODIR/boot/grub"
cp "$KELF" "$ISODIR/boot/kernel.elf"
# Ship d8's ext4 image INSIDE the ISO as a GRUB Multiboot2 module (-> kernel
# ram0), so the whole thing is one self-contained ISO — no separate QEMU -drive.
cp "$DISK" "$ISODIR/boot/v8.img"
sed -e 's|@TIMEOUT@|0|g' -e 's|@ARCH@|x86_64|g' \
    -e "s|@CMDLINE@|b1nix.test=1 $V8FLAGS|g" \
    -e 's|@MODULE_CMD@|module2 /boot/v8.img v8img|g' \
    "$ROOT_DIR/boot/grub/grub.cfg" > "$ISODIR/boot/grub/grub.cfg"
"$MKRESCUE" -o "$ISO" "$ISODIR" 2>/dev/null

echo "=== [6/6] run in QEMU (module=$PROFILE, tier=$TIER) ==="
RUNLOG="$ROOT_DIR/smoke_run/v8-run-$PROFILE-$TIER.log"
ISO="$ISO" LOG="$RUNLOG" TIMEOUT="${TIMEOUT:-120}" sh "$ROOT_DIR/tools/v8-run-qemu.sh"
echo "serial log: $RUNLOG"
