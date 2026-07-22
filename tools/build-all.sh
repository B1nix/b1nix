#!/usr/bin/env bash
# tools/build-all.sh — master build orchestrator for b1nix.
#
# ONE entry point that builds the working b1nix system end-to-end by REUSING the
# existing per-component build scripts and Makefile targets. It never inlines or
# duplicates their logic: each stage just invokes the canonical script/target
# with the right environment. So when a port or feature script under tools/ (or
# a Makefile target) changes, this orchestrator does NOT need editing.
#
# Architecture: x86_64 is the ONLY actively-maintained arch (x86/i686 is FROZEN,
# per docs/roadmap.md "Current Project Status"). This script therefore defaults
# to ARCH=x86_64 and never builds or smokes x86. Override with ARCH=... if you
# really must, but that path is unsupported.
#
# ─────────────────────────────────────────────────────────────────────────────
# DEFAULT (no flags): build the OS + bootable test ISO
#     1. Cross toolchain  — tools/toolchain/build-toolchain.sh  (only if missing)
#     2. Userspace        — make ARCH=x86_64 userspace  (libc, crt0, ELF binaries)
#     3. Kernel + ISO     — make ARCH=x86_64 KERNEL_CMDLINE=... iso
#        (the default iso already embeds the shipping ports: curl, wget, bash,
#         dropbear, busybox, netsurf, the M40 Linux + M67 rust artifacts, etc.)
#
# OPT-IN heavy/optional components (each just calls its existing script):
#     --with-cross            force-rebuild the cross GCC/binutils even if present
#                             (tools/toolchain/build-toolchain.sh)
#     --with-native-toolchain native GCC/binutils that run inside b1nix (M26)
#                             (tools/toolchain/build-native-toolchain.sh
#                              + make install-native-toolchain)
#     --with-native-clang     native Clang/LLVM that runs inside b1nix (M64)
#                             (tools/build-native-clang.sh --b1nix-elf)
#     --with-dynamic-clang    dynamic (libLLVM.so) form of the native clang
#                             (tools/build-native-clang-dynamic.sh)
#     --with-rust             Rust cross build for x86_64-unknown-b1nix (M67)
#                             (tools/build-rust-toolchain.sh)
#     --with-v8               full V8 d8 pipeline: gen -> build -> link -> ISO
#                             (tools/v8/v8-build-run.sh)
#     --with-mesa-llvmpipe    Mesa llvmpipe software-GL stack (M75)
#                             (MESA_LLVMPIPE=1 tools/ports/build-mesa.sh)
#     --with-ports            install the userspace port packages into the rootfs
#                             (make ARCH=x86_64 install-ports)
#     --all                   enable every --with-* above
#     -h | --help             show this help and exit
#
# Each flag has an env equivalent (WITH_NATIVE_CLANG=1, WITH_V8=1, ...).
# Other passthrough env:
#     ARCH=x86_64             target arch (do not change; x86 is frozen)
#     KERNEL_CMDLINE          grub cmdline for the final ISO (default b1nix.test=1)
#     ISO_TARGET              make iso target (default: iso)
#
# The cross toolchain and the kernel/ISO are the only mandatory stages; the cross
# stage is skipped when its compiler already exists (idempotent re-runs). The
# kernel/ISO is ALWAYS rebuilt so the ISO reflects everything built this run.
#
# Verify syntax without running:  sh -n tools/build-all.sh
# ─────────────────────────────────────────────────────────────────────────────
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

# ── Arch (x86_64 only — x86 is frozen) ───────────────────────────────────────
ARCH="${ARCH:-x86_64}"
export ARCH
export B1NIX_ARCH="$ARCH"
KERNEL_CMDLINE="${KERNEL_CMDLINE:-b1nix.test=1}"
ISO_TARGET="${ISO_TARGET:-iso}"
CROSS_CC="$(ls "$ROOT_DIR/build/${ARCH}/toolchain/llvm/cross/bin/${ARCH}-b1nix-cc" "$ROOT_DIR/build/${ARCH}/toolchain/${ARCH}-b1nix/cross/bin/${ARCH}-b1nix-cc" 2>/dev/null | head -1)"

# ── Flags (CLI flag OR WITH_* env var) ───────────────────────────────────────
FORCE_CROSS="${WITH_CROSS:-0}"
WITH_NATIVE_TOOLCHAIN="${WITH_NATIVE_TOOLCHAIN:-0}"
WITH_NATIVE_CLANG="${WITH_NATIVE_CLANG:-0}"
WITH_DYNAMIC_CLANG="${WITH_DYNAMIC_CLANG:-0}"
WITH_RUST="${WITH_RUST:-0}"
WITH_V8="${WITH_V8:-0}"
WITH_MESA_LLVMPIPE="${WITH_MESA_LLVMPIPE:-0}"
WITH_PORTS="${WITH_PORTS:-0}"

usage() { sed -n '2,60p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

for arg in "$@"; do
	case "$arg" in
		--with-cross)            FORCE_CROSS=1 ;;
		--with-native-toolchain) WITH_NATIVE_TOOLCHAIN=1 ;;
		--with-native-clang)     WITH_NATIVE_CLANG=1 ;;
		--with-dynamic-clang)    WITH_DYNAMIC_CLANG=1 ;;
		--with-rust)             WITH_RUST=1 ;;
		--with-v8)               WITH_V8=1 ;;
		--with-mesa-llvmpipe)    WITH_MESA_LLVMPIPE=1 ;;
		--with-ports)            WITH_PORTS=1 ;;
		--all)
			WITH_NATIVE_TOOLCHAIN=1; WITH_NATIVE_CLANG=1; WITH_DYNAMIC_CLANG=1
			WITH_RUST=1; WITH_V8=1; WITH_MESA_LLVMPIPE=1; WITH_PORTS=1 ;;
		-h|--help) usage; exit 0 ;;
		*) echo "build-all: unknown argument '$arg' (try --help)" >&2; exit 2 ;;
	esac
done

# ── Stage runner: banner + failure attribution ───────────────────────────────
CUR_LABEL=""; CUR_SCRIPT=""; CUR_NO=0; TOTAL=0
on_fail() {
	echo >&2
	echo "!!! build-all FAILED at stage [$CUR_NO/$TOTAL] $CUR_LABEL" >&2
	echo "!!!   invoking: $CUR_SCRIPT" >&2
	exit 1
}
trap on_fail ERR

# ── Stage implementations (pure orchestration — no inlined logic) ────────────
stage_cross() {
	if [ -x "$CROSS_GCC" ] && [ "$FORCE_CROSS" != "1" ]; then
		echo "  cross compiler already present ($CROSS_GCC) — skipping (use --with-cross to force)"
		return 0
	fi
	tools/toolchain/build-toolchain.sh
}

stage_userspace() {
	make ARCH="$ARCH" userspace
}

stage_native_toolchain() {
	tools/toolchain/build-native-toolchain.sh
	make ARCH="$ARCH" install-native-toolchain
}

stage_native_clang() {
	tools/build-native-clang.sh --b1nix-elf
	make ARCH="$ARCH" install-native-toolchain
}

stage_dynamic_clang() {
	tools/build-native-clang-dynamic.sh
}

stage_rust() {
	tools/build-rust-toolchain.sh
}

stage_v8() {
	if [ ! -d "$ROOT_DIR/build/x86_64/toolchain/v8-skeleton/v8" ]; then
		echo "  V8 source tree missing (build/x86_64/toolchain/v8-skeleton/v8)." >&2
		echo "  Run tools/v8/sync-v8.sh first (multi-GB checkout)." >&2
		return 1
	fi
	sh tools/v8/v8-build-run.sh
}

stage_mesa_llvmpipe() {
	# build-mesa.sh prints ONLY its install dir on stdout (progress -> stderr).
	local mesa_dir
	mesa_dir="$(MESA_LLVMPIPE=1 B1NIX_ARCH="$ARCH" tools/ports/build-mesa.sh)"
	echo "  Mesa (llvmpipe) installed at: $mesa_dir"
}

stage_ports() {
	make ARCH="$ARCH" install-ports
}

stage_kernel_iso() {
	make ARCH="$ARCH" KERNEL_CMDLINE="$KERNEL_CMDLINE" "$ISO_TARGET"
}

# ── Build the enabled-stage list (label / reported-script / function) ────────
LABELS=(); SCRIPTS=(); FUNCS=()
add() { LABELS+=("$1"); SCRIPTS+=("$2"); FUNCS+=("$3"); }

add "cross toolchain"      "tools/toolchain/build-toolchain.sh"          stage_cross
add "userspace"            "make ARCH=$ARCH userspace"                   stage_userspace
[ "$WITH_NATIVE_CLANG" = "1" ]     && add "native Clang"         "tools/build-native-clang.sh --b1nix-elf"   stage_native_clang
[ "$WITH_DYNAMIC_CLANG" = "1" ]    && add "dynamic Clang"        "tools/build-native-clang-dynamic.sh"       stage_dynamic_clang
[ "$WITH_RUST" = "1" ]             && add "rust cross"           "tools/rust/build-rust.sh --toolchain"      stage_rust
[ "$WITH_V8" = "1" ]               && add "V8 (d8)"              "tools/v8/build-v8.sh --build"              stage_v8
[ "$WITH_MESA_LLVMPIPE" = "1" ]    && add "Mesa llvmpipe"        "MESA_LLVMPIPE=1 tools/ports/build-mesa.sh" stage_mesa_llvmpipe
[ "$WITH_PORTS" = "1" ]            && add "userspace ports"      "make ARCH=$ARCH install-ports"             stage_ports
add "kernel + ISO"         "make ARCH=$ARCH KERNEL_CMDLINE='$KERNEL_CMDLINE' $ISO_TARGET" stage_kernel_iso

TOTAL=${#FUNCS[@]}

# ── ccache stats: zero the counters so the summary below reflects only this run.
HAVE_CCACHE=0
if command -v ccache >/dev/null 2>&1 && [ "${B1NIX_NO_CCACHE:-0}" != "1" ]; then
	HAVE_CCACHE=1
	ccache -z >/dev/null 2>&1 || true
fi
BUILD_START=$(date +%s 2>/dev/null || echo 0)

echo "============================================================"
echo " b1nix build-all — ARCH=$ARCH, $TOTAL stage(s)"
echo "============================================================"

for idx in "${!FUNCS[@]}"; do
	CUR_NO=$((idx + 1))
	CUR_LABEL="${LABELS[$idx]}"
	CUR_SCRIPT="${SCRIPTS[$idx]}"
	echo
	echo "=== [$CUR_NO/$TOTAL] $CUR_LABEL: $CUR_SCRIPT ==="
	"${FUNCS[$idx]}"
done

BUILD_END=$(date +%s 2>/dev/null || echo 0)
ELAPSED=$((BUILD_END - BUILD_START))

echo
echo "============================================================"
echo " build-all: all $TOTAL stage(s) completed for ARCH=$ARCH"
echo " ISO: build/$ARCH/b1nix.iso"
printf ' elapsed: %dm%02ds\n' "$((ELAPSED / 60))" "$((ELAPSED % 60))"
if [ "$HAVE_CCACHE" = "1" ]; then
	echo " ccache stats for this run:"
	ccache -s 2>/dev/null | sed 's/^/   /'
fi
echo "============================================================"
